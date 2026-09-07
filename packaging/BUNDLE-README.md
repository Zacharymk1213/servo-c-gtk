# djoter + servo-gtk source bundle

Everything needed to build djoter — the djot editor whose live preview is
rendered by the Servo web engine — from source.

```
build-linux.sh      build on Linux
build-msys2.sh      build under MSYS2 on Windows (UCRT64 shell)
servo-gtk/          the ServoGtkWebView widget and its Rust FFI to Servo
darantlr/           the dar/djot compiler; produces djot.web.js
djoter/             the editor itself, a GNOME Text Editor fork
```

## Build it

Linux:

```sh
./build-linux.sh                 # installs into ./install
./run-djoter.sh some-file.dj
```

Windows, from the **MSYS2 UCRT64** shell (`C:\msys64\ucrt64.exe`):

```sh
./build-msys2.sh --install-deps
./run-djoter.sh some-file.dj
```

Both accept `--prefix DIR`, `--debug`, `--jobs N`, and `--rebuild-darantlr`.
Run either with `--help` for the details.

## What to expect

The first build compiles the whole Servo engine. Budget **tens of minutes**,
around **20 GB of disk**, and a few GB of RAM. Afterwards only the parts you
change rebuild, in seconds.

`darantlr` is only rebuilt when you pass `--rebuild-darantlr`; djoter ships the
compiled bundle at `djoter/src/djot.web.js`, so a normal build needs no
JavaScript toolchain at all.

## Prerequisites

The scripts check for these and tell you what is missing.

**Linux** — a C toolchain, CMake, Ninja, Meson, pkg-config, and the development
packages for GTK4, GtkSourceView 5, libadwaita, libspelling and json-glib.
GObject-introspection is optional; without it the typelib is skipped and the
widget still works from C.

**rustup is required on both platforms**, not just a distro `rustc`. servo-gtk
pins `-Ztls-model=global-dynamic`, so it needs a nightly toolchain, and its
CMake invokes `rustup run nightly cargo` directly. The scripts install the
nightly toolchain for you if rustup is present.

**MSYS2** — everything needed is packaged for UCRT64, including `libspelling`,
`rustup` and `angleproject`. The `--install-deps` flag runs the pacman line;
without it the line is printed for you to run yourself.

## Windows notes

The MSYS2 script targets the **GNU ABI** (`x86_64-pc-windows-gnu`) so the Rust
library and the MinGW-built C libraries agree. The repository also carries a
MinGW *cross*-build from Linux, which targets the MSVC ABI through cargo-xwin —
the two are not interchangeable, and the CMake picks the right cargo driver from
the target triple.

Servo drives OpenGL ES through ANGLE on Windows: surfman loads `libEGL.dll` and
`libGLESv2.dll` **by name at run time**, so they must sit next to the binaries.
The script stages them from `/ucrt64/bin` and warns if it cannot find them.

## Using the preview

The djot preview lives in the right-hand pane of the editor's splitter, and only
switches on for documents GtkSourceView recognises as djot — `*.dj`, `*.djot`,
`*.djt`. Two things commonly hide it:

- The splitter position is remembered in GSettings (`djot-preview-size`). If it
  is wider than the window the preview has no room; drag the divider back.
- Running the editor uninstalled will not find `djot.lang`, so the language is
  not recognised and the preview never turns on. `run-djoter.sh` sets
  `XDG_DATA_DIRS` to the install prefix, which is where it ends up.

## Status

The Linux path is the one that has actually been run: it builds, installs, and
renders djot through Servo.

**The MSYS2 path has not been executed** — there was no Windows machine to run
it on. Every package name in it has been checked against the MSYS2 repositories,
and the ABI and ANGLE handling follow the repository's existing Windows support,
but treat the first run as a shakedown rather than a tested recipe. The most
likely rough edges are Servo's own Windows build and the ANGLE staging.
