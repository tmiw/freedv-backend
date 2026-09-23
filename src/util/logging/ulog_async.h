//=========================================================================
// Name:            ulog_async.h
// Purpose:         Real-time-safe front end for ulog. Lets log_*() calls be
//                  issued from any thread -- audio/real-time threads included --
//                  without allocating memory, taking locks, or touching stdio
//                  on the calling thread. Messages are captured into a
//                  lock-free ring and rendered/emitted later on a dedicated
//                  consumer thread.
//
//                  Nothing needs to be called to turn this on. When ULOG_ASYNC
//                  is defined the consumer thread starts automatically before
//                  main() and stops at process exit, and every log_*() call
//                  from every thread is routed through it. The functions below
//                  are for diagnostics, tests, and deliberate early shutdown
//                  only.
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

/// @brief True when ulog_log() should hand the calling thread's log calls to
///        the async ring rather than format them inline.
///
/// True once the consumer thread is running (which it is, automatically, for
/// essentially all of the process lifetime) and the caller is not the consumer
/// thread itself. Used internally by ulog_log(); real-time safe (one atomic
/// load plus a thread-local read). Applications do not normally need this.
bool ulog_async_is_active(void);

/// @brief Starts the async logging consumer thread.
///
/// Called automatically from a static constructor before main(); applications
/// never need to call it. Idempotent. Exposed only for tests and for code that
/// deliberately stops and later restarts async logging.
void ulog_async_start(void);

/// @brief Stops the async logging consumer thread after draining the ring.
///
/// Called automatically at process exit; applications never need to call it.
/// Idempotent. Call it explicitly only for an early, deterministic shutdown --
/// for example just before tearing down a custom ulog lock. After it returns,
/// log_*() falls back to synchronous logging until ulog_async_start() runs
/// again.
void ulog_async_stop(void);

/// @brief Blocks (briefly, non-real-time) until the ring has been drained.
///
/// Optional. Useful before reading log output in tests, or immediately before a
/// hard shutdown. No-op if the consumer is not running.
void ulog_async_flush(void);

/// @brief Number of log records dropped because the ring was full (or because
///        the consumer was not running). Monotonic.
unsigned long ulog_async_dropped_count(void);

/// @brief Captures one log record for later rendering on the consumer thread.
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
