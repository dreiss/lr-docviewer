#!/usr/bin/env python3
"""One-page and two-page modes, switched with A + B.

The pages are 600 x 800 points, or 1200 x 1600 pixels for CBZ.
One page fits the screen at 810 x 1080 pixels,
and two side by side at 1620 x 1080, meeting at the screen center.
"""

import collections.abc
import dataclasses
import pathlib
import re

import pytest
from libretro.api.log import LogLevel

from content import PdfPage, make_png, write_cbz, write_pdf
from harness import HEIGHT, WIDTH, Bounds, Core, Frame, Viewer

COLORS = [(200, 40, 40), (40, 200, 40), (40, 40, 200), (200, 200, 40), (40, 200, 200)]
ALONE = Bounds(555, 0, 1365, HEIGHT)
LEFT = Bounds(150, 0, 960, HEIGHT)
RIGHT = Bounds(960, 0, 1770, HEIGHT)


@pytest.fixture(params=["pdf", "cbz"])
def viewer(
    request: pytest.FixtureRequest, core: Core, tmp_path: pathlib.Path
) -> collections.abc.Iterator[Viewer]:
    if request.param == "pdf":
        content = write_pdf(tmp_path / "pages.pdf", [PdfPage(600, 800, rgb) for rgb in COLORS])
    else:
        content = write_cbz(
            tmp_path / "pages.cbz",
            {f"page{i}.png": make_png(1200, 1600, rgb) for i, rgb in enumerate(COLORS)},
        )
    with core.open(content) as viewer:
        yield viewer


def next_mode(viewer: Viewer) -> Frame:
    """Holds A, taps B, releases A, and returns the settled frame."""
    viewer.run(a=True)
    viewer.run(a=True, b=True)
    viewer.run(a=True)
    return viewer.run()


def shown(frame: Frame) -> dict[int, Bounds]:
    """Returns where each page is shown, by page number."""
    pages = {}
    for number, rgb in enumerate(COLORS, start=1):
        bounds = frame.locate(rgb)
        if bounds is not None:
            pages[number] = bounds
    return pages


def assert_shown(frame: Frame, expected: dict[int, Bounds]) -> None:
    """Asserts where pages are shown, allowing a pixel of blending at their edges."""
    actual = shown(frame)
    assert actual.keys() == expected.keys(), actual
    for number, bounds in expected.items():
        pairs = zip(dataclasses.astuple(actual[number]), dataclasses.astuple(bounds), strict=True)
        assert all(abs(a - e) <= 1 for a, e in pairs), (number, actual[number], bounds)


def page_numbers(viewer: Viewer) -> list[str]:
    assert viewer.messages is not None
    return viewer.messages.page_numbers()


def test_single_page_is_the_default(viewer: Viewer) -> None:
    assert_shown(viewer.run(), {1: ALONE})


def test_odd_pages_on_left(viewer: Viewer) -> None:
    assert_shown(next_mode(viewer), {1: LEFT, 2: RIGHT})
    assert_shown(viewer.tap("r"), {3: LEFT, 4: RIGHT})
    # The last page has no partner.
    assert_shown(viewer.tap("r"), {5: ALONE})
    assert_shown(viewer.tap("r"), {5: ALONE})
    assert_shown(viewer.tap("l"), {3: LEFT, 4: RIGHT})
    assert_shown(viewer.tap("l"), {1: LEFT, 2: RIGHT})
    assert page_numbers(viewer) == [
        "Page 1 of 5",
        "Two pages, odd on left: Pages 1-2 of 5",
        "Pages 3-4 of 5",
        "Page 5 of 5",
        "Pages 3-4 of 5",
        "Pages 1-2 of 5",
    ]


def test_even_pages_on_left(viewer: Viewer) -> None:
    next_mode(viewer)
    # The first page has no partner.
    assert_shown(next_mode(viewer), {1: ALONE})
    assert_shown(viewer.tap("r"), {2: LEFT, 3: RIGHT})
    assert_shown(viewer.tap("r"), {4: LEFT, 5: RIGHT})
    assert_shown(viewer.tap("r"), {4: LEFT, 5: RIGHT})
    assert page_numbers(viewer)[2:] == [
        "Two pages, even on left: Page 1 of 5",
        "Pages 2-3 of 5",
        "Pages 4-5 of 5",
    ]


def test_switching_modes_keeps_the_left_page(viewer: Viewer) -> None:
    next_mode(viewer)
    viewer.tap("r")
    assert_shown(next_mode(viewer), {2: LEFT, 3: RIGHT})
    assert_shown(next_mode(viewer), {2: ALONE})
    assert page_numbers(viewer)[-1] == "One page: Page 2 of 5"


def test_page_mode_is_kept_across_jumps(core: Core, tmp_path: pathlib.Path) -> None:
    pdf = write_pdf(tmp_path / "long.pdf", [PdfPage(600, 800)] * 25)
    with core.open(pdf) as viewer:
        next_mode(viewer)
        viewer.tap("r2", times=3)
        viewer.tap("l2")
        assert page_numbers(viewer)[2:] == [
            "Pages 11-12 of 25",
            "Pages 21-22 of 25",
            "Page 25 of 25",
            "Pages 15-16 of 25",
        ]


def test_rotation_turns_the_spread(viewer: Viewer) -> None:
    next_mode(viewer)
    viewer.run(a=True)
    viewer.run(a=True, r=True)
    viewer.run(a=True)
    # Turned clockwise, the left page is on top.
    assert_shown(viewer.run(), {1: Bounds(600, 0, 1320, 540), 2: Bounds(600, 540, 1320, 1080)})


def test_zoom_is_centered_on_the_spread(viewer: Viewer) -> None:
    next_mode(viewer)
    viewer.run(60, y=True)
    pages = shown(viewer.run())
    assert abs(pages[1].right - WIDTH / 2) <= 1
    assert abs(pages[2].left - WIDTH / 2) <= 1


def test_reset_returns_to_one_page(viewer: Viewer) -> None:
    next_mode(viewer)
    viewer.tap("r")
    viewer.reset()
    assert_shown(viewer.run(), {1: ALONE})


def test_pages_of_different_heights_are_centered(core: Core, tmp_path: pathlib.Path) -> None:
    pdf = write_pdf(
        tmp_path / "pages.pdf", [PdfPage(600, 800, COLORS[0]), PdfPage(600, 400, COLORS[1])]
    )
    with core.open(pdf) as viewer:
        assert_shown(next_mode(viewer), {1: LEFT, 2: Bounds(960, 270, 1770, 810)})


def test_unreadable_page_in_a_spread(core: Core, tmp_path: pathlib.Path) -> None:
    cbz = write_cbz(
        tmp_path / "pages.cbz",
        {"page1.png": make_png(1200, 1600, COLORS[0]), "page2.png": b"not a PNG"},
    )
    with core.open(cbz) as viewer:
        frame = next_mode(viewer)
        # The placeholder is black and the same size as the page beside it.
        assert_shown(frame, {1: LEFT})
        assert frame.locate((0, 0, 0)) == RIGHT
        assert viewer.messages is not None
        last = viewer.messages.messages[-1]
        assert last.text == "Two pages, odd on left: Pages 1-2 of 2; page 2 couldn't be loaded"
        assert last.level == LogLevel.WARNING


def test_switching_modes_reuses_loaded_pages(viewer: Viewer) -> None:
    def loaded() -> list[str]:
        """Returns the index of each page loaded so far."""
        return re.findall(
            r"Loaded \w+ page at index (\d+)", "\n".join(viewer.log.messages(LogLevel.DEBUG))
        )

    viewer.run()
    # Odd on left adds page 2, and then even on left and one page show page 1 alone.
    for _ in range(3):
        next_mode(viewer)
    assert loaded() == ["0", "1"]
