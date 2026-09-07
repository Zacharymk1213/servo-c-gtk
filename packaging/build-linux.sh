#!/usr/bin/env bash
#
# Build servo-gtk and djoter from this source bundle, on Linux.
#
#   ./build-linux.sh [--prefix DIR] [--debug] [--rebuild-darantlr] [--jobs N]
#
# Everything is installed under the prefix (default ./install), and a
# run-djoter.sh wrapper is written next to this script.
#
# The long pole is Servo: the first build compiles its whole dependency tree,
# which takes tens of minutes and wants ~20 GB of disk and a few GB of RAM.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="$HERE/install"
BUILD_TYPE=Release
REBUILD_DARANTLR=0
JOBS="$(nproc 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
  case "$1" in
    --prefix)           PREFIX="$2"; shift 2 ;;
    --debug)            BUILD_TYPE=Debug; shift ;;
    --rebuild-darantlr) REBUILD_DARANTLR=1; shift ;;
    --jobs)             JOBS="$2"; shift 2 ;;
    -h|--help)          sed -n '2,/^[^#]/p' "$0" | sed 's/^#\\? \\?//;$d'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

say()  { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- preflight
say "Checking prerequisites"

missing=()
for tool in cc cmake ninja meson pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
[ ${#missing[@]} -eq 0 ] || die "missing tools: ${missing[*]}
Debian/Ubuntu: sudo apt install build-essential cmake ninja-build meson pkg-config
Fedora:        sudo dnf install gcc cmake ninja-build meson pkgconf-pkg-config
Arch:          sudo pacman -S base-devel cmake ninja meson pkgconf"

# servo-gtk's CMake shells out to `rustup run nightly cargo`, so rustup itself
# has to be on PATH — a distro rustc is not enough.
command -v rustup >/dev/null 2>&1 || die "rustup not found.
servo-gtk pins -Ztls-model=global-dynamic, which needs a nightly toolchain, and
its CMake invokes rustup directly. Install it with:
  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
or from your distro (Arch: pacman -S rustup)."

if ! rustup toolchain list 2>/dev/null | grep -q '^nightly'; then
  say "Installing the Rust nightly toolchain"
  rustup toolchain install nightly --profile minimal
fi

pkgs=(gtk4 gtksourceview-5 libadwaita-1 libspelling-1 json-glib-1.0)
missing=()
for p in "${pkgs[@]}"; do
  pkg-config --exists "$p" || missing+=("$p")
done
[ ${#missing[@]} -eq 0 ] || die "missing development packages: ${missing[*]}
Debian/Ubuntu: sudo apt install libgtk-4-dev libgtksourceview-5-dev libadwaita-1-dev libspelling-1-dev libjson-glib-dev
Fedora:        sudo dnf install gtk4-devel gtksourceview5-devel libadwaita-devel libspelling-devel json-glib-devel
Arch:          sudo pacman -S gtk4 gtksourceview5 libadwaita libspelling json-glib"

# GObject-introspection is optional: without it the typelib is skipped and the
# widget is still perfectly usable from C.
GIR_FLAG=()
if pkg-config --exists gobject-introspection-1.0 && command -v g-ir-scanner >/dev/null 2>&1; then
  GIR_FLAG=(-DBUILD_GIR=ON)
else
  warn "gobject-introspection not found; building without GIR/typelib"
  GIR_FLAG=(-DBUILD_GIR=OFF)
fi

[ -d "$HERE/servo-gtk/servo_c_gtk" ] || die "servo-gtk/servo_c_gtk not found next to this script"
[ -d "$HERE/djoter" ]                || die "djoter not found next to this script"

mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"

# ------------------------------------------------------- darantlr (optional)
# djoter ships the built bundle at src/djot.web.js, so this is only needed when
# the grammar changed.
if [ "$REBUILD_DARANTLR" = 1 ]; then
  say "Rebuilding the darantlr bundle"
  command -v npm >/dev/null 2>&1 || die "npm not found, needed for --rebuild-darantlr"
  [ -d "$HERE/darantlr" ] || die "darantlr/ not found next to this script"
  ( cd "$HERE/darantlr"
    if [ -f package-lock.json ]; then npm ci; else npm install; fi
    npm run build )
  cp -v "$HERE/darantlr/dist/dar.web.js" "$HERE/djoter/src/djot.web.js"
else
  say "Using the prebuilt djot bundle (djoter/src/djot.web.js); --rebuild-darantlr to regenerate"
fi

# ---------------------------------------------------------------- servo-gtk
say "Building servo-gtk (this compiles Servo; expect tens of minutes on a first run)"
cd "$HERE/servo-gtk/servo_c_gtk"

# Cargo.lock is not checked in; CMake depends on it existing.
[ -f Cargo.lock ] || rustup run nightly cargo generate-lockfile

cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      "${GIR_FLAG[@]}"
cmake --build build --parallel "$JOBS"
cmake --install build

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
# Launch the installed djoter against the libraries built alongside it.
set -euo pipefail
PREFIX="$PREFIX"
export LD_LIBRARY_PATH="\$PREFIX/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
# djot.lang lives in the prefix; GtkSourceView finds it through XDG_DATA_DIRS,
# and without it the document is not recognised as djot and the preview stays
# switched off.
export XDG_DATA_DIRS="\$PREFIX/share:\${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export GSETTINGS_SCHEMA_DIR="\$PREFIX/share/glib-2.0/schemas"
exec "\$PREFIX/bin/gnome-text-editor" "\$@"
RUNNER
chmod +x "$HERE/run-djoter.sh"

say "Done"
cat <<DONE

  Installed to: $PREFIX

  Run it with:  $HERE/run-djoter.sh [file.dj]

  The djot preview only turns on for documents GtkSourceView recognises as
  djot (*.dj, *.djot, *.djt), and it lives in the right-hand pane of the
  editor's splitter — drag the divider over if you cannot see it.

DONE
