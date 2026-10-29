#!/usr/bin/env bash
set -euo pipefail

INSTALL_ROOT="${XDG_DATA_HOME:-${HOME}/.local/share}/pasteshelf-install"
APPLICATIONS_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/applications"
AUTOSTART_DIR="${XDG_CONFIG_HOME:-${HOME}/.config}/autostart"
DATA_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/PasteShelf"
DESKTOP_NAME="io.github.pasteshelf.PasteShelf.desktop"
ICON_PATH="${XDG_DATA_HOME:-${HOME}/.local/share}/icons/hicolor/512x512/apps/io.github.pasteshelf.PasteShelf.png"
LEGACY_INSTALL_ROOT="${XDG_DATA_HOME:-${HOME}/.local/share}/pastetray-install"
LEGACY_DATA_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/PasteTray"
LEGACY_DESKTOP_NAME="io.github.pastetray.PasteTray.desktop"
LEGACY_ICON_PATH="${XDG_DATA_HOME:-${HOME}/.local/share}/icons/hicolor/512x512/apps/io.github.pastetray.PasteTray.png"

rm -f -- "${APPLICATIONS_DIR}/${DESKTOP_NAME}"
rm -f -- "${AUTOSTART_DIR}/${DESKTOP_NAME}"
rm -f -- "${ICON_PATH}"
rm -rf -- "${INSTALL_ROOT}"
rm -f -- "${APPLICATIONS_DIR}/${LEGACY_DESKTOP_NAME}"
rm -f -- "${AUTOSTART_DIR}/${LEGACY_DESKTOP_NAME}"
rm -f -- "${LEGACY_ICON_PATH}"
rm -rf -- "${LEGACY_INSTALL_ROOT}"
if command -v kwriteconfig6 >/dev/null 2>&1; then
    kwriteconfig6 \
        --file kglobalshortcutsrc \
        --group kwin \
        --key pastetray-capture-clipboard \
        --delete \
        --notify \
        ''
fi

if [[ "${1:-}" == "--purge" ]]; then
    rm -rf -- "${DATA_DIR}"
    rm -rf -- "${LEGACY_DATA_DIR}"
    echo "PasteShelf and its saved shelf data were removed."
else
    echo "PasteShelf was removed. Saved shelf data was kept in ${DATA_DIR}."
fi
