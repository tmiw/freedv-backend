// Tests for the real-time-safe async ulog front end (ulog_async.{h,cpp}).
//
// stderr (ulog's default sink) is redirected to a temp file around each
// exercise so the rendered output can be inspected.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#define DUP _dup
#define DUP2 _dup2
#define FILENO _fileno
#define CLOSE _close
#define READ _read
#define LSEEK _lseek
#else
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define FILENO fileno
#define CLOSE close
#define READ read
#define LSEEK lseek
#endif

#include "../logging/ulog.h"
#include "../logging/ulog_async.h"

namespace
{

std::string captureStderr(const std::function<void()>& fn)
{
    std::fflush(stderr);

#if defined(_WIN32)
    char path[L_tmpnam];
    tmpnam(path);
    int fd = _open(path, _O_CREAT | _O_RDWR | _O_BINARY, 0600);
#else
    char path[] = "/tmp/ulog_async_test_XXXXXX";
    int fd = mkstemp(path);
#endif
    if (fd < 0)
    {
        std::cerr << "captureStderr: could not create temp file\n";
        return {};
    }

    int saved = DUP(FILENO(stderr));
    DUP2(fd, FILENO(stderr));

    fn();

    std::fflush(stderr);
    DUP2(saved, FILENO(stderr));
    CLOSE(saved);

    LSEEK(fd, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    for (;;)
    {
        auto n = READ(fd, buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    CLOSE(fd);
    std::remove(path);
    return out;
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

size_t countOccurrences(const std::string& hay, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size()))
    {
        ++n;
    }
    return n;
}

// -------------------------------------------------------------------------

bool testDeferredFormatting()
{
    std::cout << "Test 1 (deferred formatting from a realtime thread): ";

    std::string out = captureStderr([]() {
        ulog_async_start();
        std::thread rt([]() {
            ulog_set_thread_realtime(true);
            log_info("int=%d str=%s flt=%.2f", 42, "hello", 3.14159);
            log_warn("widths l=%ld ll=%lld u=%u z=%zu", 7L, 8LL, 9u,
                     static_cast<size_t>(10));
            ulog_set_thread_realtime(false);
        });
        rt.join();
        ulog_async_flush();
        ulog_async_stop();
    });

    bool result = contains(out, "int=42 str=hello flt=3.14");
    result &= contains(out, "widths l=7 ll=8 u=9 z=10");
    // File/line of the original call site must survive.
    result &= contains(out, "UlogAsyncTest.cpp:");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testNonRealtimeUnaffected()
{
    std::cout << "Test 2 (non-realtime threads stay synchronous): ";

    std::string out = captureStderr([]() {
        ulog_async_start();
        // No ulog_set_thread_realtime(true): must be written inline, before
        // any flush.
        log_info("synchronous %d", 123);
        ulog_async_stop();
    });

    bool result = contains(out, "synchronous 123");
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testNullAndLongString()
{
    std::cout << "Test 3 (NULL and over-long %s are handled): ";

    std::string big(4096, 'A');

    std::string out = captureStderr([&]() {
        ulog_async_start();
        std::thread rt([&]() {
            ulog_set_thread_realtime(true);
            const char* np = nullptr;
            log_info("null=[%s]", np);
            log_info("big=[%s]", big.c_str());
            ulog_set_thread_realtime(false);
        });
        rt.join();
        ulog_async_flush();
        ulog_async_stop();
    });

    bool result = contains(out, "null=[(null)]");
    // The long string must be truncated, not crash, and be flagged.
    result &= contains(out, "big=[AAAA");
    result &= contains(out, "truncated");
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out.substr(0, 400) << "...\n---\n";
    return result;
}

bool testDropsWhenNotStarted()
{
    std::cout << "Test 4 (records are dropped, counted, not written when consumer is down): ";

    unsigned long before = ulog_async_dropped_count();

    std::string out = captureStderr([]() {
        std::thread rt([]() {
            ulog_set_thread_realtime(true);
            for (int i = 0; i < 20; ++i)
            {
                log_info("must be dropped %d", i);
            }
            ulog_set_thread_realtime(false);
        });
        rt.join();
    });

    unsigned long dropped = ulog_async_dropped_count() - before;
    bool result = (dropped == 20);
    result &= !contains(out, "must be dropped");
    std::cout << (result ? "PASS" : "FAIL") << " (dropped=" << dropped << ")\n";
    return result;
}

// For each producer thread, walks the "prod T seq I" lines in the order they
// appear in the output and checks the I values are strictly increasing -- i.e.
// the consumer never reordered or duplicated a given thread's records.
bool survivorsInOrder(const std::string& out, int threads, int perThread,
                      size_t& totalSurvivors)
{
    totalSurvivors = 0;
    for (int t = 0; t < threads; ++t)
    {
        std::string prefix = "prod " + std::to_string(t) + " seq ";
        int lastSeq = -1;
        for (size_t at = out.find(prefix); at != std::string::npos;
             at = out.find(prefix, at + prefix.size()))
        {
            int seq = std::atoi(out.c_str() + at + prefix.size());
            if (seq <= lastSeq) return false; // reordered or duplicated
            lastSeq = seq;
            ++totalSurvivors;
            if (seq >= perThread) return false; // garbled
        }
    }
    return true;
}

bool testMultiProducerInvariants()
{
    std::cout << "Test 5 (many realtime producers: accounting + ordering hold): ";

    constexpr int kThreads = 6;
    constexpr int kPerThread = 400;
    constexpr int kTotal = kThreads * kPerThread;

    unsigned long before = ulog_async_dropped_count();

    std::string out = captureStderr([&]() {
        ulog_async_start();

        std::vector<std::thread> producers;
        for (int t = 0; t < kThreads; ++t)
        {
            producers.emplace_back([t]() {
                ulog_set_thread_realtime(true);
                for (int i = 0; i < kPerThread; ++i)
                {
                    log_info("prod %d seq %d", t, i);
                }
                ulog_set_thread_realtime(false);
            });
        }
        for (auto& p : producers) p.join();

        ulog_async_flush();
        ulog_async_stop();
    });

    unsigned long dropped = ulog_async_dropped_count() - before;
    size_t producerLines = countOccurrences(out, "prod ");
    size_t summaryLines = countOccurrences(out, "record(s) dropped");

    size_t survivors = 0;
    bool ordered = survivorsInOrder(out, kThreads, kPerThread, survivors);

    // Core invariants for a lossy-but-honest realtime logger:
    //  * every record is either emitted or counted as dropped, none vanish;
    //  * whatever survives keeps per-thread order;
    //  * the visible producer lines match the survivors we can account for;
    //  * if anything was dropped, the consumer reported it at least once.
    bool result = ordered;
    result &= (producerLines + dropped == static_cast<unsigned long>(kTotal));
    result &= (survivors == producerLines);
    result &= (dropped == 0 || summaryLines >= 1);

    std::cout << (result ? "PASS" : "FAIL")
              << " (emitted=" << producerLines << " dropped=" << dropped
              << " summaries=" << summaryLines << " total=" << kTotal
              << " ordered=" << ordered << ")\n";
    return result;
}

} // namespace

int main()
{
    bool result = true;

    result &= testDeferredFormatting();
    result &= testNonRealtimeUnaffected();
    result &= testNullAndLongString();
    result &= testDropsWhenNotStarted();
    result &= testMultiProducerInvariants();

    return result ? 0 : -1;
}
