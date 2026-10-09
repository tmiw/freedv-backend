// Tests for ThreadedObject: tasks run in order on the object's thread, and
// once a derived class's destructor has waited for its tasks
// (waitForAllTasksComplete_(), as TcpConnectionHandler/SocketIoClient do), no
// task may still be running or start afterwards -- otherwise it runs against a
// destroyed object.

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "../ThreadedObject.h"

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

// Outlives each TestObject so tasks can record what they saw.
struct Tracker
{
    std::atomic<bool> destroyed{false};
    std::atomic<int> ran{0};
    std::atomic<int> violations{0};
};

class TestObject : public ThreadedObject
{
public:
    explicit TestObject(std::shared_ptr<Tracker> tracker)
        : ThreadedObject("TestObject")
        , tracker_(std::move(tracker))
    {
    }

    virtual ~TestObject()
    {
        // As TcpConnectionHandler and SocketIoClient do: wait for our tasks
        // before our members go away.
        waitForAllTasksComplete_();
        tracker_->destroyed = true;
    }

    // Queues a task that checks it isn't running on a destroyed object, both
    // when it starts and when it finishes.
    void enqueueChecked(std::chrono::microseconds work)
    {
        auto tracker = tracker_;
        enqueue_([tracker, work]() {
            if (tracker->destroyed) tracker->violations++;
            std::this_thread::sleep_for(work);
            tracker->ran++;
            if (tracker->destroyed) tracker->violations++;
        });
    }

    void enqueue(std::function<void()> fn) { enqueue_(std::move(fn)); }
    void waitAll() { waitForAllTasksComplete_(); }

private:
    std::shared_ptr<Tracker> tracker_;
};

bool testOrdering()
{
    std::cout << "Test 1 (tasks run in order on one thread): ";

    auto tracker = std::make_shared<Tracker>();
    std::vector<int> order;
    std::thread::id firstThread;
    std::atomic<bool> sameThread(true);
    {
        TestObject obj(tracker);
        for (int i = 0; i < 100; i++)
        {
            obj.enqueue([&, i]() {
                if (i == 0) firstThread = std::this_thread::get_id();
                else if (std::this_thread::get_id() != firstThread) sameThread = false;
                order.push_back(i);
            });
        }
        obj.waitAll();
    }

    bool result = CHECK(order.size() == 100);
    for (int i = 0; i < (int)order.size(); i++)
    {
        if (order[i] != i)
        {
            result &= CHECK(order[i] == i);
            break;
        }
    }
    result &= CHECK(sameThread.load());
    result &= CHECK(firstThread != std::this_thread::get_id());
    return report(result);
}

bool testLongTaskBlocksDestruction()
{
    std::cout << "Test 2 (destructor waits for a task longer than 250 ms): ";

    // The Linux implementation used to give up waiting after ~250 ms and let
    // the destructor continue while the task was still running.
    auto tracker = std::make_shared<Tracker>();
    {
        TestObject obj(tracker);
        obj.enqueueChecked(600ms);
        std::this_thread::sleep_for(20ms); // let it start
    }

    bool result = CHECK(tracker->ran == 1);
    result &= CHECK(tracker->violations == 0);
    return report(result);
}

bool testDestroyWithTasksInFlight()
{
    std::cout << "Test 3 (no task runs after destruction, many cycles): ";

    // Repeatedly destroy objects with tasks queued and running, from the main
    // thread and a second thread enqueueing concurrently. Any task that
    // starts or is still running after the destructor's wait returned is a
    // violation.
    auto tracker = std::make_shared<Tracker>();
    int cycles = 0;
    for (int i = 0; i < 300; i++)
    {
        tracker->destroyed = false;
        auto obj = std::make_unique<TestObject>(tracker);
        std::atomic<bool> stop(false);
        std::thread producer([&]() {
            int n = 0;
            while (!stop && n < 20)
            {
                obj->enqueueChecked(std::chrono::microseconds(50 + (n % 4) * 50));
                n++;
            }
        });
        for (int n = 0; n < 5; n++)
        {
            obj->enqueueChecked(std::chrono::microseconds((i + n) % 3 * 100));
        }
        std::this_thread::sleep_for(std::chrono::microseconds((i % 5) * 200));
        stop = true;
        producer.join();
        obj.reset();
        cycles++;
    }
    std::this_thread::sleep_for(20ms);

    std::cout << "(" << cycles << " cycles, " << tracker->ran << " tasks, "
              << tracker->violations << " violations) ";
    bool result = CHECK(tracker->violations == 0);
    result &= CHECK(tracker->ran > 0);
    return report(result);
}

#if !defined(__APPLE__)
bool testWaitFromOwnTask()
{
    std::cout << "Test 4 (waiting from the object's own task returns): ";

    // A task can't wait for its own queue (it would wait for itself); the
    // call must return instead of hanging. (Linux implementation only; the
    // GCD-based macOS implementation counts the running task as queued.)
    auto tracker = std::make_shared<Tracker>();
    std::atomic<bool> returned(false);
    {
        TestObject obj(tracker);
        obj.enqueue([&]() {
            obj.waitAll();
            returned = true;
        });
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!returned && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
    }
    return report(CHECK(returned.load()));
}
#endif // !defined(__APPLE__)

} // namespace

int main(int, char**)
{
    bool result = true;

    result &= testOrdering();
    result &= testLongTaskBlocksDestruction();
    result &= testDestroyWithTasksInFlight();
#if !defined(__APPLE__)
    result &= testWaitFromOwnTask();
#endif // !defined(__APPLE__)

    return result ? 0 : -1;
}
