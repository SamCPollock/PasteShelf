#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="${SCRIPT_DIR}/build/native-release"
INSTALL_ROOT="${XDG_DATA_HOME:-${HOME}/.local/share}/pasteshelf-native-install"
BIN_DIR="${INSTALL_ROOT}/bin"
APPLICATIONS_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/applications"
ICON_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/icons/hicolor/512x512/apps"
DESKTOP_NAME="io.github.pasteshelf.PasteShelfNative.desktop"
ICON_NAME="io.github.pasteshelf.PasteShelfNative.png"

cmake -S "${SCRIPT_DIR}/native" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --parallel

install -d "${BIN_DIR}" "${APPLICATIONS_DIR}" "${ICON_DIR}"
install -m 0755 "${BUILD_DIR}/pasteshelf-native" "${BIN_DIR}/pasteshelf-native"
sed -e "s|@EXEC@|${BIN_DIR}/pasteshelf-native|g" \
    -e "s|@ICON@|${ICON_DIR}/${ICON_NAME}|g" \
    "${SCRIPT_DIR}/native/packaging/${DESKTOP_NAME}.in" \
    > "${APPLICATIONS_DIR}/${DESKTOP_NAME}"
chmod 0644 "${APPLICATIONS_DIR}/${DESKTOP_NAME}"
install -m 0644 \
    "${SCRIPT_DIR}/src/pasteshelf/assets/io.github.pasteshelf.PasteShelf.png" \
    "${ICON_DIR}/${ICON_NAME}"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APPLICATIONS_DIR}" >/dev/null 2>&1 || true
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi

echo "PasteShelf Native installed alongside PasteShelf."
echo "Search for 'PasteShelf Native' in KDE. Login startup remains disabled."
