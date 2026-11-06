/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "document.h"
#include "libretro.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cairo.h>

// The output size when the resolution option is unset.
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define RESOLUTION_OPTION "docviewer_resolution"
// Holding a zoom button doubles or halves the zoom every second.
#define ZOOM_PER_SECOND 2
#define MAX_ZOOM 8
// Panning speed at 1080 lines; other output heights scale it.
#define PAN_PIXELS_PER_FRAME 24
// Left stick deflection below this fraction of full is ignored,
// so a resting stick that doesn't read exactly 0 doesn't drift.
#define STICK_DEAD_ZONE 0.15
// L2 and R2 move back or forward this many pages.
#define PAGE_JUMP 10
#define BACKGROUND_GREY (1.0 / 8)
// Size of a placeholder for an unloadable first page, in points: US Letter.
#define PLACEHOLDER_WIDTH 612
#define PLACEHOLDER_HEIGHT 792
#define PAGE_MESSAGE_MILLISECONDS 2000
#define HELP_HINT_MILLISECONDS 4000
#define FRAMES_PER_SECOND 60

// === State object

enum page_mode {
  PAGE_MODE_SINGLE,
  // Two pages side by side: 1 and 2, 3 and 4, and so on.
  PAGE_MODE_ODD_LEFT,
  // Page 1 alone, then two pages side by side: 2 and 3, 4 and 5, and so on.
  PAGE_MODE_EVEN_LEFT,
  PAGE_MODE_COUNT,
};

/// A page on screen.
struct shown_page {
  int index;
  // The backend's loaded page, or NULL for a placeholder for a page that couldn't be loaded.
  void* page;
  // Size in points.
  double width;
  double height;
  // Position of the page's top-left corner in the spread, in points.
  double x;
  double y;
};

struct docviewer_state {
  retro_environment_t environment_cb;
  retro_video_refresh_t video_cb;
  retro_input_poll_t input_poll_cb;
  retro_input_state_t input_state_cb;
  retro_log_printf_t log;

  const struct document_vtable* backend;
  void* document;

  // The output size, from the resolution option.
  int width;
  int height;
  cairo_surface_t* framebuffer;
  cairo_surface_t* cached_preview;

  int page_count;
  enum page_mode page_mode;
  // The spread: the one or two pages on screen, side by side and touching.
  struct shown_page shown[2];
  int shown_count;
  // The spread's size, in points.
  double spread_width;
  double spread_height;
  // Size of the last page that loaded, for sizing placeholders; 0 before any has.
  double last_page_width;
  double last_page_height;
  // Quarter turns clockwise, from 0 to 3.  Kept across page turns.
  int rotation;

  // Frames that zoom in has been held, net of zoom out; 0 is fit-page zoom.
  // Counting frames keeps zooming in and back out exact.
  int zoom_frames;
  double zoom_factor;
  // Offset from the page center to the viewport center, in document units.
  // (0, 0) centers the page.
  double pan_x;
  double pan_y;

  unsigned previous_buttons;
  bool help_visible;
  bool settled_render_pending;
  // Whether the frontend accepts NULL to repeat the previous frame.
  bool can_dupe;
  // Whether the framebuffer has changed since it was last sent.
  bool frame_changed;
  // The frontend's message interface version: 1 or more supports retro_message_ext.
  unsigned message_version;

  // Frames drawn while zoom or pan buttons were held, for a timing summary.
  int gesture_frames;
  double gesture_milliseconds;
  double gesture_slowest_milliseconds;
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

/// Returns a monotonic time in milliseconds, for measuring durations.
static double now_milliseconds(void) {
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec * 1000 + (double)time.tv_nsec / 1e6;
}

/// Returns the spread's width as displayed, after rotation, in points.
static double view_width(void) {
  return state.rotation % 2 ? state.spread_height : state.spread_width;
}

/// Returns the spread's height as displayed, after rotation, in points.
static double view_height(void) {
  return state.rotation % 2 ? state.spread_width : state.spread_height;
}

/// Returns the index of the first page on screen.
static int first_shown(void) {
  return state.shown[0].index;
}

/// Returns the index of the last page on screen.
static int last_shown(void) {
  return state.shown[state.shown_count - 1].index;
}

static double fit_scale(void) {
  return fmin(state.width / view_width(), state.height / view_height());
}

/// Turns a context whose origin is the displayed spread's top-left corner, in points,
/// so that drawing in spread coordinates appears rotated.
static void rotate_spread(cairo_t* cr) {
  // Exact matrices, rather than cairo_rotate(), keep fit-page frames pixel-exact.
  cairo_matrix_t matrix;
  switch (state.rotation) {
  case 1:
    cairo_matrix_init(&matrix, 0, 1, -1, 0, view_width(), 0);
    break;
  case 2:
    cairo_matrix_init(&matrix, -1, 0, 0, -1, view_width(), view_height());
    break;
  case 3:
    cairo_matrix_init(&matrix, 0, -1, 1, 0, 0, view_height());
    break;
  default:
    return;
  }
  cairo_transform(cr, &matrix);
}

// === Libretro init and deinit

static const struct retro_input_descriptor bindings[] = {
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Previous page"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Next page"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Back 10 pages"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Forward 10 pages"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Zoom in"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Zoom out"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Pan up"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Pan down"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Pan left"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Pan right"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Hold for options: L/R rotate, B pages"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Help"},
    {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Pan"},
    {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Pan"},
    {0, 0, 0, 0, NULL}
};

// === Core options

static struct retro_core_option_v2_category option_categories[] = {
    {NULL, NULL, NULL},
};

static struct retro_core_option_v2_definition option_definitions[] = {
    {
        RESOLUTION_OPTION,
        "Output Resolution",
        NULL,
        "Size of the frames sent to RetroArch.  "
        "Matching the screen gives the sharpest text, and smaller sizes render faster.  "
        "Takes effect the next time a document is opened.",
        NULL,
        NULL,
        {
            {"1280x720", "1280 x 720 (720p)"},
            {"1600x900", "1600 x 900"},
            {"1920x1080", "1920 x 1080 (1080p)"},
            {"2560x1440", "2560 x 1440 (1440p)"},
            {"3840x2160", "3840 x 2160 (4K)"},
            {"640x480", "640 x 480 (4:3)"},
            {"1024x768", "1024 x 768 (4:3)"},
            {"1440x1080", "1440 x 1080 (4:3)"},
            {NULL, NULL},
        },
        "1920x1080",
    },
    {NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL},
};

// For frontends without core options version 2.  The first value is the default.
static const struct retro_variable option_variables[] = {
    {RESOLUTION_OPTION,
     "Output Resolution; "
     "1920x1080|1280x720|1600x900|2560x1440|3840x2160|640x480|1024x768|1440x1080"},
    {NULL, NULL},
};

static void set_core_options(void) {
  unsigned version = 0;
  if (state.environment_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
    struct retro_core_options_v2 options = {option_categories, option_definitions};
    state.environment_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
  } else {
    state.environment_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)option_variables);
  }
}

/// Reads the output resolution option, or the default if it's unset or invalid.
static void read_resolution(int* width, int* height) {
  *width = DEFAULT_WIDTH;
  *height = DEFAULT_HEIGHT;
  struct retro_variable variable = {RESOLUTION_OPTION, NULL};
  if (!state.environment_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) || !variable.value) {
    return;
  }
  char* end;
  long parsed_width = strtol(variable.value, &end, 10);
  long parsed_height = *end == 'x' ? strtol(end + 1, &end, 10) : 0;
  if (*end || parsed_width < 320 || parsed_height < 240 || parsed_width > 7680 ||
      parsed_height > 4320) {
    LOG(RETRO_LOG_WARN,
        "Document Viewer: Invalid output resolution %s; using %d x %d\n",
        variable.value,
        DEFAULT_WIDTH,
        DEFAULT_HEIGHT);
    return;
  }
  *width = (int)parsed_width;
  *height = (int)parsed_height;
}

void retro_set_environment(retro_environment_t cb) {
  state.environment_cb = cb;
  set_core_options();
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
  // The output size is read once, when the document is opened.
  info->geometry = (struct retro_game_geometry){
      .base_width = state.width,
      .base_height = state.height,
      .max_width = state.width,
      .max_height = state.height,
      .aspect_ratio = (float)state.width / (float)state.height,
  };
  info->timing = (struct retro_system_timing){
      .fps = FRAMES_PER_SECOND,
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
  for (int i = 0; i < state.shown_count; i++) {
    if (state.shown[i].page) {
      state.backend->close_page(state.shown[i].page);
    }
  }
  state.shown_count = 0;
  if (state.backend && state.document) {
    state.backend->close(state.document);
  }
  state.document = NULL;
  state.backend = NULL;
  state.framebuffer = NULL;
  state.cached_preview = NULL;
  state.settled_render_pending = false;
  state.gesture_frames = 0;
  state.gesture_milliseconds = 0;
  state.gesture_slowest_milliseconds = 0;
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

/// Returns the requested page if it was loaded with valid dimensions, or else NULL.
static void* load_page(int page_number, double* width, double* height) {
  double start = now_milliseconds();
  void* page = state.backend->load_page(state.document, page_number, width, height);
  if (!page) {
    LOG(RETRO_LOG_ERROR,
        "Document Viewer: Could not load %s page at index %d; showing a placeholder\n",
        state.backend->name,
        page_number);
    return NULL;
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
    state.backend->close_page(page);
    return NULL;
  }
  LOG(RETRO_LOG_DEBUG,
      "Document Viewer: Loaded %s page at index %d in %.1f ms\n",
      state.backend->name,
      page_number,
      now_milliseconds() - start);
  return page;
}

/// Briefly shows the page numbers on screen, after an optional prefix.
static void show_page_message(const char* prefix) {
  int failed = 0;
  int failed_index = 0;
  for (int i = 0; i < state.shown_count; i++) {
    if (!state.shown[i].page) {
      failed++;
      failed_index = state.shown[i].index;
    }
  }
  char pages[64];
  if (state.shown_count == 1) {
    snprintf(pages, sizeof(pages), "Page %d of %d", first_shown() + 1, state.page_count);
  } else {
    snprintf(
        pages,
        sizeof(pages),
        "Pages %d-%d of %d",
        first_shown() + 1,
        last_shown() + 1,
        state.page_count
    );
  }
  char problem[64] = "";
  if (failed == state.shown_count) {
    snprintf(problem, sizeof(problem), " couldn't be loaded");
  } else if (failed) {
    snprintf(problem, sizeof(problem), "; page %d couldn't be loaded", failed_index + 1);
  }
  char text[160];
  snprintf(
      text, sizeof(text), "%s%s%s%s", prefix ? prefix : "", prefix ? ": " : "", pages, problem
  );
  if (state.message_version >= 1) {
    // A status message replaces the previous one, instead of queueing behind it.
    struct retro_message_ext message = {
        .msg = text,
        .duration = PAGE_MESSAGE_MILLISECONDS,
        .priority = 1,
        .level = failed ? RETRO_LOG_WARN : RETRO_LOG_INFO,
        .target = RETRO_MESSAGE_TARGET_OSD,
        .type = RETRO_MESSAGE_TYPE_STATUS,
        .progress = -1,
    };
    state.environment_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
  } else {
    struct retro_message message = {
        .msg = text,
        .frames = PAGE_MESSAGE_MILLISECONDS * FRAMES_PER_SECOND / 1000,
    };
    state.environment_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
  }
}

/// Describes the pages on screen for log messages, like "page at index 2".
static void describe_shown(char* text, size_t size) {
  if (state.shown_count == 1) {
    snprintf(text, size, "%s page at index %d", state.backend->name, first_shown());
  } else {
    snprintf(
        text, size, "%s pages at index %d and %d", state.backend->name, first_shown(), last_shown()
    );
  }
}

/// Briefly suggests the help screen, if the frontend can show it alongside the page number.
static void show_help_hint(void) {
  // Older frontends replace one message with the next, so only the page number is shown.
  if (state.message_version < 1) {
    return;
  }
  struct retro_message_ext message = {
      .msg = "Press Start for help",
      .duration = HELP_HINT_MILLISECONDS,
      .priority = 1,
      .level = RETRO_LOG_INFO,
      .target = RETRO_MESSAGE_TARGET_OSD,
      .type = RETRO_MESSAGE_TYPE_NOTIFICATION,
      .progress = -1,
  };
  state.environment_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
}

/// Returns the index of the first page of the spread that contains a page.
static int spread_start(int page_index) {
  switch (state.page_mode) {
  case PAGE_MODE_ODD_LEFT:
    return page_index - page_index % 2;
  case PAGE_MODE_EVEN_LEFT:
    return page_index == 0 ? 0 : page_index - (page_index - 1) % 2;
  default:
    return page_index;
  }
}

/// Returns how many pages the spread starting at a page shows.
static int spread_size(int start) {
  if (state.page_mode == PAGE_MODE_SINGLE ||
      (state.page_mode == PAGE_MODE_EVEN_LEFT && start == 0)) {
    return 1;
  }
  return start + 1 < state.page_count ? 2 : 1;
}

/// Shows the spread that starts at a page, at fit-page zoom,
/// with placeholders for pages that can't be loaded,
/// and shows its page numbers after an optional prefix.
static void select_spread(int start, const char* message_prefix) {
  struct shown_page next[2];
  int count = spread_size(start);
  for (int i = 0; i < count; i++) {
    next[i] = (struct shown_page){.index = start + i};
    // Keep pages that are already loaded.
    for (int j = 0; j < state.shown_count; j++) {
      if (state.shown[j].page && state.shown[j].index == next[i].index) {
        next[i] = state.shown[j];
        state.shown[j].page = NULL;
      }
    }
    if (!next[i].page) {
      next[i].page = load_page(next[i].index, &next[i].width, &next[i].height);
    }
    if (next[i].page) {
      state.last_page_width = next[i].width;
      state.last_page_height = next[i].height;
    }
  }
  for (int j = 0; j < state.shown_count; j++) {
    if (state.shown[j].page) {
      state.backend->close_page(state.shown[j].page);
    }
  }

  // Size placeholders like a page beside them, or else the last page that loaded,
  // so the view doesn't jump.
  for (int i = 0; i < count; i++) {
    if (next[i].page) {
      continue;
    }
    const struct shown_page* other = count == 2 && next[1 - i].page ? &next[1 - i] : NULL;
    bool have_last = state.last_page_width > 0 && state.last_page_height > 0;
    next[i].width = other ? other->width : have_last ? state.last_page_width : PLACEHOLDER_WIDTH;
    next[i].height = other       ? other->height
                     : have_last ? state.last_page_height
                                 : PLACEHOLDER_HEIGHT;
  }

  // Lay the pages out side by side, centered vertically.
  state.spread_width = 0;
  state.spread_height = 0;
  for (int i = 0; i < count; i++) {
    next[i].x = state.spread_width;
    state.spread_width += next[i].width;
    state.spread_height = fmax(state.spread_height, next[i].height);
  }
  for (int i = 0; i < count; i++) {
    next[i].y = (state.spread_height - next[i].height) / 2;
    state.shown[i] = next[i];
  }
  state.shown_count = count;

  state.zoom_frames = 0;
  state.zoom_factor = 1;
  state.pan_x = 0;
  state.pan_y = 0;
  if (state.cached_preview) {
    cairo_surface_destroy(state.cached_preview);
    state.cached_preview = NULL;
  }
  if (state.backend->prefer_cached_preview) {
    create_cached_preview();
  }
  show_page_message(message_prefix);
}

/// Shows the spread that contains a page, stopping at the first and last pages.
/// Returns true if a different spread was selected.
static bool go_to_page(int page_index) {
  if (page_index < 0) {
    page_index = 0;
  }
  if (page_index > state.page_count - 1) {
    page_index = state.page_count - 1;
  }
  int start = spread_start(page_index);
  if (start == first_shown()) {
    return false;
  }
  select_spread(start, NULL);
  return true;
}

/// Switches to the next page mode, keeping the first page on screen.
static void cycle_page_mode(void) {
  static const char* const names[PAGE_MODE_COUNT] = {
      [PAGE_MODE_SINGLE] = "One page",
      [PAGE_MODE_ODD_LEFT] = "Two pages, odd on left",
      [PAGE_MODE_EVEN_LEFT] = "Two pages, even on left",
  };
  int first = first_shown();
  state.page_mode = (enum page_mode)((state.page_mode + 1) % PAGE_MODE_COUNT);
  select_spread(spread_start(first), names[state.page_mode]);
}

/// Rotates the view by quarter turns, positive for clockwise,
/// keeping the same spot on the page at the center of the screen.
/// Takes at most a full turn counterclockwise, -4 quarter turns.
static void rotate_view(int quarter_turns) {
  // Adding a full turn keeps the sum nonnegative, since C's % keeps the sign.
  state.rotation = (state.rotation + quarter_turns + 4) % 4;
  double pan_x = state.pan_x;
  double pan_y = state.pan_y;
  if (quarter_turns > 0) {
    state.pan_x = -pan_y;
    state.pan_y = pan_x;
  } else {
    state.pan_x = pan_y;
    state.pan_y = -pan_x;
  }
  if (state.cached_preview) {
    cairo_surface_destroy(state.cached_preview);
    state.cached_preview = NULL;
    create_cached_preview();
  }
}

enum render_mode {
  RENDER_INTERACTIVE,
  RENDER_SETTLED,
};

// === Rendering

/// Fills behind each page on screen, in spread coordinates:
/// white for transparent areas of loaded pages, and black for placeholders.
static void fill_backings(cairo_t* cr) {
  for (int i = 0; i < state.shown_count; i++) {
    const struct shown_page* page = &state.shown[i];
    double backing = page->page ? 1 : 0;
    cairo_set_source_rgb(cr, backing, backing, backing);
    cairo_rectangle(cr, page->x, page->y, page->width, page->height);
    cairo_fill(cr);
  }
}

/// Renders the loaded pages on screen, in spread coordinates.
/// Returns false if any failed to render.
static bool draw_pages(cairo_t* cr) {
  bool rendered = true;
  for (int i = 0; i < state.shown_count; i++) {
    const struct shown_page* page = &state.shown[i];
    if (!page->page) {
      continue;
    }
    cairo_save(cr);
    cairo_translate(cr, page->x, page->y);
    rendered &= state.backend->render(page->page, cr);
    cairo_restore(cr);
  }
  return rendered;
}

/// Returns true if a fit-to-screen preview was rendered and cached.
static bool create_cached_preview(void) {
  bool any_loaded = false;
  for (int i = 0; i < state.shown_count; i++) {
    any_loaded |= state.shown[i].page != NULL;
  }
  if (!any_loaded) {
    return false;
  }
  double start = now_milliseconds();
  // Allocate a screen-sized surface to render as a preview when panning/zooming.
  cairo_surface_t* preview =
      cairo_image_surface_create(CAIRO_FORMAT_RGB24, state.width, state.height);
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
  rotate_spread(cr);
  fill_backings(cr);
  bool rendered = draw_pages(cr);
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
  char shown[64];
  describe_shown(shown, sizeof(shown));
  if (!rendered) {
    LOG(RETRO_LOG_WARN,
        "Document Viewer: Could not render %s into cached preview; rendering directly\n",
        shown);
    cairo_surface_destroy(preview);
    return false;
  }
  state.cached_preview = preview;
  LOG(RETRO_LOG_DEBUG,
      "Document Viewer: Rendered %s as a preview in %.1f ms\n",
      shown,
      now_milliseconds() - start);
  return true;
}

/// Draws the help screen over the whole frame.
static void draw_help(cairo_t* cr) {
  static const char* const controls[][2] = {
      {"L / R", "Previous / next page"},
      {"L2 / R2", "Back / forward 10 pages"},
      {"Y / X", "Zoom in / out"},
      {"D-pad, left stick", "Pan"},
      {"A + L / R", "Rotate counterclockwise / clockwise"},
      {"A + B", "One or two pages, odd or even on left"},
      {"Start", "Show or hide this help"},
  };
  const int count = sizeof(controls) / sizeof(*controls);

  cairo_identity_matrix(cr);
  cairo_reset_clip(cr);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
  cairo_paint(cr);

  double size = state.height / 24.0;
  double line = size * 1.6;
  double gap = size;
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, size);
  double button_width = 0;
  for (int i = 0; i < count; i++) {
    cairo_text_extents_t extents;
    cairo_text_extents(cr, controls[i][0], &extents);
    button_width = fmax(button_width, extents.x_advance);
  }
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  double action_width = 0;
  for (int i = 0; i < count; i++) {
    cairo_text_extents_t extents;
    cairo_text_extents(cr, controls[i][1], &extents);
    action_width = fmax(action_width, extents.x_advance);
  }

  // Center the title, the table, and the closing note as one block.
  double left = round((state.width - (button_width + gap + action_width)) / 2);
  double y = round((state.height - line * (count + 3)) / 2 + size);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_move_to(cr, left, y);
  cairo_show_text(cr, "Controls");
  y += line * 1.5;
  for (int i = 0; i < count; i++) {
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_move_to(cr, left, y);
    cairo_show_text(cr, controls[i][0]);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_move_to(cr, left + button_width + gap, y);
    cairo_show_text(cr, controls[i][1]);
    y += line;
  }
  y += line * 0.5;
  cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
  cairo_move_to(cr, left, y);
  cairo_show_text(cr, "Press any button to close.");
}

/// Draws the current view into the framebuffer and returns how long it took, in milliseconds.
static double render_frame(enum render_mode mode) {
  double start = now_milliseconds();
  cairo_t* cr = cairo_create(state.framebuffer);

  // Fill with background color (to show outside of the document).
  cairo_set_source_rgb(cr, BACKGROUND_GREY, BACKGROUND_GREY, BACKGROUND_GREY);
  cairo_paint(cr);

  // Translate and scale draw context to receive page.
  double preview_scale = fit_scale();
  double scale = preview_scale * state.zoom_factor;
  double scaled_page_width = view_width() * scale;
  double scaled_page_height = view_height() * scale;
  // Put the page's corner on a whole pixel,
  // so at fit-page zoom the preview can be copied pixel for pixel.
  double page_x = round((state.width - scaled_page_width) / 2 - state.pan_x * scale);
  double page_y = round((state.height - scaled_page_height) / 2 - state.pan_y * scale);
  cairo_translate(cr, page_x, page_y);
  cairo_scale(cr, scale, scale);
  rotate_spread(cr);

  fill_backings(cr);

  bool rendered = true;
  // Spreads of only placeholders have no cached preview.
  bool use_cached_preview = mode == RENDER_INTERACTIVE && state.cached_preview;
  if (use_cached_preview) {
    // While panning/zooming, just render the cached preview
    // to avoid blocking on a slow renderer.
    cairo_save(cr);
    for (int i = 0; i < state.shown_count; i++) {
      const struct shown_page* page = &state.shown[i];
      cairo_rectangle(cr, page->x, page->y, page->width, page->height);
    }
    cairo_clip(cr);
    // Build the transform without dividing by the preview scale,
    // so it's exact at fit-page zoom.
    cairo_identity_matrix(cr);
    cairo_translate(cr, page_x, page_y);
    cairo_scale(cr, state.zoom_factor, state.zoom_factor);
    cairo_set_source_surface(cr, state.cached_preview, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    // Repeat edge pixels instead of filtering in transparency,
    // so the source is opaque and pixman can use its fast path.
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
    cairo_restore(cr);
  } else {
    rendered = draw_pages(cr);
  }
  if (state.help_visible) {
    draw_help(cr);
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
    char shown[64];
    describe_shown(shown, sizeof(shown));
    LOG(RETRO_LOG_ERROR, "Document Viewer: Could not render %s\n", shown);
  }
  // At fit-page zoom, the copied preview is already the full-quality view.
  state.settled_render_pending = use_cached_preview && state.zoom_factor != 1;
  state.frame_changed = true;
  return now_milliseconds() - start;
}

/// Logs how long a frame took to draw.
static void log_frame(double milliseconds) {
  char shown[64];
  describe_shown(shown, sizeof(shown));
  LOG(RETRO_LOG_DEBUG,
      "Document Viewer: Drew %s at %.2fx zoom in %.1f ms\n",
      shown,
      state.zoom_factor,
      milliseconds);
}

/// Adds a frame drawn during a zoom or pan gesture to the gesture's timing summary.
static void record_gesture_frame(double milliseconds) {
  state.gesture_frames++;
  state.gesture_milliseconds += milliseconds;
  state.gesture_slowest_milliseconds = fmax(state.gesture_slowest_milliseconds, milliseconds);
}

/// Logs and clears the timing summary of a finished zoom or pan gesture.
static void log_gesture(void) {
  if (state.gesture_frames == 0) {
    return;
  }
  LOG(RETRO_LOG_DEBUG,
      "Document Viewer: Drew %d %s while zooming or panning "
      "in %.1f ms on average and %.1f ms at most\n",
      state.gesture_frames,
      state.gesture_frames == 1 ? "frame" : "frames",
      state.gesture_milliseconds / state.gesture_frames,
      state.gesture_slowest_milliseconds);
  state.gesture_frames = 0;
  state.gesture_milliseconds = 0;
  state.gesture_slowest_milliseconds = 0;
}

// === Input handling

/// Reads the left stick, with each axis from -1 to 1, and 0 inside the dead zone.
static void read_left_stick(double* x, double* y) {
  // libretro.h documents analog axes as ranging from -0x8000 to 0x7fff (32767).
  // Full left or up is slightly past -1, but the speed below is limited to 1.
  double raw_x =
      state.input_state_cb(
          0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X
      ) /
      32767.0;
  double raw_y =
      state.input_state_cb(
          0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y
      ) /
      32767.0;
  double magnitude = hypot(raw_x, raw_y);
  if (magnitude <= STICK_DEAD_ZONE) {
    *x = 0;
    *y = 0;
    return;
  }
  // Start from zero at the edge of the dead zone, so speed doesn't jump.
  double speed = fmin(1, (magnitude - STICK_DEAD_ZONE) / (1 - STICK_DEAD_ZONE));
  *x = raw_x / magnitude * speed;
  *y = raw_y / magnitude * speed;
}

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

  // The help screen takes all input, and the press that closes it does nothing else.
  if (state.help_visible) {
    if (just_pressed) {
      state.help_visible = false;
      return (struct input_result){.view_changed = true};
    }
    return (struct input_result){0};
  }
  if (just_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_START)) {
    state.help_visible = true;
    return (struct input_result){.view_changed = true};
  }

  // Holding A gives these buttons other meanings.
  const unsigned option_buttons =
      (1u << RETRO_DEVICE_ID_JOYPAD_L) | (1u << RETRO_DEVICE_ID_JOYPAD_R) |
      (1u << RETRO_DEVICE_ID_JOYPAD_UP) | (1u << RETRO_DEVICE_ID_JOYPAD_DOWN) |
      (1u << RETRO_DEVICE_ID_JOYPAD_LEFT) | (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT) |
      (1u << RETRO_DEVICE_ID_JOYPAD_X) | (1u << RETRO_DEVICE_ID_JOYPAD_Y) |
      (1u << RETRO_DEVICE_ID_JOYPAD_B);
  unsigned option_pressed = 0;
  if (buttons & (1u << RETRO_DEVICE_ID_JOYPAD_A)) {
    option_pressed = just_pressed & option_buttons;
    buttons &= ~option_buttons;
    just_pressed &= ~option_buttons;
  }

  bool view_changed = false;

  if (option_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_L)) {
    rotate_view(-1);
    view_changed = true;
  }
  if (option_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_R)) {
    rotate_view(1);
    view_changed = true;
  }
  if (option_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_B)) {
    cycle_page_mode();
    view_changed = true;
  }
  if (just_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_L)) {
    view_changed |= go_to_page(first_shown() - 1);
  }
  if (just_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_R)) {
    view_changed |= go_to_page(last_shown() + 1);
  }
  if (just_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_L2)) {
    view_changed |= go_to_page(first_shown() - PAGE_JUMP);
  }
  if (just_pressed & (1u << RETRO_DEVICE_ID_JOYPAD_R2)) {
    view_changed |= go_to_page(first_shown() + PAGE_JUMP);
  }

  int zoom_frames = state.zoom_frames + !!(buttons & (1u << RETRO_DEVICE_ID_JOYPAD_Y)) -
                    !!(buttons & (1u << RETRO_DEVICE_ID_JOYPAD_X));
  double calculated_zoom = pow(ZOOM_PER_SECOND, (double)zoom_frames / FRAMES_PER_SECOND);
  if (zoom_frames != state.zoom_frames && zoom_frames >= 0 && calculated_zoom <= MAX_ZOOM) {
    state.zoom_frames = zoom_frames;
    state.zoom_factor = calculated_zoom;
    view_changed = true;
  }

  double scale = fit_scale() * state.zoom_factor;
  double max_pan_x = fmax(0, view_width() - state.width / scale) / 2;
  double max_pan_y = fmax(0, view_height() - state.height / scale) / 2;
  double stick_x;
  double stick_y;
  read_left_stick(&stick_x, &stick_y);
  double dx = clamp(
      !!(buttons & (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT)) -
          !!(buttons & (1u << RETRO_DEVICE_ID_JOYPAD_LEFT)) + stick_x,
      -1,
      1
  );
  double dy = clamp(
      !!(buttons & (1u << RETRO_DEVICE_ID_JOYPAD_DOWN)) -
          !!(buttons & (1u << RETRO_DEVICE_ID_JOYPAD_UP)) + stick_y,
      -1,
      1
  );
  double pan_step = PAN_PIXELS_PER_FRAME * state.height / 1080.0 / scale;
  double pan_x = clamp(state.pan_x + dx * pan_step, -max_pan_x, max_pan_x);
  double pan_y = clamp(state.pan_y + dy * pan_step, -max_pan_y, max_pan_y);
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
      .view_controls_held = (buttons & view_buttons) != 0 || stick_x != 0 || stick_y != 0,
  };
}

/// Returns a new framebuffer of the given size, or NULL after logging why it couldn't be made.
static cairo_surface_t* create_framebuffer(int width, int height) {
  cairo_surface_t* framebuffer = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  cairo_status_t status = cairo_surface_status(framebuffer);
  if (status != CAIRO_STATUS_SUCCESS) {
    LOG(RETRO_LOG_ERROR,
        "Document Viewer: Could not create %d x %d framebuffer: %s\n",
        width,
        height,
        cairo_status_to_string(status));
    cairo_surface_destroy(framebuffer);
    return NULL;
  }
  return framebuffer;
}

void retro_run(void) {
  if (!state.document || !state.framebuffer) {
    return;
  }
  struct input_result input = process_input();
  if (!input.view_controls_held) {
    log_gesture();
  }
  if (input.view_changed) {
    double milliseconds = render_frame(RENDER_INTERACTIVE);
    if (input.view_controls_held) {
      record_gesture_frame(milliseconds);
    } else {
      log_frame(milliseconds);
    }
  } else if (!input.view_controls_held && state.settled_render_pending) {
    log_frame(render_frame(RENDER_SETTLED));
  }

  // Repeating the previous frame avoids copying an unchanged framebuffer.
  bool send = state.frame_changed || !state.can_dupe;
  state.video_cb(
      send ? cairo_image_surface_get_data(state.framebuffer) : NULL,
      state.width,
      state.height,
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
  unsigned message_version = 0;
  state.message_version =
      state.environment_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &message_version)
          ? message_version
          : 0;

  state.backend = backend;
  double start = now_milliseconds();
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
  LOG(RETRO_LOG_DEBUG,
      "Document Viewer: Opened %s with %d %s in %.1f ms\n",
      game->path,
      state.page_count,
      state.page_count == 1 ? "page" : "pages",
      now_milliseconds() - start);
  read_resolution(&state.width, &state.height);
  state.framebuffer = create_framebuffer(state.width, state.height);
  if (!state.framebuffer) {
    retro_unload_game();
    return false;
  }

  state.previous_buttons = 0;
  state.help_visible = false;
  state.last_page_width = 0;
  state.last_page_height = 0;
  state.rotation = 0;
  state.page_mode = PAGE_MODE_SINGLE;
  select_spread(0, NULL);
  show_help_hint();
  log_frame(render_frame(RENDER_INTERACTIVE));
  return true;
}

void retro_reset(void) {
  if (state.document) {
    state.help_visible = false;
    state.rotation = 0;
    state.page_mode = PAGE_MODE_SINGLE;
    select_spread(0, NULL);
    log_frame(render_frame(RENDER_INTERACTIVE));
  }
}
