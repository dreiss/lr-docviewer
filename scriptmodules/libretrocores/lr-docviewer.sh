#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

# A RetroPie-Setup module for the document viewer core.
# Clone this repository into RetroPie-Setup's ext directory to add it; see README.md.

rp_module_id="lr-docviewer"
rp_module_desc="Document viewer for PDF and CBZ game manuals"
rp_module_help="ROM Extensions: .pdf .cbz\n\nCopy your manuals to $romdir/manuals"
rp_module_licence="GPL3 https://raw.githubusercontent.com/dreiss/lr-docviewer/main/LICENSE"
rp_module_repo="git https://github.com/dreiss/lr-docviewer.git main"
rp_module_section="exp"

function depends_lr-docviewer() {
    getDepends cmake pkg-config \
      libpoppler-glib-dev libcairo2-dev libzip-dev libjpeg-dev libpng-dev \
      fonts-dejavu-core
}

function sources_lr-docviewer() {
    gitPullOrClone
}

function build_lr-docviewer() {
    rm -rf build
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDOCVIEWER_BUILD_TESTS=OFF
    cmake --build build --parallel
    md_ret_require="$md_build/build/docviewer_libretro.so"
}

function install_lr-docviewer() {
    md_ret_files=(
        'build/docviewer_libretro.so'
        'docviewer_libretro.info'
        'LICENSE'
        'README.md'
    )
}

function configure_lr-docviewer() {
    mkRomDir "manuals"
    defaultRAConfig "manuals"
    addEmulator 1 "$md_id" "manuals" "$md_inst/docviewer_libretro.so"
    addSystem "manuals" "Manuals" ".pdf .cbz"
}
