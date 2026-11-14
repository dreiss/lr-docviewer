/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "document.h"

#include <stdarg.h>
#include <stdio.h>

static void log_formatted(
    document_log_t log, enum document_log_level level, const char* format, va_list arguments
) {
  if (!log) {
    return;
  }
  char message[512];
  vsnprintf(message, sizeof(message), format, arguments);
  log(level, message);
}

void document_log_message(document_log_t log, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  log_formatted(log, DOCUMENT_LOG_WARNING, format, arguments);
  va_end(arguments);
}

void document_log_debug(document_log_t log, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  log_formatted(log, DOCUMENT_LOG_DEBUG, format, arguments);
  va_end(arguments);
}
