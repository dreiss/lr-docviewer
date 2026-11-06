#!/usr/bin/env python3
"""Rotating the view with A + L and A + R.

The grid pages are 600 x 800 points, or 1200 x 1600 pixels for CBZ.
Upright, they fit the screen at 810 x 1080 pixels;
turned a quarter, at 1440 x 1080.
"""

import collections.abc
import pathlib

import pytest

from content import GRID_CELLS, PdfPage, grid_color, make_grid_png, write_cbz, write_pdf
from harness import HEIGHT, WIDTH, Bounds, Core, Frame, Viewer

UPRIGHT = Bounds(555, 0, 1365, HEIGHT)
SIDEWAYS = Bounds(240, 0, 1680, HEIGHT)
LAST = GRID_CELLS - 1


@pytest.fixture(params=["pdf", "cbz"])
def viewer(
    request: pytest.FixtureRequest, core: Core, tmp_path: pathlib.Path
) -> collections.abc.Iterator[Viewer]:
    if request.param == "pdf":
        content = write_pdf(tmp_path / "grid.pdf", [PdfPage(600, 800, grid=True)] * 2)
    else:
        content = write_cbz(
            tmp_path / "grid.cbz",
            {f"page{page}.png": make_grid_png(1200, 1600, page) for page in range(2)},
        )
    with core.open(content) as viewer:
        yield viewer


def rotate(viewer: Viewer, button: str) -> Frame:
    """Holds A, taps L or R, releases A, and returns the settled frame."""
    viewer.run(a=True)
    viewer.run(a=True, **{button: True})
    viewer.run(a=True)
    return viewer.run()


def top_left(frame: Frame, page: int = 0) -> Bounds:
    """Returns where the page's top-left cell is shown."""
    bounds = frame.locate(grid_color(page, 0, 0))
    assert bounds is not None
    return bounds


def assert_corner(frame: Frame, bounds: Bounds, corner: str) -> None:
    page = frame.page_bounds()
    assert page is not None
    vertical, horizontal = corner.split()
    if vertical == "top":
        assert abs(bounds.top - page.top) <= 1
    else:
        assert abs(bounds.bottom - page.bottom) <= 1
    if horizontal == "left":
        assert abs(bounds.left - page.left) <= 1
    else:
        assert abs(bounds.right - page.right) <= 1


@pytest.mark.parametrize(
    ("button", "corner"), [("r", "top right"), ("l", "bottom left")], ids=["clockwise", "counter"]
)
def test_rotation_turns_the_page(viewer: Viewer, button: str, corner: str) -> None:
    assert viewer.run().page_bounds() == UPRIGHT
    frame = rotate(viewer, button)
    assert frame.page_bounds() == SIDEWAYS
    assert_corner(frame, top_left(frame), corner)
    # Holding A, L and R rotate instead of turning pages.
    assert viewer.messages is not None
    assert viewer.messages.page_numbers() == ["Page 1 of 2"]


def test_half_turn(viewer: Viewer) -> None:
    rotate(viewer, "r")
    frame = rotate(viewer, "r")
    assert frame.page_bounds() == UPRIGHT
    assert_corner(frame, top_left(frame), "bottom right")


def test_rotating_back_restores_the_view(viewer: Viewer) -> None:
    upright = viewer.run()
    rotate(viewer, "r")
    assert rotate(viewer, "l") == upright
    for _ in range(4):
        frame = rotate(viewer, "r")
    assert frame == upright


def test_rotation_is_kept_across_page_turns(viewer: Viewer) -> None:
    rotate(viewer, "r")
    frame = viewer.tap("r")
    assert frame.page_bounds() == SIDEWAYS
    assert_corner(frame, top_left(frame, page=1), "top right")


def test_reset_clears_rotation(viewer: Viewer) -> None:
    upright = viewer.run()
    rotate(viewer, "r")
    viewer.reset()
    assert viewer.run() == upright


def test_option_button_replaces_other_controls(viewer: Viewer) -> None:
    upright = viewer.run()
    for button in ["up", "down", "left", "right", "x", "y", "b"]:
        viewer.run(10, a=True, **{button: True})
    viewer.run(a=True)
    assert viewer.run() == upright
    assert viewer.messages is not None
    assert viewer.messages.page_numbers() == ["Page 1 of 2"]


def test_rotation_keeps_the_centered_spot(viewer: Viewer) -> None:
    # Zoom to 4x, then pan 120 pixels, which is 22 points, right and down,
    # so the screen center shows the inside of cell (6, 6).
    viewer.run(120, y=True)
    viewer.run(5, right=True, down=True)
    assert viewer.run().pixel(WIDTH // 2, HEIGHT // 2) == grid_color(0, 6, 6)
    for button in ["r", "r", "l", "l", "l"]:
        assert rotate(viewer, button).pixel(WIDTH // 2, HEIGHT // 2) == grid_color(0, 6, 6)
