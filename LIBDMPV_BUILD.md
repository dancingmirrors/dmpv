# Building libdmpv Shared Library

This document describes how to build dmpv as a shared library (libdmpv) for use with applications like mpvpaper.

## Overview

The libdmpv shared library provides a compatible API with libmpv, allowing applications designed for libmpv to work with dmpv. The library is named `libdmpv` to avoid conflicts with regular mpv installations.

## Building

To build libdmpv, use the `--enable-libdmpv-shared` configure option:

```bash
./configure --enable-libdmpv-shared
ninja -C build
```

This will build:
- `libdmpv.so.2.1.0` - The shared library
- `libdmpv.so.2` - SONAME symlink
- `libdmpv.so` - Development symlink

## Installing

To install the library and headers:

```bash
ninja -C build install
```

This installs:
- Library files to `$PREFIX/lib/`:
  - `libdmpv.so.2.1.0`
  - `libdmpv.so.2` (symlink)
  - `libdmpv.so` (symlink)
- Header files to `$PREFIX/include/mpv/`:
  - `client.h`
  - `render.h`
  - `render_gl.h`
  - `stream_cb.h`
- Pkg-config file to `$PREFIX/lib/pkgconfig/libdmpv.pc`

## API Compatibility

The public headers in `include/mpv/` provide compatibility with the standard libmpv API by mapping `mpv_*` symbols to their `dmpv_*` equivalents:

```c
#include <mpv/client.h>
#include <mpv/render_gl.h>

// These functions map to dmpv_create(), dmpv_initialize(), etc.
mpv_handle *mpv = mpv_create();
mpv_initialize(mpv);
```

## Using with mpvpaper

Once installed, mpvpaper should be able to find and use libdmpv:

```bash
# Build mpvpaper
cd mpvpaper
meson setup build
ninja -C build

# The build system will find libdmpv through pkg-config
```

## Differences from libmpv

The main differences:
1. Library is named `libdmpv` instead of `libmpv`
2. Internal symbols use `dmpv_*` prefix
3. Public headers provide `mpv_*` → `dmpv_*` mapping for compatibility
4. No support for vo_libmpv (macOS-only feature)
5. Only supports Linux and BSD systems

## Pkg-config

Applications can use pkg-config to find libdmpv:

```bash
pkg-config --cflags --libs libdmpv
```

The pkg-config file (`libdmpv.pc`) provides the necessary compiler and linker flags.

## Notes

- This fork only supports Linux and BSDs
- The shared library is built with `-fPIC` and uses SONAME versioning
- Headers are installed to a standard location (`$PREFIX/include/mpv/`)
- The library can coexist with regular libmpv installations

