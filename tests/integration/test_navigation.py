#!/usr/bin/env python3
"""Page turning, reset, and reloading."""

import collections.abc
import pathlib

import pytest

from content import GRID_PAGES, PdfPage, grid_color, make_grid_png, write_cbz, write_pdf
from harness import Core, Frame, Viewer

PAGES = GRID_PAGES
SECOND = 60


def page_number(frame: Frame) -> int:
    """Returns which page of a grid document is shown."""
    shown = [page for page in range(PAGES) if frame.locate(grid_color(page, 6, 6))]
    assert len(shown) == 1, f"pages shown: {shown}"
    return shown[0]


@pytest.fixture(params=["pdf", "cbz"])
def document(request: pytest.FixtureRequest, tmp_path: pathlib.Path) -> pathlib.Path:
    """A grid document with every page color."""
    if request.param == "pdf":
        return write_pdf(tmp_path / "pages.pdf", [PdfPage(600, 600, grid=True)] * PAGES)
    return write_cbz(
        tmp_path / "pages.cbz",
        {f"page{page}.png": make_grid_png(1200, 1200, page) for page in range(PAGES)},
    )


@pytest.fixture
def viewer(core: Core, document: pathlib.Path) -> collections.abc.Iterator[Viewer]:
    with core.open(document) as viewer:
        yield viewer


def test_next_and_previous_page(viewer: Viewer) -> None:
    assert page_number(viewer.run()) == 0
    assert page_number(viewer.tap("r")) == 1
    assert page_number(viewer.tap("r")) == 2
    assert page_number(viewer.tap("l")) == 1


def test_previous_page_stops_at_first_page(viewer: Viewer) -> None:
    first = viewer.run()
    assert viewer.tap("l") == first


def test_next_page_stops_at_last_page(viewer: Viewer) -> None:
    last = viewer.tap("r", times=PAGES - 1)
    assert page_number(last) == PAGES - 1
    assert viewer.tap("r") == last


def test_page_turn_resets_zoom(viewer: Viewer) -> None:
    second = viewer.tap("r")
    viewer.tap("l")
    viewer.run(SECOND, y=True)
    viewer.run(10, right=True)
    viewer.run()
    assert viewer.tap("r") == second


def test_reset_returns_to_first_page_at_fit(viewer: Viewer) -> None:
    first = viewer.run()
    viewer.tap("r", times=2)
    viewer.run(SECOND, y=True)
    viewer.reset()
    assert viewer.run() == first


def test_reloading_shows_the_same_frames(core: Core, tmp_path: pathlib.Path) -> None:
    pdf = write_pdf(tmp_path / "pages.pdf", [PdfPage(600, 600, grid=True)] * PAGES)
    cbz = write_cbz(tmp_path / "pages.cbz", {"page.png": make_grid_png(1200, 1200, 1)})

    def frames(content: pathlib.Path) -> list[Frame]:
        with core.open(content) as viewer:
            return [viewer.run(), viewer.tap("r")]

    first = frames(pdf)
    assert frames(cbz)
    assert frames(pdf) == first
