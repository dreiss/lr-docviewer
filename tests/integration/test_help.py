#!/usr/bin/env python3
"""The help screen, shown with Start."""

import collections.abc
import pathlib

import pytest

from content import PdfPage, grid_color, write_pdf
from harness import Core, Viewer

WHITE = (255, 255, 255)


@pytest.fixture
def viewer(core: Core, tmp_path: pathlib.Path) -> collections.abc.Iterator[Viewer]:
    pdf = write_pdf(tmp_path / "grid.pdf", [PdfPage(600, 600, grid=True)] * 2)
    with core.open(pdf) as viewer:
        yield viewer


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
