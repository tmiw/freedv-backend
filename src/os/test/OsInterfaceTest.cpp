// Tests for the parts of os_interface.h that need no GUI or user approval:
// the OS name reported to FreeDV Reporter, thread naming, the low-latency
// activity (macOS) and the microphone check where it can't prompt.

#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif // defined(_WIN32)

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#endif // defined(__APPLE__)

#include "os/os_interface.h"

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

// The calling thread's name as the OS reports it ("" if unavailable).
std::string currentThreadName()
{
#if defined(_WIN32)
    using GetThreadDescriptionFn = HRESULT (WINAPI*)(HANDLE, PWSTR*);
    auto getThreadDescription = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    if (getThreadDescription == nullptr)
    {
        return "";
    }
    PWSTR description = nullptr;
    std::string result;
    if (SUCCEEDED(getThreadDescription(GetCurrentThread(), &description)) && description != nullptr)
    {
        for (PWSTR p = description; *p != 0; p++)
        {
            result += (char)*p; // names are ASCII
        }
        LocalFree(description);
    }
    return result;
#else
    char name[64] = {0};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    return name;
#endif // defined(_WIN32)
}

// Whether the OS can report thread names back here at all.
bool canReadThreadNames()
{
#if defined(_WIN32)
    return GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription") != nullptr;
#elif defined(__linux__) || defined(__APPLE__)
    return true;
#else
    return false;
#endif // defined(_WIN32)
}

// Names a new thread and returns what the OS then reports for it.
std::string nameOfThreadCalled(const std::string& name)
{
    std::string result;
    std::thread([&]() {
        SetThreadName(name);
        result = currentThreadName();
    }).join();
    return result;
}

bool testOperatingSystemString()
{
    std::cout << "Test 1 (operating system name sent to FreeDV Reporter): ";

#if defined(_WIN32)
    const char* expected = "windows";
#elif defined(__APPLE__)
    const char* expected = "macos";
#elif defined(__linux__)
    const char* expected = "linux";
#else
    const char* expected = "other";
#endif // defined(_WIN32)
    bool result = CHECK(GetOperatingSystemString() == expected);
    return report(result);
}

bool testThreadNames()
{
    std::cout << "Test 2 (thread names are set with the \"FDV \" prefix): ";

    if (!canReadThreadNames())
    {
        std::cout << "SKIP (can't read thread names back here) ";
        return report(true);
    }

    bool result = CHECK(nameOfThreadCalled("Timer") == "FDV Timer");

    // Linux allows 15 characters and used to reject longer names outright,
    // leaving e.g. freedv-gui's SerialController thread unnamed. They're
    // now shortened there; other systems keep the whole name.
    std::string longName = nameOfThreadCalled("SerialController");
#if defined(__linux__)
    result &= CHECK(longName == "FDV SerialContr");
#else
    result &= CHECK(longName == "FDV SerialController");
#endif // defined(__linux__)

    return report(result);
}

#if defined(__APPLE__)
// Power-management assertions (e.g. "prevent idle sleep") held by this
// process, which is what an NSProcessInfo activity creates.
int ownPowerAssertions()
{
    CFDictionaryRef byProcess = nullptr;
    if (IOPMCopyAssertionsByProcess(&byProcess) != kIOReturnSuccess || byProcess == nullptr)
    {
        return -1;
    }
    int pid = getpid();
    CFNumberRef key = CFNumberCreate(nullptr, kCFNumberIntType, &pid);
    CFArrayRef assertions = (CFArrayRef)CFDictionaryGetValue(byProcess, key);
    int count = assertions != nullptr ? (int)CFArrayGetCount(assertions) : 0;
    CFRelease(key);
    CFRelease(byProcess);
    return count;
}
#endif // defined(__APPLE__)

bool testLowLatencyActivity()
{
    std::cout << "Test 3 (low-latency activity: start/stop pairs, repeats and stray stops): ";

    bool result = true;

    // A stop without a start, and repeated stops, must be harmless.
    StopLowLatencyActivity();

#if defined(__APPLE__)
    int baseline = ownPowerAssertions();
    result &= CHECK(baseline >= 0);

    StartLowLatencyActivity();
    result &= CHECK(ownPowerAssertions() > baseline);
    StopLowLatencyActivity();
    result &= CHECK(ownPowerAssertions() == baseline);

    // Starting twice used to leak the first activity (and its assertion)
    // until the process exited.
    StartLowLatencyActivity();
    StartLowLatencyActivity();
    StopLowLatencyActivity();
    result &= CHECK(ownPowerAssertions() == baseline);
#else
    StartLowLatencyActivity();
    StartLowLatencyActivity();
    StopLowLatencyActivity();
#endif // defined(__APPLE__)

    StopLowLatencyActivity();
    return report(result);
}

bool testMicrophonePermissions()
{
    std::cout << "Test 4 (microphone permission check answers without prompting): ";

#if defined(__APPLE__)
    // On macOS this asks the user if they haven't decided yet, which a test
    // can't answer.
    std::cout << "SKIP (would prompt on macOS) ";
    return report(true);
#else
    std::promise<bool> promise;
    auto future = promise.get_future();
    VerifyMicrophonePermissions(promise);
    bool result = CHECK(future.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    result &= CHECK(future.get());
    return report(result);
#endif // defined(__APPLE__)
}

} // namespace

int main()
{
    bool result = true;
    result &= testOperatingSystemString();
    result &= testThreadNames();
    result &= testLowLatencyActivity();
    result &= testMicrophonePermissions();
    return result ? 0 : -1;
}
