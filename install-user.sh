#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
INSTALL_ROOT="${XDG_DATA_HOME:-${HOME}/.local/share}/pasteshelf-install"
APP_DIR="${INSTALL_ROOT}/app"
BIN_DIR="${INSTALL_ROOT}/bin"
APPLICATIONS_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/applications"
AUTOSTART_DIR="${XDG_CONFIG_HOME:-${HOME}/.config}/autostart"
DESKTOP_NAME="io.github.pasteshelf.PasteShelf.desktop"
ICON_NAME="io.github.pasteshelf.PasteShelf.png"
ICON_DIR="${XDG_DATA_HOME:-${HOME}/.local/share}/icons/hicolor/512x512/apps"
PYTHON_BIN=$(command -v python3)
LEGACY_INSTALL_ROOT="${XDG_DATA_HOME:-${HOME}/.local/share}/pastetray-install"
LEGACY_DESKTOP="io.github.pastetray.PasteTray.desktop"
LEGACY_ICON="${XDG_DATA_HOME:-${HOME}/.local/share}/icons/hicolor/512x512/apps/io.github.pastetray.PasteTray.png"

python3 -c 'import PySide6; from PySide6.QtCore import qVersion; print(f"Using PySide6/Qt {qVersion()}")'

if [[ ! -f /usr/lib64/qt6/plugins/platforms/libqwayland.so ]]; then
    echo "PasteShelf requires Fedora's Qt 6 Wayland platform plugin." >&2
    exit 1
fi

rm -rf -- "${APP_DIR}"
install -d "${APP_DIR}" "${BIN_DIR}"
cp -R "${SCRIPT_DIR}/src/pasteshelf" "${APP_DIR}/pasteshelf"
sed \
    -e "s|@APP_DIR@|${APP_DIR}|g" \
    -e "s|@PYTHON@|${PYTHON_BIN}|g" \
    "${SCRIPT_DIR}/packaging/pasteshelf-launcher.in" \
    > "${BIN_DIR}/pasteshelf"
chmod 0755 "${BIN_DIR}/pasteshelf"

install -d "${APPLICATIONS_DIR}" "${ICON_DIR}"
sed "s|@EXEC@|${BIN_DIR}/pasteshelf|g" \
    "${SCRIPT_DIR}/packaging/${DESKTOP_NAME}.in" \
    > "${APPLICATIONS_DIR}/${DESKTOP_NAME}"
chmod 0644 "${APPLICATIONS_DIR}/${DESKTOP_NAME}"
install -m 0644 \
    "${SCRIPT_DIR}/src/pasteshelf/assets/${ICON_NAME}" \
    "${ICON_DIR}/${ICON_NAME}"

# Remove the obsolete launcher package. Saved cards and preferences are moved
# by PasteShelf itself on first launch, after the old process has exited.
rm -f -- "${APPLICATIONS_DIR}/${LEGACY_DESKTOP}"
rm -f -- "${AUTOSTART_DIR}/${LEGACY_DESKTOP}"
rm -f -- "${LEGACY_ICON}"
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

# Installing the launcher must not silently opt the user into login startup.
# The in-app "Launch at login" toggle can create this entry later.
rm -f -- "${AUTOSTART_DIR}/${DESKTOP_NAME}"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "${APPLICATIONS_DIR}" >/dev/null 2>&1 || true
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi

echo "PasteShelf installed in the KDE application launcher."
echo "Search for PasteShelf, or start it with: ${BIN_DIR}/pasteshelf"
echo "Launch at login remains disabled."
