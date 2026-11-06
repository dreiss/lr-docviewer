#!/usr/bin/env python3
"""Zooming and panning, measured with grid pages.

A square grid page fits the screen height at a cell size of exactly 90 pixels.
The page center is the corner shared by cells (5, 5) and (6, 6),
and it starts at the screen center.

Enlarged CBZ images blur the edges between cells,
so most measurements use cell centers, which symmetric blur doesn't move.
"""

import collections.abc
import pathlib

import pytest

from content import GRID_CELLS, PdfPage, grid_color, make_grid_png, write_cbz, write_pdf
from harness import HEIGHT, WIDTH, Bounds, Core, Frame, Viewer

CELL = HEIGHT // GRID_CELLS
# Holding zoom in for a second doubles the zoom, up to 8 times fit.
SECOND = 60
MAX_ZOOM = 8
PAN_PIXELS_PER_FRAME = 24


def cell(frame: Frame, column: int, row: int) -> Bounds:
    bounds = frame.locate(grid_color(0, column, row))
    assert bounds is not None, f"cell ({column}, {row}) is not visible"
    return bounds


def center(frame: Frame, column: int, row: int) -> tuple[float, float]:
    """Returns the center of a cell, which must be entirely on screen."""
    bounds = cell(frame, column, row)
    return (bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2


def spacing(frame: Frame, column: int, row: int) -> float:
    """Returns the horizontal distance between a cell's center and the next cell's."""
    return center(frame, column + 1, row)[0] - center(frame, column, row)[0]


def assert_near(actual: tuple[float, ...], expected: tuple[float, ...], tolerance: float) -> None:
    assert all(abs(a - e) <= tolerance for a, e in zip(actual, expected, strict=True)), (
        f"{actual} is not within {tolerance} of {expected}"
    )


@pytest.fixture(params=["pdf", "cbz"])
def grid_viewer(
    request: pytest.FixtureRequest, core: Core, tmp_path: pathlib.Path
) -> collections.abc.Iterator[Viewer]:
    if request.param == "pdf":
        content = write_pdf(tmp_path / "grid.pdf", [PdfPage(600, 600, grid=True)])
    else:
        content = write_cbz(tmp_path / "grid.cbz", {"page.png": make_grid_png(1200, 1200)})
    with core.open(content) as viewer:
        yield viewer


def test_fit_places_grid(grid_viewer: Viewer) -> None:
    frame = grid_viewer.run()
    assert frame.page_bounds() == Bounds(420, 0, 1500, HEIGHT)
    assert cell(frame, 0, 0) == Bounds(420, 0, 420 + CELL, CELL)
    assert cell(frame, 6, 6) == Bounds(960, 540, 960 + CELL, 540 + CELL)


def test_zoom_in_scales_about_screen_center(grid_viewer: Viewer) -> None:
    grid_viewer.run(SECOND, y=True)
    frame = grid_viewer.run()
    size = CELL * 2
    # The corner of cells (5, 5) and (6, 6) stays at the screen center.
    assert_near(center(frame, 6, 6), (WIDTH / 2 + size / 2, HEIGHT / 2 + size / 2), 1)
    assert_near(center(frame, 5, 5), (WIDTH / 2 - size / 2, HEIGHT / 2 - size / 2), 1)
    assert_near((spacing(frame, 6, 6),), (size,), 1)


def test_zoom_is_continuous(grid_viewer: Viewer) -> None:
    grid_viewer.run()
    sizes = [spacing(grid_viewer.run(y=True), 6, 6) for _ in range(SECOND // 2)]
    # Every frame zooms in a little more.
    assert all(a < b for a, b in zip([CELL, *sizes], sizes, strict=False)), sizes
    # Half a second zooms by the square root of 2.
    assert_near((sizes[-1],), (CELL * 2**0.5,), 1)


def test_zoom_out_restores_fit(grid_viewer: Viewer) -> None:
    fit = grid_viewer.run()
    grid_viewer.run(SECOND, y=True)
    grid_viewer.run(SECOND, x=True)
    assert grid_viewer.run() == fit


def test_zoom_out_stops_at_fit(grid_viewer: Viewer) -> None:
    fit = grid_viewer.run()
    grid_viewer.run(SECOND // 2, y=True)
    grid_viewer.run(SECOND, x=True)
    assert grid_viewer.run() == fit


def test_zoom_in_stops_at_maximum(grid_viewer: Viewer) -> None:
    grid_viewer.run(4 * SECOND, y=True)
    maximum = grid_viewer.run()
    size = CELL * MAX_ZOOM
    # Cell (6, 6) is cut off at the bottom of the screen, so check only its left half.
    assert_near((cell(maximum, 6, 6).left,), (WIDTH / 2,), size / 100)
    assert_near((center(maximum, 5, 5)[0],), (WIDTH / 2 - size / 2,), 1)
    grid_viewer.run(5, y=True)
    assert grid_viewer.run() == maximum


def test_holding_both_zoom_buttons_keeps_zoom(grid_viewer: Viewer) -> None:
    grid_viewer.run(SECOND // 2, y=True)
    zoomed_frame = grid_viewer.run()
    grid_viewer.run(5, x=True, y=True)
    assert grid_viewer.run() == zoomed_frame


ZOOMED_CELL = CELL * 4


def zoomed(viewer: Viewer) -> Frame:
    """Zooms in far enough to leave room to pan in every direction."""
    viewer.run(2 * SECOND, y=True)
    return viewer.run()


def test_pan_moves_by_fixed_screen_distance(grid_viewer: Viewer) -> None:
    x, y = center(zoomed(grid_viewer), 6, 6)
    grid_viewer.run(5, right=True, down=True)
    distance = 5 * PAN_PIXELS_PER_FRAME
    assert_near(center(grid_viewer.run(), 6, 6), (x - distance, y - distance), 1)


@pytest.mark.parametrize(
    ("direction", "column", "row", "expected_center"),
    [
        ("left", 0, 6, (ZOOMED_CELL / 2, None)),
        ("right", GRID_CELLS - 1, 6, (WIDTH - ZOOMED_CELL / 2, None)),
        ("up", 6, 0, (None, ZOOMED_CELL / 2)),
        ("down", 6, GRID_CELLS - 1, (None, HEIGHT - ZOOMED_CELL / 2)),
    ],
)
def test_pan_stops_at_page_edge(
    grid_viewer: Viewer,
    direction: str,
    column: int,
    row: int,
    expected_center: tuple[float | None, float | None],
) -> None:
    zoomed(grid_viewer)
    grid_viewer.run(200, **{direction: True})
    frame = grid_viewer.run()
    # The edge cell's center is half a cell from the screen edge,
    # so the page edge is exactly at the screen edge.
    for actual, expected in zip(center(frame, column, row), expected_center, strict=True):
        if expected is not None:
            assert abs(actual - expected) <= 1
    grid_viewer.run(5, **{direction: True})
    assert grid_viewer.run() == frame


def test_pdf_preview_matches_final_render(core: Core, tmp_path: pathlib.Path) -> None:
    pdf = write_pdf(tmp_path / "grid.pdf", [PdfPage(600, 600, grid=True)])
    with core.open(pdf) as viewer:
        viewer.run()
        preview = viewer.run(SECOND // 3, y=True)
        final = viewer.run()
        assert final != preview
        # Cells that are entirely on screen at 1.26 times fit.
        for column, row in [(2, 2), (5, 5), (6, 6), (9, 9)]:
            assert_near(center(preview, column, row), center(final, column, row), 1)


def test_cbz_zoom_is_final_immediately(core: Core, tmp_path: pathlib.Path) -> None:
    cbz = write_cbz(tmp_path / "grid.cbz", {"page.png": make_grid_png(1200, 1200)})
    with core.open(cbz) as viewer:
        viewer.run()
        zoomed_frame = viewer.run(y=True)
        assert viewer.run() == zoomed_frame
        # The idle frame repeats the zoomed one instead of rendering again.
        assert viewer.video.refreshes[-1] is False
