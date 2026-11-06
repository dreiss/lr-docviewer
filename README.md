# Document viewer libretro core

Controller-operated PDF/CBZ reader for RetroArch,
intended for game manuals on RetroPie, etc.

**Status: prototype.**

PDF pages are rendered by Poppler through Cairo.
CBZ archives are read with libzip;
libpng validates PNG metadata before Cairo decodes the pages, and
the libjpeg-compatible API from libjpeg-turbo decodes JPEG pages.
CBZ page names use bytewise ASCII ordering.
Pages larger than 2^24 pixels produce a warning and a black placeholder.

During PDF zooming and panning,
the viewer transforms a screen-resolution preview.
On the first idle frame, Poppler renders the visible view
directly into the framebuffer at full quality.
CBZ pages are transformed directly from their decoded image surface.

Controls, in RetroArch's SNES-style button names
(remap them in RetroArch):

| Control | Action |
|---|---|
| L / R | Previous / next page |
| L2 / R2 | Back / forward 10 pages |
| Y / X | Zoom in / out |
| D-pad or left stick | Pan the enlarged page |
| A + L / R | Rotate counterclockwise / clockwise |

A is an option button:
while it's held, L and R rotate the view instead of turning pages,
and the d-pad, X, Y, and B do nothing.
Rotation stays the same when you turn pages.
Page buttons act once per press.
Holding a zoom button doubles or halves the zoom every second,
and panning is continuous.
The left stick pans faster the further it's pushed.
RetroArch's Reset returns to the first page, upright, at fit-page zoom.
Close the viewer with your normal RetroArch exit hotkey.

Output is 1920 x 1080, which RetroArch scales to the screen,
and zoom ranges from fit-page to 8 times that.
Rendering is synchronous, so complex PDFs may briefly pause input.
With RetroArch's logging verbosity on
and its core logging level set to Debug,
the core logs how long opening the document, loading each page,
rendering each PDF preview, and drawing each frame take,
with one summary for each zoom or pan gesture.
Password-protected PDFs are rejected,
and there's no search, table of contents, or links.
A manual opens as separate RetroArch content,
not as an overlay on a running game.
The core doesn't open a window or need X11 or Wayland.

Build commands:

```sh
sudo apt install build-essential cmake pkg-config \
    libpoppler-glib-dev libcairo2-dev libzip-dev libjpeg-dev libpng-dev
sudo dnf install gcc cmake pkgconf poppler-glib-devel cairo-devel libzip-devel \
    libjpeg-turbo-devel libpng-devel
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

`cmake --install build` installs the core into `<libdir>/libretro`
and its info file into `<datadir>/libretro/info`,
under the install prefix (`/usr/local` by default).
Set `DOCVIEWER_INSTALL_COREDIR` and `DOCVIEWER_INSTALL_INFODIR`
to match your RetroArch's core and core info directories.
Build for the same architecture as RetroArch:
a 64-bit core can't load into a 32-bit RetroArch, or the reverse.
`file` shows each binary's architecture.

Development additionally uses `just`, clang-format, and clang-tidy.
Its builds treat warnings as errors.

```sh
just release          # build the optimized core with clang-tidy
just debug            # the same, with debug information
just venv             # once: create .venv from requirements.txt (Python 3.12+)
just pytest           # build the debug core and run the tests
just pytest-sanitize  # the same, against an ASan/UBSan build
just pytest -k cbz    # pass arguments through to pytest
```

The project is licensed under GPL-3.0-or-later; see `LICENSE`.
`libretro.h`, from the libretro-common submodule, keeps its own license.
