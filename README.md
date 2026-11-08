# Document viewer libretro core

Controller-operated PDF/CBZ reader for RetroArch,
intended for game manuals on RetroPie, etc.

**Status: functional.**

- Opens PDF or CBZ files as "ROMs" to allow viewing game manuals.
- Supports pan, zoom, rotation, and 2-page mode.
- Press Start to show help (SNES-style button naming).

Output is 1080p by default, which RetroArch scales to the screen.
The Output Resolution core option, under Core Options in RetroArch's Quick Menu,
offers other sizes.
Matching the size of your screen gives the best balance of speed and quality.
Setting size takes effect the next time you open a document.

The project is licensed under GPL-3.0-or-later; see `LICENSE`.
`libretro.h`, from the libretro-common submodule, keeps its own license.

## Build and install

Build commands:

```sh
# Debian deps
sudo apt install build-essential cmake pkg-config \
    libpoppler-glib-dev libcairo2-dev libzip-dev libjpeg-dev libpng-dev
# Fedora deps
sudo dnf install gcc cmake pkgconf poppler-glib-devel cairo-devel libzip-devel \
    libjpeg-turbo-devel libpng-devel
# Build
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

RetroPie install (note: makes a separate clone and installs from main):

```sh
git clone https://github.com/dreiss/lr-docviewer.git ~/RetroPie-Setup/ext/lr-docviewer
sudo ~/RetroPie-Setup/retropie_packages.sh lr-docviewer
```

Custom install:

```sh
cmake -S . -B build \
    -DDOCVIEWER_INSTALL_COREDIR=<RetroArch's core directory> \
    -DDOCVIEWER_INSTALL_INFODIR=<RetroArch's core info directory>
cmake --install build
```

## Technical details

PDF pages are rendered by Poppler through Cairo.
CBZ archives are read with libzip;
libpng validates PNG metadata before Cairo decodes the pages,
and the libjpeg-compatible API from libjpeg-turbo decodes JPEG pages.
CBZ page names use bytewise ASCII ordering.
Pages larger than 2^24 pixels produce a warning and a black placeholder.

During PDF zooming and panning,
the viewer transforms a screen-resolution preview.
On the first idle frame, Poppler renders the visible view
directly into the framebuffer at full quality.
CBZ pages are transformed directly from their decoded image surface.

Rendering is synchronous, so complex PDFs may briefly pause input.
With RetroArch's logging verbosity on and its core logging level set to Debug,
the core logs various timing statistics.

## Development

Development additionally uses `just`, clang-format, and clang-tidy,
and the tests use Tesseract with its English data
(`tesseract-ocr` and `tesseract-ocr-eng` with apt,
`tesseract` and `tesseract-langpack-eng` with dnf)
to read the help screen.
Development builds treat warnings as errors.

```sh
just release          # build the optimized core with clang-tidy
just debug            # the same, with debug information
just venv             # once: create .venv from requirements.txt (Python 3.12+)
just pytest           # build the debug core and run the tests
just pytest-sanitize  # the same, against an ASan/UBSan build
just pytest -k cbz    # pass arguments through to pytest
```

