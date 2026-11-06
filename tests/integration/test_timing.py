#!/usr/bin/env python3
"""Debug-level logs of how long opening, loading, and drawing take."""

import pathlib
import re

import pytest
from libretro.api.log import LogLevel

from content import PdfPage, make_grid_png, write_cbz, write_pdf
from harness import Core, Viewer

PREFIX = "Document Viewer: "


class Timings:
    """Reads a viewer's debug messages, with durations replaced by "T"."""

    def __init__(self, viewer: Viewer) -> None:
        self.viewer = viewer
        self.seen = 0

    def new(self) -> list[str]:
        """Returns the debug messages logged since the last call."""
        messages = self.viewer.log.messages(LogLevel.DEBUG)
        new = messages[self.seen :]
        self.seen = len(messages)
        return [re.sub(r"\d+\.\d ms", "T ms", message.removeprefix(PREFIX)) for message in new]


@pytest.fixture
def pdf(tmp_path: pathlib.Path) -> pathlib.Path:
    return write_pdf(tmp_path / "pages.pdf", [PdfPage(600, 600, grid=True)] * 3)


def test_pdf_open_and_page_turn(core: Core, pdf: pathlib.Path) -> None:
    with core.open(pdf) as viewer:
        timings = Timings(viewer)
        assert timings.new() == [
            f"Opened {pdf} with 3 pages in T ms",
            "Loaded PDF page at index 0 in T ms",
            "Rendered PDF page at index 0 as a preview in T ms",
            "Drew PDF page at index 0 at 1.00x zoom in T ms",
        ]
        viewer.tap("r")
        assert timings.new() == [
            "Loaded PDF page at index 1 in T ms",
            "Rendered PDF page at index 1 as a preview in T ms",
            "Drew PDF page at index 1 at 1.00x zoom in T ms",
        ]
        viewer.reset()
        assert timings.new() == [
            "Loaded PDF page at index 0 in T ms",
            "Rendered PDF page at index 0 as a preview in T ms",
            "Drew PDF page at index 0 at 1.00x zoom in T ms",
        ]


def test_cbz_open_has_no_preview(core: Core, tmp_path: pathlib.Path) -> None:
    cbz = write_cbz(tmp_path / "page.cbz", {"page.png": make_grid_png(1200, 1200, 0)})
    with core.open(cbz) as viewer:
        timings = Timings(viewer)
        assert timings.new() == [
            f"Opened {cbz} with 1 page in T ms",
            "Loaded CBZ page at index 0 in T ms",
            "Drew CBZ page at index 0 at 1.00x zoom in T ms",
        ]


def test_gestures_are_summarized_when_released(core: Core, pdf: pathlib.Path) -> None:
    with core.open(pdf) as viewer:
        timings = Timings(viewer)
        timings.new()
        viewer.tap("x")
        assert timings.new() == [
            "Drew 1 frame while zooming or panning in T ms on average and T ms at most",
            "Drew PDF page at index 0 at 1.25x zoom in T ms",
        ]
        viewer.run(5, down=True)
        assert timings.new() == []
        viewer.run()
        assert timings.new() == [
            "Drew 5 frames while zooming or panning in T ms on average and T ms at most",
            "Drew PDF page at index 0 at 1.25x zoom in T ms",
        ]
        viewer.run(5)
        assert timings.new() == []
