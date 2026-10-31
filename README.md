# PastaShelf

PastaShelf is a small Yoink-style drag shelf for Linux and macOS, implemented
in C++ with Qt Widgets. A narrow handle stays at the selected screen edge.
Drag something onto the handle to open the shelf, drop it, navigate to the
destination, and drag it back out.

It supports files, folders, multi-file stacks, plain and rich text, links,
images, and screenshots. Cards persist across restarts until they are removed.
Normal files remain references to their originals; in-memory and temporary
content is copied into PastaShelf's private data directory.

## Linux (KDE Plasma)

### Requirements

- Fedora KDE Plasma on Wayland
- CMake 3.24 or newer
- Ninja
- Qt 6.8 or newer with Core, Gui, Widgets, and Network

### Build and run

```bash
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
./build/release/pastashelf
```

### User installation

```bash
./install-user.sh
```

The installer builds PastaShelf and adds a **PastaShelf** entry to KDE's
application launcher. It does not enable launch at login or download
dependencies.

To uninstall while keeping saved cards:

```bash
./uninstall-user.sh
```

Use `./uninstall-user.sh --purge` to remove saved PastaShelf data and
preferences as well.

## macOS

PastaShelf supports macOS 12 or newer on Apple Silicon and Intel Macs.

### Download from GitHub

Tagged GitHub releases automatically build an ad-hoc-signed Apple Silicon DMG.
Download `PastaShelf-macOS-arm64.dmg`, open it, and drag **PastaShelf** into the
Applications shortcut.

Because the app is not notarized with a paid Apple Developer ID, macOS may
block the first launch. Control-click **PastaShelf** in Applications, choose
**Open**, then confirm **Open**. Later launches work normally.

### Build on a Mac

Install the Xcode Command Line Tools and Homebrew, clone this repository, then
run:

```bash
./scripts/build-macos.sh
```

The script installs missing Homebrew build dependencies, builds and tests the
app for the Mac's current architecture, embeds the Qt frameworks with
`macdeployqt`, ad-hoc signs the bundle, and creates:

```text
dist/PastaShelf-macOS-arm64.dmg
```

On an Intel Mac the filename ends in `x86_64.dmg` instead. The macOS build uses
a normal `.app` bundle, a menu-bar icon, the same edge handle, and Finder's
**Reveal** behavior for stored files.

## Features

- File and folder stacks
- Plain text, rich text, links, images, and screenshots
- Transactional persistence and crash-safe single-instance recovery
- Pinned cards and drag-out copy semantics
- Persistent drag reordering and full hover previews
- One-click clipboard copy and quick text-note creation
- Screen and edge selection
- Tray controls
- Scaled-at-decode thumbnails

Double-click a copied or created text card to edit it in place. The complete
stack or an individual expanded member can be dragged out.

## Tests

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure
```

The executable also has a direct persistence, MIME, and cleanup self-test:

```bash
QT_QPA_PLATFORM=offscreen build/debug/pastashelf --self-test
```

Cross-application drag behavior should additionally be checked on the target
desktop with its file manager, browser, text editor, and screenshot tool.

## KDE Wayland behavior

Wayland applications only receive a drag after it enters one of their surfaces.
PastaShelf therefore keeps a small edge handle visible instead of monitoring
drags globally. KWin does not deliver external drag offers to LayerShell
surfaces, so PastaShelf uses a normal native Wayland surface. While PastaShelf
is running, it loads a temporary KWin script that keeps the frameless window at
the chosen edge; the script is unloaded when PastaShelf exits.
