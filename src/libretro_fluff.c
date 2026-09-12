/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "libretro.h"

unsigned retro_api_version(void) {
  return RETRO_API_VERSION;
}

void retro_set_audio_sample(retro_audio_sample_t cb) {
  (void)cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
  (void)cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
  (void)port;
  (void)device;
}

size_t retro_serialize_size(void) {
  return 0;
}

/// Always returns false because save states are unsupported.
bool retro_serialize(void* data, size_t size) {
  (void)data;
  (void)size;
  return false;
}

/// Always returns false because save states are unsupported.
bool retro_unserialize(const void* data, size_t size) {
  (void)data;
  (void)size;
  return false;
}

void retro_cheat_reset(void) {}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
  (void)index;
  (void)enabled;
  (void)code;
}

/// Always returns false because subsystem content is unsupported.
bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t count) {
  (void)type;
  (void)info;
  (void)count;
  return false;
}

unsigned retro_get_region(void) {
  return RETRO_REGION_NTSC;
}

void* retro_get_memory_data(unsigned id) {
  (void)id;
  return NULL;
}

size_t retro_get_memory_size(unsigned id) {
  (void)id;
  return 0;
}
