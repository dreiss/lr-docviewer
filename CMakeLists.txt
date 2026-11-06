cmake_minimum_required(VERSION 3.16)

project(
  lr-docviewer
  VERSION 0.1.1
  DESCRIPTION "Libretro document viewer"
  LANGUAGES C
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DOCVIEWER_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)
if(DOCVIEWER_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy)
  if(NOT CLANG_TIDY_EXECUTABLE)
    message(FATAL_ERROR "DOCVIEWER_ENABLE_CLANG_TIDY requires clang-tidy")
  endif()
  set(CMAKE_C_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE}")
endif()

option(DOCVIEWER_WERROR "Treat compiler warnings as errors" OFF)
set(DOCVIEWER_WARNINGS -Wall -Wextra -Wpedantic $<$<BOOL:${DOCVIEWER_WERROR}>:-Werror>)

option(DOCVIEWER_ENABLE_SANITIZERS "Enable ASan and UBSan" OFF)
if(DOCVIEWER_ENABLE_SANITIZERS AND NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR "DOCVIEWER_ENABLE_SANITIZERS requires Clang or GCC")
endif()

find_package(PkgConfig REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG 1.6 REQUIRED)
pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-glib)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)

add_library(
  docviewer_libretro SHARED
  src/document.h
  src/docviewer.c
  src/document.c
  src/libretro_fluff.c
  src/cbz_document.c
  src/pdf_document.c
)
set_target_properties(
  docviewer_libretro
  PROPERTIES C_VISIBILITY_PRESET hidden
             PREFIX ""
             OUTPUT_NAME docviewer_libretro
             POSITION_INDEPENDENT_CODE ON
)
target_compile_features(docviewer_libretro PRIVATE c_std_11)
target_compile_options(docviewer_libretro PRIVATE ${DOCVIEWER_WARNINGS})
if(DOCVIEWER_ENABLE_SANITIZERS)
  target_compile_options(
    docviewer_libretro PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
  )
  target_link_options(docviewer_libretro PRIVATE -fsanitize=address,undefined)
endif()
target_compile_definitions(
  docviewer_libretro
  PRIVATE
    DOCVIEWER_NAME=\"${PROJECT_DESCRIPTION}\"
    DOCVIEWER_VERSION=\"${PROJECT_VERSION}\"
)
target_include_directories(
  docviewer_libretro PRIVATE vendor/libretro-common/include
)
target_link_libraries(
  docviewer_libretro
  PRIVATE
    JPEG::JPEG
    PNG::PNG
    PkgConfig::CAIRO
    PkgConfig::LIBZIP
    PkgConfig::POPPLER
    m
)

include(GNUInstallDirs)
set(DOCVIEWER_INSTALL_COREDIR
    "${CMAKE_INSTALL_LIBDIR}/libretro"
    CACHE STRING "Core install directory, absolute or relative to the install prefix"
)
set(DOCVIEWER_INSTALL_INFODIR
    "${CMAKE_INSTALL_DATADIR}/libretro/info"
    CACHE STRING "Core info install directory, absolute or relative to the install prefix"
)
install(TARGETS docviewer_libretro LIBRARY DESTINATION "${DOCVIEWER_INSTALL_COREDIR}")
install(FILES docviewer_libretro.info DESTINATION "${DOCVIEWER_INSTALL_INFODIR}")

option(DOCVIEWER_BUILD_TESTS "Build test support libraries" ON)
if(DOCVIEWER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
