#!/usr/bin/env python3
"""CBZ archive reading and image decoding."""

import collections.abc
import pathlib

import pytest
from libretro.api.log import LogLevel

from content import make_jpeg, make_png, write_cbz
from harness import HEIGHT, WIDTH, Core, Frame

# Pages larger than the screen are scaled down, which keeps their colors and edges exact.


def test_jpeg_pages(core: Core, tmp_path: pathlib.Path) -> None:
    red = (200, 0, 0)
    blue = (0, 0, 200)
    cbz = write_cbz(
        tmp_path / "jpeg.cbz",
        {"page1.jpg": make_jpeg(1500, 1500, red), "page2.JPEG": make_jpeg(3000, 1000, blue)},
    )
    with core.open(cbz) as viewer:
        frame = viewer.run()
        assert frame.locate(red, tolerance=4) is not None
        bounds = frame.page_bounds()
        assert bounds is not None
        assert (bounds.width, bounds.height) == (HEIGHT, HEIGHT)

        frame = viewer.tap("r")
        assert frame.locate(blue, tolerance=4) is not None
        bounds = frame.page_bounds()
        assert bounds is not None
        assert (bounds.width, bounds.height) == (WIDTH, WIDTH // 3)


def test_pages_use_bytewise_name_order(core: Core, tmp_path: pathlib.Path) -> None:
    red = (200, 0, 0)
    green = (0, 200, 0)
    blue = (0, 0, 200)
    cbz = write_cbz(
        tmp_path / "order.cbz",
        {
            "page2.png": make_png(3000, 1000, blue),
            "page1.png": make_png(1500, 1500, red),
            "page10.png": make_png(2000, 1000, green),
        },
    )
    with core.open(cbz) as viewer:
        pages = []
        for frame in [viewer.run(), viewer.tap("r"), viewer.tap("r")]:
            bounds = frame.page_bounds()
            assert bounds is not None
            pages.append(((bounds.width, bounds.height), frame.pixel(WIDTH // 2, HEIGHT // 2)))
        # page1, then page10, then page2.
        assert pages == [
            ((HEIGHT, HEIGHT), red),
            ((WIDTH, WIDTH // 2), green),
            ((WIDTH, WIDTH // 3), blue),
        ]


@pytest.mark.parametrize(
    ("name", "make_image"), [("page.png", make_png), ("page.jpg", make_jpeg)], ids=["png", "jpeg"]
)
def test_oversized_page_is_black_placeholder(
    core: Core,
    tmp_path: pathlib.Path,
    name: str,
    make_image: collections.abc.Callable[[int, int], bytes],
) -> None:
    # 5000 x 4000 is more than 2 ** 24 pixels.
    cbz = write_cbz(tmp_path / "oversized.cbz", {name: make_image(5000, 4000)})
    with core.open(cbz) as viewer:
        frame = viewer.run()
        bounds = frame.page_bounds()
        assert bounds is not None
        assert (bounds.width, bounds.height) == (HEIGHT * 5 // 4, HEIGHT)
        assert frame.pixel(WIDTH // 2, HEIGHT // 2) == (0, 0, 0)
        warnings = viewer.log.messages(LogLevel.WARNING)
        assert any("exceeds the 16777216-pixel limit" in m for m in warnings), warnings


BLACK = (0, 0, 0)


def page_size(frame: Frame) -> tuple[int, int]:
    bounds = frame.page_bounds()
    assert bounds is not None
    return bounds.width, bounds.height


def test_unreadable_page_is_placeholder(core: Core, tmp_path: pathlib.Path) -> None:
    red = (200, 0, 0)
    blue = (0, 0, 200)
    cbz = write_cbz(
        tmp_path / "corrupt.cbz",
        {
            "page1.png": make_png(1500, 1500, red),
            "page2.png": b"not a PNG",
            "page3.png": make_png(3000, 1000, blue),
        },
    )
    with core.open(cbz) as viewer:
        assert viewer.run().pixel(WIDTH // 2, HEIGHT // 2) == red
        # The placeholder keeps the previous page's size.
        placeholder = viewer.tap("r")
        assert page_size(placeholder) == (HEIGHT, HEIGHT)
        assert placeholder.pixel(WIDTH // 2, HEIGHT // 2) == BLACK
        errors = viewer.log.messages(LogLevel.ERROR)
        assert any("showing a placeholder" in m for m in errors), errors

        last = viewer.tap("r")
        assert page_size(last) == (WIDTH, WIDTH // 3)
        assert last.pixel(WIDTH // 2, HEIGHT // 2) == blue
        assert viewer.tap("l").pixel(WIDTH // 2, HEIGHT // 2) == BLACK
        assert viewer.tap("l").pixel(WIDTH // 2, HEIGHT // 2) == red


def test_unreadable_first_page_is_placeholder(core: Core, tmp_path: pathlib.Path) -> None:
    blue = (0, 0, 200)
    cbz = write_cbz(
        tmp_path / "corrupt.cbz",
        {"page1.png": b"not a PNG", "page2.png": make_png(3000, 1000, blue)},
    )
    with core.open(cbz) as viewer:
        placeholder = viewer.run()
        # US Letter proportions, with a partly covered pixel at each side.
        width, height = page_size(placeholder)
        assert height == HEIGHT
        assert abs(width - HEIGHT * 612 / 792) <= 2
        assert placeholder.pixel(WIDTH // 2, HEIGHT // 2) == BLACK
        second = viewer.tap("r")
        assert page_size(second) == (WIDTH, WIDTH // 3)
        assert second.pixel(WIDTH // 2, HEIGHT // 2) == blue


def test_hidden_and_macos_entries_are_not_pages(core: Core, tmp_path: pathlib.Path) -> None:
    red = (200, 0, 0)
    blue = (0, 0, 200)
    # Valid images in other colors, so they would show up as pages if they weren't skipped.
    cbz = write_cbz(
        tmp_path / "macos.cbz",
        {
            "__MACOSX/._page1.png": make_png(2000, 1000, (0, 200, 0)),
            "chapter/.thumbnails/page1.png": make_png(2000, 1000, (200, 200, 0)),
            "./page1.png": make_png(1500, 1500, red),
            "page2.png": make_png(3000, 1000, blue),
            "page3/.hidden.png": make_png(2000, 1000, (0, 200, 200)),
        },
    )
    with core.open(cbz) as viewer:
        first = viewer.run()
        assert page_size(first) == (HEIGHT, HEIGHT)
        assert first.pixel(WIDTH // 2, HEIGHT // 2) == red
        last = viewer.tap("r")
        assert page_size(last) == (WIDTH, WIDTH // 3)
        assert last.pixel(WIDTH // 2, HEIGHT // 2) == blue
        assert viewer.tap("r") == last


def test_enlarged_page_has_sharp_edges(core: Core, tmp_path: pathlib.Path) -> None:
    red = (200, 0, 0)
    cbz = write_cbz(tmp_path / "small.cbz", {"page1.png": make_png(100, 100, red)})
    with core.open(cbz) as viewer:
        frame = viewer.run()
        bounds = frame.page_bounds()
        assert bounds is not None
        assert (bounds.width, bounds.height) == (HEIGHT, HEIGHT)
        assert frame.pixel(bounds.left, HEIGHT // 2) == red
        assert frame.pixel(bounds.right - 1, HEIGHT // 2) == red
        assert frame.pixel(WIDTH // 2, bounds.top) == red
        assert frame.pixel(WIDTH // 2, bounds.bottom - 1) == red
