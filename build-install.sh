#!/usr/bin/env bash
#
# build-install.sh - Build and install the scrcpy-camera OBS plugin on Linux.
#
# Usage:
#   ./build-install.sh [options]
#
# Options:
#   -h, --help             Show this help and exit.
#   -s, --system           Install system-wide (uses `cmake --install`, needs sudo).
#   -u, --user             Install per-user into ~/.config/obs-studio/plugins (default).
#   -b, --build-dir DIR    Build directory to use (default: build_x86_64).
#   -c, --clean            Remove the build directory and reconfigure from scratch.
#   -n, --no-build         Skip the build; only install from an existing build dir.
#   -i, --install-dir DIR  Override the per-user plugin install directory
#                          (default: $HOME/.config/obs-studio/plugins).
#   -D, --dry-run          Print the actions that would be taken without running them.

set -euo pipefail

# Resolve the repo root (parent of this script), following symlinks.
SOURCE="${BASH_SOURCE[0]}"
while [ -h "$SOURCE" ]; do
  DIR="$(cd -P "$(dirname "$SOURCE")" >/dev/null 2>&1 && pwd)"
  SOURCE="$(readlink "$SOURCE")"
  [[ $SOURCE != /* ]] && SOURCE="$DIR/$SOURCE"
done
ROOT="$(cd -P "$(dirname "$SOURCE")" >/dev/null 2>&1 && pwd)"
cd "$ROOT"

INSTALL_MODE="user"
BUILD_DIR="build_x86_64"
USER_PLUGIN_DIR="${HOME}/.config/obs-studio/plugins"
DO_CLEAN="no"
DO_BUILD="yes"
DRY_RUN="no"

PLUGIN_NAME="scrcpy-camera"
PLUGIN_SO="${PLUGIN_NAME}.so"
DATA_SRC="$ROOT/data"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
info()  { printf '\033[1;34m[scrcpy-camera]\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m[scrcpy-camera]\033[0m %s\n' "$*" >&2; }
error() { printf '\033[1;31m[scrcpy-camera]\033[0m %s\n' "$*" >&2; }

usage() {
  sed -n '2,13p' "$ROOT/build-install.sh" | sed 's/^# //; s/^#$//'
}

run() {
  if [ "$DRY_RUN" = "1" ]; then
    printf '  (dry-run) %s\n' "$*"
    return 0
  fi
  "$@"
}

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    -s|--system) INSTALL_MODE="system" ;;
    -u|--user) INSTALL_MODE="user" ;;
    -b|--build-dir)
      [ $# -ge 2 ] || { error "option '$1' requires an argument"; exit 1; }
      shift; BUILD_DIR="$1" ;;
    -c|--clean) DO_CLEAN="yes" ;;
    -n|--no-build) DO_BUILD="no" ;;
    -i|--install-dir)
      [ $# -ge 2 ] || { error "option '$1' requires an argument"; exit 1; }
      shift; USER_PLUGIN_DIR="$1" ;;
    -D|--dry-run) DRY_RUN="1" ;;
    *) error "unknown option: $1"; usage >&2; exit 1 ;;
  esac
  shift
done

# ---------------------------------------------------------------------------
# Tool / dependency checks
# ---------------------------------------------------------------------------
command -v cmake >/dev/null 2>&1 || { error "cmake is required but was not found."; exit 1; }
if ! command -v ninja >/dev/null 2>&1; then
  warn "ninja not found; required by the ubuntu-x86_64 preset. Install with: sudo apt install ninja-build"
fi

MISSING_DEPS=0
check_dev_header() {
  local header="$1" pkg="$2"
  if [ ! -f "$header" ]; then
    warn "missing header '$header' (package: $pkg)"
    MISSING_DEPS=1
  fi
}
check_dev_header "/usr/include/obs/obs-module.h" "libobs-dev"
check_dev_header "/usr/include/x86_64-linux-gnu/libavcodec/avcodec.h" "libavcodec-dev"
check_dev_header "/usr/include/x86_64-linux-gnu/libavutil/avutil.h" "libavutil-dev"
check_dev_header "/usr/include/x86_64-linux-gnu/libswscale/swscale.h" "libswscale-dev"
check_dev_header "/usr/include/x86_64-linux-gnu/libswresample/swresample.h" "libswresample-dev"

if [ "$MISSING_DEPS" != "0" ]; then
  error "Some development dependencies are missing. Install them with:"
  error "  sudo apt install build-essential cmake ninja-build \\"
  error "    libobs-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev"
  exit 1
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if [ "$DO_BUILD" = "yes" ]; then
  if [ "$DO_CLEAN" = "yes" ]; then
    info "cleaning build dir: $BUILD_DIR"
    run rm -rf "$BUILD_DIR"
  fi

  info "configuring CMake (preset: ubuntu-x86_64)..."
  if [ "$BUILD_DIR" = "build_x86_64" ]; then
    run cmake --preset ubuntu-x86_64
  else
    run cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  fi

  info "building plugin..."
  run cmake --build "$BUILD_DIR"
else
  info "skipping build (--no-build)"
fi

# ---------------------------------------------------------------------------
# Locate built output
# ---------------------------------------------------------------------------
BUILD_OUT="$ROOT/$BUILD_DIR"
SO_PATH="$BUILD_OUT/$PLUGIN_SO"
if [ ! -f "$SO_PATH" ]; then
  SO_PATH="$(find "$BUILD_OUT" -name "$PLUGIN_SO" -type f 2>/dev/null | head -n1)"
fi
if [ -z "$SO_PATH" ] || [ ! -f "$SO_PATH" ]; then
  error "could not find $PLUGIN_SO under $BUILD_OUT (build the plugin first)."
  exit 1
fi

info "plugin binary: $SO_PATH"
info "install mode:  $INSTALL_MODE"

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
case "$INSTALL_MODE" in
  user)
    TARGET_DIR="$USER_PLUGIN_DIR/$PLUGIN_NAME"
    TARGET_BIN="$TARGET_DIR/bin/64bit"
    TARGET_DATA="$TARGET_DIR/data"
    info "installing per-user into: $TARGET_DIR"
    run mkdir -p "$TARGET_BIN" "$TARGET_DATA"
    run cp -f "$SO_PATH" "$TARGET_BIN/$PLUGIN_SO"
    run cp -rf "$DATA_SRC/." "$TARGET_DATA/"
    info "installed to: $TARGET_DIR"
    ;;
  system)
    info "installing system-wide via 'cmake --install' (into /usr/local by default)..."
    run sudo cmake --install "$BUILD_OUT"
    ;;
  *)
    error "unknown install mode: $INSTALL_MODE"
    exit 1
    ;;
esac

info "done."
info "Fully restart OBS Studio (quit completely, not just closing the window) to load the plugin."
info "See https://obsproject.com/kb/plugins-guide for plugin locations and OBS_PLUGINS_* overrides."