/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "document.h"
#include "libretro.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include <cairo.h>

#define DOCVIEWER_WIDTH 1920
#define DOCVIEWER_HEIGHT 1080
#define ZOOM_FACTOR 1.25
#define MAX_ZOOM 8
#define PAN_PIXELS_PER_FRAME 24
#define BACKGROUND_GREY (1.0 / 8)
// Size of a placeholder for an unloadable first page, in points: US Letter.
#define PLACEHOLDER_WIDTH 612
#define PLACEHOLDER_HEIGHT 792

// === State object

struct docviewer_state {
  retro_environment_t environment_cb;
  retro_video_refresh_t video_cb;
  retro_input_poll_t input_poll_cb;
  retro_input_state_t input_state_cb;
  retro_log_printf_t log;

  const struct document_vtable* backend;
  void* document;

  cairo_surface_t* framebuffer;
  cairo_surface_t* cached_preview;

  int page_count;
  int page_index;
  double page_width;
  double page_height;
  // Whether the current page couldn't be loaded and is shown as a placeholder.
  bool page_failed;

  double zoom_factor;
  // Offset from the page center to the viewport center, in document units.
  // (0, 0) centers the page.
  double pan_x;
  double pan_y;

  unsigned previous_buttons;
  bool settled_render_pending;
  // Whether the frontend accepts NULL to repeat the previous frame.
  bool can_dupe;
  // Whether the framebuffer has changed since it was last sent.
  bool frame_changed;
};

static struct docviewer_state state;

#define LOG(...) \
  do { \
    if (state.log) { \
      state.log(__VA_ARGS__); \
    } \
  } while (0)

// === Forward declarations

static bool create_cached_preview(void);

// === Utilities

static double clamp(double value, double low, double high) {
  return fmax(low, fmin(value, high));
}

static double fit_scale(void) {
  return fmin(DOCVIEWER_WIDTH / state.page_width, DOCVIEWER_HEIGHT / state.page_height);
}

// === Libretro init and deinit

static const struct retro_input_descriptor bindings[] = {
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Previous page"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Next page"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Zoom in"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Zoom out"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Pan up"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Pan down"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Pan left"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Pan right"},
    {0, 0, 0, 0, NULL}
};

void retro_set_environment(retro_environment_t cb) {
  state.environment_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
  state.video_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb) {
  state.input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb) {
  state.input_state_cb = cb;
}

void retro_get_system_info(struct retro_system_info* info) {
  memset(info, 0, sizeof(*info));
  info->library_name = DOCVIEWER_NAME;
  info->library_version = DOCVIEWER_VERSION;
  info->valid_extensions = "pdf|cbz";
  info->need_fullpath = true;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
  memset(info, 0, sizeof(*info));
  info->geometry = (struct retro_game_geometry){
      .base_width = DOCVIEWER_WIDTH,
      .base_height = DOCVIEWER_HEIGHT,
      .max_width = DOCVIEWER_WIDTH,
      .max_height = DOCVIEWER_HEIGHT,
      .aspect_ratio = (float)DOCVIEWER_WIDTH / DOCVIEWER_HEIGHT,
  };
  info->timing = (struct retro_system_timing){
      .fps = 60.0,
      .sample_rate = 0.0,
  };
}

void retro_init(void) {
  struct retro_log_callback log;
  if (state.environment_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
    state.log = log.log;
  }
  state.environment_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)bindings);
}

void retro_unload_game(void) {
  if (state.cached_preview) {
    cairo_surface_destroy(state.cached_preview);
  }
  if (state.framebuffer) {
    cairo_surface_destroy(state.framebuffer);
  }
  if (state.backend && state.document) {
    state.backend->close(state.document);
  }
  state.document = NULL;
  state.backend = NULL;
  state.framebuffer = NULL;
  state.cached_preview = NULL;
  state.settled_render_pending = false;
}

void retro_deinit(void) {
  retro_unload_game();
  state.log = NULL;
}

// === Backend loading

static const struct document_vtable* const backends[] = {
    &pdf_document_vtable,
    &cbz_document_vtable,
};

static bool extension_matches(const char* path, const char* extensions) {
  const char* extension = strrchr(path, '.');

  if (!extension) {
    return false;
  }
  ++extension;
  while (*extensions) {
    const char* actual = extension;
    const char* expected = extensions;
    while (*actual && *expected && *expected != '|' &&
           tolower((unsigned char)*actual) == tolower((unsigned char)*expected)) {
      ++actual;
      ++expected;
    }
    if (!*actual && (!*expected || *expected == '|')) {
      return true;
    }
    extensions = strchr(extensions, '|');
    if (!extensions) {
      return false;
    }
    ++extensions;
  }
  return false;
}

static const struct document_vtable* find_backend(const char* path) {
  for (size_t i = 0; i < sizeof(backends) / sizeof(*backends); i++) {
    if (extension_matches(path, backends[i]->extensions)) {
      return backends[i];
    }
  }
  return NULL;
}

// === Page selection

/// Returns true if the requested page was loaded with valid dimensions.
static bool load_page(int page_number, double* width, double* height) {
  if (!state.backend->load_page(state.document, page_number, width, height)) {
    LOG(RETRO_LOG_ERROR,
        "Document Viewer: Could not load %s page at index %d; showing a placeholder\n",
        state.backend->name,
        page_number);
    return false;
  }
  if (!isfinite(*width) || !isfinite(*height) || *width < .01 || *height < .01 || *width > 1e8 ||
      *height > 1e8) {
    LOG(RETRO_LOG_ERROR,
        "Document Viewer: %s page at index %d has invalid dimensions %.17g x %.17g; "
        "showing a placeholder\n",
        state.backend->name,
        page_number,
        *width,
        *height);
    return false;
  }
  return true;
}

/// Selects a page at fit-page zoom, or a placeholder if it can't be loaded.
static void select_page(int page_number) {
  double width;
  double height;
  state.page_failed = !load_page(page_number, &width, &height);
  if (state.page_failed) {
    // Keep the previous page's size, so the view doesn't jump.
    bool have_previous = state.page_width > 0 && state.page_height > 0;
    width = have_previous ? state.page_width : PLACEHOLDER_WIDTH;
    height = have_previous ? state.page_height : PLACEHOLDER_HEIGHT;
  }
  state.page_index = page_number;
  state.page_width = width;
  state.page_height = height;
  state.zoom_factor = 1;
  state.pan_x = state.pan_y = 0;
  if (state.cached_preview) {
    cairo_surface_destroy(state.cached_preview);
    state.cached_preview = NULL;
  }
  if (!state.page_failed && state.backend->prefer_cached_preview) {
    create_cached_preview();
  }
}

/// Returns true if a different page was selected.
static bool turn_page(int delta) {
  if (state.page_index == 0 && delta < 0) {
    return false;
  }
  if (state.page_index == state.page_count - 1 && delta > 0) {
    return false;
  }
  select_page(state.page_index + delta);
  return true;
}

enum render_mode {
  RENDER_INTERACTIVE,
  RENDER_SETTLED,
};

// === Rendering

/// Returns true if a fit-to-screen preview was rendered and cached.
static bool create_cached_preview(void) {
  // Allocate a screen-sized surface to render as a preview when panning/zooming.
  cairo_surface_t* preview =
      cairo_image_surface_create(CAIRO_FORMAT_RGB24, DOCVIEWER_WIDTH, DOCVIEWER_HEIGHT);
  cairo_status_t surface_status = cairo_surface_status(preview);
  if (surface_status != CAIRO_STATUS_SUCCESS) {
    LOG(RETRO_LOG_WARN,
        "Document Viewer: Could not create cached preview: %s; rendering directly\n",
        cairo_status_to_string(surface_status));
    cairo_surface_destroy(preview);
    return false;
  }

  // Paint a white background because PDF doesn't draw the background.
  cairo_t* cr = cairo_create(preview);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_paint(cr);

  // Render at the appropriate scale.
  double preview_scale = fit_scale();
  cairo_scale(cr, preview_scale, preview_scale);
  bool rendered = state.backend->render(state.document, cr);
  cairo_status_t context_status = cairo_status(cr);
  cairo_destroy(cr);
  cairo_surface_flush(preview);
  surface_status = cairo_surface_status(preview);
  if (context_status != CAIRO_STATUS_SUCCESS) {
    LOG(RETRO_LOG_WARN,
        "Document Viewer: Could not draw cached preview: %s; rendering directly\n",
        cairo_status_to_string(context_status));
    cairo_surface_destroy(preview);
    return false;
  }
  if (surface_status != CAIRO_STATUS_SUCCESS) {
    LOG(RETRO_LOG_WARN,
        "Document Viewer: Could not finish cached preview: %s; rendering directly\n",
        cairo_status_to_string(surface_status));
    cairo_surface_destroy(preview);
    return false;
  }
  if (!rendered) {
    LOG(RETRO_LOG_WARN,
        "Document Viewer: Could not render %s page at index %d into cached preview; "
        "rendering directly\n",
        state.backend->name,
        state.page_index);
    cairo_surface_destroy(preview);
    return false;
  }
  state.cached_preview = preview;
  return true;
}

static void render_frame(enum render_mode mode) {
  cairo_t* cr = cairo_create(state.framebuffer);

  // Fill with background color (to show outside of the document).
  cairo_set_source_rgb(cr, BACKGROUND_GREY, BACKGROUND_GREY, BACKGROUND_GREY);
  cairo_paint(cr);

  // Translate and scale draw context to receive page.
  double preview_scale = fit_scale();
  double scale = preview_scale * state.zoom_factor;
  double scaled_page_width = state.page_width * scale;
  double scaled_page_height = state.page_height * scale;
  // Put the page's corner on a whole pixel,
  // so at fit-page zoom the preview can be copied pixel for pixel.
  double page_x = round((DOCVIEWER_WIDTH - scaled_page_width) / 2 - state.pan_x * scale);
  double page_y = round((DOCVIEWER_HEIGHT - scaled_page_height) / 2 - state.pan_y * scale);
  cairo_translate(cr, page_x, page_y);
  cairo_scale(cr, scale, scale);

  // Fill in white background as backing for transparent pixels,
  // or black for a page that couldn't be loaded.
  double backing = state.page_failed ? 0 : 1;
  cairo_set_source_rgb(cr, backing, backing, backing);
  cairo_rectangle(cr, 0, 0, state.page_width, state.page_height);
  cairo_fill(cr);

  bool rendered = true;
  // Placeholders have no cached preview.
  bool use_cached_preview = mode == RENDER_INTERACTIVE && state.cached_preview;
  if (use_cached_preview) {
    // While panning/zooming, just render the cached preview
    // to avoid blocking on a slow renderer.
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, state.page_width, state.page_height);
    cairo_clip(cr);
    // Build the transform without dividing by the preview scale,
    // so it's exact at fit-page zoom.
    cairo_identity_matrix(cr);
    cairo_translate(cr, page_x, page_y);
    cairo_scale(cr, state.zoom_factor, state.zoom_factor);
    cairo_set_source_surface(cr, state.cached_preview, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);
  } else if (!state.page_failed) {
    rendered = state.backend->render(state.document, cr);
  }
  cairo_status_t context_status = cairo_status(cr);
  cairo_destroy(cr);
  cairo_surface_flush(state.framebuffer);
  cairo_status_t surface_status = cairo_surface_status(state.framebuffer);
  if (context_status != CAIRO_STATUS_SUCCESS) {
    LOG(RETRO_LOG_ERROR,
        "Document Viewer: Could not draw frame: %s\n",
        cairo_status_to_string(context_status));
  } else if (surface_status != CAIRO_STATUS_SUCCESS) {
    LOG(RETRO_LOG_ERROR,
        "Document Viewer: Could not finish frame: %s\n",
        cairo_status_to_string(surface_status));
  } else if (!rendered) {
    LOG(RETRO_LOG_ERROR,
        "Document Viewer: Could not render %s page at index %d\n",
        state.backend->name,
        state.page_index);
  }
  // At fit-page zoom, the copied preview is already the full-quality view.
  state.settled_render_pending = use_cached_preview && state.zoom_factor != 1;
  state.frame_changed = true;
}

// === Input handling

struct input_result {
  bool view_changed;
  bool view_controls_held;
};

static struct input_result process_input(void) {
  state.input_poll_cb();
  unsigned buttons = 0;
  for (unsigned id = 0; id < 16; ++id) {
    if (state.input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id)) {
      buttons |= 1u << id;
    }
  }
  unsigned just_pressed = buttons & ~state.previous_buttons;
  state.previous_buttons = buttons;

  bool view_changed = false;

  if (just_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_L)) {
    view_changed |= turn_page(-1);
  }
  if (just_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_R)) {
    view_changed |= turn_page(1);
  }

  double zoom = state.zoom_factor;
  if (just_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_X)) {
    zoom *= ZOOM_FACTOR;
  }
  if (just_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_Y)) {
    zoom /= ZOOM_FACTOR;
  }
  zoom = clamp(zoom, 1, MAX_ZOOM);
  if (zoom != state.zoom_factor) {
    state.zoom_factor = zoom;
    view_changed = true;
  }

  double scale = fit_scale() * state.zoom_factor;
  double max_pan_x = fmax(0, state.page_width - DOCVIEWER_WIDTH / scale) / 2;
  double max_pan_y = fmax(0, state.page_height - DOCVIEWER_HEIGHT / scale) / 2;
  double dx = !!(buttons & (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT)) -
              !!(buttons & (1u << RETRO_DEVICE_ID_JOYPAD_LEFT));
  double dy = !!(buttons & (1u << RETRO_DEVICE_ID_JOYPAD_DOWN)) -
              !!(buttons & (1u << RETRO_DEVICE_ID_JOYPAD_UP));
  double pan_x = clamp(state.pan_x + dx * PAN_PIXELS_PER_FRAME / scale, -max_pan_x, max_pan_x);
  double pan_y = clamp(state.pan_y + dy * PAN_PIXELS_PER_FRAME / scale, -max_pan_y, max_pan_y);
  if (pan_x != state.pan_x) {
    state.pan_x = pan_x;
    view_changed = true;
  }
  if (pan_y != state.pan_y) {
    state.pan_y = pan_y;
    view_changed = true;
  }

  const unsigned view_buttons =
      (1u << RETRO_DEVICE_ID_JOYPAD_X) | (1u << RETRO_DEVICE_ID_JOYPAD_Y) |
      (1u << RETRO_DEVICE_ID_JOYPAD_UP) | (1u << RETRO_DEVICE_ID_JOYPAD_DOWN) |
      (1u << RETRO_DEVICE_ID_JOYPAD_LEFT) | (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);
  return (struct input_result){
      .view_changed = view_changed,
      .view_controls_held = (buttons & view_buttons) != 0,
  };
}

void retro_run(void) {
  if (!state.document || !state.framebuffer) {
    return;
  }
  struct input_result input = process_input();
  if (input.view_changed) {
    render_frame(RENDER_INTERACTIVE);
  } else if (!input.view_controls_held && state.settled_render_pending) {
    render_frame(RENDER_SETTLED);
  }

  // Repeating the previous frame avoids copying an unchanged framebuffer.
  bool send = state.frame_changed || !state.can_dupe;
  state.video_cb(
      send ? cairo_image_surface_get_data(state.framebuffer) : NULL,
      DOCVIEWER_WIDTH,
      DOCVIEWER_HEIGHT,
      cairo_image_surface_get_stride(state.framebuffer)
  );
  state.frame_changed = false;
}

static void document_log(const char* message) {
  LOG(RETRO_LOG_WARN, "Document Viewer: %s\n", message);
}

/// Returns true if the document and its initial frame were loaded.
bool retro_load_game(const struct retro_game_info* game) {
  retro_unload_game();
  if (!game || !game->path) {
    LOG(RETRO_LOG_ERROR, "Document Viewer: No content path provided\n");
    return false;
  }
  const struct document_vtable* backend = find_backend(game->path);
  if (!backend) {
    LOG(RETRO_LOG_ERROR, "Document Viewer: Unsupported content type: %s\n", game->path);
    return false;
  }
  enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
  if (!state.environment_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
    LOG(RETRO_LOG_ERROR, "Document Viewer: Frontend does not support XRGB8888 video\n");
    return false;
  }

  bool can_dupe = false;
  state.can_dupe = state.environment_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe) && can_dupe;

  state.backend = backend;
  state.document = backend->open(game->path, document_log);
  state.page_count = 0;
  if (state.document) {
    state.page_count = backend->page_count(state.document);
  }
  if (state.page_count < 1) {
    LOG(RETRO_LOG_ERROR, "Document Viewer: Could not open %s as %s\n", game->path, backend->name);
    retro_unload_game();
    return false;
  }
  state.framebuffer =
      cairo_image_surface_create(CAIRO_FORMAT_RGB24, DOCVIEWER_WIDTH, DOCVIEWER_HEIGHT);
  cairo_status_t frame_status = cairo_surface_status(state.framebuffer);
  if (frame_status != CAIRO_STATUS_SUCCESS) {
    LOG(RETRO_LOG_ERROR,
        "Document Viewer: Could not create %d x %d framebuffer: %s\n",
        DOCVIEWER_WIDTH,
        DOCVIEWER_HEIGHT,
        cairo_status_to_string(frame_status));
    retro_unload_game();
    return false;
  }

  state.previous_buttons = 0;
  state.page_width = 0;
  state.page_height = 0;
  select_page(0);
  render_frame(RENDER_INTERACTIVE);
  return true;
}

void retro_reset(void) {
  if (state.document) {
    select_page(0);
    render_frame(RENDER_INTERACTIVE);
  }
}
