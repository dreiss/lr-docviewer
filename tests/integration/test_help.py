#!/usr/bin/env python3
"""The help screen, shown with Start."""

import collections.abc
import dataclasses
import itertools
import pathlib

import pytesseract
import pytest

from content import PdfPage, grid_color, write_pdf
from harness import Bounds, Core, Frame, Viewer

WHITE = (255, 255, 255)
# The title, each button and its action, and the closing note.
TEXT = [
    ["Controls"],
    ["L / R", "Previous / next page"],
    ["L2 / R2", "Back / forward 10 pages"],
    ["Y / X", "Zoom in / out"],
    ["D-pad, left stick", "Pan"],
    ["A + L / R", "Rotate counterclockwise / clockwise"],
    ["A + B", "One or two pages, odd or even on left"],
    ["Start", "Show or hide this help"],
    ["Press any button to close."],
]
RESOLUTIONS = [
    "640x480",
    "1024x768",
    "1440x1080",
    "1280x720",
    "1600x900",
    "1920x1080",
    "2560x1440",
    "3840x2160",
]


@pytest.fixture
def viewer(core: Core, tmp_path: pathlib.Path) -> collections.abc.Iterator[Viewer]:
    pdf = write_pdf(tmp_path / "grid.pdf", [PdfPage(600, 600, grid=True)] * 2)
    with core.open(pdf) as viewer:
        yield viewer


@dataclasses.dataclass(frozen=True)
class Word:
    text: str
    bounds: Bounds


def read_lines(frame: Frame) -> list[list[Word]]:
    """Returns the words that Tesseract reads in a frame, by line from top to bottom."""
    try:
        data = pytesseract.image_to_data(frame.image(), output_type=pytesseract.Output.DICT)
    except pytesseract.TesseractNotFoundError:
        pytest.fail("Tesseract isn't installed; see README.md")
    lines: dict[tuple[int, int, int], list[Word]] = {}
    for i, text in enumerate(data["text"]):
        if not text.strip():
            continue
        left = data["left"][i]
        top = data["top"][i]
        bounds = Bounds(left, top, left + data["width"][i], top + data["height"][i])
        key = (data["block_num"][i], data["par_num"][i], data["line_num"][i])
        lines.setdefault(key, []).append(Word(text, bounds))
    return sorted(
        (sorted(words, key=lambda word: word.bounds.left) for words in lines.values()),
        key=lambda words: words[0].bounds.top,
    )


def split_columns(line: list[Word], gap: float) -> list[list[Word]]:
    """Splits a line wherever its words are more than a gap apart."""
    columns = [[line[0]]]
    for previous, word in itertools.pairwise(line):
        if word.bounds.left - previous.bounds.right > gap:
            columns.append([])
        columns[-1].append(word)
    return columns


def normalize(text: str) -> str:
    """Ignores spacing and case, which Tesseract reads inconsistently around slashes and X."""
    return "".join(text.split()).casefold()


@pytest.mark.parametrize("resolution", RESOLUTIONS)
def test_help_text_is_complete_and_on_screen(
    core: Core, tmp_path: pathlib.Path, resolution: str
) -> None:
    pdf = write_pdf(tmp_path / "grid.pdf", [PdfPage(600, 600, grid=True)])
    with core.open(pdf, options={"docviewer_resolution": resolution}) as viewer:
        viewer.run()
        frame = viewer.tap("start")
    # The font size is a 24th of the height, and the columns are at least that far apart.
    # Tesseract's word boxes are loose, so words in a column can be 0.7 of that apart.
    font_size = frame.height / 24
    rows = [split_columns(line, 0.9 * font_size) for line in read_lines(frame)]
    read = [[" ".join(word.text for word in column) for column in row] for row in rows]
    assert [[normalize(text) for text in row] for row in read] == [
        [normalize(text) for text in row] for row in TEXT
    ], read

    # Lines start at one left edge, and actions at another.
    tolerance = frame.height / 120
    for starts in (
        [row[0][0].bounds.left for row in rows],
        [row[1][0].bounds.left for row in rows if len(row) > 1],
    ):
        assert max(starts) - min(starts) <= tolerance, starts

    margin = frame.height / 48
    for row in rows:
        for column in row:
            for word in column:
                bounds = word.bounds
                assert margin <= bounds.left and bounds.right <= frame.width - margin, word
                assert margin <= bounds.top and bounds.bottom <= frame.height - margin, word


def test_start_shows_help_over_the_dimmed_page(viewer: Viewer) -> None:
    page = viewer.run()
    assert page.locate(WHITE) is None
    shown = viewer.tap("start")
    assert shown.locate(WHITE) is not None
    # The bottom-right cell is below the text, and dimmed to a quarter of its brightness.
    dimmed = tuple(round(channel / 4) for channel in grid_color(0, 11, 11))
    actual = shown.pixel(1455, 1035)
    assert all(abs(a - d) <= 1 for a, d in zip(actual, dimmed, strict=True)), actual


def test_start_hides_help(viewer: Viewer) -> None:
    page = viewer.run()
    viewer.tap("start")
    assert viewer.tap("start") == page


def test_closing_button_does_nothing_else(viewer: Viewer) -> None:
    page = viewer.run()
    viewer.tap("start")
    assert viewer.tap("r") == page
    assert viewer.messages is not None
    assert viewer.messages.page_numbers() == ["Page 1 of 2"]


def test_reset_hides_help(viewer: Viewer) -> None:
    page = viewer.run()
    viewer.tap("start")
    viewer.reset()
    assert viewer.run() == page
