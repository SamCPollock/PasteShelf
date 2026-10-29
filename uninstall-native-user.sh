#!/usr/bin/env bash
set -euo pipefail

INSTALL_ROOT="${XDG_DATA_HOME:-${HOME}/.local/share}/pasteshelf-native-install"
APPLICATIONS_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/applications"
ICON_PATH="${XDG_DATA_HOME:-${HOME}/.local/share}/icons/hicolor/512x512/apps/io.github.pasteshelf.PasteShelfNative.png"
DATA_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/PasteShelf/PasteShelfNative"
CONFIG_FILE="${XDG_CONFIG_HOME:-${HOME}/.config}/PasteShelf/PasteShelfNative.conf"

rm -f -- "${APPLICATIONS_DIR}/io.github.pasteshelf.PasteShelfNative.desktop"
rm -f -- "${ICON_PATH}"
rm -rf -- "${INSTALL_ROOT}"

if [[ "${1:-}" == "--purge" ]]; then
    rm -rf -- "${DATA_DIR}"
    rm -f -- "${CONFIG_FILE}"
    echo "PasteShelf Native and its separate test data were removed."
else
    echo "PasteShelf Native was removed. Its test data was kept."
fi
