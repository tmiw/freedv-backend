//=========================================================================
// Name:            ulog_async.h
// Purpose:         Real-time-safe front end for ulog. Lets log_*() calls be
//                  issued from audio/real-time threads without allocating
//                  memory, taking locks, or touching stdio on the calling
//                  thread. Messages are captured into a lock-free ring and
//                  rendered/emitted later on a dedicated consumer thread.
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

#ifndef ULOG_ASYNC_H
#define ULOG_ASYNC_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Marks (or unmarks) the calling thread as a real-time thread.
///
/// While a thread is marked real-time, any log_*()/ulog_log() call made from
/// it is captured into a lock-free ring buffer instead of being formatted and
/// written inline. The message is rendered and emitted later by the async
/// consumer thread. The flag is thread-local; every other thread keeps ulog's
/// normal synchronous behavior.
///
/// Safe to call from a real-time thread (no allocation, no locks). Typically
/// called once with true right after a thread requests real-time scheduling,
/// and once with false just before it returns to normal scheduling.
///
/// @param is_realtime - true to route this thread's logs through the async
///                      path, false to restore synchronous logging.
void ulog_set_thread_realtime(bool is_realtime);

/// @brief Returns true if the calling thread is currently marked real-time.
bool ulog_async_is_realtime_thread(void);

/// @brief Starts the async logging consumer thread.
///
/// Must be called once during application start-up, from a normal
/// (non-real-time) thread, before any real-time thread begins logging.
/// Idempotent; a second call while already running is a no-op.
void ulog_async_start(void);

/// @brief Stops the async logging consumer thread after draining the ring.
///
/// Must be called from a normal thread during shutdown, after all real-time
/// threads have stopped. Idempotent.
void ulog_async_stop(void);

/// @brief Blocks (briefly, non-real-time) until the ring has been drained.
///
/// Intended for shutdown and for tests. No-op if the consumer is not running.
void ulog_async_flush(void);

/// @brief Number of real-time log records dropped because the ring was full
///        (or because the consumer was not running). Monotonic.
unsigned long ulog_async_dropped_count(void);

/// @brief Captures one log record from a real-time thread.
///
/// Real-time safe: no allocation, no locks, no stdio. The format string
/// pointer is stored as-is (it must have static lifetime, which is true for
/// every string literal passed to log_*()). Integer/floating arguments are
/// copied by value; string (%s) arguments are copied inline up to an internal
/// cap. Actual printf-style formatting happens later on the consumer thread.
///
/// Unsupported in the captured format string (the record is still emitted,
/// but truncated at the offending conversion): %n, %*/%.* dynamic width or
/// precision, positional (%1$) specifiers, and wide-character %ls/%lc.
///
/// @param level   - ulog level (LOG_TRACE .. LOG_FATAL)
/// @param file    - source file (static lifetime, e.g. __FILE__)
/// @param line    - source line
/// @param fmt     - printf-style format string (static lifetime)
/// @param args    - argument list matching fmt
void ulog_async_enqueue(int level, const char *file, int line,
                        const char *fmt, va_list args);

#ifdef __cplusplus
}
#endif

#endif // ULOG_ASYNC_H
