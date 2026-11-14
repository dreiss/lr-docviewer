/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "document.h"

#include <math.h>
#include <stdlib.h>

#include <cairo-tee.h>
#include <poppler.h>

struct pdf_document {
  document_log_t log;
  PopplerDocument* document;
};

/// The size of the longest side of the image that a page's operations are counted on.
#define COUNTING_IMAGE_SIZE 256.0

struct pdf_page {
  document_log_t log;
  PopplerPage* page;
  double width;
  double height;
  bool render_attempted;
  // A recording to replay instead of rendering the page, or NULL; see record_page().
  cairo_surface_t* recording;
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

/// Returns the loaded page, or NULL if it couldn't be loaded.
static void* load_page(void* opaque_document, int page_number, double* width, double* height) {
  struct pdf_document* document = opaque_document;
  PopplerPage* poppler_page = poppler_document_get_page(document->document, page_number);

  if (!poppler_page) {
    document_log_message(document->log, "PDF: Could not load page at index %d", page_number);
    return NULL;
  }
  double page_width;
  double page_height;
  poppler_page_get_size(poppler_page, &page_width, &page_height);
  if (!isfinite(page_width) || !isfinite(page_height) || page_width <= 0 || page_height <= 0) {
    document_log_message(
        document->log,
        "PDF: Page at index %d has invalid dimensions %.17g x %.17g",
        page_number,
        page_width,
        page_height
    );
    g_object_unref(poppler_page);
    return NULL;
  }
  struct pdf_page* page = calloc(1, sizeof(*page));
  if (!page) {
    document_log_message(document->log, "PDF: Could not allocate page at index %d", page_number);
    g_object_unref(poppler_page);
    return NULL;
  }
  page->log = document->log;
  page->page = poppler_page;
  page->width = page_width;
  page->height = page_height;
  *width = page_width;
  *height = page_height;
  return page;
}

static void close_page(void* opaque_page) {
  struct pdf_page* page = opaque_page;

  if (page->recording) {
    cairo_surface_destroy(page->recording);
  }
  g_object_unref(page->page);
  free(page);
}

static void count_operation(cairo_surface_t* observer, cairo_surface_t* target, void* count) {
  (void)observer;
  (void)target;
  (*(int*)count)++;
}

/// Has an observer count strokes and, separately, other operations except text,
/// and returns the first error.
static cairo_status_t count_operations(cairo_surface_t* observer, int* strokes, int* operations) {
  const struct {
    cairo_status_t (*add_callback)(cairo_surface_t*, cairo_surface_observer_callback_t, void*);
    int* count;
  } counters[] = {
      {cairo_surface_observer_add_stroke_callback, strokes},
      {cairo_surface_observer_add_paint_callback, operations},
      {cairo_surface_observer_add_mask_callback, operations},
      {cairo_surface_observer_add_fill_callback, operations},
  };

  for (size_t i = 0; i < sizeof(counters) / sizeof(counters[0]); i++) {
    cairo_status_t status = counters[i].add_callback(observer, count_operation, counters[i].count);
    if (status != CAIRO_STATUS_SUCCESS) {
      return status;
    }
  }
  return CAIRO_STATUS_SUCCESS;
}

/// Returns a recording of the page if it's safe to replay, or NULL otherwise.
///
/// Poppler decodes images every time it renders, and a recording keeps them decoded,
/// so replaying is far faster for scans.
/// But when Cairo replays a path under a pure scaling transform,
/// it rounds the scale to 1/256, so vector art moves relative to everything else.
/// A scan is safe: it's one image, drawn as one filled rectangle,
/// so only the image's outer edges move, by less than a pixel.
/// Cairo doesn't say what an operation drew,
/// so only pages with at most one operation are safe, not counting text, which isn't affected.
/// Other pages have to be rendered again, so they pay for this one extra pass.
static cairo_surface_t* record_page(const struct pdf_page* page) {
  int index = poppler_page_get_index(page->page);

  // An observer can't watch the recording itself.
  // After each operation, it reads and writes back a pixel of its target
  // (see _cairo_surface_sync in Cairo's cairo-surface-observer.c),
  // which would add a patch of that pixel's color to a recording.
  // So a tee also sends the page to an observer of a small image, just to count operations.
  // It doesn't matter what happens to that image, since it's discarded.
  double scale = COUNTING_IMAGE_SIZE / fmax(page->width, page->height);
  cairo_surface_t* image = cairo_image_surface_create(
      CAIRO_FORMAT_RGB24, (int)ceil(page->width * scale), (int)ceil(page->height * scale)
  );
  cairo_surface_t* observer = cairo_surface_create_observer(image, CAIRO_SURFACE_OBSERVER_NORMAL);
  cairo_surface_set_device_scale(observer, scale, scale);
  int strokes = 0;
  int operations = 0;
  cairo_status_t status = count_operations(observer, &strokes, &operations);
  if (status != CAIRO_STATUS_SUCCESS) {
    document_log_message(
        page->log,
        "PDF: Could not count operations on page at index %d, so it won't be replayed: %s",
        index,
        cairo_status_to_string(status)
    );
    cairo_surface_destroy(observer);
    cairo_surface_destroy(image);
    return NULL;
  }

  cairo_rectangle_t extents = {0, 0, page->width, page->height};
  cairo_surface_t* recording = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, &extents);
  cairo_surface_t* tee = cairo_tee_surface_create(recording);
  cairo_tee_surface_add(tee, observer);
  cairo_t* cr = cairo_create(tee);
  poppler_page_render(page->page, cr);
  // The tee passes errors from both surfaces to the context,
  // so checking the recording too is only insurance for the surface that's kept.
  status = cairo_status(cr);
  if (status == CAIRO_STATUS_SUCCESS) {
    status = cairo_surface_status(recording);
  }
  cairo_destroy(cr);
  cairo_surface_destroy(tee);
  cairo_surface_destroy(observer);
  cairo_surface_destroy(image);
  if (status != CAIRO_STATUS_SUCCESS) {
    document_log_message(
        page->log,
        "PDF: Could not record page at index %d, so it won't be replayed: %s",
        index,
        cairo_status_to_string(status)
    );
    cairo_surface_destroy(recording);
    return NULL;
  }
  if (strokes > 0 || operations > 1) {
    document_log_debug(
        page->log,
        "PDF: Rendering page at index %d directly (strokes: %d, other operations: %d)",
        index,
        strokes,
        operations
    );
    cairo_surface_destroy(recording);
    return NULL;
  }
  document_log_debug(page->log, "PDF: Replaying page at index %d", index);
  return recording;
}

/// Returns true if the page was rendered without a Cairo error.
static bool render(void* opaque_page, cairo_t* cr, enum document_quality quality) {
  (void)quality; // The core uses the cached preview instead during zooms and pans.
  struct pdf_page* page = opaque_page;

  if (!page->render_attempted) {
    page->render_attempted = true;
    page->recording = record_page(page);
  }
  // Poppler draws only the page's content, so paint the paper first.
  // This isn't part of the recording, where it would count as an operation.
  cairo_save(cr);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_rectangle(cr, 0, 0, page->width, page->height);
  cairo_fill(cr);
  if (page->recording) {
    cairo_set_source_surface(cr, page->recording, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
  } else {
    cairo_restore(cr);
    poppler_page_render(page->page, cr);
  }
  cairo_status_t status = cairo_status(cr);
  if (status != CAIRO_STATUS_SUCCESS) {
    document_log_message(
        page->log, "PDF: Could not render page: %s", cairo_status_to_string(status)
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
    .close_page = close_page,
    .render = render,
};
