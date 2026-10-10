//=========================================================================
// Name:            ThreadedTimer.h
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

#ifndef THREADED_TIMER_H
#define THREADED_TIMER_H

#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>
#include <chrono>
#include <functional>
#include <atomic>

class ThreadedTimer
{
public:
    using TimerCallbackFn = std::function<void(ThreadedTimer&)>;
    
    ThreadedTimer();
    ThreadedTimer(int milliseconds, TimerCallbackFn fn, bool repeat);
    virtual ~ThreadedTimer();

    void setTimeout(int milliseconds);
    void setCallback(TimerCallbackFn fn);
    void setRepeat(bool repeat);
    
    void start();
    void stop();
    void restart();
    
    bool isRunning();

private:
    // A single thread that fires every ThreadedTimer. All scheduling state
    // (the queue and each timer's nextFireTime_/scheduledIntervalMs_/
    // scheduledRepeat_) is only touched under mutex_, so the server never
    // dereferences a timer that's been unregistered.
    class TimerServer
    {
    public:
        TimerServer();
        virtual ~TimerServer();

        void registerTimer(ThreadedTimer* timer, int intervalMs, bool repeat);

        // Removes the timer and, unless called from the timer thread (i.e.
        // from a timer callback), waits for any callback of it that's
        // currently running, so the timer can be destroyed safely afterwards.
        void unregisterTimer(ThreadedTimer* timer);

    private:
        struct FireTimeComparator
        {
            inline bool operator()(const ThreadedTimer* lhs, const ThreadedTimer* rhs) const
            {
                return lhs->nextFireTime_ > rhs->nextFireTime_;
            }
        };

        std::mutex mutex_;
        // Shutdown flag; timerQueue_ is guarded by mutex_ and the final barrier is
        // objectThread_.join(), so relaxed ordering is enough.
        std::atomic<bool> isDestroying_;
        std::thread objectThread_;
        std::condition_variable timerCV_;
        std::condition_variable firingDoneCV_;
        ThreadedTimer* firingTimer_; // timer whose callback is running, guarded by mutex_
        std::vector<ThreadedTimer*> timerQueue_; // min-heap on nextFireTime_, guarded by mutex_

        void removeLocked_(ThreadedTimer* timer);
        void eventLoop_();
    };

    // "Is this timer currently scheduled" state, for isRunning(). Relaxed
    // ordering is enough; scheduling itself is guarded by the server's mutex_.
    std::atomic<bool> isRunning_;

    // Guarded by TheTimerServer_.mutex_.
    std::chrono::time_point<std::chrono::steady_clock> nextFireTime_;
    int scheduledIntervalMs_ = 0;
    bool scheduledRepeat_ = false;

    void fire_();

    static TimerServer TheTimerServer_;

    // Guards the timer's configuration (fn_, repeat_, timeoutMilliseconds_).
    std::mutex timerMutex_;
    
    TimerCallbackFn fn_;
    bool repeat_;
    int timeoutMilliseconds_;
};

#endif // THREADED_TIMER_H
