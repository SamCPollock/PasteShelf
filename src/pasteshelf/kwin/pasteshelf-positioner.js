// Temporary KWin helper loaded by PasteShelf while the app is running.
// Native Wayland clients cannot position their own top-level windows, so the
// desired rectangle is encoded in the title as PasteShelf@x,y,width,height.

const prefix = "PasteShelf@";
const watched = new Map();

registerShortcut(
    "pasteshelf-capture-clipboard",
    "PasteShelf: Add Clipboard",
    "Meta+Shift+V",
    () => {
        callDBus(
            "io.github.pasteshelf.PasteShelf",
            "/PasteShelf",
            "io.github.pasteshelf.PasteShelf",
            "CaptureClipboard",
            () => {}
        );
    }
);

function log(message) {
    console.info("pasteshelf-positioner: " + message);
}

function desiredGeometry(window) {
    const caption = window.caption || "";
    if (!caption.startsWith(prefix)) {
        return null;
    }
    // KWin may append the application name to the caption (for example,
    // "PasteShelf@2165,1452,28,112 — PasteShelf"). Parse only the leading
    // coordinate payload rather than requiring the caption to end there.
    const match = caption.match(
        /^PasteShelf@(-?\d+(?:\.\d+)?),(-?\d+(?:\.\d+)?),(\d+(?:\.\d+)?),(\d+(?:\.\d+)?)/
    );
    if (!match) {
        return null;
    }
    const values = match.slice(1).map(Number);
    return {
        x: Math.round(values[0]),
        y: Math.round(values[1]),
        width: Math.max(1, Math.round(values[2])),
        height: Math.max(1, Math.round(values[3]))
    };
}

function apply(window) {
    const desired = desiredGeometry(window);
    if (!desired) {
        return;
    }

    window.skipTaskbar = true;
    window.skipPager = true;
    window.skipSwitcher = true;
    window.keepAbove = true;
    window.noBorder = true;

    const current = window.frameGeometry;
    if (Math.round(current.x) !== desired.x
            || Math.round(current.y) !== desired.y
            || Math.round(current.width) !== desired.width
            || Math.round(current.height) !== desired.height) {
        window.frameGeometry = desired;
        log("positioned " + window.caption + " at "
            + desired.x + "," + desired.y + " "
            + desired.width + "x" + desired.height);
    }
}

function watch(window) {
    if (watched.has(window)) {
        return;
    }
    // windowAdded can arrive before the client has published its first title.
    // Observe the caption from the start so that race cannot leave PasteShelf
    // unpositioned.
    watched.set(window, true);
    window.captionChanged.connect(() => apply(window));
    window.closed.connect(() => watched.delete(window));
    apply(window);
}

workspace.windowList().forEach(window => watch(window));
workspace.windowAdded.connect(window => watch(window));
