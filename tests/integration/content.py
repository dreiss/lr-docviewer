#!/usr/bin/env python3
"""Builds test content."""

import collections.abc
import dataclasses
import io
import pathlib
import zipfile

from PIL import Image
from reportlab.pdfgen import canvas


def make_png(width: int, height: int, rgb: tuple[int, int, int] = (128, 128, 128)) -> bytes:
    """Returns a solid-color 8-bit RGB PNG."""
    output = io.BytesIO()
    Image.new("RGB", (width, height), rgb).save(output, format="PNG")
    return output.getvalue()


def write_cbz(path: pathlib.Path, entries: collections.abc.Mapping[str, bytes]) -> pathlib.Path:
    """Writes a CBZ whose members appear in the given order."""
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in entries.items():
            archive.writestr(name, data)
    return path


@dataclasses.dataclass(frozen=True)
class PdfPage:
    """A solid-color PDF page, measured in points."""

    width: float
    height: float
    rgb: tuple[int, int, int] = (128, 128, 128)


def write_pdf(path: pathlib.Path, pages: collections.abc.Sequence[PdfPage]) -> pathlib.Path:
    """Writes a PDF with the given pages.

    Pages contain no text, so rendering doesn't depend on fonts.
    """
    document = canvas.Canvas(str(path), invariant=True)
    for page in pages:
        document.setPageSize((page.width, page.height))
        document.setFillColorRGB(*(channel / 255 for channel in page.rgb))
        document.rect(0, 0, page.width, page.height, stroke=0, fill=1)
        document.showPage()
    document.save()
    return path
