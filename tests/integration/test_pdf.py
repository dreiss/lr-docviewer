#!/usr/bin/env python3
"""PDF page geometry and damaged files."""

import collections.abc
import pathlib

import pytest
from libretro.api.log import LogLevel

from content import GRID_CELLS, PdfPage, grid_color, write_pdf
from harness import HEIGHT, Bounds, Core

# A page displayed as 600 x 800 points fits the screen at a scale of 1.35.
PAGE = Bounds(555, 0, 1365, HEIGHT)
LAST = GRID_CELLS - 1


@pytest.mark.parametrize(
    ("rotation", "corner"),
    [(0, "top left"), (90, "top right"), (180, "bottom right"), (270, "bottom left")],
)
def test_rotated_page(core: Core, tmp_path: pathlib.Path, rotation: int, corner: str) -> None:
    """A page's rotation turns it clockwise, moving its top-left corner to the given corner."""
    pdf = write_pdf(tmp_path / "rotated.pdf", [PdfPage(600, 800, grid=True, rotation=rotation)])
    with core.open(pdf) as viewer:
        frame = viewer.run()
        assert frame.page_bounds() == PAGE
        top_left = frame.locate(grid_color(0, 0, 0))
        assert top_left is not None
        vertical, horizontal = corner.split()
        if vertical == "top":
            assert top_left.top == PAGE.top
        else:
            assert abs(top_left.bottom - PAGE.bottom) <= 1
        if horizontal == "left":
            assert abs(top_left.left - PAGE.left) <= 1
        else:
            assert abs(top_left.right - PAGE.right) <= 1


def test_crop_box_limits_the_page(core: Core, tmp_path: pathlib.Path) -> None:
    # Crop a 600 x 600-point grid to its right half.
    pdf = write_pdf(
        tmp_path / "cropped.pdf", [PdfPage(600, 600, grid=True, crop=(300, 0, 600, 600))]
    )
    with core.open(pdf) as viewer:
        frame = viewer.run()
        assert frame.page_bounds() == Bounds(690, 0, 1230, HEIGHT)
        assert frame.locate(grid_color(0, 6, 0)) == Bounds(690, 0, 780, 90)
        assert frame.locate(grid_color(0, LAST, LAST)) == Bounds(1140, 990, 1230, HEIGHT)
        assert frame.locate(grid_color(0, 5, 0)) is None


@pytest.mark.parametrize(
    "keep",
    [lambda size: size // 2, lambda size: size * 9 // 10, lambda size: 200],
    ids=["half", "most", "200 bytes"],
)
def test_truncated_pdf_is_rejected(
    core: Core, tmp_path: pathlib.Path, keep: collections.abc.Callable[[int], int]
) -> None:
    complete = write_pdf(tmp_path / "complete.pdf", [PdfPage(600, 600, grid=True)] * 2)
    data = complete.read_bytes()
    truncated = tmp_path / "truncated.pdf"
    truncated.write_bytes(data[: keep(len(data))])
    log = core.load_failure(truncated)
    errors = log.messages(LogLevel.ERROR)
    assert any(str(truncated) in message for message in errors), errors
