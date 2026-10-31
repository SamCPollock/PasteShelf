#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "This packaging script must run on macOS." >&2
    exit 1
fi

if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is required. Install it from https://brew.sh and rerun this script." >&2
    exit 1
fi

brew install cmake ninja qt

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_DIR=$(cd -- "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR="${PROJECT_DIR}/build/macos"
DIST_DIR="${PROJECT_DIR}/dist"
QT_PREFIX=${QT_PREFIX:-$(brew --prefix qt)}
ARCHITECTURE=$(uname -m)
APP_PATH="${BUILD_DIR}/PastaShelf.app"
DEPLOYED_APP="${DIST_DIR}/PastaShelf.app"
DMG_PATH="${DIST_DIR}/PastaShelf-macOS-${ARCHITECTURE}.dmg"

cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${QT_PREFIX}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
cmake --build "${BUILD_DIR}" --parallel
ctest --test-dir "${BUILD_DIR}" --output-on-failure

mkdir -p "${DIST_DIR}"
rm -rf -- "${DEPLOYED_APP}"
rm -f -- "${DMG_PATH}"
ditto "${APP_PATH}" "${DEPLOYED_APP}"
"${QT_PREFIX}/bin/macdeployqt" "${DEPLOYED_APP}" \
    -always-overwrite \
    -codesign=-

DMG_STAGE=$(mktemp -d)
trap 'rm -rf -- "${DMG_STAGE}"' EXIT
ditto "${DEPLOYED_APP}" "${DMG_STAGE}/PastaShelf.app"
ln -s /Applications "${DMG_STAGE}/Applications"
hdiutil create \
    -volname "PastaShelf" \
    -srcfolder "${DMG_STAGE}" \
    -ov \
    -format UDZO \
    "${DMG_PATH}"

echo "Created ${DMG_PATH}"
