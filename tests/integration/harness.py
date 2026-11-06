#!/usr/bin/env python3
"""Drives the Document Viewer core through libretro.py for integration tests."""

import collections.abc
import contextlib
import ctypes
import dataclasses
import logging
import os
import sys
from typing import Any, Self, override

from libretro import Session
from libretro.api.input import JoypadState
from libretro.api.log import LogLevel, retro_log_callback, retro_log_printf_t
from libretro.api.video import PixelFormat
from libretro.drivers import ArrayVideoDriver, LogDriver
from libretro.drivers.video import FrameBufferSpecial
from PIL import Image, ImageChops

WIDTH = 1920
HEIGHT = 1080
# BACKGROUND_GREY in docviewer.c, rounded by Cairo.
BACKGROUND = (32, 32, 32)

_core_logger = logging.getLogger("docviewer.core")

_LOG_SINK = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p)


@dataclasses.dataclass(frozen=True)
class LogRecord:
    level: LogLevel
    message: str


class RecordingLogDriver(LogDriver):
    """Keeps every core log message and copies it to Python logging.

    pytest shows the copied messages when a test fails.
    """

    def __init__(self) -> None:
        self.records: list[LogRecord] = []

    @override
    def log(self, level: LogLevel, fmt: bytes) -> None:
        message = fmt.decode(errors="replace").rstrip("\n")
        self.records.append(LogRecord(level, message))
        _core_logger.log(level.logging_level, "%s", message)

    def messages(self, level: LogLevel | None = None) -> list[str]:
        """Returns the messages logged at the given level, or all messages."""
        return [r.message for r in self.records if level is None or r.level == level]


class LogShim:
    """Formats the core's printf-style log messages in C before Python sees them.

    See log_shim.c for why this is needed.
    """

    def __init__(self, path: str | os.PathLike[str]) -> None:
        self._library = ctypes.CDLL(str(path))
        self._library.log_shim_set_sink.argtypes = [_LOG_SINK]
        self._library.log_shim_set_sink.restype = None
        address = ctypes.cast(self._library.log_shim_log, ctypes.c_void_p).value
        self.callback = retro_log_callback(log=retro_log_printf_t(address))
        self._sink: Any = None

    def attach(self, driver: LogDriver) -> None:
        def sink(level: int, message: bytes) -> None:
            if level in LogLevel:
                driver.log(LogLevel(level), message)

        # Keep the ctypes callback alive for as long as C can call it.
        self._sink = _LOG_SINK(sink)
        self._library.log_shim_set_sink(self._sink)

    def detach(self) -> None:
        """Discards later messages, so a stale core can't call a freed callback."""
        self._library.log_shim_set_sink(_LOG_SINK())
        self._sink = None


class DocviewerSession(Session):
    """A Session that gives the core the C log shim as its log callback."""

    def __init__(self, *args: Any, log_shim: LogShim, **kwargs: Any) -> None:
        self._log_shim = log_shim
        super().__init__(*args, **kwargs)

    @override
    def _get_log_interface(self, interface: Any) -> bool:
        if self.log is None:
            return False
        if not interface:
            raise ValueError("RETRO_ENVIRONMENT_GET_LOG_INTERFACE doesn't accept NULL")
        self._log_shim.attach(self.log)
        interface[0] = self._log_shim.callback
        return True

    @override
    def __enter__(self) -> Self:
        try:
            return super().__enter__()
        except BaseException:
            self._log_shim.detach()
            raise

    @override
    def __exit__(self, *exception: object) -> Any:
        try:
            return super().__exit__(*exception)
        finally:
            self._log_shim.detach()


@dataclasses.dataclass(frozen=True)
class Bounds:
    """A half-open pixel rectangle."""

    left: int
    top: int
    right: int
    bottom: int

    @property
    def width(self) -> int:
        return self.right - self.left

    @property
    def height(self) -> int:
        return self.bottom - self.top


@dataclasses.dataclass(frozen=True)
class Frame:
    """One XRGB8888 frame from the core."""

    data: bytes
    width: int
    height: int
    pitch: int

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """Returns the (red, green, blue) value at a pixel."""
        offset = y * self.pitch + x * 4
        value = int.from_bytes(self.data[offset : offset + 4], sys.byteorder)
        return (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF

    def image(self) -> Image.Image:
        """Returns the frame as an RGB image that shares this frame's pixel data."""
        # XRGB8888 pixels are native-endian 32-bit integers.
        layout = "BGRX" if sys.byteorder == "little" else "XRGB"
        return Image.frombuffer(
            "RGB", (self.width, self.height), self.data, "raw", layout, self.pitch, 1
        )

    def page_bounds(self) -> Bounds | None:
        """Returns the bounding box of every pixel that differs from the background.

        A page whose outermost pixels exactly match the background looks smaller,
        so test content shouldn't use the background color at its edges.
        """
        image = self.image()
        background = Image.new("RGB", image.size, BACKGROUND)
        box = ImageChops.difference(image, background).getbbox()
        if box is None:
            return None
        left, top, right, bottom = box
        return Bounds(left=left, top=top, right=right, bottom=bottom)


class FrameRecorder(ArrayVideoDriver):
    """Keeps the latest frame and records whether each refresh sent new pixels.

    Only the latest frame is kept because each one is about 8 MB.
    """

    def __init__(self) -> None:
        super().__init__()
        self.frame: Frame | None = None
        # True for new pixels, False for a request to repeat the previous frame.
        self.refreshes: list[bool] = []

    @override
    def refresh(
        self, data: memoryview | FrameBufferSpecial, width: int, height: int, pitch: int
    ) -> None:
        if isinstance(data, memoryview):
            self.frame = Frame(bytes(data), width, height, pitch)
            self.refreshes.append(True)
        elif data is FrameBufferSpecial.DUPE:
            self.refreshes.append(False)
        super().refresh(data, width, height, pitch)


class Pad:
    """Joypad state for port 0, held until changed."""

    def __init__(self) -> None:
        self.state = JoypadState()

    def states(self) -> collections.abc.Iterator[JoypadState]:
        while True:
            yield self.state


class Viewer:
    """A running core with helpers for scripted input."""

    def __init__(
        self,
        session: DocviewerSession,
        pad: Pad,
        video: FrameRecorder,
        log: RecordingLogDriver,
    ) -> None:
        self.session = session
        self.pad = pad
        self.video = video
        self.log = log

    @property
    def pixel_format(self) -> PixelFormat:
        return self.video.pixel_format

    def run(self, frames: int = 1, **buttons: bool) -> Frame:
        """Holds the given buttons for some frames and returns the last frame shown."""
        self.pad.state = JoypadState(**buttons)
        for _ in range(frames):
            self.session.run()
        assert self.video.frame is not None, "the core has not sent a frame"
        return self.video.frame

    def tap(self, button: str) -> Frame:
        """Presses a button for one frame, releases it for one frame, and returns the result."""
        self.run(**{button: True})
        return self.run()


class Core:
    """Loads content into the core under test."""

    def __init__(self, core_path: str, log_shim: LogShim) -> None:
        self.core_path = core_path
        self.log_shim = log_shim

    def _session(
        self, content: str | os.PathLike[str], pad: Pad, video: FrameRecorder, log: LogDriver
    ) -> DocviewerSession:
        return DocviewerSession(
            self.core_path,
            str(content),
            input=pad.states(),
            video=video,
            log=log,
            log_shim=self.log_shim,
        )

    @contextlib.contextmanager
    def open(self, content: str | os.PathLike[str]) -> collections.abc.Iterator[Viewer]:
        """Loads content, failing the test if the core rejects it."""
        pad = Pad()
        video = FrameRecorder()
        log = RecordingLogDriver()
        with self._session(content, pad, video, log) as session:
            yield Viewer(session, pad, video, log)

    def load_failure(self, content: str | os.PathLike[str]) -> RecordingLogDriver:
        """Asserts that the core rejects content, and returns what it logged."""
        log = RecordingLogDriver()
        try:
            with self._session(content, Pad(), FrameRecorder(), log):
                pass
        except RuntimeError as error:
            if "Failed to load game" not in str(error):
                raise
            return log
        raise AssertionError(f"the core loaded {content}")
