# PasteShelf

PasteShelf is a small Yoink-style drag shelf for KDE Plasma. A narrow
handle stays at the right-center edge of the active screen. Drag something onto
the handle to open the shelf, drop it, navigate to the destination, and drag it
back out.

The first release supports files, folders, multi-file stacks, plain and rich
text, links, images, and screenshots. Cards persist across restarts until they
are removed. Normal files remain references to their originals; in-memory and
temporary content is copied into PasteShelf's private data directory.

Applications that keep text drags inside their own UI can use explicit
clipboard capture instead: copy the content, then press `Meta+Shift+V` or choose
**Add clipboard** in PasteShelf. PasteShelf reads the clipboard only when that
action is invoked; it does not monitor clipboard history.

## Requirements

- Fedora KDE Plasma on Wayland
- Python 3.11 or newer
- PySide6 / Qt 6
- Qt Wayland and KWin scripting support (included with Fedora KDE)

Fedora KDE already supplies the runtime pieces on the target system. If needed:

```bash
sudo dnf install python3-pyside6 qt6-qtwayland
```

## Run from the checkout

```bash
PYTHONPATH=src python3 -m pasteshelf
```

## User installation

```bash
./install-user.sh
```

The installer copies PasteShelf into your user data directory, uses Fedora's
system Python and PySide6, and installs a **PasteShelf** entry in KDE's
application launcher. It does not enable login startup or download dependencies.
After installation, search for **PasteShelf** in the launcher like any other app.
Login startup can be enabled later from PasteShelf's menu if desired.

To uninstall while keeping saved cards:

```bash
./uninstall-user.sh
```

Use `./uninstall-user.sh --purge` to remove the saved shelf data as well.

## PasteShelf Native performance build

`native/` contains a separate C++/Qt Widgets implementation for testing a much
smaller always-on footprint. It does not replace the Python/QML application:
both versions have unique executables, desktop IDs, settings, runtime locks,
and data directories, so they can run side by side.

Install the optimized test build with:

```bash
./install-native-user.sh
```

Then launch **PasteShelf Native** from KDE. Login startup remains disabled. The
native build supports file/folder stacks, text and HTML, links, screenshots,
transactional persistence, pinning cards, drag-out copy semantics, removal
animations, persistent drag reordering, full hover previews, drawer pinning,
one-click clipboard copy for cards and stack members, quick text-note creation,
screen/edge selection, tray actions, and crash-safe
single-instance recovery. It uses scaled-at-decode thumbnails and has no
polling loop. Double-click a copied or created text card to edit it in place.

The native build intentionally omits the original app's global clipboard
shortcut while both versions are installed, avoiding a shortcut collision.
Use the shelf's **Paste** button or **Add clipboard** from its tray menu instead.
The complete stack or an individual expanded member can be dragged out.

With both versions running, compare their current RSS, proportional memory,
and lifetime CPU percentage:

```bash
./compare-pasteshelf.sh
```

Remove only the native test build with `./uninstall-native-user.sh`. Add
`--purge` to delete its separate test data and preferences too.

## Tests

```bash
PYTHONPATH=src python3 -m unittest discover -v
```

The automated suite covers persistence, MIME round-tripping, stacks, owned
payload cleanup, missing files, the list model, and drawer state transitions.
Cross-application drag behavior should additionally be checked on a live Plasma
Wayland session with Dolphin, a browser, a text editor, and Spectacle.

The native binary also contains a fast persistence/MIME/cleanup self-test:

```bash
QT_QPA_PLATFORM=offscreen build/native-release/pasteshelf-native --self-test
```

## KDE Wayland behavior

Wayland applications only receive a drag after it enters one of their surfaces.
PasteShelf therefore keeps a small edge handle visible instead of monitoring
drags globally. KWin does not deliver external drag offers to LayerShell
surfaces, so PasteShelf uses a normal native Wayland surface. While PasteShelf is
running, it loads a temporary KWin script that keeps the frameless window at the
chosen edge; the script is unloaded when PasteShelf exits.
