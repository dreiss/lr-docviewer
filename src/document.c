/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "document.h"

#include <stdarg.h>
#include <stdio.h>

void document_log_message(document_log_t log, const char* format, ...) {
  if (!log) {
    return;
  }
  char message[512];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  log(message);
}
