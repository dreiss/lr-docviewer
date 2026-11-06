#!/usr/bin/env python3
"""The output resolution core option."""

import pathlib

import pytest

from content import PdfPage, grid_color, write_pdf
from harness import Bounds, Core

OPTION = "docviewer_resolution"
SECOND = 60


@pytest.fixture
def pdf(tmp_path: pathlib.Path) -> pathlib.Path:
    return write_pdf(tmp_path / "grid.pdf", [PdfPage(600, 600, grid=True)])


def square_page(width: int, height: int) -> Bounds:
    """Returns where a square page fits on a landscape screen."""
    left = (width - height) // 2
    return Bounds(left, 0, left + height, height)


@pytest.mark.parametrize(
    ("value", "size"),
    [
        (None, (1920, 1080)),
        ("1280x720", (1280, 720)),
        ("3840x2160", (3840, 2160)),
        ("640x480", (640, 480)),
    ],
    ids=["default", "720p", "4K", "4:3"],
)
@pytest.mark.parametrize("options_version", [2, 0])
def test_resolution_option(
    core: Core,
    pdf: pathlib.Path,
    value: str | None,
    size: tuple[int, int],
    options_version: int,
) -> None:
    options = {OPTION: value} if value else None
    with core.open(pdf, options=options, options_version=options_version) as viewer:
        frame = viewer.run()
        assert (frame.width, frame.height) == size
        assert frame.page_bounds() == square_page(*size)


def test_pan_speed_follows_the_output_height(core: Core, pdf: pathlib.Path) -> None:
    with core.open(pdf, options={OPTION: "1280x720"}) as viewer:
        viewer.run(2 * SECOND, y=True)
        before = viewer.run().locate(grid_color(0, 6, 6))
        viewer.run(5, right=True)
        after = viewer.run().locate(grid_color(0, 6, 6))
        assert before is not None and after is not None
        # 24 pixels a frame at 1080 lines is 16 at 720.
        assert abs((before.left - after.left) - 5 * 16) <= 1
