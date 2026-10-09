// Tests for ulog's configuration API: level filtering, quiet mode, the lock
// hook, the custom prefix hook, level strings and ulog_event_to_cstr().
//
// Logging goes through the async front end (started before main()), so each
// test flushes it with ulog_async_flush() before inspecting captured stderr.
// Every test restores the default configuration before returning.

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <string>

#include <unistd.h>

#include "../logging/ulog.h"
#include "../logging/ulog_async.h"

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

// Runs fn with stderr redirected to a temp file, flushing the async logger
// before restoring stderr, and returns what was written.
std::string captureStderr(const std::function<void()>& fn)
{
    std::fflush(stderr);
    char path[] = "/tmp/ulog_config_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
    {
        return "<capture failed>";
    }
    int saved = dup(fileno(stderr));
    dup2(fd, fileno(stderr));

    fn();
    ulog_async_flush();

    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);

    lseek(fd, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
    {
        out.append(buf, n);
    }
    close(fd);
    unlink(path);
    return out;
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

bool testLevelStrings()
{
    std::cout << "Test 1 (level strings): ";

    bool result = CHECK(std::string(ulog_get_level_string(LOG_TRACE)) == "TRACE");
    result &= CHECK(std::string(ulog_get_level_string(LOG_DEBUG)) == "DEBUG");
    result &= CHECK(std::string(ulog_get_level_string(LOG_INFO)) == "INFO");
    result &= CHECK(std::string(ulog_get_level_string(LOG_WARN)) == "WARN");
    result &= CHECK(std::string(ulog_get_level_string(LOG_ERROR)) == "ERROR");
    result &= CHECK(std::string(ulog_get_level_string(LOG_FATAL)) == "FATAL");
    return report(result);
}

bool testSetLevel()
{
    std::cout << "Test 2 (set_level filters lower levels): ";

    std::string out = captureStderr([]() {
        ulog_set_level(LOG_WARN);
        log_debug("level-test debug");
        log_info("level-test info");
        log_warn("level-test warn");
        log_error("level-test error");
        ulog_async_flush();
        ulog_set_level(LOG_TRACE);
        log_trace("level-test trace after restore");
    });

    bool result = CHECK(!contains(out, "level-test debug"));
    result &= CHECK(!contains(out, "level-test info"));
    result &= CHECK(contains(out, "WARN"));
    result &= CHECK(contains(out, "level-test warn"));
    result &= CHECK(contains(out, "level-test error"));
    result &= CHECK(contains(out, "level-test trace after restore"));
    return report(result);
}

bool testSetQuiet()
{
    std::cout << "Test 3 (quiet mode suppresses output): ";

    std::string out = captureStderr([]() {
        ulog_set_quiet(true);
        log_error("quiet-test hidden");
        ulog_async_flush();
        ulog_set_quiet(false);
        log_error("quiet-test shown");
    });

    bool result = CHECK(!contains(out, "quiet-test hidden"));
    result &= CHECK(contains(out, "quiet-test shown"));
    return report(result);
}

struct LockState
{
    std::atomic<int> locks{0};
    std::atomic<int> unlocks{0};
    std::atomic<int> depth{0};
    std::atomic<bool> unbalanced{false};
};

void countingLock(bool lock, void* arg)
{
    auto state = static_cast<LockState*>(arg);
    if (lock)
    {
        state->locks++;
        if (state->depth.fetch_add(1) != 0) state->unbalanced = true;
    }
    else
    {
        state->unlocks++;
        if (state->depth.fetch_sub(1) != 1) state->unbalanced = true;
    }
}

bool testSetLock()
{
    std::cout << "Test 4 (lock hook brackets every emitted line): ";

    LockState state;
    std::string out = captureStderr([&]() {
        ulog_set_lock(countingLock, &state);
        log_info("lock-test 1");
        log_info("lock-test 2");
        log_info("lock-test 3");
        ulog_async_flush();
        ulog_set_lock(nullptr, nullptr);
    });

    bool result = CHECK(contains(out, "lock-test 3"));
    result &= CHECK(state.locks >= 3);
    result &= CHECK(state.locks == state.unlocks);
    result &= CHECK(!state.unbalanced);

    // With the hook removed, logging no longer calls it.
    int before = state.locks;
    captureStderr([]() { log_info("lock-test after removal"); });
    result &= CHECK(state.locks == before);
    return report(result);
}

void testPrefix(ulog_Event* ev, char* prefix, size_t prefixSize)
{
    snprintf(prefix, prefixSize, " [pfx:%s]", ulog_get_level_string(ev->level));
}

bool testPrefixFn()
{
    std::cout << "Test 5 (custom prefix hook): ";

    std::string out = captureStderr([]() {
        ulog_set_prefix_fn(testPrefix);
        log_warn("prefix-test warn");
        ulog_async_flush();
        ulog_set_prefix_fn(nullptr);
        log_warn("prefix-test plain");
    });

    bool result = CHECK(contains(out, "[pfx:WARN]"));
    result &= CHECK(contains(out, "prefix-test warn"));
    // After clearing the hook, no prefix.
    auto plainLine = out.substr(out.find("prefix-test plain") == std::string::npos ? 0 : out.rfind('\n', out.find("prefix-test plain")) + 1);
    result &= CHECK(contains(out, "prefix-test plain"));
    result &= CHECK(!contains(plainLine.substr(0, plainLine.find('\n')), "[pfx:"));
    return report(result);
}

// Builds a ulog_Event with a real va_list (as the logging path does) and
// renders it with ulog_event_to_cstr().
int renderEvent(char* out, size_t outSize, struct tm* time, int level, const char* fmt, ...)
{
    ulog_Event ev;
    memset(&ev, 0, sizeof(ev));
    ev.message = fmt;
    ev.file = "somefile.cpp";
    ev.line = 42;
    ev.level = level;
#if FEATURE_TIME
    ev.time = time;
#ifdef ULOG_ASYNC
    ev.time_is_borrowed = true;
#endif
#else
    (void)time;
#endif
    va_start(ev.message_format_args, fmt);
    int rv = ulog_event_to_cstr(&ev, out, outSize);
    va_end(ev.message_format_args);
    return rv;
}

bool testEventToCstr()
{
    std::cout << "Test 6 (ulog_event_to_cstr renders and truncates safely): ";

    struct tm tmBuf;
    memset(&tmBuf, 0, sizeof(tmBuf));
    tmBuf.tm_hour = 12;
    tmBuf.tm_min = 34;
    tmBuf.tm_sec = 56;

    char out[256];
    memset(out, 'X', sizeof(out));
    bool result = CHECK(renderEvent(out, sizeof(out), &tmBuf, LOG_WARN, "value=%d name=%s", 7, "abc") == 0);
    std::string rendered(out, strnlen(out, sizeof(out)));
    result &= CHECK(strnlen(out, sizeof(out)) < sizeof(out));
    result &= CHECK(contains(rendered, "WARN"));
    result &= CHECK(contains(rendered, "somefile.cpp:42:"));
    result &= CHECK(contains(rendered, "value=7 name=abc"));
    // No color codes or trailing newline in the string form.
    result &= CHECK(!contains(rendered, "\x1b["));
    result &= CHECK(rendered.empty() || rendered.back() != '\n');
#if FEATURE_TIME
    result &= CHECK(contains(rendered, "12:34:56"));
#endif

    // A buffer too small for the message must still come back as a
    // NUL-terminated string within the buffer, so callers can print it.
    char small[16];
    memset(small, 'X', sizeof(small));
    renderEvent(small, sizeof(small), &tmBuf, LOG_WARN, "a long message that does not fit %d", 12345);
    result &= CHECK(strnlen(small, sizeof(small)) < sizeof(small));
    if (strnlen(small, sizeof(small)) == sizeof(small))
    {
        std::cout << "[16-byte buffer not NUL-terminated: \"" << std::string(small, sizeof(small)) << "\"] ";
    }

    return report(result);
}

} // namespace

int main(int, char**)
{
    bool result = true;

    result &= testLevelStrings();
    result &= testSetLevel();
    result &= testSetQuiet();
    result &= testSetLock();
    result &= testPrefixFn();
    result &= testEventToCstr();

    return result ? 0 : -1;
}
