#!/usr/bin/env python3
"""The core's exported symbols."""

import shutil
import subprocess

import pytest

from harness import Core

LIBRETRO_API = {
    "retro_api_version",
    "retro_cheat_reset",
    "retro_cheat_set",
    "retro_deinit",
    "retro_get_memory_data",
    "retro_get_memory_size",
    "retro_get_region",
    "retro_get_system_av_info",
    "retro_get_system_info",
    "retro_init",
    "retro_load_game",
    "retro_load_game_special",
    "retro_reset",
    "retro_run",
    "retro_serialize",
    "retro_serialize_size",
    "retro_set_audio_sample",
    "retro_set_audio_sample_batch",
    "retro_set_controller_port_device",
    "retro_set_environment",
    "retro_set_input_poll",
    "retro_set_input_state",
    "retro_set_video_refresh",
    "retro_unload_game",
    "retro_unserialize",
}


def test_exports_exactly_the_libretro_api(core: Core) -> None:
    nm = shutil.which("nm")
    if nm is None:
        pytest.skip("needs nm from binutils")
    symbols = subprocess.run(
        [nm, "--dynamic", "--defined-only", core.core_path],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    assert {line.split()[-1] for line in symbols.splitlines() if line.strip()} == LIBRETRO_API
