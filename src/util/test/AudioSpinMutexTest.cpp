// Tests for audio_spin_mutex: try_lock()/unlock() semantics, mutual exclusion
// under contention, and that a thread spinning in lock() picks the lock up
// promptly once it's released (it's used where the audio thread may wait on
// another thread, so a slow hand-off means an audio glitch).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "../audio_spin_mutex.h"

using namespace std::chrono_literals;

namespace {

#define CHECK(expr) checkImpl_((expr), #expr, __LINE__)

bool checkImpl_(bool ok, const char* expr, int line)
{
    if (!ok)
    {
        std::cout << "\n    check failed at line " << line << ": " << expr << "\n    ";
    }
    return ok;
}

bool report(bool result)
{
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testTryLock()
{
    std::cout << "Test 1 (try_lock/unlock semantics): ";

    audio_spin_mutex mtx;
    bool result = CHECK(mtx.try_lock());
    result &= CHECK(!mtx.try_lock());
    mtx.unlock();
    result &= CHECK(mtx.try_lock());
    mtx.unlock();

    // lock() on an uncontended mutex returns immediately.
    mtx.lock();
    result &= CHECK(!mtx.try_lock());
    mtx.unlock();

    return report(result);
}

bool testMutualExclusion()
{
    std::cout << "Test 2 (mutual exclusion under contention): ";

    constexpr int NUM_THREADS = 4;
    constexpr int ITERATIONS = 200000;

    audio_spin_mutex mtx;
    long counter = 0;               // deliberately not atomic: protected by mtx
    std::atomic<int> inside(0);     // detects overlapping critical sections
    std::atomic<bool> overlapped(false);

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++)
    {
        threads.emplace_back([&]() {
            for (int i = 0; i < ITERATIONS; i++)
            {
                std::lock_guard<audio_spin_mutex> guard(mtx);
                if (inside.fetch_add(1, std::memory_order_relaxed) != 0)
                {
                    overlapped.store(true, std::memory_order_relaxed);
                }
                counter++;
                inside.fetch_sub(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    bool result = CHECK(counter == (long)NUM_THREADS * ITERATIONS);
    result &= CHECK(!overlapped.load());
    return report(result);
}

bool testBlockedLockWaits()
{
    std::cout << "Test 3 (lock() waits while held, including the yield phase): ";

    audio_spin_mutex mtx;
    mtx.lock();

    std::atomic<bool> acquired(false);
    std::thread waiter([&]() {
        mtx.lock();
        acquired.store(true, std::memory_order_release);
        mtx.unlock();
    });

    // Long enough for the waiter to exhaust its spin phases and reach the
    // yield loop.
    std::this_thread::sleep_for(100ms);
    bool result = CHECK(!acquired.load(std::memory_order_acquire));

    mtx.unlock();
    waiter.join();
    result &= CHECK(acquired.load(std::memory_order_acquire));
    return report(result);
}

bool testHandoffLatency()
{
    std::cout << "Test 4 (waiting thread acquires promptly after unlock): ";

    // The holder releases the lock and timestamps the release; a thread
    // already spinning in lock() records when it gets in. Report the median
    // and worst hand-off. These are generous bounds for a loaded CI machine:
    // a spinning waiter should normally get in within microseconds.
    constexpr int ROUNDS = 200;
    audio_spin_mutex mtx;
    std::vector<double> latenciesUs;

    for (int round = 0; round < ROUNDS; round++)
    {
        std::atomic<bool> waiting(false);
        std::chrono::steady_clock::time_point releasedAt;
        std::chrono::steady_clock::time_point acquiredAt;

        mtx.lock();
        std::thread waiter([&]() {
            waiting.store(true, std::memory_order_release);
            mtx.lock();
            acquiredAt = std::chrono::steady_clock::now();
            mtx.unlock();
        });

        while (!waiting.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        // Give the waiter time to be inside lock()'s spin loop.
        std::this_thread::sleep_for(200us);

        releasedAt = std::chrono::steady_clock::now();
        mtx.unlock();
        waiter.join();

        latenciesUs.push_back(std::chrono::duration<double, std::micro>(acquiredAt - releasedAt).count());
    }

    std::sort(latenciesUs.begin(), latenciesUs.end());
    double median = latenciesUs[ROUNDS / 2];
    double p95 = latenciesUs[ROUNDS * 95 / 100];
    std::cout << "[median " << median << " us, p95 " << p95 << " us, max " << latenciesUs.back() << " us] ";

    bool result = CHECK(median < 100.0);
    result &= CHECK(p95 < 1000.0);
    return report(result);
}

} // namespace

int main(int, char**)
{
    bool result = true;

    result &= testTryLock();
    result &= testMutualExclusion();
    result &= testBlockedLockWaits();
    result &= testHandoffLatency();

    return result ? 0 : -1;
}
