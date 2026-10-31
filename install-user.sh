#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="${SCRIPT_DIR}/build/release"
INSTALL_ROOT="${XDG_DATA_HOME:-${HOME}/.local/share}/pastashelf-install"
BIN_DIR="${INSTALL_ROOT}/bin"
APPLICATIONS_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/applications"
AUTOSTART_DIR="${XDG_CONFIG_HOME:-${HOME}/.config}/autostart"
ICON_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/icons/hicolor/512x512/apps"
DESKTOP_NAME="io.github.pastashelf.PastaShelf.desktop"
ICON_NAME="io.github.pastashelf.PastaShelf.png"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --parallel

rm -rf -- "${INSTALL_ROOT}"
install -d "${BIN_DIR}" "${APPLICATIONS_DIR}" "${ICON_DIR}"
install -m 0755 "${BUILD_DIR}/pastashelf" "${BIN_DIR}/pastashelf"
sed \
    -e "s|@EXEC@|${BIN_DIR}/pastashelf|g" \
    -e "s|@ICON@|${ICON_DIR}/${ICON_NAME}|g" \
    "${SCRIPT_DIR}/packaging/${DESKTOP_NAME}.in" \
    > "${APPLICATIONS_DIR}/${DESKTOP_NAME}"
chmod 0644 "${APPLICATIONS_DIR}/${DESKTOP_NAME}"
install -m 0644 \
    "${SCRIPT_DIR}/assets/${ICON_NAME}" \
    "${ICON_DIR}/${ICON_NAME}"

# Remove obsolete launchers and install roots. Legacy Python data is left
# untouched; separately branded C++ data is migrated by PastaShelf at launch.
rm -f -- "${APPLICATIONS_DIR}/io.github.pastashelf.PastaShelfNative.desktop"
rm -f -- "${APPLICATIONS_DIR}/io.github.pasteshelf.PasteShelfNative.desktop"
rm -f -- "${APPLICATIONS_DIR}/io.github.pasteshelf.PasteShelf.desktop"
rm -f -- "${APPLICATIONS_DIR}/io.github.pastetray.PasteTray.desktop"
rm -f -- "${AUTOSTART_DIR}/${DESKTOP_NAME}"
rm -f -- "${AUTOSTART_DIR}/io.github.pastashelf.PastaShelfNative.desktop"
rm -f -- "${AUTOSTART_DIR}/io.github.pasteshelf.PasteShelfNative.desktop"
rm -f -- "${AUTOSTART_DIR}/io.github.pasteshelf.PasteShelf.desktop"
rm -f -- "${AUTOSTART_DIR}/io.github.pastetray.PasteTray.desktop"
rm -f -- "${ICON_DIR}/io.github.pastashelf.PastaShelfNative.png"
rm -f -- "${ICON_DIR}/io.github.pasteshelf.PasteShelfNative.png"
rm -f -- "${ICON_DIR}/io.github.pasteshelf.PasteShelf.png"
rm -f -- "${ICON_DIR}/io.github.pastetray.PasteTray.png"
rm -rf -- "${XDG_DATA_HOME:-${HOME}/.local/share}/pastashelf-native-install"
rm -rf -- "${XDG_DATA_HOME:-${HOME}/.local/share}/pasteshelf-native-install"
rm -rf -- "${XDG_DATA_HOME:-${HOME}/.local/share}/pasteshelf-install"
rm -rf -- "${XDG_DATA_HOME:-${HOME}/.local/share}/pastetray-install"

if command -v kwriteconfig6 >/dev/null 2>&1; then
    kwriteconfig6 \
        --file kglobalshortcutsrc \
        --group kwin \
        --key pasteshelf-capture-clipboard \
        --delete \
        --notify \
        ''
    kwriteconfig6 \
        --file kglobalshortcutsrc \
        --group kwin \
        --key pastetray-capture-clipboard \
        --delete \
        --notify \
        ''
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APPLICATIONS_DIR}" >/dev/null 2>&1 || true
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi

echo "PastaShelf installed in the KDE application launcher."
echo "Search for PastaShelf, or start it with: ${BIN_DIR}/pastashelf"
echo "Launch at login remains disabled."
