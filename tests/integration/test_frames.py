#!/usr/bin/env python3
"""Which frames carry new pixels, and which repeat the previous frame."""

import pathlib

import pytest

from content import PdfPage, make_grid_png, write_cbz, write_pdf
from harness import Core

NEW = True
REPEAT = False


@pytest.fixture(params=["pdf", "cbz"])
def document(request: pytest.FixtureRequest, tmp_path: pathlib.Path) -> pathlib.Path:
    """A two-page grid document."""
    if request.param == "pdf":
        return write_pdf(tmp_path / "pages.pdf", [PdfPage(600, 600, grid=True)] * 2)
    return write_cbz(
        tmp_path / "pages.cbz",
        {f"page{page}.png": make_grid_png(1200, 1200, page) for page in range(2)},
    )


def test_idle_frames_repeat(core: Core, document: pathlib.Path) -> None:
    with core.open(document) as viewer:
        viewer.run(3)
        before = len(viewer.video.refreshes)
        viewer.run(5)
        assert viewer.video.refreshes[before:] == [REPEAT] * 5


def test_view_change_sends_new_frame(core: Core, document: pathlib.Path) -> None:
    with core.open(document) as viewer:
        viewer.run(3)
        idle = viewer.video.frame
        before = len(viewer.video.refreshes)
        turned = viewer.run(r=True)
        assert viewer.video.refreshes[before] == NEW
        assert turned != idle


@pytest.mark.parametrize("can_dupe", [False, None], ids=["refused", "unsupported"])
def test_every_frame_is_new_without_dupe_support(
    core: Core, document: pathlib.Path, can_dupe: bool | None
) -> None:
    with core.open(document, can_dupe=can_dupe) as viewer:
        viewer.run(5)
        viewer.tap("r")
        assert viewer.video.refreshes == [NEW] * 7
