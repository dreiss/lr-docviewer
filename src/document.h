/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>

#include <cairo.h>

enum document_log_level {
  /// Details for diagnosing behavior or speed.
  DOCUMENT_LOG_DEBUG,
  /// Problems, including ones the backend recovers from.
  DOCUMENT_LOG_WARNING,
};

/// Reports a backend diagnostic to the core.
typedef void (*document_log_t)(enum document_log_level level, const char* message);

/// Formats and reports a backend problem as a warning when a callback is available.
void document_log_message(document_log_t log, const char* format, ...);

/// Formats and reports a backend detail for debugging when a callback is available.
void document_log_debug(document_log_t log, const char* format, ...);

/// How carefully a backend should draw a page.
enum document_quality {
  /// As well as the backend can.
  DOCUMENT_QUALITY_FULL,
  /// During a zoom or pan, when speed matters more.  Backends may treat this as full quality.
  DOCUMENT_QUALITY_FAST,
};

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
  /// Loads a page and its dimensions, and returns an opaque page, or NULL on failure.
  /// Several pages can be loaded at once.  Close each page before its document.
  void* (*load_page)(void* document, int page, double* width, double* height);
  /// Releases a loaded page.
  void (*close_page)(void* page);
  /// Draws a loaded page from (0, 0) to its width and height, leaving nothing transparent,
  /// and returns true if it was rendered without an error.
  bool (*render)(void* page, cairo_t* cr, enum document_quality quality);
};

extern const struct document_vtable cbz_document_vtable;
extern const struct document_vtable pdf_document_vtable;
