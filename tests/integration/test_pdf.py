#!/usr/bin/env python3
"""PDF page geometry, replaying recorded pages, and damaged files."""

import collections.abc
import pathlib

import pytest
from libretro.api.log import LogLevel

from content import GRID_CELLS, PdfPage, grid_color, make_checker_png, write_pdf
from harness import HEIGHT, WIDTH, Bounds, Core, Viewer

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


def test_blank_page_is_white(core: Core, tmp_path: pathlib.Path) -> None:
    pdf = write_pdf(tmp_path / "blank.pdf", [PdfPage(600, 800, rgb=None)])
    white = (255, 255, 255)
    with core.open(pdf) as viewer:
        assert viewer.run().locate(white) == PAGE
        # Zoomed in to 2 times, the page is 1620 pixels wide,
        # both in the preview while zooming and once rendered at full quality.
        zoomed = Bounds((WIDTH - 1620) // 2, 0, (WIDTH + 1620) // 2, HEIGHT)
        assert viewer.run(60, y=True).locate(white) == zoomed
        assert viewer.run().locate(white) == zoomed


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


CHECKER = ((200, 40, 40), (40, 40, 200))


def colors_at_max_zoom(viewer: Viewer) -> set[tuple[int, int, int]]:
    """Returns the colors in the settled view of the middle of the page at maximum zoom."""
    viewer.run(4 * 60, y=True)
    view = viewer.run().image().crop((800, 400, 1120, 680))
    return {color for _, color in view.getcolors(view.width * view.height)}


def test_scan_is_replayed(core: Core, tmp_path: pathlib.Path) -> None:
    # Poppler stops smoothing images enlarged 4 times or more when it renders
    # (see getFilterForSurface in its CairoOutputDev.cc),
    # but a recording keeps the smoothing it was recorded with, at 1 pixel to a point.
    pdf = write_pdf(
        tmp_path / "scan.pdf", [PdfPage(600, 600, image=make_checker_png(600, 600, CHECKER))]
    )
    with core.open(pdf) as viewer:
        assert len(colors_at_max_zoom(viewer)) > len(CHECKER)


def test_replay_leaves_no_mark_at_the_page_center(core: Core, tmp_path: pathlib.Path) -> None:
    pdf = write_pdf(
        tmp_path / "scan.pdf", [PdfPage(600, 600, image=make_checker_png(600, 600, CHECKER))]
    )
    with core.open(pdf) as viewer:
        viewer.run(4 * 60, y=True)
        # Observing a recording would add a square of one color, a point wide
        # (see _cairo_surface_sync in Cairo's cairo-surface-observer.c),
        # next to the center of the page, which is 14.4 pixels wide at this zoom.
        square = viewer.run().image().crop((961, 541, 973, 553))
        assert len(square.getcolors(square.width * square.height)) > 1


def test_scan_with_vector_art_is_rendered_directly(core: Core, tmp_path: pathlib.Path) -> None:
    # Rendered directly, Poppler doesn't smooth the image at this zoom,
    # so only its own colors appear; see test_scan_is_replayed.
    pdf = write_pdf(
        tmp_path / "scan.pdf",
        [PdfPage(600, 600, image=make_checker_png(600, 600, CHECKER), outlined=True)],
    )
    with core.open(pdf) as viewer:
        assert colors_at_max_zoom(viewer) == set(CHECKER)


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
