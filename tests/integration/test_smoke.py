#!/usr/bin/env python3
import pathlib

from libretro.api.log import LogLevel
from libretro.api.video import PixelFormat

from content import PdfPage, make_png, write_cbz, write_pdf
from harness import BACKGROUND, HEIGHT, WIDTH, Bounds, Core


def test_pdf_first_page_fits_screen(core: Core, tmp_path: pathlib.Path) -> None:
    red = (200, 0, 0)
    # Fits the screen at a scale of 1.35, so the page edges fall on whole pixels.
    pdf = write_pdf(tmp_path / "page.pdf", [PdfPage(600, 800, red)])
    with core.open(pdf) as viewer:
        frame = viewer.run()
        assert viewer.pixel_format == PixelFormat.XRGB8888
        assert (frame.width, frame.height) == (WIDTH, HEIGHT)
        assert frame.pixel(0, 0) == BACKGROUND
        assert frame.page_bounds() == Bounds(555, 0, 1365, HEIGHT)
        assert frame.pixel(WIDTH // 2, HEIGHT // 2) == red


def test_cbz_next_page(core: Core, tmp_path: pathlib.Path) -> None:
    red = (200, 0, 0)
    blue = (0, 0, 200)
    # Larger than the screen, so pages are scaled down.
    cbz = write_cbz(
        tmp_path / "pages.cbz",
        {"page1.png": make_png(1500, 1500, red), "page2.png": make_png(3000, 1000, blue)},
    )
    with core.open(cbz) as viewer:
        frame = viewer.run()
        bounds = frame.page_bounds()
        assert bounds is not None
        assert (bounds.width, bounds.height) == (HEIGHT, HEIGHT)
        assert frame.pixel(WIDTH // 2, HEIGHT // 2) == red

        frame = viewer.tap("r")
        bounds = frame.page_bounds()
        assert bounds is not None
        assert (bounds.width, bounds.height) == (WIDTH, WIDTH // 3)
        assert frame.pixel(WIDTH // 2, HEIGHT // 2) == blue


def test_missing_file_logs_formatted_error(core: Core, tmp_path: pathlib.Path) -> None:
    missing = tmp_path / "missing.pdf"
    log = core.load_failure(missing)
    errors = log.messages(LogLevel.ERROR)
    assert any(str(missing) in message for message in errors), errors
    # The log shim formatted every message, so no printf conversions remain.
    assert not any("%s" in message or "%d" in message for message in log.messages())
