//=========================================================================
// Name:            ThreadedTimer.cpp
// Purpose:         Timer object with minimal dependencies.
//
// Authors:         Mooneer Salem
// License:
//
//  All rights reserved.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//=========================================================================

#include "ThreadedTimer.h"
#include "../os/os_interface.h"

#include <algorithm>
#include <cinttypes>

#if defined(__APPLE__)
#include <pthread.h>
#endif // defined(__APPLE__)

ThreadedTimer::TimerServer ThreadedTimer::TheTimerServer_;

ThreadedTimer::TimerServer::TimerServer()
    : isDestroying_(false)
    , firingTimer_(nullptr)
{
    objectThread_ = std::thread(std::bind(&ThreadedTimer::TimerServer::eventLoop_, this));
}

ThreadedTimer::TimerServer::~TimerServer()
{
    {
        std::unique_lock<std::mutex> lk(mutex_);
        isDestroying_.store(true, std::memory_order_relaxed);
    }
    timerCV_.notify_one();
    objectThread_.join();
}

void ThreadedTimer::TimerServer::removeLocked_(ThreadedTimer* timer)
{
    auto it = std::find(timerQueue_.begin(), timerQueue_.end(), timer);
    if (it != timerQueue_.end())
    {
        timerQueue_.erase(it);
        std::make_heap(timerQueue_.begin(), timerQueue_.end(), FireTimeComparator());
    }
}

void ThreadedTimer::TimerServer::registerTimer(ThreadedTimer* timer, int intervalMs, bool repeat)
{
    {
        std::unique_lock<std::mutex> lk(mutex_);
        removeLocked_(timer);
        timer->scheduledIntervalMs_ = intervalMs;
        timer->scheduledRepeat_ = repeat;
        timer->nextFireTime_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMs);
        timer->isRunning_.store(true, std::memory_order_relaxed);
        timerQueue_.push_back(timer);
        std::push_heap(timerQueue_.begin(), timerQueue_.end(), FireTimeComparator());
    }
    timerCV_.notify_one(); // update wait time
}

void ThreadedTimer::TimerServer::unregisterTimer(ThreadedTimer* timer)
{
    {
        std::unique_lock<std::mutex> lk(mutex_);
        removeLocked_(timer);
        timer->isRunning_.store(false, std::memory_order_relaxed);

        // Wait out a callback that's already running so the caller can destroy
        // the timer (and whatever the callback uses) afterwards. A callback
        // stopping its own timer runs on this server's thread and must not
        // wait for itself.
        if (std::this_thread::get_id() != objectThread_.get_id())
        {
            firingDoneCV_.wait(lk, [&]() { return firingTimer_ != timer; });
        }
    }
    timerCV_.notify_one(); // update wait time
}

void ThreadedTimer::TimerServer::eventLoop_()
{
#if defined(__APPLE__)
    // Timer callbacks previously ran on GCD's utility-QoS queue; keep that.
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif // defined(__APPLE__)

    SetThreadName("Timer");

    std::unique_lock<std::mutex> lk(mutex_);
    while (!isDestroying_.load(std::memory_order_relaxed))
    {
        if (timerQueue_.empty())
        {
            timerCV_.wait(lk);
            continue;
        }

        auto nextFireTime = timerQueue_.front()->nextFireTime_;
        if (std::chrono::steady_clock::now() < nextFireTime)
        {
            timerCV_.wait_until(lk, nextFireTime);
            continue;
        }

        // Earliest timer is due. Reschedule (or retire) it before running its
        // callback, while still holding mutex_, so a concurrent stop() always
        // finds it in the queue and a stopped timer can never be re-added.
        std::pop_heap(timerQueue_.begin(), timerQueue_.end(), FireTimeComparator());
        ThreadedTimer* timer = timerQueue_.back();
        timerQueue_.pop_back();
        if (timer->scheduledRepeat_)
        {
            timer->nextFireTime_ += std::chrono::milliseconds(timer->scheduledIntervalMs_);
            timerQueue_.push_back(timer);
            std::push_heap(timerQueue_.begin(), timerQueue_.end(), FireTimeComparator());
        }
        else
        {
            timer->isRunning_.store(false, std::memory_order_relaxed);
        }

        // Run the callback without holding mutex_ (it may start/stop timers);
        // unregisterTimer() waits on firingTimer_ instead.
        firingTimer_ = timer;
        lk.unlock();
        timer->fire_();
        lk.lock();
        firingTimer_ = nullptr;
        firingDoneCV_.notify_all();
    }
}

void ThreadedTimer::fire_()
{
    TimerCallbackFn fn;
    {
        std::unique_lock<std::mutex> lk(timerMutex_);
        fn = fn_;
    }
    if (fn)
    {
        fn(*this);
    }
}
ThreadedTimer::ThreadedTimer()
    : repeat_(false)
    , timeoutMilliseconds_(0)
{
    isRunning_.store(false, std::memory_order_relaxed);
}

ThreadedTimer::ThreadedTimer(int milliseconds, TimerCallbackFn fn, bool repeat)
{
    isRunning_.store(false, std::memory_order_relaxed);
    setTimeout(milliseconds);
    setCallback(std::move(fn));
    setRepeat(repeat);
}

ThreadedTimer::~ThreadedTimer()
{
    stop();
}

void ThreadedTimer::setTimeout(int milliseconds)
{
    std::unique_lock<std::mutex> lk(timerMutex_);
    timeoutMilliseconds_ = milliseconds;
}

void ThreadedTimer::setCallback(TimerCallbackFn fn)
{
    std::unique_lock<std::mutex> lk(timerMutex_);
    fn_ = std::move(fn);
}

void ThreadedTimer::setRepeat(bool repeat)
{
    std::unique_lock<std::mutex> lk(timerMutex_);
    repeat_ = repeat;
}

bool ThreadedTimer::isRunning()
{
    return isRunning_.load(std::memory_order_relaxed);
}
    
void ThreadedTimer::start()
{
    int intervalMs = 0;
    bool repeat = false;
    {
        std::unique_lock<std::mutex> lk(timerMutex_);
        intervalMs = timeoutMilliseconds_;
        repeat = repeat_;
    }
    TheTimerServer_.registerTimer(this, intervalMs, repeat);
}

void ThreadedTimer::stop()
{
    // Always unregister, even if not scheduled: a one-shot timer's callback
    // may be running right now, and unregisterTimer() waits for it.
    TheTimerServer_.unregisterTimer(this);
}

void ThreadedTimer::restart()
{
    // registerTimer() replaces any existing schedule.
    start();
}
