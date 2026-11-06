#!/usr/bin/env python3
"""Builds test content."""

import collections.abc
import dataclasses
import io
import pathlib
import zipfile

from PIL import Image
from reportlab.pdfgen import canvas

GRID_CELLS = 12
"""The number of columns and rows in a grid page."""


def grid_color(page: int, column: int, row: int) -> tuple[int, int, int]:
    """Returns the color of one cell of a grid page.

    Every cell of every page up to page 2 has a distinct color,
    and none matches the viewer's background or white.
    """
    return 40 + 16 * column, 40 + 16 * row, 64 + 64 * page


def _encode(image: Image.Image, image_format: str) -> bytes:
    output = io.BytesIO()
    image.save(output, format=image_format, quality=95)
    return output.getvalue()


def _grid_image(width: int, height: int, page: int) -> Image.Image:
    image = Image.new("RGB", (width, height))
    for column in range(GRID_CELLS):
        for row in range(GRID_CELLS):
            box = (
                column * width // GRID_CELLS,
                row * height // GRID_CELLS,
                (column + 1) * width // GRID_CELLS,
                (row + 1) * height // GRID_CELLS,
            )
            image.paste(grid_color(page, column, row), box)
    return image


def make_png(width: int, height: int, rgb: tuple[int, int, int] = (128, 128, 128)) -> bytes:
    """Returns a solid-color 8-bit RGB PNG."""
    return _encode(Image.new("RGB", (width, height), rgb), "PNG")


def make_jpeg(width: int, height: int, rgb: tuple[int, int, int] = (128, 128, 128)) -> bytes:
    """Returns a solid-color JPEG."""
    return _encode(Image.new("RGB", (width, height), rgb), "JPEG")


def make_grid_png(width: int, height: int, page: int = 0) -> bytes:
    """Returns a PNG of a grid page; see grid_color()."""
    return _encode(_grid_image(width, height, page), "PNG")


def write_cbz(path: pathlib.Path, entries: collections.abc.Mapping[str, bytes]) -> pathlib.Path:
    """Writes a CBZ whose members appear in the given order."""
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return path


@dataclasses.dataclass(frozen=True)
class PdfPage:
    """A PDF page, measured in points.

    The page is filled with a solid color, or with a grid if grid is set.
    Grid colors use the page's index in the document; see grid_color().

    width and height are the page's size as displayed.
    A rotation of 90 or 270 degrees swaps them for the unrotated page,
    and the grid's rows and columns follow the unrotated page.
    crop, if set, is the crop box as (left, bottom, right, top)
    in unrotated page coordinates.
    """

    width: float
    height: float
    rgb: tuple[int, int, int] = (128, 128, 128)
    grid: bool = False
    rotation: int = 0
    crop: tuple[float, float, float, float] | None = None


def write_pdf(path: pathlib.Path, pages: collections.abc.Sequence[PdfPage]) -> pathlib.Path:
    """Writes a PDF with the given pages.

    Pages contain no text, so rendering doesn't depend on fonts.
    """
    document = canvas.Canvas(str(path), invariant=True)

    def fill(rgb: tuple[int, int, int], x: float, y: float, width: float, height: float) -> None:
        document.setFillColorRGB(*(channel / 255 for channel in rgb))
        document.rect(x, y, width, height, stroke=0, fill=1)

    for index, page in enumerate(pages):
        # ReportLab takes the displayed size and draws in unrotated coordinates.
        document.setPageSize((page.width, page.height))
        document.setPageRotation(page.rotation)
        # ReportLab keeps a crop box for later pages unless it's reset.
        document.setCropBox(page.crop)
        width, height = page.width, page.height
        if page.rotation % 180:
            width, height = height, width
        if not page.grid:
            fill(page.rgb, 0, 0, width, height)
        else:
            cell_width = width / GRID_CELLS
            cell_height = height / GRID_CELLS
            for column in range(GRID_CELLS):
                for row in range(GRID_CELLS):
                    # PDF coordinates start at the bottom left; grid rows start at the top.
                    y = height - (row + 1) * cell_height
                    fill(
                        grid_color(index, column, row),
                        column * cell_width,
                        y,
                        cell_width,
                        cell_height,
                    )
        document.showPage()
    document.save()
    return path
