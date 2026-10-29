import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    objectName: "pasteShelfWindow"

    SystemPalette {
        id: palette
        colorGroup: SystemPalette.Active
    }

    readonly property bool horizontalHandle: positionController.placement === "top-center"
                                             || positionController.placement === "bottom-center"
    readonly property int collapsedWidth: horizontalHandle ? 112 : 28
    readonly property int collapsedHeight: horizontalHandle ? 28 : 112
    readonly property int expandedWidth: 380
    readonly property int panelHeight: 460
    readonly property color panelColor: Qt.alpha(palette.window, 0.985)
    readonly property color cardColor: Qt.alpha(palette.base, 0.76)
    readonly property color cardHoverColor: Qt.alpha(palette.base, 0.96)
    readonly property color mutedColor: Qt.alpha(palette.text, 0.62)
    property bool externalDragActive: false
    property string dropPreviewTitle: ""
    property string dropPreviewDetail: ""
    property string dropPreviewGlyph: "↓"
    property bool surfaceExpanded: false
    property bool handleVisible: false
    property real panelProgress: 0

    function handleArrow() {
        const placement = positionController.placement
        if (placement.indexOf("left") >= 0)
            return "▸"
        if (placement.indexOf("right") >= 0)
            return "◂"
        if (placement.indexOf("top") >= 0)
            return "▾"
        return "▴"
    }

    function panelOffsetX() {
        const remaining = 1 - panelProgress
        if (positionController.placement.indexOf("left") >= 0)
            return -42 * remaining
        if (positionController.placement.indexOf("right") >= 0)
            return 42 * remaining
        return 0
    }

    function panelOffsetY() {
        const remaining = 1 - panelProgress
        if (positionController.placement === "top-center")
            return -42 * remaining
        if (positionController.placement === "bottom-center")
            return 42 * remaining
        return 0
    }

    function isPasteShelfDrag(formats) {
        return formats.indexOf("application/x-pasteshelf-item") >= 0
    }

    function prepareDropPreview(drag) {
        externalDragActive = true
        dropPreviewGlyph = "↓"
        if (drag.urls.length > 0) {
            const firstUrl = drag.urls[0].toString()
            const pieces = firstUrl.split("/")
            dropPreviewTitle = decodeURIComponent(pieces[pieces.length - 1]) || firstUrl
            dropPreviewDetail = drag.urls.length > 1
                ? drag.urls.length + " files or links · Release to add here"
                : "File or link · Release to add here"
            dropPreviewGlyph = drag.urls.length > 1 ? "▦" : "□"
            return
        }
        for (let index = 0; index < drag.formats.length; ++index) {
            if (drag.formats[index].indexOf("image/") === 0) {
                dropPreviewTitle = "Image"
                dropPreviewDetail = "Image content · Release to add here"
                dropPreviewGlyph = "▣"
                return
            }
        }
        if (drag.hasText || drag.hasHtml) {
            const compact = (drag.text || "Rich text").replace(/\s+/g, " ").trim()
            dropPreviewTitle = compact.length > 64
                ? compact.substring(0, 61) + "…"
                : compact
            dropPreviewDetail = drag.hasHtml
                ? "Rich text · Release to add here"
                : "Text snippet · Release to add here"
            dropPreviewGlyph = "≡"
            return
        }
        dropPreviewTitle = "Unsupported item"
        dropPreviewDetail = "PasteShelf cannot store this drag"
        dropPreviewGlyph = "!"
    }

    Component.onCompleted: {
        root.surfaceExpanded = drawerController.expanded
        root.handleVisible = !drawerController.expanded
        root.panelProgress = drawerController.expanded ? 1 : 0
    }

    Timer {
        id: handleRevealTimer
        interval: 100
        repeat: false
        onTriggered: {
            if (!drawerController.expanded && !root.surfaceExpanded)
                root.handleVisible = true
        }
    }

    NumberAnimation {
        id: openPanelAnimation
        target: root
        property: "panelProgress"
        to: 1
        duration: 190
        easing.type: Easing.OutCubic
    }

    SequentialAnimation {
        id: closePanelAnimation
        NumberAnimation {
            target: root
            property: "panelProgress"
            to: 0
            duration: 145
            easing.type: Easing.InCubic
        }
        ScriptAction {
            script: {
                if (!drawerController.expanded) {
                    root.surfaceExpanded = false
                    handleRevealTimer.restart()
                }
            }
        }
    }

    Connections {
        target: drawerController
        function onExpandedChanged() {
            if (drawerController.expanded) {
                closePanelAnimation.stop()
                handleRevealTimer.stop()
                root.handleVisible = false
                root.surfaceExpanded = true
                openPanelAnimation.restart()
            } else {
                openPanelAnimation.stop()
                closePanelAnimation.restart()
            }
        }
    }

    HoverHandler {
        id: windowHover
        onHoveredChanged: drawerController.setHovered(hovered)
    }

    Rectangle {
        id: handle
        visible: root.handleVisible
        anchors.fill: parent
        radius: 8
        color: palette.highlight
        border.color: Qt.alpha(palette.highlightedText, 0.45)
        border.width: 1

        Column {
            anchors.centerIn: parent
            spacing: 6

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.handleArrow()
                color: palette.highlightedText
                font.pixelSize: 11
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: shelfModel.count > 99 ? "99+" : shelfModel.count
                color: palette.highlightedText
                font.bold: true
                font.pixelSize: 10
                rotation: 90
            }
        }

        TapHandler { onTapped: drawerController.open() }
    }

    Rectangle {
        id: panel
        visible: root.surfaceExpanded
        opacity: root.panelProgress
        anchors.fill: parent
        radius: 16
        color: root.panelColor
        border.color: Qt.alpha(palette.text, 0.16)
        border.width: 1
        transform: Translate {
            x: root.panelOffsetX()
            y: root.panelOffsetY()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                spacing: 7

                Image {
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    source: "../assets/io.github.pasteshelf.PasteShelf.png"
                    sourceSize: Qt.size(56, 56)
                    fillMode: Image.PreserveAspectFit
                }

                Label {
                    text: "PasteShelf"
                    font.pixelSize: 17
                    font.bold: true
                    Layout.fillWidth: true
                }

                Rectangle {
                    Layout.preferredWidth: Math.max(22, countLabel.implicitWidth + 10)
                    Layout.preferredHeight: 22
                    visible: shelfModel.count > 0
                    radius: 11
                    color: Qt.alpha(palette.highlight, 0.13)

                    Label {
                        id: countLabel
                        anchors.centerIn: parent
                        text: shelfModel.count
                        color: palette.highlight
                        font.bold: true
                        font.pixelSize: 10
                    }
                }

                CompactAction {
                    text: "+"
                    font.pixelSize: 18
                    Accessible.name: "Add clipboard to PasteShelf"
                    ToolTip.visible: hovered
                    ToolTip.text: "Add clipboard (Meta+Shift+V)"
                    onClicked: desktopController.captureClipboard()
                }
                CompactAction {
                    text: drawerController.pinned ? "●" : "○"
                    active: drawerController.pinned
                    font.pixelSize: 14
                    Accessible.name: drawerController.pinned
                        ? "Unpin PasteShelf"
                        : "Keep PasteShelf open"
                    ToolTip.visible: hovered
                    ToolTip.text: drawerController.pinned ? "Unpin drawer" : "Keep drawer open"
                    onClicked: drawerController.togglePinned()
                }
                CompactAction {
                    text: "⋮"
                    font.pixelSize: 18
                    Accessible.name: "PasteShelf menu"
                    ToolTip.visible: hovered
                    ToolTip.text: "PasteShelf menu"
                    onClicked: appMenu.open()
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Qt.alpha(palette.text, 0.10)
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: shelfModel.count === 0 && !root.externalDragActive

                Column {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 32, 260)
                    spacing: 8

                    Image {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 58
                        height: 58
                        source: "../assets/io.github.pasteshelf.PasteShelf.png"
                        sourceSize: Qt.size(116, 116)
                        fillMode: Image.PreserveAspectFit
                        opacity: 0.72
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "Your shelf is empty"
                        font.bold: true
                        font.pixelSize: 14
                    }
                    Label {
                        width: parent.width
                        text: "Drag files, text, links, or screenshots onto the edge handle."
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        color: root.mutedColor
                        font.pixelSize: 11
                    }
                }
            }

            ListView {
                id: shelfList
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: shelfModel.count > 0 || root.externalDragActive
                clip: true
                spacing: 7
                model: shelfModel
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                add: Transition {
                    ParallelAnimation {
                        NumberAnimation {
                            property: "opacity"
                            from: 0
                            to: 1
                            duration: 170
                        }
                        NumberAnimation {
                            property: "scale"
                            from: 0.72
                            to: 1
                            duration: 210
                            easing.type: Easing.OutBack
                        }
                    }
                }

                remove: Transition {
                    ParallelAnimation {
                        NumberAnimation {
                            property: "opacity"
                            to: 0
                            duration: 150
                        }
                        NumberAnimation {
                            property: "scale"
                            to: 0.62
                            duration: 175
                            easing.type: Easing.InBack
                        }
                    }
                }

                displaced: Transition {
                    NumberAnimation {
                        properties: "x,y"
                        duration: 180
                        easing.type: Easing.OutCubic
                    }
                }

                delegate: Rectangle {
                    id: card
                    objectName: "shelfCard"
                    required property string groupId
                    required property string title
                    required property string subtitle
                    required property string kind
                    required property int itemCount
                    required property string previewUrl
                    required property bool missing
                    required property bool expanded
                    required property var items
                    required property string detailText
                    required property bool pinned
                    property bool textPreviewExpanded: false
                    property bool hoverPreviewRequested: cardHover.hovered
                    property real poofProgress: 0
                    readonly property bool isDragSource:
                        shelfModel.dragActive
                        && shelfModel.draggingGroupId === card.groupId
                        && shelfModel.draggingItemId === ""

                    width: ListView.view.width
                    height: cardColumn.implicitHeight + 18
                    opacity: (isDragSource ? 0.42 : 1) * (1 - poofProgress)
                    scale: (isDragSource ? 0.96 : 1) * (1 - 0.38 * poofProgress)
                    radius: 11
                    color: cardHover.hovered ? root.cardHoverColor : root.cardColor
                    border.color: missing
                        ? Qt.alpha("#e8a23a", 0.8)
                        : card.pinned
                            ? Qt.alpha(palette.highlight, 0.42)
                            : Qt.alpha(palette.text, 0.11)

                    Behavior on opacity { NumberAnimation { duration: 90 } }
                    Behavior on scale {
                        NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                    }
                    Behavior on color { ColorAnimation { duration: 100 } }
                    NumberAnimation {
                        id: cardPoofAnimation
                        target: card
                        property: "poofProgress"
                        to: 1
                        duration: 175
                        easing.type: Easing.InBack
                    }
                    Connections {
                        target: shelfModel
                        function onGroupPoofRequested(groupId) {
                            if (groupId === card.groupId)
                                cardPoofAnimation.restart()
                        }
                    }
                    HoverHandler {
                        id: cardHover
                        objectName: "cardHoverHandler"
                    }

                    ToolTip {
                        id: cardHoverPreview
                        objectName: "cardHoverPreview"
                        parent: card
                        visible: card.hoverPreviewRequested
                            && !card.textPreviewExpanded
                            && !shelfModel.dragActive
                            && !root.externalDragActive
                        delay: 425
                        timeout: -1
                        width: Math.min(330, card.width - 12)
                        x: (card.width - width) / 2
                        y: {
                            const cardPosition = card.mapToItem(root, 0, 0)
                            const below = card.height + 7
                            if (cardPosition.y + below + implicitHeight
                                    <= root.height - 12) {
                                return below
                            }
                            return -implicitHeight - 7
                        }
                        padding: 10

                        background: Rectangle {
                            radius: 10
                            color: Qt.alpha(palette.window, 0.99)
                            border.color: Qt.alpha(palette.text, 0.28)
                            border.width: 1
                        }

                        contentItem: ColumnLayout {
                            spacing: 7

                            Image {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 150
                                visible: card.kind === "image"
                                source: card.previewUrl
                                sourceSize: Qt.size(320, 220)
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Image {
                                    Layout.preferredWidth: 38
                                    Layout.preferredHeight: 38
                                    visible: card.kind !== "image"
                                        && card.kind !== "text"
                                        && card.kind !== "html"
                                    source: card.previewUrl
                                    sourceSize: Qt.size(72, 72)
                                    fillMode: Image.PreserveAspectFit
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Label {
                                        Layout.fillWidth: true
                                        text: card.title
                                        wrapMode: Text.Wrap
                                        maximumLineCount: 2
                                        elide: Text.ElideRight
                                        font.bold: true
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: card.subtitle
                                        color: root.mutedColor
                                        font.pixelSize: 10
                                    }
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: card.detailText.length > 0
                                text: card.detailText.length > 1200
                                    ? card.detailText.substring(0, 1199) + "…"
                                    : card.detailText
                                wrapMode: Text.Wrap
                                maximumLineCount: 11
                                elide: Text.ElideRight
                                color: palette.text
                            }
                        }
                    }

                    ColumnLayout {
                        id: cardColumn
                        z: 2
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 8
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 7

                            Rectangle {
                                Layout.preferredWidth: 42
                                Layout.preferredHeight: 40
                                visible: card.kind !== "text" && card.kind !== "html"
                                radius: 8
                                color: Qt.alpha(palette.window, 0.66)
                                clip: true

                                Image {
                                    anchors.fill: parent
                                    anchors.margins: 4
                                    source: card.previewUrl
                                    sourceSize: Qt.size(84, 64)
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    Layout.fillWidth: true
                                    text: card.title
                                    elide: Text.ElideRight
                                    font.bold: true
                                    font.pixelSize: 12
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: card.subtitle + (card.detailText.length > 0
                                        ? (card.textPreviewExpanded
                                            ? " · click to collapse"
                                            : " · click to preview")
                                        : card.itemCount > 1
                                            ? (card.expanded
                                                ? " · click to collapse"
                                                : " · click to expand")
                                            : "")
                                    elide: Text.ElideRight
                                    color: card.missing ? "#e8a23a" : root.mutedColor
                                    font.pixelSize: 10
                                }
                            }

                            CompactAction {
                                text: card.pinned ? "★" : "☆"
                                active: card.pinned
                                font.pixelSize: 15
                                Accessible.name: card.pinned
                                    ? "Unpin " + card.title
                                    : "Keep " + card.title + " after dragging"
                                ToolTip.visible: hovered
                                ToolTip.text: card.pinned
                                    ? "Pinned: keep after dragging out"
                                    : "Keep this card after dragging out"
                                onClicked: shelfModel.togglePinned(card.groupId)
                            }
                            CompactAction {
                                text: "×"
                                destructive: true
                                font.pixelSize: 16
                                Accessible.name: "Remove " + card.title
                                ToolTip.visible: hovered
                                ToolTip.text: "Remove"
                                onClicked: shelfModel.animateRemoveGroup(card.groupId)
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: card.detailText.length > 0
                                && card.textPreviewExpanded
                            spacing: 0

                            Rectangle {
                                id: textPreviewFrame
                                objectName: "textPreviewFrame"
                                property real previewHeight: Math.min(
                                    240, Math.max(56, previewText.contentHeight + 18))
                                Layout.fillWidth: true
                                Layout.preferredHeight: previewHeight
                                radius: 7
                                color: Qt.alpha(palette.window, 0.58)
                                border.color: Qt.alpha(palette.text, 0.10)
                                clip: true

                                Behavior on previewHeight {
                                    NumberAnimation {
                                        duration: 150
                                        easing.type: Easing.OutCubic
                                    }
                                }

                                ScrollView {
                                    id: textPreviewScroll
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    clip: true
                                    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                                    ScrollBar.vertical.policy: ScrollBar.AsNeeded

                                    TextArea {
                                        id: previewText
                                        width: textPreviewScroll.availableWidth
                                        text: card.detailText
                                        readOnly: true
                                        selectByMouse: true
                                        wrapMode: TextEdit.Wrap
                                        padding: 8
                                        color: palette.text
                                        background: null
                                        Accessible.name: "Contents of " + card.title
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: card.expanded && card.itemCount > 1
                            spacing: 4

                            Repeater {
                                model: card.items

                                delegate: Rectangle {
                                    id: member
                                    required property var modelData
                                    property real poofProgress: 0
                                    readonly property bool isDragSource:
                                        shelfModel.dragActive
                                        && shelfModel.draggingGroupId === card.groupId
                                        && shelfModel.draggingItemId === member.modelData.itemId
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 38
                                    opacity: (isDragSource ? 0.42 : 1) * (1 - poofProgress)
                                    scale: (isDragSource ? 0.96 : 1) * (1 - 0.30 * poofProgress)
                                    radius: 6
                                    color: Qt.alpha(palette.window, 0.55)

                                    Behavior on opacity { NumberAnimation { duration: 90 } }
                                    Behavior on scale {
                                        NumberAnimation {
                                            duration: 110
                                            easing.type: Easing.OutCubic
                                        }
                                    }
                                    NumberAnimation {
                                        id: memberPoofAnimation
                                        target: member
                                        property: "poofProgress"
                                        to: 1
                                        duration: 175
                                        easing.type: Easing.InBack
                                    }
                                    Connections {
                                        target: shelfModel
                                        function onItemPoofRequested(groupId, itemId) {
                                            if (groupId === card.groupId
                                                    && itemId === member.modelData.itemId) {
                                                memberPoofAnimation.restart()
                                            }
                                        }
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 7
                                        anchors.rightMargin: 7
                                        spacing: 7

                                        Image {
                                            Layout.preferredWidth: 26
                                            Layout.preferredHeight: 26
                                            source: member.modelData.previewUrl
                                            sourceSize: Qt.size(48, 48)
                                            fillMode: Image.PreserveAspectFit
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: member.modelData.title
                                            elide: Text.ElideMiddle
                                            color: member.modelData.missing
                                                ? "#e8a23a"
                                                : palette.text
                                        }
                                    }

                                    DragArea {
                                        anchors.fill: parent
                                        groupId: card.groupId
                                        itemId: member.modelData.itemId
                                    }
                                }
                            }
                        }
                    }

                    DragArea {
                        z: 1
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.rightMargin: 64
                        anchors.top: parent.top
                        height: 56
                        groupId: card.groupId
                        itemId: ""
                        onClickedWithoutDrag: {
                            if (card.detailText.length > 0)
                                card.textPreviewExpanded = !card.textPreviewExpanded
                            else if (card.itemCount > 1)
                                shelfModel.toggleExpanded(card.groupId)
                        }
                    }
                }
            }

            Rectangle {
                id: dropPreview
                objectName: "dropPreview"
                property real animatedHeight: root.externalDragActive ? 72 : 0
                Layout.fillWidth: true
                Layout.preferredHeight: animatedHeight
                visible: opacity > 0.01
                radius: 10
                color: Qt.alpha(palette.highlight, 0.20)
                border.color: palette.highlight
                border.width: 2
                opacity: root.externalDragActive ? 1 : 0

                Behavior on animatedHeight {
                    NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                }
                Behavior on opacity {
                    NumberAnimation { duration: 110 }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 10

                    Label {
                        text: root.dropPreviewGlyph
                        color: palette.highlight
                        font.pixelSize: 26
                        font.bold: true
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            text: "ADD AS NEXT CARD"
                            color: palette.highlight
                            font.pixelSize: 9
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.dropPreviewTitle
                            elide: Text.ElideRight
                            font.bold: true
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.dropPreviewDetail
                            elide: Text.ElideRight
                            color: root.mutedColor
                            font.pixelSize: 10
                        }
                    }
                }
            }

        }
    }

    component CompactAction: ToolButton {
        id: compactAction
        property bool active: false
        property bool destructive: false

        implicitWidth: 28
        implicitHeight: 28
        padding: 0
        hoverEnabled: true

        contentItem: Label {
            text: compactAction.text
            font: compactAction.font
            color: compactAction.destructive && compactAction.hovered
                ? "#d95c5c"
                : compactAction.active
                    ? palette.highlight
                    : palette.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: 7
            color: compactAction.down
                ? Qt.alpha(palette.highlight, 0.22)
                : compactAction.hovered
                    ? Qt.alpha(palette.text, 0.09)
                    : compactAction.active
                        ? Qt.alpha(palette.highlight, 0.10)
                        : "transparent"
            border.color: compactAction.active
                ? Qt.alpha(palette.highlight, 0.24)
                : "transparent"
            border.width: 1
        }
    }

    component DragArea: MouseArea {
        id: dragArea
        required property string groupId
        required property string itemId
        property point pressPoint
        property bool started: false
        signal clickedWithoutDrag()

        acceptedButtons: Qt.LeftButton
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        preventStealing: true

        onPressed: function(mouse) {
            pressPoint = Qt.point(mouse.x, mouse.y)
            started = false
        }
        onPositionChanged: function(mouse) {
            if (!pressed || started)
                return
            const distance = Math.abs(mouse.x - pressPoint.x)
                           + Math.abs(mouse.y - pressPoint.y)
            if (distance >= 8) {
                started = true
                shelfModel.beginDrag(groupId, itemId)
            }
        }
        onReleased: function(mouse) {
            if (!started)
                clickedWithoutDrag()
            started = false
        }
        onCanceled: started = false
    }

    Menu {
        id: appMenu
        onOpened: drawerController.setMenuActive(true)
        onClosed: drawerController.setMenuActive(false)

        MenuItem {
            text: "Add clipboard    Meta+Shift+V"
            onTriggered: desktopController.captureClipboard()
        }
        MenuSeparator {}
        MenuItem {
            text: "Clear all…"
            enabled: shelfModel.count > 0
            onTriggered: shelfModel.requestClearConfirmation()
        }
        MenuSeparator {}
        MenuItem {
            text: "Position and monitor…"
            onTriggered: settingsDialog.open()
        }
        MenuSeparator {}
        MenuItem {
            text: "Launch at login"
            checkable: true
            checked: desktopController.autostartEnabled
            onTriggered: desktopController.setAutostartEnabled(checked)
        }
        MenuSeparator {}
        MenuItem {
            text: "Quit PasteShelf"
            onTriggered: desktopController.quit()
        }
    }

    Dialog {
        id: settingsDialog
        title: "Drawer position"
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Close
        onOpened: drawerController.setMenuActive(true)
        onClosed: drawerController.setMenuActive(false)

        ColumnLayout {
            width: 300
            spacing: 10

            Label { text: "Position"; font.bold: true }
            ComboBox {
                id: placementCombo
                Layout.fillWidth: true
                model: positionController.placementLabels
                currentIndex: positionController.currentPlacementIndex
                onActivated: function(index) {
                    positionController.setPlacementIndex(index)
                }
            }
            Label { text: "Monitor"; font.bold: true }
            ComboBox {
                id: monitorCombo
                Layout.fillWidth: true
                model: positionController.screenLabels
                currentIndex: positionController.currentScreenIndex
                onActivated: function(index) {
                    positionController.setScreenIndex(index)
                }
            }
            Label {
                Layout.fillWidth: true
                text: "Changes are saved automatically."
                color: root.mutedColor
                font.pixelSize: 11
            }
        }
    }

    Dialog {
        id: clearDialog
        title: "Clear PasteShelf?"
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: drawerController.setMenuActive(true)
        onClosed: drawerController.setMenuActive(false)
        onAccepted: shelfModel.clearAll()

        Label {
            width: 260
            text: "This removes every card and owned snippet. Original referenced files will not be deleted."
            wrapMode: Text.WordWrap
        }
    }

    Rectangle {
        id: toast
        property string message: ""
        visible: opacity > 0
        opacity: 0
        z: 100
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 16
        width: Math.min(parent.width - 24, toastLabel.implicitWidth + 28)
        height: toastLabel.implicitHeight + 18
        radius: 9
        color: Qt.alpha(palette.text, 0.92)

        Label {
            id: toastLabel
            anchors.centerIn: parent
            width: parent.width - 18
            text: toast.message
            color: palette.base
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        Behavior on opacity { NumberAnimation { duration: 140 } }
        Timer {
            id: toastTimer
            interval: 2400
            onTriggered: toast.opacity = 0
        }
    }

    Connections {
        target: shelfModel

        function onNotification(message) {
            drawerController.open()
            toast.message = message
            toast.opacity = 1
            toastTimer.restart()
        }
        function onShowRequested() {
            drawerController.open()
            root.raise()
        }
        function onClearConfirmationRequested() {
            drawerController.open()
            clearDialog.open()
        }
    }
}
