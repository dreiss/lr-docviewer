/* SPDX-License-Identifier: GPL-3.0-or-later */
// Test-only libretro log callback for the Python integration harness.
//
// ctypes callbacks cannot receive C variadic arguments,
// so a libretro.py log driver would see only the format string.
// The harness gives the core log_shim_log() as its log callback instead.
// It formats each message here and forwards the finished text
// to a non-variadic Python sink.
#include <stdarg.h>
#include <stdio.h>

#include "libretro.h"

/// Receives a formatted log message.
typedef void (*log_shim_sink_t)(int level, const char* message);

static log_shim_sink_t sink;

/// Sets the receiver of formatted messages, or discards messages if NULL.
void log_shim_set_sink(log_shim_sink_t new_sink) {
  sink = new_sink;
}

/// Implements retro_log_printf_t.
void log_shim_log(enum retro_log_level level, const char* format, ...) {
  if (!sink) {
    return;
  }
  char message[4096];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  sink((int)level, message);
}
