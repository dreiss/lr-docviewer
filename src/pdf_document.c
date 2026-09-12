/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "document.h"

#include <math.h>
#include <stdlib.h>

#include <poppler.h>

struct pdf_document {
  document_log_t log;
  PopplerDocument* document;
  PopplerPage* page;
};

static void* open_document(const char* filename, document_log_t log) {
  char* path = g_canonicalize_filename(filename, NULL);
  if (!path) {
    document_log_message(log, "PDF: Could not canonicalize path '%s'", filename);
    return NULL;
  }

  GError* error = NULL;
  char* uri = g_filename_to_uri(path, NULL, &error);
  g_free(path);
  if (!uri) {
    document_log_message(
        log,
        "PDF: Could not convert path '%s' to a URI: %s",
        filename,
        error ? error->message : "unknown error"
    );
    g_clear_error(&error);
    return NULL;
  }

  PopplerDocument* poppler = poppler_document_new_from_file(uri, NULL, &error);
  g_free(uri);
  if (!poppler) {
    document_log_message(
        log, "PDF: Could not open '%s': %s", filename, error ? error->message : "unknown error"
    );
    g_clear_error(&error);
    return NULL;
  }
  g_clear_error(&error);

  struct pdf_document* document = calloc(1, sizeof(*document));
  if (!document) {
    document_log_message(log, "PDF: Could not allocate document state");
    g_object_unref(poppler);
    return NULL;
  }
  document->log = log;
  document->document = poppler;
  return document;
}

static void close_document(void* opaque_document) {
  struct pdf_document* document = opaque_document;

  if (document->page) {
    g_object_unref(document->page);
  }
  g_object_unref(document->document);
  free(document);
}

static int page_count(void* opaque_document) {
  struct pdf_document* document = opaque_document;
  int count = poppler_document_get_n_pages(document->document);

  if (count < 1) {
    document_log_message(document->log, "PDF: Invalid page count: %d", count);
  }
  return count;
}

/// Returns true if the page and its dimensions were loaded.
static bool load_page(void* opaque_document, int page_number, double* width, double* height) {
  struct pdf_document* document = opaque_document;
  PopplerPage* page = poppler_document_get_page(document->document, page_number);

  if (!page) {
    document_log_message(document->log, "PDF: Could not load page at index %d", page_number);
    return false;
  }
  double page_width;
  double page_height;
  poppler_page_get_size(page, &page_width, &page_height);
  if (!isfinite(page_width) || !isfinite(page_height) || page_width <= 0 || page_height <= 0) {
    document_log_message(
        document->log,
        "PDF: Page at index %d has invalid dimensions %.17g x %.17g",
        page_number,
        page_width,
        page_height
    );
    g_object_unref(page);
    return false;
  }
  if (document->page) {
    g_object_unref(document->page);
  }
  document->page = page;
  *width = page_width;
  *height = page_height;
  return true;
}

/// Returns true if the current page was rendered without a Cairo error.
static bool render(void* opaque_document, cairo_t* cr) {
  struct pdf_document* document = opaque_document;

  if (!document->page) {
    document_log_message(document->log, "PDF: Cannot render because no page is loaded");
    return false;
  }
  poppler_page_render(document->page, cr);
  cairo_status_t status = cairo_status(cr);
  if (status != CAIRO_STATUS_SUCCESS) {
    document_log_message(
        document->log, "PDF: Could not render page: %s", cairo_status_to_string(status)
    );
    return false;
  }
  return true;
}

const struct document_vtable pdf_document_vtable = {
    .name = "PDF",
    .extensions = "pdf",
    .prefer_cached_preview = true,
    .open = open_document,
    .close = close_document,
    .page_count = page_count,
    .load_page = load_page,
    .render = render,
};
