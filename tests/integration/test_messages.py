#!/usr/bin/env python3
"""The on-screen page indicator."""

import pathlib

import pytest
from libretro.api.log import LogLevel
from libretro.api.message import MessageTarget, MessageType

from content import PdfPage, make_png, write_cbz, write_pdf
from harness import Core, Message, Viewer

PAGES = 3


@pytest.fixture
def document(tmp_path: pathlib.Path) -> pathlib.Path:
    return write_pdf(tmp_path / "pages.pdf", [PdfPage(600, 600, grid=True)] * PAGES)


def texts(viewer: Viewer) -> list[str]:
    assert viewer.messages is not None
    return viewer.messages.texts()


def test_page_number_is_shown_on_load_and_page_turns(core: Core, document: pathlib.Path) -> None:
    with core.open(document) as viewer:
        viewer.run()
        assert texts(viewer) == ["Page 1 of 3"]
        viewer.tap("r")
        viewer.tap("r")
        viewer.tap("l")
        assert texts(viewer) == ["Page 1 of 3", "Page 2 of 3", "Page 3 of 3", "Page 2 of 3"]


def test_page_number_is_a_brief_on_screen_status(core: Core, document: pathlib.Path) -> None:
    with core.open(document) as viewer:
        viewer.run()
        assert viewer.messages is not None
        assert viewer.messages.messages == [
            Message(
                text="Page 1 of 3",
                duration=2000,
                level=LogLevel.INFO,
                target=MessageTarget.OSD,
                type=MessageType.STATUS,
            )
        ]


def test_no_page_number_without_a_page_change(core: Core, document: pathlib.Path) -> None:
    with core.open(document) as viewer:
        viewer.tap("l")
        viewer.tap("y")
        viewer.run(5, right=True)
        viewer.tap("x")
        viewer.tap("r", times=PAGES - 1)
        viewer.tap("r")
        assert texts(viewer) == ["Page 1 of 3", "Page 2 of 3", "Page 3 of 3"]


def test_reset_shows_the_first_page_number(core: Core, document: pathlib.Path) -> None:
    with core.open(document) as viewer:
        viewer.tap("r")
        viewer.reset()
        assert texts(viewer)[-1] == "Page 1 of 3"


def test_unreadable_page_is_announced(core: Core, tmp_path: pathlib.Path) -> None:
    cbz = write_cbz(
        tmp_path / "corrupt.cbz",
        {"page1.png": make_png(1500, 1500), "page2.png": b"not a PNG"},
    )
    with core.open(cbz) as viewer:
        viewer.tap("r")
        assert viewer.messages is not None
        last = viewer.messages.messages[-1]
        assert last.text == "Page 2 of 2 couldn't be loaded"
        assert last.level == LogLevel.WARNING


def test_older_frontends_get_a_legacy_message(core: Core, document: pathlib.Path) -> None:
    with core.open(document, message_version=0) as viewer:
        viewer.run()
        assert viewer.messages is not None
        # Two seconds at 60 frames per second.
        assert viewer.messages.messages == [Message(text="Page 1 of 3", frames=120)]


def test_viewer_works_without_on_screen_messages(core: Core, document: pathlib.Path) -> None:
    with core.open(document, message_version=None) as viewer:
        first = viewer.run()
        assert viewer.tap("r") != first
