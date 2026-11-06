#!/usr/bin/env python3
"""CBZ archive reading and image decoding."""

import collections.abc
import pathlib

import pytest
from libretro.api.log import LogLevel

from content import make_jpeg, make_png, write_cbz
from harness import HEIGHT, WIDTH, Core

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
