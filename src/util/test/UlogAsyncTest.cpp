// Tests for the async ulog front end (ulog_async.{h,cpp}).
//
// The consumer thread starts automatically (static constructor), so these
// tests never call ulog_async_start()/ulog_async_stop() except where they
// deliberately exercise the stop/restart path. Every log_*() call -- from the
// main thread or a worker -- is routed through the ring, so the tests flush
// before inspecting the captured output.
//
// stderr (ulog's default sink) is redirected to a temp file around each
// exercise so the rendered output can be inspected.

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
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
    std::cout << "Test 1 (deferred formatting from a worker thread): ";

    std::string out = captureStderr([]() {
        std::thread rt([]() {
            log_info("int=%d str=%s flt=%.2f", 42, "hello", 3.14159);
            log_warn("widths l=%ld ll=%lld u=%u z=%zu", 7L, 8LL, 9u,
                     static_cast<size_t>(10));
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = contains(out, "int=42 str=hello flt=3.14");
    result &= contains(out, "widths l=7 ll=8 u=9 z=10");
    // File/line of the original call site must survive.
    result &= contains(out, "UlogAsyncTest.cpp:");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testMainThreadRoutedThroughAsync()
{
    std::cout << "Test 2 (main-thread logs go through the async path too): ";

    std::string out = captureStderr([]() {
        log_info("from the main thread %d", 123);
        // Not written inline any more: it only reaches stderr once drained.
        ulog_async_flush();
    });

    bool result = contains(out, "from the main thread 123");
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testNullAndLongString()
{
    std::cout << "Test 3 (NULL and over-long %s are handled): ";

    std::string big(4096, 'A');

    std::string out = captureStderr([&]() {
        std::thread rt([&]() {
            const char* np = nullptr;
            log_info("null=[%s]", np);
            log_info("big=[%s]", big.c_str());
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = contains(out, "null=[(null)]");
    // The long string must be truncated, not crash, and be flagged.
    result &= contains(out, "big=[AAAA");
    result &= contains(out, "truncated");
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out.substr(0, 400) << "...\n---\n";
    return result;
}

bool testSynchronousFallbackWhenStopped()
{
    std::cout << "Test 4 (stop -> synchronous fallback, start -> async resumes): ";

    // While the consumer is stopped, ulog_async_is_active() is false and
    // ulog_log() formats inline: the line is present with no flush at all.
    std::string stopped = captureStderr([]() {
        ulog_async_stop();
        std::thread w([]() { log_info("sync fallback %d", 7); });
        w.join();
        // deliberately no ulog_async_flush()
    });

    bool result = contains(stopped, "sync fallback 7");

    // Bring the consumer back and confirm async delivery works again.
    std::string resumed = captureStderr([]() {
        ulog_async_start();
        std::thread w([]() { log_info("async resumed %d", 9); });
        w.join();
        ulog_async_flush();
    });

    result &= contains(resumed, "async resumed 9");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result)
    {
        std::cout << "--- stopped ---\n" << stopped
                  << "--- resumed ---\n" << resumed << "---\n";
    }
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
    std::cout << "Test 5 (many producers, ring overflows: accounting + ordering hold): ";

    constexpr int kThreads = 6;
    constexpr int kPerThread = 400;
    constexpr int kTotal = kThreads * kPerThread;

    unsigned long before = ulog_async_dropped_count();

    std::string out = captureStderr([&]() {
        std::vector<std::thread> producers;
        for (int t = 0; t < kThreads; ++t)
        {
            producers.emplace_back([t]() {
                for (int i = 0; i < kPerThread; ++i)
                {
                    log_info("prod %d seq %d", t, i);
                }
            });
        }
        for (auto& p : producers) p.join();

        ulog_async_flush();
    });

    unsigned long dropped = ulog_async_dropped_count() - before;
    size_t producerLines = countOccurrences(out, "prod ");
    size_t summaryLines = countOccurrences(out, "record(s) dropped");

    size_t survivors = 0;
    bool ordered = survivorsInOrder(out, kThreads, kPerThread, survivors);

    // Core invariants for a lossy-but-honest logger:
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

// Logs fmt both through ulog (deferred formatting on the consumer thread) and
// through snprintf() directly, and records what snprintf() produced so the
// test can check ulog rendered the same text. The <> markers keep one case
// from matching inside another.
#define LOG_LIKE_PRINTF(expected, fmt, ...)                         \
    do                                                              \
    {                                                               \
        char e_[512];                                               \
        std::snprintf(e_, sizeof e_, "<" fmt ">", __VA_ARGS__);     \
        (expected).push_back(e_);                                   \
        log_info("<" fmt ">", __VA_ARGS__);                         \
    } while (0)

bool testMatchesPrintf()
{
    std::cout << "Test 6 (every supported conversion renders like printf): ";

    std::vector<std::string> expected;
    std::string out = captureStderr([&]() {
        std::thread rt([&]() {
            LOG_LIKE_PRINTF(expected, "100%% of %d%%", 7);
            LOG_LIKE_PRINTF(expected, "c=%c|%-3c|%3c", 'x', 'y', 'z');
            LOG_LIKE_PRINTF(expected, "p=%p", reinterpret_cast<void*>(0x1234));
            LOG_LIKE_PRINTF(expected, "hh=%hhd h=%hd hhu=%hhu hu=%hu", 300, 70000, 300, 70000);
            LOG_LIKE_PRINTF(expected, "hhneg=%hhd hneg=%hd hhx=%hhx hx=%hx", -129, -32769, -1, -1);
            LOG_LIKE_PRINTF(expected, "j=%jd z=%zd t=%td", static_cast<intmax_t>(-5),
                            static_cast<std::ptrdiff_t>(-6), static_cast<std::ptrdiff_t>(-7));
            LOG_LIKE_PRINTF(expected, "lu=%lu llu=%llu ju=%ju zu=%zu tx=%tx", 1UL, 2ULL,
                            static_cast<uintmax_t>(3), static_cast<size_t>(4),
                            static_cast<std::ptrdiff_t>(255));
            LOG_LIKE_PRINTF(expected, "ld=%ld lld=%lld neg", -8L, -9LL);
            LOG_LIKE_PRINTF(expected, "o=%o x=%x X=%X #x=%#x", 8u, 255u, 255u, 255u);
            LOG_LIKE_PRINTF(expected, "flags=%+d|% d|%05d|%-5d|", 3, 3, 3, 3);
            LOG_LIKE_PRINTF(expected, "e=%e g=%g a=%a 5.1f=%5.1f", 1.5, 0.0001, 1.0, 3.14159);
            // Not compared with snprintf(): Windows' C library treats long
            // double as double, but clang/mingw pass an 80-bit long double.
            log_info("<Lf=%Lf>", 2.5L);
            expected.push_back("<Lf=2.500000>");
            LOG_LIKE_PRINTF(expected, "s=%.3s|%10s|%-10s|", "abcdef", "hi", "hi");
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = true;
    for (auto const& e : expected)
    {
        if (!contains(out, e))
        {
            std::cout << "\n    missing: " << e;
            result = false;
        }
    }
    std::cout << (result ? "" : "\n    ") << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testUnsupportedConversions()
{
    std::cout << "Test 7 (unsupported conversions render as <?> and flag truncation): ";

    // None of these may reach a real printf: %n would write through the
    // pointer, and %* / %.* / positional / wide conversions can't be
    // reproduced from the captured arguments. Everything from the first
    // unsupported conversion on renders as <?>.
    int n = 0;
    std::string out = captureStderr([&]() {
        std::thread rt([&]() {
            log_info("n: a=%d %n b=%d", 1, &n, 2);
            log_info("star: %*d|%d", 5, 3, 4);
            log_info("dotstar: %.*f", 2, 1.5);
            log_info("positional: %1$d", 6);
            log_info("wide: %ls %lc", L"w", static_cast<wint_t>(L'w'));
            log_info("unknown: %y");
            log_info("trailing %");
            log_info("trailing lenmod %l");
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = contains(out, "n: a=1 <?> b=<?> [ulog_async: truncated]");
    result &= (n == 0);
    result &= contains(out, "star: <?>|<?> [ulog_async: truncated]");
    result &= contains(out, "dotstar: <?> [ulog_async: truncated]");
    result &= contains(out, "positional: <?> [ulog_async: truncated]");
    result &= contains(out, "wide: <?> <?> [ulog_async: truncated]");
    result &= contains(out, "unknown: <?> [ulog_async: truncated]");
    result &= contains(out, "trailing <?> [ulog_async: truncated]");
    result &= contains(out, "trailing lenmod <?> [ulog_async: truncated]");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

bool testOverflow()
{
    std::cout << "Test 8 (argument, message and spec overflow are truncated safely): ";

    // Three 160-byte strings need more argument space than a record has: the
    // third is dropped and the record flagged, but the rest still renders.
    static const std::string a(160, 'a'), b(160, 'b'), c(160, 'c');

    // A message longer than the render buffer (512) is cut at 511 characters.
    static const std::string longFmt = std::string(600, 'x') + "%d";

    std::string out = captureStderr([&]() {
        std::thread rt([&]() {
            log_info("args: %s|%s|%s|end", a.c_str(), b.c_str(), c.c_str());
            log_info(longFmt.c_str(), 1);
            // Flags/width longer than ulog copies (24 chars): the value still
            // comes out, just without the full width.
            log_info("prefix: [%0000000000000000000000000007d]", 42);
        });
        rt.join();
        ulog_async_flush();
    });

    bool result = contains(out, "args: " + a + "|" + b + "||end [ulog_async: truncated]");
    result &= !contains(out, std::string(10, 'c'));
    result &= contains(out, std::string(511, 'x'));
    result &= !contains(out, std::string(512, 'x'));
    result &= contains(out, "prefix: [") && contains(out, "42]");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out.substr(0, 2000) << "\n---\n";
    return result;
}

void enqueueDirect(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ulog_async_enqueue(LOG_INFO, __FILE__, __LINE__, fmt, ap);
    va_end(ap);
}

bool testLifecycleEdges()
{
    std::cout << "Test 9 (start/stop/flush are idempotent; enqueue while stopped counts a drop): ";

    bool result = true;
    std::string out = captureStderr([&]() {
        // Already running: a second start is a no-op and logging still works.
        ulog_async_start();
        log_info("after double start %d", 1);
        ulog_async_flush();

        ulog_async_stop();
        ulog_async_stop(); // no-op
        ulog_async_flush(); // no-op while stopped; must not hang

        // Enqueueing directly while stopped can't be delivered: it's counted.
        unsigned long before = ulog_async_dropped_count();
        enqueueDirect("never delivered %d", 2);
        result &= (ulog_async_dropped_count() == before + 1);

        ulog_async_start();
        log_info("after restart %d", 3);
        ulog_async_flush();
    });

    result &= contains(out, "after double start 1");
    result &= !contains(out, "never delivered");
    result &= contains(out, "after restart 3");

    std::cout << (result ? "PASS" : "FAIL") << "\n";
    if (!result) std::cout << "---\n" << out << "---\n";
    return result;
}

} // namespace

int main()
{
    bool result = true;

    result &= testDeferredFormatting();
    result &= testMainThreadRoutedThroughAsync();
    result &= testNullAndLongString();
    result &= testSynchronousFallbackWhenStopped();
    result &= testMultiProducerInvariants();
    result &= testMatchesPrintf();
    result &= testUnsupportedConversions();
    result &= testOverflow();
    result &= testLifecycleEdges();

    return result ? 0 : -1;
}
