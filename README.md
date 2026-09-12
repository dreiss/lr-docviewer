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

Build commands:

```sh
sudo apt install build-essential cmake pkg-config \
    libpoppler-glib-dev libcairo2-dev libzip-dev libjpeg-dev libpng-dev
sudo dnf install poppler-glib-devel cairo-devel libzip-devel \
    libjpeg-turbo-devel libpng-devel
git submodule update --init
just release
```
