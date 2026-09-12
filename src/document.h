/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>

#include <cairo.h>

/// Reports a backend diagnostic to the core.
typedef void (*document_log_t)(const char* message);

/// Formats and reports a backend diagnostic when a callback is available.
void document_log_message(document_log_t log, const char* format, ...);

struct document_vtable {
  /// Human-readable backend name used in log messages.
  const char* name;
  /// Pipe-separated filename extensions without leading dots.
  const char* extensions;
  /// Whether interactive rendering should use a fit-to-screen cache.
  bool prefer_cached_preview;
  /// Opens a path and returns an opaque document, or NULL on failure.
  void* (*open)(const char* path, document_log_t log);
  /// Releases an open document.
  void (*close)(void* document);
  /// Returns the number of pages in an open document.
  int (*page_count)(void* document);
  /// Returns true if the page and its dimensions were loaded.
  bool (*load_page)(void* document, int page, double* width, double* height);
  /// Returns true if the current page was rendered without an error.
  bool (*render)(void* document, cairo_t* cr);
};

extern const struct document_vtable cbz_document_vtable;
extern const struct document_vtable pdf_document_vtable;
