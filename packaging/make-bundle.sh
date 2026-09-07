#!/usr/bin/env bash
#
# Assemble the source zip that build-linux.sh / build-msys2.sh build from.
#
#   ./make-bundle.sh [-o OUTPUT.zip] [--servo-gtk DIR] [--darantlr DIR] [--djoter DIR]
#
# Build outputs, dependency trees and git history are left out: what ships is
# the sources needed to build, plus the prebuilt djot bundle so that a normal
# build needs no JavaScript toolchain.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVO_GTK="$(cd "$HERE/.." && pwd)"
DARANTLR="$SERVO_GTK/../darantlr-project"
DJOTER="$SERVO_GTK/../djoter_experiments/djoter"
OUT="$PWD/djoter-servo-src.zip"

while [ $# -gt 0 ]; do
  case "$1" in
    -o|--output)  OUT="$2"; shift 2 ;;
    --servo-gtk)  SERVO_GTK="$2"; shift 2 ;;
    --darantlr)   DARANTLR="$2"; shift 2 ;;
    --djoter)     DJOTER="$2"; shift 2 ;;
    -h|--help)    sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

command -v zip >/dev/null 2>&1 || die "zip not found (apt/dnf/pacman install zip)"
for d in "$SERVO_GTK" "$DARANTLR" "$DJOTER"; do
  [ -d "$d" ] || die "not a directory: $d"
done

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
ROOT="$STAGE/djoter-servo-src"
mkdir -p "$ROOT"

# Excluded everywhere: version control, build trees, dependency trees, and the
# Servo target directory (tens of GB once built).
common_excludes=(
  --exclude=.git --exclude=.git/**
  --exclude=node_modules --exclude=node_modules/**
  --exclude=target --exclude=target/**
  --exclude=build --exclude=build/**
  --exclude=cmake-build-* --exclude=cmake-build-*/**
  --exclude=_build --exclude=_build/**
  --exclude=install --exclude=install/**
  --exclude=.cache --exclude=.cache/**
)

copy_tree() { # copy_tree SRC DEST [extra rsync excludes...]
  local src="$1" dest="$2"; shift 2
  mkdir -p "$dest"
  rsync -a "${common_excludes[@]}" "$@" "$src"/ "$dest"/
}

command -v rsync >/dev/null 2>&1 || die "rsync not found (apt/dnf/pacman install rsync)"

echo "staging servo-gtk from $SERVO_GTK"
copy_tree "$SERVO_GTK" "$ROOT/servo-gtk"

echo "staging darantlr from $DARANTLR"
# test_files is ~100 MB of fixtures and is not needed to produce the bundle.
copy_tree "$DARANTLR" "$ROOT/darantlr" --exclude=test_files --exclude=test_files/**

echo "staging djoter from $DJOTER"
copy_tree "$DJOTER" "$ROOT/djoter"

[ -f "$ROOT/djoter/src/djot.web.js" ] \
  || echo "warning: djoter/src/djot.web.js is missing; builds will need --rebuild-darantlr" >&2

install -m 755 "$HERE/build-linux.sh"  "$ROOT/build-linux.sh"
install -m 755 "$HERE/build-msys2.sh"  "$ROOT/build-msys2.sh"
install -m 644 "$HERE/BUNDLE-README.md" "$ROOT/README.md"

rm -f "$OUT"
( cd "$STAGE" && zip -qr "$OUT" djoter-servo-src )

printf '\nwrote %s (%s)\n' "$OUT" "$(du -h "$OUT" | cut -f1)"
printf 'contents:\n'
( cd "$STAGE" && find djoter-servo-src -maxdepth 2 -mindepth 1 -type d | sort | sed 's/^/  /' )
