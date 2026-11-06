/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "document.h"

#include <ctype.h>
#include <limits.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>
#include <zip.h>

#define MAX_ARCHIVE_MEMBER_SIZE (512ULL * 1024 * 1024)
#define MAX_IMAGE_PIXELS (1ULL << 24)

struct cbz_page {
  /// Filename owned by the archive.
  const char* name;
  zip_uint64_t index;
};

struct cbz_document {
  document_log_t log;
  zip_t* archive;
  struct cbz_page* pages;
  int page_count;
};

/// A decoded page.
struct cbz_loaded_page {
  document_log_t log;
  // NULL for an oversized page, which is drawn as a black placeholder.
  cairo_surface_t* image;
  unsigned int image_width;
  unsigned int image_height;
};

struct png_reader {
  const unsigned char* data;
  size_t size;
  size_t position;
};

struct jpeg_error_state {
  struct jpeg_error_mgr manager;
  jmp_buf jump;
};

struct jpeg_decoder {
  struct jpeg_decompress_struct jpeg;
  struct jpeg_error_state error;
  cairo_surface_t* surface;
  bool created;
};

typedef cairo_surface_t* (*image_decoder_t)(
    const unsigned char* data,
    size_t size,
    unsigned int* image_width,
    unsigned int* image_height,
    bool* oversized,
    document_log_t log,
    const char* name
);

static image_decoder_t find_decoder(const char* path);

static void close_archive(zip_t* archive, document_log_t log) {
  if (zip_close(archive) != 0) {
    document_log_message(
        log, "CBZ: Could not close archive: %s", zip_error_strerror(zip_get_error(archive))
    );
    zip_discard(archive);
  }
}

static bool extension_is(const char* path, const char* expected) {
  const char* extension = strrchr(path, '.');
  if (!extension) {
    return false;
  }
  ++extension;
  while (*extension && *expected) {
    if (tolower((unsigned char)*extension) != tolower((unsigned char)*expected)) {
      return false;
    }
    ++extension;
    ++expected;
  }
  return !*extension && !*expected;
}

static bool is_image(const char* path) {
  return find_decoder(path) != NULL;
}

/// Returns true if a path component is hidden, like ".DS_Store" or "._page1.png".
/// "." is not hidden, so names like "./page1.png" are accepted.
static bool is_hidden(const char* component) {
  return component[0] == '.' && component[1] != '/' && component[1] != '\0';
}

/// Returns true if an archive entry is a page image.
/// Hidden files and directories, and macOS "__MACOSX/" metadata, are not pages.
static bool is_page_entry(const char* name) {
  const char* component = name;
  while (true) {
    if (is_hidden(component) || strncmp(component, "__MACOSX/", strlen("__MACOSX/")) == 0) {
      return false;
    }
    const char* slash = strchr(component, '/');
    if (!slash) {
      break;
    }
    component = slash + 1;
  }
  return is_image(name);
}

static int compare_pages(const void* left, const void* right) {
  const struct cbz_page* a = left;
  const struct cbz_page* b = right;

  return strcmp(a->name, b->name);
}

static void close_document(void* opaque_document) {
  struct cbz_document* document = opaque_document;

  free(document->pages);
  close_archive(document->archive, document->log);
  free(document);
}

static void* open_document(const char* path, document_log_t log) {
  int error;
  zip_t* archive = zip_open(path, ZIP_RDONLY, &error);

  if (!archive) {
    zip_error_t zip_error;
    zip_error_init_with_code(&zip_error, error);
    document_log_message(log, "CBZ: Could not open '%s': %s", path, zip_error_strerror(&zip_error));
    zip_error_fini(&zip_error);
    return NULL;
  }
  struct cbz_document* document = calloc(1, sizeof(*document));
  if (!document) {
    document_log_message(log, "CBZ: Could not allocate document state");
    close_archive(archive, log);
    return NULL;
  }
  document->log = log;
  document->archive = archive;

  zip_int64_t entry_count = zip_get_num_entries(archive, 0);
  if (entry_count < 0) {
    document_log_message(
        log, "CBZ: Could not read archive directory: %s", zip_error_strerror(zip_get_error(archive))
    );
    close_document(document);
    return NULL;
  }
  for (zip_uint64_t i = 0; i < (zip_uint64_t)entry_count; ++i) {
    const char* name = zip_get_name(archive, i, 0);
    if (!name) {
      document_log_message(
          log,
          "CBZ: Could not read the name of archive entry %llu: %s",
          (unsigned long long)i,
          zip_error_strerror(zip_get_error(archive))
      );
      continue;
    }
    if (!is_page_entry(name)) {
      continue;
    }
    if (document->page_count == INT_MAX) {
      document_log_message(log, "CBZ: Archive contains more than %d image entries", INT_MAX);
      break;
    }
    struct cbz_page* pages =
        realloc(document->pages, (size_t)(document->page_count + 1) * sizeof(*document->pages));
    if (!pages) {
      document_log_message(log, "CBZ: Could not allocate page directory");
      close_document(document);
      return NULL;
    }
    document->pages = pages;
    document->pages[document->page_count].name = name;
    document->pages[document->page_count].index = i;
    ++document->page_count;
  }
  if (!document->page_count) {
    document_log_message(log, "CBZ: Archive contains no JPEG or PNG images");
    close_document(document);
    return NULL;
  }
  qsort(document->pages, (size_t)document->page_count, sizeof(*document->pages), compare_pages);
  return document;
}

static int page_count(void* opaque_document) {
  struct cbz_document* document = opaque_document;

  return document->page_count;
}

/// Returns true if the complete archive member was read.
static bool read_page(
    struct cbz_document* document, int page_number, unsigned char** data, size_t* size
) {
  zip_stat_t stat;
  zip_stat_init(&stat);
  const char* name = document->pages[page_number].name;
  if (zip_stat_index(document->archive, document->pages[page_number].index, 0, &stat) != 0) {
    document_log_message(
        document->log,
        "CBZ: Could not inspect page '%s': %s",
        name,
        zip_error_strerror(zip_get_error(document->archive))
    );
    return false;
  }
  if (!(stat.valid & ZIP_STAT_SIZE)) {
    document_log_message(document->log, "CBZ: Archive does not report the size of page '%s'", name);
    return false;
  }
  if (!stat.size) {
    document_log_message(document->log, "CBZ: Page '%s' is empty", name);
    return false;
  }
  if (stat.size > MAX_ARCHIVE_MEMBER_SIZE) {
    document_log_message(
        document->log,
        "CBZ: Page '%s' is %llu bytes and exceeds the %llu-byte archive-member limit",
        name,
        (unsigned long long)stat.size,
        (unsigned long long)MAX_ARCHIVE_MEMBER_SIZE
    );
    return false;
  }
  if (stat.size > SIZE_MAX) {
    document_log_message(document->log, "CBZ: Page '%s' is too large for this platform", name);
    return false;
  }
  zip_file_t* file = zip_fopen_index(document->archive, document->pages[page_number].index, 0);
  if (!file) {
    document_log_message(
        document->log,
        "CBZ: Could not open page '%s': %s",
        name,
        zip_error_strerror(zip_get_error(document->archive))
    );
    return false;
  }
  unsigned char* contents = malloc((size_t)stat.size);
  if (!contents) {
    document_log_message(
        document->log,
        "CBZ: Could not allocate %llu bytes for page '%s'",
        (unsigned long long)stat.size,
        name
    );
    if (zip_fclose(file) != 0) {
      document_log_message(document->log, "CBZ: Could not close page '%s'", name);
    }
    return false;
  }

  size_t done = 0;
  bool read_error = false;
  while (done < (size_t)stat.size) {
    zip_int64_t count = zip_fread(file, contents + done, stat.size - done);
    if (count < 0) {
      document_log_message(
          document->log,
          "CBZ: Could not read page '%s': %s",
          name,
          zip_error_strerror(zip_file_get_error(file))
      );
      read_error = true;
      break;
    }
    if (!count) {
      break;
    }
    done += (size_t)count;
  }
  bool complete = done == (size_t)stat.size;
  bool closed = zip_fclose(file) == 0;
  if (!complete && !read_error) {
    document_log_message(
        document->log,
        "CBZ: Read %zu of %llu bytes from page '%s'",
        done,
        (unsigned long long)stat.size,
        name
    );
  }
  if (!closed) {
    document_log_message(document->log, "CBZ: Could not close page '%s'", name);
  }
  if (read_error || !complete || !closed) {
    free(contents);
    return false;
  }
  *data = contents;
  *size = done;
  return true;
}

static cairo_status_t read_png(void* closure, unsigned char* output, unsigned int length) {
  struct png_reader* reader = closure;

  if (length > reader->size - reader->position) {
    return CAIRO_STATUS_READ_ERROR;
  }
  memcpy(output, reader->data + reader->position, length);
  reader->position += length;
  return CAIRO_STATUS_SUCCESS;
}

static cairo_surface_t* decode_png(
    const unsigned char* data,
    size_t size,
    unsigned int* image_width,
    unsigned int* image_height,
    bool* oversized,
    document_log_t log,
    const char* name
) {
  png_image header = {.version = PNG_IMAGE_VERSION};
  if (!png_image_begin_read_from_memory(&header, data, size)) {
    document_log_message(
        log, "CBZ: Could not read PNG header for page '%s': %s", name, header.message
    );
    png_image_free(&header);
    return NULL;
  }

  unsigned int width = header.width;
  unsigned int height = header.height;
  png_image_free(&header);
  if (!width || !height) {
    document_log_message(
        log, "CBZ: PNG page '%s' has invalid dimensions %u x %u", name, width, height
    );
    return NULL;
  }
  *image_width = width;
  *image_height = height;
  if ((uint64_t)width * height > MAX_IMAGE_PIXELS) {
    *oversized = true;
    document_log_message(
        log,
        "CBZ: PNG page '%s' is %u x %u and exceeds the %llu-pixel limit; using black",
        name,
        width,
        height,
        (unsigned long long)MAX_IMAGE_PIXELS
    );
    return NULL;
  }

  struct png_reader reader = {.data = data, .size = size};
  cairo_surface_t* surface = cairo_image_surface_create_from_png_stream(read_png, &reader);
  cairo_status_t status = cairo_surface_status(surface);
  if (status != CAIRO_STATUS_SUCCESS) {
    document_log_message(
        log, "CBZ: Could not decode PNG page '%s': %s", name, cairo_status_to_string(status)
    );
    cairo_surface_destroy(surface);
    return NULL;
  }
  return surface;
}

static void jpeg_error_exit(j_common_ptr jpeg) {
  struct jpeg_error_state* error = (struct jpeg_error_state*)jpeg->err;

  longjmp(error->jump, 1);
}

static void destroy_jpeg_decoder(struct jpeg_decoder* decoder) {
  if (decoder->surface) {
    cairo_surface_destroy(decoder->surface);
  }
  if (decoder->created) {
    jpeg_destroy_decompress(&decoder->jpeg);
  }
  free(decoder);
}

static cairo_surface_t* decode_jpeg(
    const unsigned char* data,
    size_t size,
    unsigned int* image_width,
    unsigned int* image_height,
    bool* oversized,
    document_log_t log,
    const char* name
) {
  // Keep mutable decoder state off the stack.  After longjmp(), modified
  // non-volatile automatic variables have indeterminate values.
  struct jpeg_decoder* decoder = calloc(1, sizeof(*decoder));
  if (!decoder) {
    document_log_message(log, "CBZ: Could not allocate JPEG decoder for page '%s'", name);
    return NULL;
  }
  struct jpeg_decompress_struct* jpeg = &decoder->jpeg;
  jpeg->err = jpeg_std_error(&decoder->error.manager);
  decoder->error.manager.error_exit = jpeg_error_exit;

  if (setjmp(decoder->error.jump)) {
    char message[JMSG_LENGTH_MAX];
    decoder->error.manager.format_message((j_common_ptr)jpeg, message);
    document_log_message(log, "CBZ: Could not decode JPEG page '%s': %s", name, message);
    destroy_jpeg_decoder(decoder);
    return NULL;
  }

  jpeg_create_decompress(jpeg);
  decoder->created = true;
  jpeg_mem_src(jpeg, data, (unsigned long)size);
  if (jpeg_read_header(jpeg, TRUE) != JPEG_HEADER_OK) {
    document_log_message(log, "CBZ: Page '%s' does not have a complete JPEG header", name);
    destroy_jpeg_decoder(decoder);
    return NULL;
  }
  *image_width = jpeg->image_width;
  *image_height = jpeg->image_height;
  if (!*image_width || !*image_height) {
    document_log_message(
        log, "CBZ: JPEG page '%s' has invalid dimensions %u x %u", name, *image_width, *image_height
    );
    destroy_jpeg_decoder(decoder);
    return NULL;
  }
  if ((uint64_t)*image_width * *image_height > MAX_IMAGE_PIXELS) {
    *oversized = true;
    document_log_message(
        log,
        "CBZ: JPEG page '%s' is %u x %u and exceeds the %llu-pixel limit; using black",
        name,
        *image_width,
        *image_height,
        (unsigned long long)MAX_IMAGE_PIXELS
    );
    destroy_jpeg_decoder(decoder);
    return NULL;
  }
  // Cairo RGB24 stores native-endian 0x00RRGGBB pixels.  Request the
  // corresponding libjpeg byte order for the surface memory.
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  jpeg->out_color_space = JCS_EXT_BGRX;
#else
  jpeg->out_color_space = JCS_EXT_XRGB;
#endif
  if (!jpeg_start_decompress(jpeg)) {
    document_log_message(log, "CBZ: Could not start decoding JPEG page '%s'", name);
    destroy_jpeg_decoder(decoder);
    return NULL;
  }
  decoder->surface = cairo_image_surface_create(
      CAIRO_FORMAT_RGB24, (int)jpeg->output_width, (int)jpeg->output_height
  );
  cairo_status_t status = cairo_surface_status(decoder->surface);
  if (status != CAIRO_STATUS_SUCCESS) {
    document_log_message(
        log,
        "CBZ: Could not allocate an image surface for page '%s': %s",
        name,
        cairo_status_to_string(status)
    );
    destroy_jpeg_decoder(decoder);
    return NULL;
  }
  unsigned char* pixels = cairo_image_surface_get_data(decoder->surface);
  int stride = cairo_image_surface_get_stride(decoder->surface);
  while (jpeg->output_scanline < jpeg->output_height) {
    JSAMPROW row = pixels + (size_t)jpeg->output_scanline * stride;
    if (jpeg_read_scanlines(jpeg, &row, 1) != 1) {
      document_log_message(log, "CBZ: JPEG decoder stopped early on page '%s'", name);
      destroy_jpeg_decoder(decoder);
      return NULL;
    }
  }
  if (!jpeg_finish_decompress(jpeg)) {
    document_log_message(log, "CBZ: Could not finish decoding JPEG page '%s'", name);
    destroy_jpeg_decoder(decoder);
    return NULL;
  }
  cairo_surface_mark_dirty(decoder->surface);
  cairo_surface_t* surface = decoder->surface;
  decoder->surface = NULL;
  destroy_jpeg_decoder(decoder);
  return surface;
}

struct decoder_entry {
  const char* extension;
  image_decoder_t decode;
};

static const struct decoder_entry image_decoders[] = {
    {.extension = "jpg", .decode = decode_jpeg},
    {.extension = "jpeg", .decode = decode_jpeg},
    {.extension = "png", .decode = decode_png},
};

static image_decoder_t find_decoder(const char* path) {
  for (size_t i = 0; i < sizeof(image_decoders) / sizeof(*image_decoders); ++i) {
    if (extension_is(path, image_decoders[i].extension)) {
      return image_decoders[i].decode;
    }
  }
  return NULL;
}

/// Returns the decoded page, or NULL if it couldn't be loaded.
static void* load_page(void* opaque_document, int page_number, double* width, double* height) {
  struct cbz_document* document = opaque_document;

  if (page_number < 0 || page_number >= document->page_count) {
    document_log_message(document->log, "CBZ: Page index %d is out of range", page_number);
    return NULL;
  }
  const char* name = document->pages[page_number].name;
  image_decoder_t decode = find_decoder(name);
  if (!decode) {
    document_log_message(document->log, "CBZ: Page '%s' has no image decoder", name);
    return NULL;
  }
  unsigned char* data;
  size_t size;
  if (!read_page(document, page_number, &data, &size)) {
    return NULL;
  }

  unsigned int image_width = 0;
  unsigned int image_height = 0;
  bool oversized = false;
  cairo_surface_t* image =
      decode(data, size, &image_width, &image_height, &oversized, document->log, name);
  free(data);
  if (!image && !oversized) {
    return NULL;
  }
  struct cbz_loaded_page* page = calloc(1, sizeof(*page));
  if (!page) {
    document_log_message(document->log, "CBZ: Could not allocate page '%s'", name);
    if (image) {
      cairo_surface_destroy(image);
    }
    return NULL;
  }
  page->log = document->log;
  page->image = image;
  page->image_width = image_width;
  page->image_height = image_height;
  *width = image_width;
  *height = image_height;
  return page;
}

static void close_page(void* opaque_page) {
  struct cbz_loaded_page* page = opaque_page;

  if (page->image) {
    cairo_surface_destroy(page->image);
  }
  free(page);
}

/// Returns true if the page was rendered without a Cairo error.
static bool render(void* opaque_page, cairo_t* cr) {
  struct cbz_loaded_page* page = opaque_page;

  if (!page->image) {
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_rectangle(cr, 0, 0, page->image_width, page->image_height);
    cairo_fill(cr);
  } else {
    // Filter enlarged images with repeated edge pixels, and draw only the image's area,
    // so the image neither fades at its edges nor bleeds past them.
    cairo_set_source_surface(cr, page->image, 0, 0);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_rectangle(cr, 0, 0, page->image_width, page->image_height);
    cairo_fill(cr);
  }
  cairo_status_t status = cairo_status(cr);
  if (status != CAIRO_STATUS_SUCCESS) {
    document_log_message(
        page->log, "CBZ: Could not render page: %s", cairo_status_to_string(status)
    );
    return false;
  }
  return true;
}

const struct document_vtable cbz_document_vtable = {
    .name = "CBZ",
    .extensions = "cbz",
    .prefer_cached_preview = false,
    .open = open_document,
    .close = close_document,
    .page_count = page_count,
    .load_page = load_page,
    .close_page = close_page,
    .render = render,
};
