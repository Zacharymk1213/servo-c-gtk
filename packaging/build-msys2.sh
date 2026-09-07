#!/usr/bin/env bash
#
# Build servo-gtk and djoter from this source bundle under MSYS2 on Windows.
#
# Run it from the *UCRT64* shell (not the plain MSYS shell):
#
#   ./build-msys2.sh [--prefix DIR] [--debug] [--rebuild-darantlr] [--jobs N]
#                    [--install-deps]
#
# --install-deps runs the pacman line for you; otherwise it is only printed.
#
# NOTE: this path targets the GNU ABI (x86_64-pc-windows-gnu) so the Rust
# library and the MinGW-built C libraries agree. The repository's other Windows
# path — MinGW cross-compilation *from Linux* — targets the MSVC ABI through
# cargo-xwin instead; the two are not interchangeable.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="$HERE/install"
BUILD_TYPE=Release
REBUILD_DARANTLR=0
INSTALL_DEPS=0
JOBS="$(nproc 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)           PREFIX="$2"; shift 2 ;;
    --debug)            BUILD_TYPE=Debug; shift ;;
    --rebuild-darantlr) REBUILD_DARANTLR=1; shift ;;
    --install-deps)     INSTALL_DEPS=1; shift ;;
    --jobs)             JOBS="$2"; shift 2 ;;
    -h|--help)          sed -n '2,/^[^#]/p' "$0" | sed 's/^#\\? \\?//;$d'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

say()  { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- preflight
[ "${MSYSTEM:-}" = "UCRT64" ] || die "run this from the MSYS2 UCRT64 shell (MSYSTEM is '${MSYSTEM:-unset}').
Open 'MSYS2 UCRT64' from the Start menu, or run:  C:\\msys64\\ucrt64.exe"

PAC=(
  mingw-w64-ucrt-x86_64-toolchain
  mingw-w64-ucrt-x86_64-cmake
  mingw-w64-ucrt-x86_64-ninja
  mingw-w64-ucrt-x86_64-meson
  mingw-w64-ucrt-x86_64-pkgconf
  mingw-w64-ucrt-x86_64-gtk4
  mingw-w64-ucrt-x86_64-gtksourceview5
  mingw-w64-ucrt-x86_64-libadwaita
  mingw-w64-ucrt-x86_64-libspelling
  mingw-w64-ucrt-x86_64-json-glib
  mingw-w64-ucrt-x86_64-gobject-introspection
  mingw-w64-ucrt-x86_64-rustup
  mingw-w64-ucrt-x86_64-angleproject
  mingw-w64-ucrt-x86_64-nodejs
  git
)

if [ "$INSTALL_DEPS" = 1 ]; then
  say "Installing MSYS2 packages"
  pacman -S --needed --noconfirm "${PAC[@]}"
else
  say "Package prerequisites (re-run with --install-deps to install them)"
  printf '  pacman -S --needed %s\n' "${PAC[*]}"
fi

missing=()
for tool in gcc cmake ninja meson pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
[ ${#missing[@]} -eq 0 ] || die "missing tools: ${missing[*]} — run with --install-deps"

pkgs=(gtk4 gtksourceview-5 libadwaita-1 libspelling-1 json-glib-1.0)
missing=()
for p in "${pkgs[@]}"; do
  pkg-config --exists "$p" || missing+=("$p")
done
[ ${#missing[@]} -eq 0 ] || die "missing development packages: ${missing[*]} — run with --install-deps"

# servo-gtk's CMake calls `rustup run nightly cargo`, and the pinned
# -Ztls-model=global-dynamic needs nightly.
command -v rustup >/dev/null 2>&1 || die "rustup not found — run with --install-deps,
or:  pacman -S mingw-w64-ucrt-x86_64-rustup"

if ! rustup toolchain list 2>/dev/null | grep -q '^nightly'; then
  say "Installing the Rust nightly toolchain (GNU ABI)"
  rustup toolchain install nightly-x86_64-pc-windows-gnu --profile minimal
fi

mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"

# ------------------------------------------------------- darantlr (optional)
if [ "$REBUILD_DARANTLR" = 1 ]; then
  say "Rebuilding the darantlr bundle"
  command -v npm >/dev/null 2>&1 || die "npm not found, needed for --rebuild-darantlr"
  ( cd "$HERE/darantlr"
    if [ -f package-lock.json ]; then npm ci; else npm install; fi
    npm run build )
  cp -v "$HERE/darantlr/dist/dar.web.js" "$HERE/djoter/src/djot.web.js"
else
  say "Using the prebuilt djot bundle (djoter/src/djot.web.js)"
fi

# ---------------------------------------------------------------- servo-gtk
say "Building servo-gtk (this compiles Servo; expect a long first run)"
cd "$HERE/servo-gtk/servo_c_gtk"

[ -f Cargo.lock ] || rustup run nightly cargo generate-lockfile

# CARGO_TARGET_TRIPLE selects the GNU ABI, which also tells the CMake to drive
# plain `cargo build` rather than cargo-xwin (which only supplies an MSVC
# toolchain and has nothing to contribute here).
cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCARGO_TARGET_TRIPLE=x86_64-pc-windows-gnu \
      -DBUILD_GIR=OFF
cmake --build build --parallel "$JOBS"
cmake --install build

# Servo drives OpenGL ES through ANGLE on Windows: surfman loads libEGL.dll and
# libGLESv2.dll by name at run time, so they have to sit next to the binaries.
say "Staging the ANGLE runtime"
angle_found=0
for dll in libEGL.dll libGLESv2.dll; do
  for dir in /ucrt64/bin "$PREFIX/bin"; do
    if [ -f "$dir/$dll" ]; then
      cp -v "$dir/$dll" "$PREFIX/bin/" 2>/dev/null || true
      angle_found=1
      break
    fi
  done
done
[ "$angle_found" = 1 ] || warn "libEGL.dll / libGLESv2.dll not found.
Servo will fail to create a rendering context without them. Install
mingw-w64-ucrt-x86_64-angleproject, or copy a prebuilt ANGLE pair into
$PREFIX/bin."

# ------------------------------------------------------------------- djoter
say "Building djoter"
cd "$HERE/djoter"

# --pkg-config-path is recorded in the build directory, so it survives a later
# reconfigure; the environment variable alone does not. Reconfigure an existing
# tree rather than wiping it, so a rebuild stays incremental.
meson_args=(--prefix "$PREFIX" --pkg-config-path "$PREFIX/lib/pkgconfig")
if [ -f build/meson-info/meson-info.json ]; then
  meson setup build --reconfigure "${meson_args[@]}"
else
  rm -rf build
  meson setup build "${meson_args[@]}"
fi

meson compile -C build -j "$JOBS"
meson install -C build

# ----------------------------------------------------------------- run script
cat > "$HERE/run-djoter.sh" <<RUNNER
#!/usr/bin/env bash
# Launch the installed djoter from the MSYS2 UCRT64 shell.
set -euo pipefail
PREFIX="$PREFIX"
export PATH="\$PREFIX/bin:/ucrt64/bin:\$PATH"
export XDG_DATA_DIRS="\$PREFIX/share;/ucrt64/share"
export GSETTINGS_SCHEMA_DIR="\$PREFIX/share/glib-2.0/schemas"
exec "\$PREFIX/bin/gnome-text-editor.exe" "\$@"
RUNNER
chmod +x "$HERE/run-djoter.sh"

say "Done"
cat <<DONE

  Installed to: $PREFIX

  Run it with:  $HERE/run-djoter.sh [file.dj]

  To run it outside the MSYS2 shell, the DLLs it needs must be alongside the
  executable or on PATH: everything in $PREFIX/bin, the GTK4 stack from
  /ucrt64/bin, and the ANGLE pair (libEGL.dll, libGLESv2.dll).

DONE
