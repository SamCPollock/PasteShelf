#!/usr/bin/env bash
set -euo pipefail

DATA_HOME="${XDG_DATA_HOME:-${HOME}/.local/share}"
CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME}/.config}"
INSTALL_ROOT="${DATA_HOME}/pastashelf-install"
APPLICATIONS_DIR="${DATA_HOME}/applications"
AUTOSTART_DIR="${CONFIG_HOME}/autostart"
ICON_DIR="${DATA_HOME}/icons/hicolor/512x512/apps"
DATA_DIR="${DATA_HOME}/PastaShelf/PastaShelf"
CONFIG_FILE="${CONFIG_HOME}/PastaShelf/PastaShelf.conf"

rm -f -- "${APPLICATIONS_DIR}/io.github.pastashelf.PastaShelf.desktop"
rm -f -- "${APPLICATIONS_DIR}/io.github.pastashelf.PastaShelfNative.desktop"
rm -f -- "${APPLICATIONS_DIR}/io.github.pasteshelf.PasteShelfNative.desktop"
rm -f -- "${APPLICATIONS_DIR}/io.github.pasteshelf.PasteShelf.desktop"
rm -f -- "${APPLICATIONS_DIR}/io.github.pastetray.PasteTray.desktop"
rm -f -- "${AUTOSTART_DIR}/io.github.pastashelf.PastaShelf.desktop"
rm -f -- "${AUTOSTART_DIR}/io.github.pastashelf.PastaShelfNative.desktop"
rm -f -- "${AUTOSTART_DIR}/io.github.pasteshelf.PasteShelfNative.desktop"
rm -f -- "${AUTOSTART_DIR}/io.github.pasteshelf.PasteShelf.desktop"
rm -f -- "${AUTOSTART_DIR}/io.github.pastetray.PasteTray.desktop"
rm -f -- "${ICON_DIR}/io.github.pastashelf.PastaShelf.png"
rm -f -- "${ICON_DIR}/io.github.pastashelf.PastaShelfNative.png"
rm -f -- "${ICON_DIR}/io.github.pasteshelf.PasteShelfNative.png"
rm -f -- "${ICON_DIR}/io.github.pasteshelf.PasteShelf.png"
rm -f -- "${ICON_DIR}/io.github.pastetray.PasteTray.png"
rm -rf -- "${INSTALL_ROOT}"
rm -rf -- "${DATA_HOME}/pastashelf-native-install"
rm -rf -- "${DATA_HOME}/pasteshelf-native-install"
rm -rf -- "${DATA_HOME}/pasteshelf-install"
rm -rf -- "${DATA_HOME}/pastetray-install"

if command -v kwriteconfig6 >/dev/null 2>&1; then
    kwriteconfig6 \
        --file kglobalshortcutsrc \
        --group kwin \
        --key pasteshelf-capture-clipboard \
        --delete \
        --notify \
        ''
fi

if [[ "${1:-}" == "--purge" ]]; then
    rm -rf -- "${DATA_DIR}"
    rm -f -- "${CONFIG_FILE}"
    rm -rf -- "${DATA_HOME}/PastaShelf/PastaShelfNative"
    rm -f -- "${CONFIG_HOME}/PastaShelf/PastaShelfNative.conf"
    rm -rf -- "${DATA_HOME}/PasteShelf/PasteShelfNative"
    rm -f -- "${CONFIG_HOME}/PasteShelf/PasteShelfNative.conf"
    echo "PastaShelf and its saved data were removed."
else
    echo "PastaShelf was removed. Saved data was kept in ${DATA_DIR}."
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APPLICATIONS_DIR}" >/dev/null 2>&1 || true
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi
