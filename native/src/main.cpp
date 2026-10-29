#include <QApplication>
#include <QBoxLayout>
#include <QBuffer>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QParallelAnimationGroup>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVariantAnimation>
#include <QWidget>

#include <algorithm>
#include <cerrno>
#include <functional>
#include <memory>
#include <signal.h>
#include <unistd.h>

namespace {

constexpr auto kInternalMime = "application/x-pasteshelf-native-item";
constexpr auto kDesktopId = "io.github.pasteshelf.PasteShelfNative";

QString compactText(const QString &text, int maximum = 72)
{
    QString result = text.simplified();
    if (result.size() > maximum)
        result = result.left(maximum - 1).trimmed() + QStringLiteral("…");
    return result;
}

QString humanKind(const QString &kind)
{
    if (kind == QLatin1String("file")) return QStringLiteral("File");
    if (kind == QLatin1String("folder")) return QStringLiteral("Folder");
    if (kind == QLatin1String("url")) return QStringLiteral("Link");
    if (kind == QLatin1String("image")) return QStringLiteral("Image");
    if (kind == QLatin1String("html")) return QStringLiteral("Rich text");
    return QStringLiteral("Text");
}

struct ShelfItem {
    QString id;
    QString kind;
    QString title;
    QString value;
    QString plainText;
    QString html;
    bool owned = false;

    QJsonObject toJson() const
    {
        return {
            {QStringLiteral("id"), id},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("title"), title},
            {QStringLiteral("value"), value},
            {QStringLiteral("plainText"), plainText},
            {QStringLiteral("html"), html},
            {QStringLiteral("owned"), owned},
        };
    }

    static ShelfItem fromJson(const QJsonObject &object)
    {
        return {
            object.value(QStringLiteral("id")).toString(),
            object.value(QStringLiteral("kind")).toString(),
            object.value(QStringLiteral("title")).toString(),
            object.value(QStringLiteral("value")).toString(),
            object.value(QStringLiteral("plainText")).toString(),
            object.value(QStringLiteral("html")).toString(),
            object.value(QStringLiteral("owned")).toBool(),
        };
    }
};

struct ShelfGroup {
    QString id;
    QString title;
    bool pinned = false;
    QList<ShelfItem> items;

    QJsonObject toJson() const
    {
        QJsonArray array;
        for (const auto &item : items)
            array.append(item.toJson());
        return {
            {QStringLiteral("id"), id},
            {QStringLiteral("title"), title},
            {QStringLiteral("pinned"), pinned},
            {QStringLiteral("items"), array},
        };
    }

    static ShelfGroup fromJson(const QJsonObject &object)
    {
        ShelfGroup group;
        group.id = object.value(QStringLiteral("id")).toString();
        group.title = object.value(QStringLiteral("title")).toString();
        group.pinned = object.value(QStringLiteral("pinned")).toBool();
        const auto array = object.value(QStringLiteral("items")).toArray();
        for (const auto &value : array)
            group.items.append(ShelfItem::fromJson(value.toObject()));
        return group;
    }
};

class ShelfStore {
public:
    explicit ShelfStore(QString root)
        : m_root(std::move(root)),
          m_content(m_root + QStringLiteral("/content")),
          m_statePath(m_root + QStringLiteral("/state.json"))
    {
        QDir().mkpath(m_content);
        load();
    }

    const QList<ShelfGroup> &groups() const { return m_groups; }

    const ShelfGroup *group(const QString &id) const
    {
        const auto it = std::find_if(m_groups.cbegin(), m_groups.cend(),
            [&](const ShelfGroup &entry) { return entry.id == id; });
        return it == m_groups.cend() ? nullptr : &*it;
    }

    static bool canImport(const QMimeData *mime)
    {
        return mime && !mime->hasFormat(kInternalMime)
            && (mime->hasUrls() || mime->hasImage() || mime->hasHtml() || mime->hasText());
    }

    static QString internalWholeGroupId(const QMimeData *mime)
    {
        if (!mime || !mime->hasFormat(kInternalMime))
            return {};
        const QString payload = QString::fromUtf8(mime->data(kInternalMime));
        const qsizetype separator = payload.indexOf(QLatin1Char('/'));
        // A whole-card drag is encoded as "group-id/". Member drags have an
        // item id after the slash and must not reorder (or clone) the card.
        if (separator <= 0 || separator != payload.size() - 1)
            return {};
        return payload.left(separator);
    }

    bool add(const QMimeData *mime)
    {
        if (!canImport(mime))
            return false;

        ShelfGroup group;
        group.id = QUuid::createUuid().toString(QUuid::WithoutBraces);

        if (mime->hasUrls()) {
            for (const auto &url : mime->urls()) {
                ShelfItem item;
                item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                if (url.isLocalFile()) {
                    const QFileInfo info(url.toLocalFile());
                    item.kind = info.isDir() ? QStringLiteral("folder") : QStringLiteral("file");
                    item.title = info.fileName().isEmpty() ? info.absoluteFilePath() : info.fileName();
                    item.value = info.absoluteFilePath();
                } else {
                    item.kind = QStringLiteral("url");
                    item.title = url.fileName().isEmpty() ? url.host() : url.fileName();
                    if (item.title.isEmpty()) item.title = compactText(url.toDisplayString());
                    item.value = url.toString();
                }
                group.items.append(item);
            }
        } else if (mime->hasImage()) {
            QImage image = qvariant_cast<QImage>(mime->imageData());
            if (image.isNull()) {
                const QPixmap pixmap = qvariant_cast<QPixmap>(mime->imageData());
                image = pixmap.toImage();
            }
            if (image.isNull())
                return false;
            const QString fileName = group.id + QStringLiteral(".png");
            const QString path = m_content + QLatin1Char('/') + fileName;
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return false;
            if (!image.save(&file, "PNG") || !file.commit())
                return false;
            ShelfItem item;
            item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            item.kind = QStringLiteral("image");
            item.title = QStringLiteral("Screenshot %1 × %2").arg(image.width()).arg(image.height());
            item.value = path;
            item.owned = true;
            group.items.append(item);
        } else {
            ShelfItem item;
            item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            item.kind = mime->hasHtml() ? QStringLiteral("html") : QStringLiteral("text");
            item.plainText = mime->text();
            item.html = mime->hasHtml() ? mime->html() : QString();
            item.title = compactText(item.plainText.isEmpty() ? QStringLiteral("Rich text") : item.plainText);
            group.items.append(item);
        }

        if (group.items.isEmpty())
            return false;
        group.title = group.items.size() == 1
            ? group.items.first().title
            : QStringLiteral("%1 items").arg(group.items.size());
        m_groups.prepend(group);
        return save();
    }

    void setPinned(const QString &id, bool pinned)
    {
        for (auto &group : m_groups) {
            if (group.id == id) {
                group.pinned = pinned;
                save();
                return;
            }
        }
    }

    bool reorderGroup(const QString &id, int destinationIndex)
    {
        const auto original = m_groups;
        qsizetype sourceIndex = -1;
        for (qsizetype index = 0; index < m_groups.size(); ++index) {
            if (m_groups[index].id == id) {
                sourceIndex = index;
                break;
            }
        }
        if (sourceIndex < 0)
            return false;

        ShelfGroup moving = m_groups.takeAt(sourceIndex);
        const qsizetype boundedDestination = std::clamp<qsizetype>(
            destinationIndex, 0, m_groups.size());
        m_groups.insert(boundedDestination, std::move(moving));
        if (sourceIndex == boundedDestination)
            return true;
        if (save())
            return true;
        m_groups = original;
        return false;
    }

    bool remove(const QString &id)
    {
        for (qsizetype index = 0; index < m_groups.size(); ++index) {
            if (m_groups[index].id != id)
                continue;
            removeOwned(m_groups[index]);
            m_groups.removeAt(index);
            return save();
        }
        return false;
    }

    bool removeItem(const QString &groupId, const QString &itemId)
    {
        for (qsizetype groupIndex = 0; groupIndex < m_groups.size(); ++groupIndex) {
            auto &group = m_groups[groupIndex];
            if (group.id != groupId)
                continue;
            for (qsizetype itemIndex = 0; itemIndex < group.items.size(); ++itemIndex) {
                if (group.items[itemIndex].id != itemId)
                    continue;
                if (group.items.size() == 1)
                    return remove(groupId);
                const auto removed = group.items[itemIndex];
                if (removed.owned && !removed.value.isEmpty())
                    QFile::remove(removed.value);
                group.items.removeAt(itemIndex);
                group.title = QStringLiteral("%1 items").arg(group.items.size());
                return save();
            }
        }
        return false;
    }

    void clear()
    {
        for (const auto &group : std::as_const(m_groups))
            removeOwned(group);
        m_groups.clear();
        save();
    }

    std::unique_ptr<QMimeData> mimeFor(const QString &id, const QString &itemId = {}) const
    {
        const ShelfGroup *entry = group(id);
        if (!entry)
            return {};
        auto mime = std::make_unique<QMimeData>();
        mime->setData(kInternalMime, (id + QLatin1Char('/') + itemId).toUtf8());

        QList<QUrl> urls;
        for (const auto &item : entry->items) {
            if (!itemId.isEmpty() && item.id != itemId)
                continue;
            if (item.kind == QLatin1String("file") || item.kind == QLatin1String("folder"))
                urls.append(QUrl::fromLocalFile(item.value));
            else if (item.kind == QLatin1String("url"))
                urls.append(QUrl(item.value));
            else if (item.kind == QLatin1String("image")) {
                QImageReader reader(item.value);
                const QImage image = reader.read();
                if (!image.isNull()) mime->setImageData(image);
                urls.append(QUrl::fromLocalFile(item.value));
            } else {
                if (!item.plainText.isEmpty()) mime->setText(item.plainText);
                if (!item.html.isEmpty()) mime->setHtml(item.html);
            }
        }
        if (!urls.isEmpty()) mime->setUrls(urls);
        return mime;
    }

    QPixmap preview(const ShelfGroup &group, const QSize &size) const
    {
        if (group.items.isEmpty())
            return {};
        return previewItem(group.items.first(), size);
    }

    QPixmap previewItem(const ShelfItem &item, const QSize &size) const
    {
        if (item.kind == QLatin1String("image")) {
            QImageReader reader(item.value);
            QSize source = reader.size();
            if (source.isValid()) {
                source.scale(size * qApp->devicePixelRatio(), Qt::KeepAspectRatio);
                reader.setScaledSize(source);
            }
            const QImage image = reader.read();
            if (!image.isNull())
                return QPixmap::fromImage(image).scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }

        QFileIconProvider provider;
        QIcon icon;
        if ((item.kind == QLatin1String("file") || item.kind == QLatin1String("folder"))
            && QFileInfo::exists(item.value)) {
            icon = provider.icon(QFileInfo(item.value));
        } else {
            const QString themeName = item.kind == QLatin1String("url") ? QStringLiteral("internet-web-browser")
                : item.kind == QLatin1String("html") ? QStringLiteral("text-html")
                : item.kind == QLatin1String("text") ? QStringLiteral("text-x-generic")
                : QStringLiteral("unknown");
            icon = QIcon::fromTheme(themeName);
        }
        return icon.pixmap(size);
    }

private:
    void load()
    {
        QFile file(m_statePath);
        if (!file.open(QIODevice::ReadOnly))
            return;
        const auto document = QJsonDocument::fromJson(file.readAll());
        for (const auto &value : document.object().value(QStringLiteral("groups")).toArray()) {
            auto group = ShelfGroup::fromJson(value.toObject());
            if (!group.id.isEmpty() && !group.items.isEmpty())
                m_groups.append(std::move(group));
        }
    }

    bool save() const
    {
        QJsonArray array;
        for (const auto &group : m_groups)
            array.append(group.toJson());
        QSaveFile file(m_statePath);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                             {QStringLiteral("groups"), array}}).toJson(QJsonDocument::Compact));
        return file.commit();
    }

    static void removeOwned(const ShelfGroup &group)
    {
        for (const auto &item : group.items) {
            if (item.owned && !item.value.isEmpty())
                QFile::remove(item.value);
        }
    }

    QString m_root;
    QString m_content;
    QString m_statePath;
    QList<ShelfGroup> m_groups;
};

bool isLocalItem(const ShelfItem &item)
{
    return item.kind == QLatin1String("file") || item.kind == QLatin1String("folder")
        || item.kind == QLatin1String("image");
}

void goToSource(const ShelfItem &item)
{
    if (item.kind == QLatin1String("url")) {
        QDesktopServices::openUrl(QUrl(item.value));
        return;
    }
    if (!isLocalItem(item) || item.value.isEmpty())
        return;
    const QFileInfo info(item.value);
    if (!info.exists())
        return;
    if (info.isDir()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()));
        return;
    }
    const QString dolphin = QStandardPaths::findExecutable(QStringLiteral("dolphin"));
    if (!dolphin.isEmpty()
        && QProcess::startDetached(dolphin, {QStringLiteral("--select"), info.absoluteFilePath()}))
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
}

using GlobalRectResolver = std::function<QRect(const QWidget *)>;

class HoverPreview final : public QFrame {
    Q_OBJECT
public:
    HoverPreview(const ShelfGroup &group, ShelfStore *store, QWidget *parent = nullptr)
        : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
    {
        setObjectName(QStringLiteral("previewPopup"));
        setAttribute(Qt::WA_ShowWithoutActivating);
        setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
        setFocusPolicy(Qt::NoFocus);
        setFixedWidth(410);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(15, 13, 15, 14);
        layout->setSpacing(8);

        auto *title = new QLabel(group.items.size() > 1
            ? QStringLiteral("%1 items").arg(group.items.size()) : group.title, this);
        title->setObjectName(QStringLiteral("previewTitle"));
        title->setWordWrap(true);
        layout->addWidget(title);

        if (group.items.size() == 1 && group.items.first().kind == QLatin1String("image")) {
            auto *image = new QLabel(this);
            image->setAlignment(Qt::AlignCenter);
            image->setPixmap(store->preview(group, QSize(370, 190)));
            image->setMaximumHeight(190);
            layout->addWidget(image);
        }

        QString content;
        if (group.items.size() == 1) {
            const auto &item = group.items.first();
            if (item.kind == QLatin1String("text") || item.kind == QLatin1String("html"))
                content = item.plainText;
            else
                content = item.value;
        } else {
            QStringList lines;
            for (const auto &item : group.items)
                lines << QStringLiteral("• %1\n  %2").arg(item.title, item.value);
            content = lines.join(QStringLiteral("\n\n"));
        }
        if (content.isEmpty())
            content = QStringLiteral("No additional preview is available.");

        auto *scroll = new QScrollArea(this);
        scroll->setObjectName(QStringLiteral("previewScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *body = new QLabel(content, scroll);
        body->setObjectName(QStringLiteral("previewBody"));
        body->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        body->setContentsMargins(1, 1, 8, 1);
        body->setMinimumWidth(360);
        body->setMaximumWidth(360);
        scroll->setWidget(body);
        scroll->setFixedHeight(std::clamp(body->sizeHint().height() + 8, 48, 240));
        layout->addWidget(scroll);
        adjustSize();
    }

    void showBeside(const QRect &anchorRect)
    {
        adjustSize();
        QScreen *screen = QGuiApplication::screenAt(anchorRect.center());
        const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
        int x = anchorRect.left() - width() - 10;
        if (x < available.left())
            x = anchorRect.right() + 10;
        x = std::clamp(x, available.left() + 8, available.right() - width() - 8);
        int y = anchorRect.top();
        y = std::clamp(y, available.top() + 8, available.bottom() - height() - 8);
        m_authoritativeGeometry = QRect(x, y, width(), height());
        setWindowTitle(QStringLiteral("PasteShelfPreview@%1,%2,%3,%4")
            .arg(x).arg(y).arg(width()).arg(height()));
        move(x, y);
        show();
        raise();
    }

    QRect authoritativeGeometry() const { return m_authoritativeGeometry; }

signals:
    void pointerEntered();
    void pointerLeft();

protected:
    void enterEvent(QEnterEvent *event) override
    {
        emit pointerEntered();
        QFrame::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        emit pointerLeft();
        QFrame::leaveEvent(event);
    }

private:
    QRect m_authoritativeGeometry;
};

class MemberRow final : public QFrame {
    Q_OBJECT
public:
    MemberRow(
        QString groupId,
        ShelfItem item,
        ShelfStore *store,
        std::function<bool()> isPinned,
        QWidget *parent = nullptr)
        : QFrame(parent),
          m_groupId(std::move(groupId)),
          m_item(std::move(item)),
          m_store(store),
          m_isPinned(std::move(isPinned))
    {
        setObjectName(QStringLiteral("memberRow"));
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(42);
        setCursor(Qt::OpenHandCursor);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(7, 4, 5, 4);
        layout->setSpacing(7);
        auto *icon = new QLabel(this);
        icon->setFixedSize(26, 26);
        icon->setAlignment(Qt::AlignCenter);
        icon->setPixmap(m_store->previewItem(m_item, QSize(24, 24)));
        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(icon);
        auto *title = new QLabel(compactText(m_item.title, 42), this);
        title->setObjectName(QStringLiteral("memberTitle"));
        title->setMinimumWidth(0);
        title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(title, 1);
        if ((isLocalItem(m_item) && QFileInfo::exists(m_item.value))
            || m_item.kind == QLatin1String("url")) {
            auto *source = actionButton(QStringLiteral("folder-open"), QStringLiteral("↗"),
                m_item.kind == QLatin1String("url") ? QStringLiteral("Open link") : QStringLiteral("Go to source"));
            connect(source, &QToolButton::clicked, this, [this] { goToSource(m_item); });
            layout->addWidget(source);
        }
        auto *remove = actionButton(QStringLiteral("edit-delete"), QStringLiteral("×"), QStringLiteral("Remove this item"));
        connect(remove, &QToolButton::clicked, this, [this] { poof(); });
        layout->addWidget(remove);
    }

signals:
    void removeRequested(const QString &groupId, const QString &itemId);
    void dragActiveChanged(bool active);

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressPosition = event->position().toPoint();
            setCursor(Qt::ClosedHandCursor);
        }
        QFrame::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        setCursor(Qt::OpenHandCursor);
        QFrame::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)
            || (event->position().toPoint() - m_pressPosition).manhattanLength() < QApplication::startDragDistance()) {
            QFrame::mouseMoveEvent(event);
            return;
        }
        auto mime = m_store->mimeFor(m_groupId, m_item.id);
        if (!mime)
            return;
        auto *drag = new QDrag(this);
        drag->setMimeData(mime.release());
        drag->setPixmap(grab().scaled(240, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        emit dragActiveChanged(true);
        const auto action = drag->exec(Qt::CopyAction, Qt::CopyAction);
        emit dragActiveChanged(false);
        setCursor(Qt::OpenHandCursor);
        if (action == Qt::CopyAction && !m_isPinned())
            poof();
    }

private:
    QToolButton *actionButton(const QString &iconName, const QString &fallback, const QString &tip)
    {
        auto *button = new QToolButton(this);
        const QIcon icon = QIcon::fromTheme(iconName);
        if (icon.isNull()) button->setText(fallback); else button->setIcon(icon);
        button->setToolTip(tip);
        button->setFixedSize(24, 24);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    }

    void poof()
    {
        if (m_removing)
            return;
        m_removing = true;
        auto *effect = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(effect);
        auto *group = new QParallelAnimationGroup(this);
        auto *fade = new QPropertyAnimation(effect, "opacity", group);
        fade->setDuration(150);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        fade->setEasingCurve(QEasingCurve::InCubic);
        auto *shrink = new QPropertyAnimation(this, "maximumHeight", group);
        shrink->setDuration(170);
        shrink->setStartValue(height());
        shrink->setEndValue(0);
        shrink->setEasingCurve(QEasingCurve::InCubic);
        connect(group, &QParallelAnimationGroup::finished, this, [this] {
            emit removeRequested(m_groupId, m_item.id);
        });
        group->start(QAbstractAnimation::DeleteWhenStopped);
    }

    QString m_groupId;
    ShelfItem m_item;
    ShelfStore *m_store = nullptr;
    std::function<bool()> m_isPinned;
    QPoint m_pressPosition;
    bool m_removing = false;
};

class CardWidget final : public QFrame {
    Q_OBJECT
public:
    CardWidget(const ShelfGroup &group, ShelfStore *store, QWidget *parent = nullptr,
        GlobalRectResolver globalRectResolver = {})
        : QFrame(parent),
          m_group(group),
          m_store(store),
          m_globalRectResolver(std::move(globalRectResolver)),
          m_pinned(group.pinned)
    {
        setObjectName(QStringLiteral("card"));
        setMinimumWidth(0);
        setCursor(Qt::OpenHandCursor);
        setFrameShape(QFrame::NoFrame);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        auto *header = new QFrame(this);
        header->setObjectName(QStringLiteral("cardHeader"));
        header->setFixedHeight(66);
        auto *layout = new QHBoxLayout(header);
        layout->setContentsMargins(9, 7, 7, 7);
        layout->setSpacing(8);

        m_previewHoverZone = new QWidget(header);
        m_previewHoverZone->setObjectName(QStringLiteral("previewHoverZone"));
        m_previewHoverZone->setMinimumWidth(0);
        m_previewHoverZone->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *previewLayout = new QHBoxLayout(m_previewHoverZone);
        previewLayout->setContentsMargins(0, 0, 0, 0);
        previewLayout->setSpacing(8);

        auto *preview = new QLabel(m_previewHoverZone);
        preview->setFixedSize(40, 40);
        preview->setAlignment(Qt::AlignCenter);
        preview->setPixmap(store->preview(group, QSize(38, 38)));
        previewLayout->addWidget(preview);

        auto *copyHost = new QWidget(m_previewHoverZone);
        copyHost->setMinimumWidth(0);
        copyHost->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *copy = new QVBoxLayout(copyHost);
        copy->setContentsMargins(0, 0, 0, 0);
        copy->setSpacing(1);
        auto *titleRow = new QHBoxLayout;
        titleRow->setSpacing(5);
        auto *title = new QLabel(compactText(group.title, 42), copyHost);
        title->setObjectName(QStringLiteral("cardTitle"));
        // Labels report their full, unelided text as a minimum width by default.
        // Let the copy column yield instead so the action buttons can never be
        // pushed outside the compact card by a long clipping or filename.
        title->setMinimumWidth(0);
        title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        titleRow->addWidget(title, 1);
        if (group.items.size() > 1) {
            auto *count = new QLabel(QString::number(group.items.size()), copyHost);
            count->setObjectName(QStringLiteral("countBadge"));
            count->setAlignment(Qt::AlignCenter);
            count->setFixedSize(22, 20);
            titleRow->addWidget(count);
        }
        copy->addLayout(titleRow);

        QString detail;
        if (group.items.size() > 1) {
            detail = QStringLiteral("Stack · click to see all items");
        } else {
            const auto &item = group.items.first();
            if (item.kind == QLatin1String("text") || item.kind == QLatin1String("html"))
                detail = compactText(item.plainText, 68);
            else {
                detail = humanKind(item.kind);
                if (isLocalItem(item) && !QFileInfo::exists(item.value))
                    detail += QStringLiteral(" · Missing source");
            }
        }
        auto *subtitle = new QLabel(detail, copyHost);
        subtitle->setObjectName(QStringLiteral("cardDetail"));
        subtitle->setMinimumWidth(0);
        subtitle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        subtitle->setMaximumHeight(18);
        copy->addWidget(subtitle);
        previewLayout->addWidget(copyHost, 1);
        layout->addWidget(m_previewHoverZone, 1);

        m_actionStrip = new QWidget(header);
        m_actionStrip->setObjectName(QStringLiteral("cardActionStrip"));
        auto *actionLayout = new QHBoxLayout(m_actionStrip);
        actionLayout->setContentsMargins(0, 0, 0, 0);
        actionLayout->setSpacing(4);

        const auto &firstItem = group.items.first();
        if ((isLocalItem(firstItem) && QFileInfo::exists(firstItem.value))
            || firstItem.kind == QLatin1String("url")) {
            auto *source = actionButton(m_actionStrip, QStringLiteral("folder-open"), QStringLiteral("↗"),
                firstItem.kind == QLatin1String("url") ? QStringLiteral("Open link") : QStringLiteral("Go to source"));
            source->setObjectName(QStringLiteral("sourceButton"));
            connect(source, &QToolButton::clicked, this, [firstItem] { goToSource(firstItem); });
            actionLayout->addWidget(source);
        }

        m_pinButton = actionButton(m_actionStrip, QStringLiteral("window-pin"), QStringLiteral("P"), QString());
        m_pinButton->setObjectName(QStringLiteral("pinButton"));
        connect(m_pinButton, &QToolButton::clicked, this, [this] {
            m_pinned = !m_pinned;
            updatePinButton();
            emit pinChanged(m_group.id, m_pinned);
        });
        updatePinButton();
        actionLayout->addWidget(m_pinButton);

        auto *remove = actionButton(m_actionStrip, QStringLiteral("edit-delete"), QStringLiteral("×"), QStringLiteral("Remove"));
        remove->setObjectName(QStringLiteral("removeButton"));
        connect(remove, &QToolButton::clicked, this, [this] { poof(false); });
        actionLayout->addWidget(remove);
        layout->addWidget(m_actionStrip);
        root->addWidget(header);

        createDetails(root);
        createHoverPreview();
        installHoverRouting();
    }

    void showPreviewForTest()
    {
        showHoverPreview();
    }

    int itemCountForTest() const { return m_group.items.size(); }

    const QString &groupId() const { return m_group.id; }

    void acceptInternalReorder()
    {
        m_internalReorderAccepted = true;
    }

    void expandForTest()
    {
        setExpanded(true);
    }

    void savePreviewForTest(const QString &path)
    {
        if (!m_previewPopup)
            showHoverPreview();
        if (m_previewPopup) {
            m_previewPopup->grab().save(path);
            qInfo() << "PasteShelf visual test card=" << frameGeometry()
                    << "preview=" << m_previewPopup->frameGeometry();
        }
    }

    QRect previewPlacementForTest() const
    {
        return m_previewPopup ? m_previewPopup->authoritativeGeometry() : QRect();
    }

signals:
    void pinChanged(const QString &id, bool pinned);
    void removeRequested(const QString &id);
    void removeItemRequested(const QString &groupId, const QString &itemId);
    void dragActiveChanged(bool active);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && m_previewHoverZone
            && (widget == m_previewHoverZone || m_previewHoverZone->isAncestorOf(widget))) {
            if (event->type() == QEvent::Enter) {
                m_previewHoveredParts.insert(watched);
                m_hidePreviewTimer.stop();
                if (!m_expanded)
                    m_showPreviewTimer.start();
            } else if (event->type() == QEvent::Leave) {
                m_previewHoveredParts.remove(watched);
                QTimer::singleShot(0, this, [this] {
                    if (m_previewHoveredParts.isEmpty() && !m_previewPopupHovered) {
                        m_showPreviewTimer.stop();
                        m_hidePreviewTimer.start();
                    }
                });
            }
        } else if (widget && m_actionStrip
            && (widget == m_actionStrip || m_actionStrip->isAncestorOf(widget))) {
            if (event->type() == QEvent::Enter) {
                m_previewHoveredParts.clear();
                m_previewPopupHovered = false;
                m_showPreviewTimer.stop();
                m_hidePreviewTimer.stop();
                if (m_previewPopup)
                    m_previewPopup->hide();
            }
        }
        return QFrame::eventFilter(watched, event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        QFrame::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_previewHoveredParts.clear();
        m_showPreviewTimer.stop();
        if (!m_previewPopupHovered)
            m_hidePreviewTimer.start();
        QFrame::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressPosition = event->position().toPoint();
            m_clickCandidate = true;
            setCursor(Qt::ClosedHandCursor);
        }
        QFrame::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        setCursor(Qt::OpenHandCursor);
        if (event->button() == Qt::LeftButton && m_clickCandidate && m_expandable)
            setExpanded(!m_expanded);
        m_clickCandidate = false;
        QFrame::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)
            || (event->position().toPoint() - m_pressPosition).manhattanLength() < QApplication::startDragDistance()) {
            QFrame::mouseMoveEvent(event);
            return;
        }
        m_clickCandidate = false;
        m_showPreviewTimer.stop();
        if (m_previewPopup) m_previewPopup->hide();
        auto mime = m_store->mimeFor(m_group.id);
        if (!mime)
            return;
        m_internalReorderAccepted = false;
        auto *drag = new QDrag(this);
        drag->setMimeData(mime.release());
        drag->setPixmap(grab().scaled(280, 110, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        emit dragActiveChanged(true);
        const auto action = drag->exec(Qt::CopyAction, Qt::CopyAction);
        emit dragActiveChanged(false);
        setCursor(Qt::OpenHandCursor);
        if (action == Qt::CopyAction && !m_internalReorderAccepted && !m_pinned)
            poof(true);
    }

private:
    void installHoverRouting()
    {
        const auto installOnTree = [this](QWidget *root) {
            root->installEventFilter(this);
            for (QWidget *child : root->findChildren<QWidget *>())
                child->installEventFilter(this);
        };
        installOnTree(m_previewHoverZone);
        installOnTree(m_actionStrip);
    }

    QToolButton *actionButton(QWidget *parent, const QString &iconName, const QString &fallback, const QString &tip)
    {
        auto *button = new QToolButton(parent);
        const QIcon icon = QIcon::fromTheme(iconName);
        if (icon.isNull()) button->setText(fallback); else button->setIcon(icon);
        button->setToolTip(tip);
        button->setFixedSize(26, 26);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    }

    void updatePinButton()
    {
        m_pinButton->setProperty("pinned", m_pinned);
        m_pinButton->setToolTip(m_pinned
            ? QStringLiteral("Pinned — keep after dragging")
            : QStringLiteral("Pin — keep after dragging"));
        const QIcon icon = QIcon::fromTheme(m_pinned ? QStringLiteral("window-unpin") : QStringLiteral("window-pin"));
        if (!icon.isNull()) {
            m_pinButton->setIcon(icon);
            m_pinButton->setText(QString());
        } else {
            m_pinButton->setText(m_pinned ? QStringLiteral("●") : QStringLiteral("P"));
        }
        m_pinButton->style()->unpolish(m_pinButton);
        m_pinButton->style()->polish(m_pinButton);
    }

    void createDetails(QVBoxLayout *root)
    {
        m_details = new QFrame(this);
        m_details->setObjectName(QStringLiteral("cardDetails"));
        m_details->setMinimumWidth(0);
        m_details->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        auto *layout = new QVBoxLayout(m_details);
        layout->setContentsMargins(8, 0, 8, 8);
        layout->setSpacing(4);

        if (m_group.items.size() > 1) {
            for (const auto &item : m_group.items) {
                auto *row = new MemberRow(m_group.id, item, m_store, [this] { return m_pinned; }, m_details);
                connect(row, &MemberRow::removeRequested, this, &CardWidget::removeItemRequested);
                connect(row, &MemberRow::dragActiveChanged, this, &CardWidget::dragActiveChanged);
                layout->addWidget(row);
            }
            m_expandable = true;
        } else {
            const auto &item = m_group.items.first();
            if ((item.kind == QLatin1String("text") || item.kind == QLatin1String("html"))
                && item.plainText.size() > 70) {
                auto *text = new QLabel(item.plainText, m_details);
                text->setObjectName(QStringLiteral("expandedText"));
                text->setWordWrap(true);
                text->setTextInteractionFlags(Qt::TextSelectableByMouse);
                text->setMaximumHeight(180);
                layout->addWidget(text);
                m_expandable = true;
            }
        }
        m_detailsExpandedHeight = std::min(m_details->sizeHint().height(), 230);
        m_details->setVisible(false);
        m_details->setMaximumHeight(0);
        root->addWidget(m_details);
    }

    void createHoverPreview()
    {
        m_showPreviewTimer.setSingleShot(true);
        m_showPreviewTimer.setInterval(420);
        m_hidePreviewTimer.setSingleShot(true);
        m_hidePreviewTimer.setInterval(120);
        connect(&m_showPreviewTimer, &QTimer::timeout, this, [this] {
            showHoverPreview();
        });
        connect(&m_hidePreviewTimer, &QTimer::timeout, this, [this] {
            if (!m_previewPopup)
                return;
            // A newly mapped Wayland surface can momentarily make the source
            // card receive Leave before the preview receives Enter. Check the
            // actual global pointer position so that transition cannot make
            // the preview hide and reopen in a loop.
            if (pointerInPreviewContext(QCursor::pos())) {
                m_hidePreviewTimer.start();
                return;
            }
            m_previewPopup->hide();
        });
    }

    bool pointerInPreviewContext(const QPoint &globalPosition) const
    {
        const auto contains = [this, &globalPosition](const QWidget *widget) {
            if (!widget || !widget->isVisible())
                return false;
            return globalRectFor(widget).contains(globalPosition);
        };
        return contains(m_previewHoverZone)
            || (m_previewPopup && m_previewPopup->isVisible()
                && m_previewPopup->authoritativeGeometry().contains(globalPosition));
    }

    QRect globalRectFor(const QWidget *widget) const
    {
        if (m_globalRectResolver) {
            const QRect resolved = m_globalRectResolver(widget);
            if (resolved.isValid())
                return resolved;
        }
        return QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size());
    }

    void showHoverPreview()
    {
        if (!m_previewPopup) {
            m_previewPopup = new HoverPreview(m_group, m_store, this);
            connect(m_previewPopup, &HoverPreview::pointerEntered, this, [this] {
                m_previewPopupHovered = true;
                m_hidePreviewTimer.stop();
            });
            connect(m_previewPopup, &HoverPreview::pointerLeft, this, [this] {
                m_previewPopupHovered = false;
                m_hidePreviewTimer.start();
            });
        }
        m_previewPopup->showBeside(globalRectFor(this));
    }

    void setExpanded(bool expanded)
    {
        if (!m_expandable || m_expanded == expanded)
            return;
        m_expanded = expanded;
        m_showPreviewTimer.stop();
        if (m_previewPopup) m_previewPopup->hide();
        const int target = expanded ? m_detailsExpandedHeight : 0;
        if (expanded) m_details->show();
        auto *animation = new QPropertyAnimation(m_details, "maximumHeight", this);
        animation->setDuration(150);
        animation->setStartValue(m_details->maximumHeight());
        animation->setEndValue(target);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        if (!expanded) {
            connect(animation, &QPropertyAnimation::finished, m_details, &QWidget::hide);
        }
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

    void poof(bool draggedOut)
    {
        if (m_removing)
            return;
        m_removing = true;
        m_showPreviewTimer.stop();
        if (m_previewPopup) m_previewPopup->hide();
        auto *effect = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(effect);
        auto *group = new QParallelAnimationGroup(this);
        auto *fade = new QPropertyAnimation(effect, "opacity", group);
        fade->setDuration(draggedOut ? 135 : 165);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        fade->setEasingCurve(QEasingCurve::InCubic);
        auto *shrink = new QPropertyAnimation(this, "maximumHeight", group);
        shrink->setDuration(draggedOut ? 170 : 190);
        shrink->setStartValue(height());
        shrink->setEndValue(0);
        shrink->setEasingCurve(QEasingCurve::InCubic);
        connect(group, &QParallelAnimationGroup::finished, this, [this] {
            emit removeRequested(m_group.id);
        });
        group->start(QAbstractAnimation::DeleteWhenStopped);
    }

    ShelfGroup m_group;
    ShelfStore *m_store = nullptr;
    GlobalRectResolver m_globalRectResolver;
    QWidget *m_previewHoverZone = nullptr;
    QWidget *m_actionStrip = nullptr;
    QToolButton *m_pinButton = nullptr;
    QFrame *m_details = nullptr;
    int m_detailsExpandedHeight = 0;
    QPointer<HoverPreview> m_previewPopup;
    QTimer m_showPreviewTimer;
    QTimer m_hidePreviewTimer;
    QSet<QObject *> m_previewHoveredParts;
    QPoint m_pressPosition;
    bool m_pinned = false;
    bool m_expandable = false;
    bool m_expanded = false;
    bool m_clickCandidate = false;
    bool m_removing = false;
    bool m_previewPopupHovered = false;
    bool m_internalReorderAccepted = false;
};

class KWinPositioner {
public:
    void start()
    {
        if (QGuiApplication::platformName() != QLatin1String("wayland"))
            return;
        m_gdbus = QStandardPaths::findExecutable(QStringLiteral("gdbus"));
        if (m_gdbus.isEmpty())
            return;
        const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        m_scriptPath = runtime + QStringLiteral("/pasteshelf-native-positioner.js");
        QSaveFile file(m_scriptPath);
        if (!file.open(QIODevice::WriteOnly))
            return;
        file.write(scriptSource());
        if (!file.commit())
            return;
        unload();
        call(QStringLiteral("org.kde.kwin.Scripting.loadScript"), {m_scriptPath, pluginName()});
        call(QStringLiteral("org.kde.kwin.Scripting.start"), {});
        m_started = true;
    }

    ~KWinPositioner()
    {
        unload();
        if (!m_scriptPath.isEmpty()) QFile::remove(m_scriptPath);
    }

private:
    static QString pluginName() { return QStringLiteral("pasteshelf-native-positioner"); }

    void unload()
    {
        if (!m_gdbus.isEmpty())
            call(QStringLiteral("org.kde.KWin.Scripting.unloadScript"), {pluginName()});
        m_started = false;
    }

    void call(const QString &method, const QStringList &extra)
    {
        QStringList arguments = {
            QStringLiteral("call"), QStringLiteral("--session"),
            QStringLiteral("--dest"), QStringLiteral("org.kde.KWin"),
            QStringLiteral("--object-path"), QStringLiteral("/Scripting"),
            QStringLiteral("--method"), method,
        };
        arguments.append(extra);
        QProcess process;
        process.start(m_gdbus, arguments);
        process.waitForFinished(2500);
    }

    static QByteArray scriptSource()
    {
        return R"JS(
const watched = new Map();
const applying = new Set();
function desiredGeometry(window) {
    const caption = window.caption || "";
    const match = caption.match(/^PasteShelf(?:Native|Preview|Handle)@(-?\d+),(-?\d+),(\d+),(\d+)/);
    if (!match) return null;
    const values = match.slice(1).map(Number);
    return {x: values[0], y: values[1], width: Math.max(1, values[2]), height: Math.max(1, values[3])};
}
function apply(window) {
    if (applying.has(window)) return;
    const desired = desiredGeometry(window);
    if (!desired) return;
    applying.add(window);
    try {
        window.skipTaskbar = true;
        window.skipPager = true;
        window.skipSwitcher = true;
        window.keepAbove = true;
        window.noBorder = true;
        const current = window.frameGeometry;
        if (Math.round(current.x) !== desired.x || Math.round(current.y) !== desired.y
                || Math.round(current.width) !== desired.width || Math.round(current.height) !== desired.height)
            window.frameGeometry = desired;
        // Make the surface sticky only after correcting its geometry. During
        // initial Wayland mapping KWin may run another placement pass when the
        // desktop membership changes; the opposite order can center the
        // handle after it was positioned correctly.
        window.onAllDesktops = true;
    } finally {
        applying.delete(window);
    }
}
function watch(window) {
    if (watched.has(window)) return;
    watched.set(window, true);
    window.captionChanged.connect(() => apply(window));
    // KWin can perform a final placement pass after a new Wayland surface is
    // mapped or made sticky. Reassert our caption-encoded geometry whenever
    // that happens instead of leaving the surface at KWin's default center.
    window.frameGeometryChanged.connect(() => apply(window));
    window.desktopsChanged.connect(() => apply(window));
    window.closed.connect(() => watched.delete(window));
    apply(window);
}
workspace.windowList().forEach(window => watch(window));
workspace.windowAdded.connect(window => watch(window));
workspace.currentDesktopChanged.connect(() => {
    workspace.windowList().forEach(window => apply(window));
});
)JS";
    }

    QString m_gdbus;
    QString m_scriptPath;
    bool m_started = false;
};

class HandleFrame final : public QFrame {
    Q_OBJECT
    Q_PROPERTY(qreal fillProgress READ fillProgress WRITE setFillProgress)
public:
    explicit HandleFrame(QWidget *parent = nullptr) : QFrame(parent)
    {
        setAttribute(Qt::WA_OpaquePaintEvent, false);
    }

    qreal fillProgress() const { return m_fillProgress; }

    void setFillProgress(qreal progress)
    {
        const qreal bounded = qBound<qreal>(0.0, progress, 1.0);
        if (qFuzzyCompare(m_fillProgress, bounded))
            return;
        m_fillProgress = bounded;
        update();
    }

    void setColors(const QColor &empty, const QColor &top, const QColor &bottom, const QColor &border)
    {
        m_emptyColor = empty;
        m_fillTop = top;
        m_fillBottom = bottom;
        m_borderColor = border;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75);
        QPainterPath shape;
        shape.addRoundedRect(bounds, 15.0, 15.0);
        painter.fillPath(shape, m_emptyColor);

        if (m_fillProgress > 0.0) {
            painter.save();
            painter.setClipPath(shape);
            const qreal fillHeight = bounds.height() * m_fillProgress;
            const QRectF fillRect(bounds.left(), bounds.bottom() - fillHeight,
                bounds.width(), fillHeight + 1.0);
            QLinearGradient gradient(bounds.topLeft(), bounds.bottomRight());
            gradient.setColorAt(0.0, m_fillTop);
            gradient.setColorAt(1.0, m_fillBottom);
            painter.fillRect(fillRect, gradient);
            painter.restore();
        }

        painter.setPen(QPen(m_borderColor, 1.0));
        painter.drawPath(shape);
    }

private:
    qreal m_fillProgress = 0.0;
    QColor m_emptyColor = QColor(QStringLiteral("#3d5466"));
    QColor m_fillTop = QColor(QStringLiteral("#3daee9"));
    QColor m_fillBottom = QColor(QStringLiteral("#2685b5"));
    QColor m_borderColor = QColor(QStringLiteral("#225f80"));
};

class ShelfWindow final : public QWidget {
    Q_OBJECT
public:
    explicit ShelfWindow(ShelfStore *store)
        : m_store(store), m_settings(QStringLiteral("PasteShelf"), QStringLiteral("PasteShelfNative"))
    {
        setAcceptDrops(true);
        setMouseTracking(true);
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setWindowIcon(QIcon::fromTheme(QString::fromLatin1(kDesktopId), QIcon::fromTheme(QStringLiteral("edit-paste"))));

        m_placement = m_settings.value(QStringLiteral("window/placement"), QStringLiteral("right-center")).toString();
        m_screenName = m_settings.value(QStringLiteral("window/screen")).toString();
        m_pinnedOpen = m_settings.value(QStringLiteral("window/pinned"), false).toBool();

        createHandle();
        createPanel();
        applyStyle();
        rebuildCards();

        m_closeTimer.setSingleShot(true);
        m_closeTimer.setInterval(kCloseHoldMs + kCloseDrainMs);
        connect(&m_closeTimer, &QTimer::timeout, this, [this] {
            if (m_pinnedOpen || m_dragActive || m_menuActive) {
                cancelCloseCountdown();
                return;
            }
            if (!m_ignoreHoverForTest && !m_hoveredWidgets.isEmpty()) {
                cancelCloseCountdown();
                return;
            }
            closeShelf();
        });
        m_drainDelayTimer.setSingleShot(true);
        m_drainDelayTimer.setInterval(kCloseHoldMs);
        connect(&m_drainDelayTimer, &QTimer::timeout, this, [this] {
            if (!m_closeTimer.isActive() || m_pinnedOpen || m_dragActive || m_menuActive
                || (!m_ignoreHoverForTest && !m_hoveredWidgets.isEmpty())) {
                cancelCloseCountdown();
                return;
            }
            animateHandleFill(0.0, kCloseDrainMs);
        });
        m_hintTimer.setSingleShot(true);
        connect(&m_hintTimer, &QTimer::timeout, this, [this] {
            if (!m_dragActive)
                setDropHintActive(false);
        });

        connect(qApp, &QGuiApplication::screenAdded, this, [this] { updatePositionTitle(); });
        connect(qApp, &QGuiApplication::screenRemoved, this, [this] { updatePositionTitle(); });

        if (m_pinnedOpen) {
            m_expanded = true;
            m_openProgress = 1.0;
            setFixedSize(expandedSize());
            m_panel->show();
        } else {
            m_openProgress = 0.0;
            setFixedSize(collapsedSize());
            m_panel->hide();
        }
        m_handle->setFillProgress(m_pinnedOpen ? 1.0 : 0.0);
        layoutSurfaces();
        updatePositionTitle();
        m_handle->show();
        qApp->installEventFilter(this);
        createTray();
    }

    ~ShelfWindow() override
    {
        qApp->removeEventFilter(this);
        delete m_handle;
    }

    bool expandedForTest() const { return m_expanded; }
    bool pinnedOpenForTest() const { return m_pinnedOpen; }
    QPoint handlePositionForTest() const
    {
        const QRect handleRect(m_handle->mapToGlobal(QPoint(0, 0)), m_handle->size());
        const int x = handleIsOnLeft() ? handleRect.left() : handleRect.right();
        int y = handleRect.center().y();
        if (m_placement.contains(QStringLiteral("top"))) y = handleRect.top();
        if (m_placement.contains(QStringLiteral("bottom"))) y = handleRect.bottom();
        return {x, y};
    }
    QPoint expectedHandlePositionForTest() const
    {
        const QPoint topLeft = desiredHandleTopLeft();
        const int x = handleIsOnLeft() ? topLeft.x() : topLeft.x() + kHandleWidth - 1;
        int y = topLeft.y() + (kHandleHeight - 1) / 2;
        if (m_placement.contains(QStringLiteral("top"))) y = topLeft.y();
        if (m_placement.contains(QStringLiteral("bottom"))) y = topLeft.y() + kHandleHeight - 1;
        return {x, y};
    }
    bool handleVisibleForTest() const { return m_handle->isVisible(); }
    bool panelVisibleForTest() const { return m_panel->isVisible(); }
    qreal handleFillForTest() const { return m_handle->fillProgress(); }
    QToolButton *handleLockButtonForTest() const { return m_handleLockButton; }
    QWidget *handleWidgetForTest() const { return m_handle; }
    bool firstPreviewAlignedForTest() const
    {
        auto *card = findChild<CardWidget *>();
        if (!card)
            return false;
        const QRect preview = card->previewPlacementForTest();
        const QRect anchor = authoritativeGlobalRect(card);
        QScreen *screen = QGuiApplication::screenAt(anchor.center());
        const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
        int expectedX = anchor.left() - preview.width() - 10;
        if (expectedX < available.left())
            expectedX = anchor.right() + 10;
        expectedX = std::clamp(expectedX,
            available.left() + 8, available.right() - preview.width() - 8);
        const int expectedY = std::clamp(anchor.top(),
            available.top() + 8, available.bottom() - preview.height() - 8);
        return preview.isValid() && preview.topLeft() == QPoint(expectedX, expectedY);
    }

public slots:
    void openShelf()
    {
        cancelCloseCountdown();
        if (m_expanded && !m_animating) {
            raise();
            m_handle->raise();
            return;
        }
        animateTo(true);
    }

    void closeShelf()
    {
        if (m_pinnedOpen || m_dragActive || !m_expanded)
            return;
        animateTo(false);
    }

    void runAnimationStressTest()
    {
        m_ignoreHoverForTest = true;
        // Exercise interrupted open/close animations at a rate far above
        // human pointer movement while keeping Qt's synthetic offscreen
        // compositor responsive enough to execute the test's watchdog timer.
        for (int step = 0; step < 12; ++step) {
            QTimer::singleShot(step * 25, this, [this, step] {
                animateTo(step % 2 == 0);
            });
        }
    }

    void runHoverHoldTest()
    {
        openShelf();
        QTimer::singleShot(240, this, [this] {
            m_hoveredWidgets.insert(this);
            m_closeTimer.start();
        });
    }

    void runHoverLeaveTest()
    {
        m_ignoreHoverForTest = true;
        openShelf();
        QTimer::singleShot(240, this, [this] {
            m_hoveredWidgets.clear();
            armCloseCountdown();
        });
        QTimer::singleShot(780, this, [this] {
            m_hoveredWidgets.insert(this);
            cancelCloseCountdown();
        });
        QTimer::singleShot(940, this, [this] {
            m_hoveredWidgets.clear();
            armCloseCountdown();
        });
    }

    void showFirstPreviewForTest()
    {
        if (auto *card = findChild<CardWidget *>())
            card->showPreviewForTest();
    }

    void holdOpenForTest()
    {
        setPinnedOpen(true);
    }

    void expandFirstStackForTest()
    {
        const auto cards = findChildren<CardWidget *>();
        for (auto *card : cards) {
            if (card->itemCountForTest() > 1) {
                card->expandForTest();
                return;
            }
        }
    }

    void saveVisualTestImages()
    {
        grab().save(QStringLiteral("/tmp/pasteshelf-native-self-main.png"));
        m_handle->grab().save(QStringLiteral("/tmp/pasteshelf-native-self-handle.png"));
        if (auto *card = findChild<CardWidget *>())
            card->savePreviewForTest(QStringLiteral("/tmp/pasteshelf-native-self-preview.png"));
    }

    void runVisualCountdownTest()
    {
        m_ignoreHoverForTest = true;
        openShelf();
        QTimer::singleShot(300, this, [this] {
            m_hoveredWidgets.clear();
            m_handle->grab().save(QStringLiteral("/tmp/pasteshelf-handle-full.png"));
            armCloseCountdown();
        });
        QTimer::singleShot(2550, this, [this] {
            m_handle->grab().save(QStringLiteral("/tmp/pasteshelf-handle-half.png"));
        });
        QTimer::singleShot(3550, this, [this] {
            m_handle->grab().save(QStringLiteral("/tmp/pasteshelf-handle-empty.png"));
        });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // The handle is a separate Wayland surface so it never shifts while
        // the drawer changes size. Forward its drag events to the same import
        // path as the drawer surface before processing ordinary hover events.
        if (watched == m_handle) {
            switch (event->type()) {
            case QEvent::DragEnter: {
                auto *dragEvent = static_cast<QDragEnterEvent *>(event);
                if (dragEvent->mimeData()->hasFormat(kInternalMime)) {
                    dragEvent->ignore();
                    return true;
                }
                dragEnterEvent(dragEvent);
                return true;
            }
            case QEvent::DragMove: {
                auto *dragEvent = static_cast<QDragMoveEvent *>(event);
                if (dragEvent->mimeData()->hasFormat(kInternalMime)) {
                    dragEvent->ignore();
                    return true;
                }
                dragMoveEvent(dragEvent);
                return true;
            }
            case QEvent::DragLeave:
                dragLeaveEvent(static_cast<QDragLeaveEvent *>(event));
                return true;
            case QEvent::Drop: {
                auto *drop = static_cast<QDropEvent *>(event);
                if (drop->mimeData()->hasFormat(kInternalMime)) {
                    drop->ignore();
                    return true;
                }
                dropEvent(drop);
                return true;
            }
            default:
                break;
            }
        }
        if (m_ignoreHoverForTest)
            return QWidget::eventFilter(watched, event);
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && ownsWidget(widget)) {
            if (event->type() == QEvent::Enter) {
                if (!widget->property("pasteshelfHoverTracked").toBool()) {
                    widget->setProperty("pasteshelfHoverTracked", true);
                    connect(widget, &QObject::destroyed, this, [this, widget] {
                        m_hoveredWidgets.remove(widget);
                        scheduleCloseAfterPointerLeave();
                    });
                }
                m_hoveredWidgets.insert(widget);
                cancelCloseCountdown();
                if (!m_expanded)
                    openShelf();
            } else if (event->type() == QEvent::Leave || event->type() == QEvent::Hide) {
                m_hoveredWidgets.remove(widget);
                scheduleCloseAfterPointerLeave();
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        cancelCloseCountdown();
        if (!m_expanded) openShelf();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        scheduleCloseAfterPointerLeave();
        QWidget::leaveEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        const QString groupId = ShelfStore::internalWholeGroupId(event->mimeData());
        if (!groupId.isEmpty() && m_store->group(groupId)) {
            m_internalReorderGroupId = groupId;
            m_dragActive = true;
            setDropHintActive(false);
            openShelf();
            updateReorderIndicator(event->position().toPoint(), groupId);
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        if (!ShelfStore::canImport(event->mimeData())) {
            event->ignore();
            return;
        }
        m_dragActive = true;
        setDropHintActive(true);
        openShelf();
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        const QString groupId = ShelfStore::internalWholeGroupId(event->mimeData());
        if (!groupId.isEmpty() && groupId == m_internalReorderGroupId
            && m_store->group(groupId)) {
            autoScrollForReorder(event->position().toPoint());
            updateReorderIndicator(event->position().toPoint(), groupId);
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        if (ShelfStore::canImport(event->mimeData())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        }
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        clearReorderIndicator();
        m_dragActive = false;
        setDropHintActive(false);
        event->accept();
    }

    void dropEvent(QDropEvent *event) override
    {
        const QString groupId = ShelfStore::internalWholeGroupId(event->mimeData());
        if (!groupId.isEmpty() && groupId == m_internalReorderGroupId
            && m_store->group(groupId)) {
            const int destination = reorderInsertionIndex(event->position().toPoint(), groupId);
            if (m_store->reorderGroup(groupId, destination)) {
                if (auto *card = qobject_cast<CardWidget *>(event->source());
                    card && card->groupId() == groupId) {
                    card->acceptInternalReorder();
                }
                syncCardLayoutOrder();
                event->setDropAction(Qt::CopyAction);
                event->accept();
            } else {
                event->ignore();
            }
            clearReorderIndicator();
            m_dragActive = false;
            setDropHintActive(false);
            return;
        }
        if (!ShelfStore::canImport(event->mimeData()) || !m_store->add(event->mimeData())) {
            event->ignore();
        } else {
            rebuildCards();
            event->setDropAction(Qt::CopyAction);
            event->accept();
        }
        m_dragActive = false;
        setDropHintActive(false);
    }

    void closeEvent(QCloseEvent *event) override
    {
        hide();
        event->ignore();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        layoutSurfaces();
    }

private:
    static constexpr int kHandleWidth = 46;
    static constexpr int kHandleHeight = 148;
    static constexpr int kPanelWidth = 400;
    static constexpr int kPanelHeight = 580;
    static constexpr int kSurfaceGap = 8;
    static constexpr int kCloseHoldMs = 1500;
    static constexpr int kCloseDrainMs = 1500;
    static QSize collapsedSize() { return {1, kHandleHeight}; }
    static QSize expandedSize() { return {kPanelWidth, kPanelHeight}; }

    static QSize surfaceSizeForProgress(qreal progress)
    {
        const auto interpolate = [progress](int from, int to) {
            return qRound(from + (to - from) * progress);
        };
        return {
            interpolate(collapsedSize().width(), expandedSize().width()),
            interpolate(collapsedSize().height(), expandedSize().height()),
        };
    }

    bool handleIsOnLeft() const
    {
        return m_placement.contains(QStringLiteral("left"));
    }

    int panelVerticalOffset(int surfaceHeight) const
    {
        if (m_placement.contains(QStringLiteral("top")))
            return 0;
        if (m_placement.contains(QStringLiteral("bottom")))
            return surfaceHeight - kPanelHeight;
        return (surfaceHeight - kPanelHeight) / 2;
    }

    void layoutSurfaces()
    {
        if (!m_handle || !m_panel)
            return;
        const int panelX = handleIsOnLeft() ? 0 : width() - kPanelWidth;
        // Keep the panel at its final size and reveal it through the growing
        // translucent parent. Its contents therefore never squash or snap
        // during the animation.
        m_panel->setGeometry(panelX, panelVerticalOffset(height()), kPanelWidth, kPanelHeight);
        m_panel->setVisible(m_openProgress > 0.0);
    }

    bool ownsWidget(const QWidget *widget) const
    {
        if (widget == m_handle || m_handle->isAncestorOf(widget))
            return true;
        for (const QObject *object = widget; object; object = object->parent()) {
            if (object == this)
                return true;
        }
        return false;
    }

    void animateHandleFill(qreal target, int duration)
    {
        if (m_handleFillAnimation) {
            auto *previous = m_handleFillAnimation.data();
            m_handleFillAnimation.clear();
            previous->stop();
            previous->deleteLater();
        }
        if (duration <= 0 || qFuzzyCompare(m_handle->fillProgress(), target)) {
            m_handle->setFillProgress(target);
            return;
        }
        auto *animation = new QVariantAnimation(this);
        m_handleFillAnimation = animation;
        animation->setStartValue(m_handle->fillProgress());
        animation->setEndValue(target);
        animation->setDuration(duration);
        animation->setEasingCurve(QEasingCurve::Linear);
        connect(animation, &QVariantAnimation::valueChanged, this, [this, animation](const QVariant &value) {
            if (m_handleFillAnimation == animation)
                m_handle->setFillProgress(value.toReal());
        });
        connect(animation, &QVariantAnimation::finished, this, [this, animation] {
            if (m_handleFillAnimation != animation)
                return;
            m_handleFillAnimation.clear();
            animation->deleteLater();
        });
        animation->start();
    }

    void cancelCloseCountdown()
    {
        m_closeTimer.stop();
        m_drainDelayTimer.stop();
        animateHandleFill(1.0, m_expanded ? 90 : 210);
    }

    void armCloseCountdown()
    {
        if (!m_expanded) {
            m_closeTimer.stop();
            m_drainDelayTimer.stop();
            animateHandleFill(0.0, 0);
            return;
        }
        if (m_pinnedOpen || m_dragActive || m_menuActive
            || (!m_ignoreHoverForTest && !m_hoveredWidgets.isEmpty())) {
            cancelCloseCountdown();
            return;
        }
        m_closeTimer.start();
        m_drainDelayTimer.start();
        animateHandleFill(1.0, 90);
    }

    void scheduleCloseAfterPointerLeave()
    {
        QTimer::singleShot(0, this, [this] {
            if (m_hoveredWidgets.isEmpty() && !m_pinnedOpen && !m_dragActive && !m_menuActive)
                armCloseCountdown();
        });
    }

    QList<CardWidget *> reorderCards(const QString &excludedGroupId = {}) const
    {
        QList<CardWidget *> cards;
        for (int index = 0; index < m_cardsLayout->count(); ++index) {
            auto *card = qobject_cast<CardWidget *>(m_cardsLayout->itemAt(index)->widget());
            if (card && card->groupId() != excludedGroupId)
                cards.append(card);
        }
        return cards;
    }

    int reorderInsertionIndex(const QPoint &windowPosition, const QString &draggedGroupId) const
    {
        const QPoint hostPosition = m_cardsHost->mapFrom(this, windowPosition);
        const auto cards = reorderCards(draggedGroupId);
        for (int index = 0; index < cards.size(); ++index) {
            if (hostPosition.y() < cards[index]->geometry().center().y())
                return index;
        }
        return cards.size();
    }

    void updateReorderIndicator(const QPoint &windowPosition, const QString &draggedGroupId)
    {
        if (!m_reorderIndicator)
            return;
        m_cardsLayout->activate();
        const auto cards = reorderCards(draggedGroupId);
        const int destination = reorderInsertionIndex(windowPosition, draggedGroupId);
        int lineY = m_cardsLayout->contentsMargins().top();
        if (!cards.isEmpty()) {
            if (destination < cards.size())
                lineY = cards[destination]->geometry().top() - (m_cardsLayout->spacing() + 1) / 2;
            else
                lineY = cards.constLast()->geometry().bottom() + (m_cardsLayout->spacing() + 1) / 2;
        }
        lineY = std::clamp(lineY, 2, qMax(2, m_cardsHost->height() - 3));
        m_reorderIndicator->setGeometry(
            7, lineY - 2, qMax(1, m_cardsHost->width() - 18), 4);
        m_reorderIndicator->show();
        m_reorderIndicator->raise();
    }

    void clearReorderIndicator()
    {
        m_internalReorderGroupId.clear();
        if (m_reorderIndicator)
            m_reorderIndicator->hide();
    }

    void autoScrollForReorder(const QPoint &windowPosition)
    {
        const QPoint viewportPosition = m_scroll->viewport()->mapFrom(this, windowPosition);
        if (!m_scroll->viewport()->rect().adjusted(0, -24, 0, 24).contains(viewportPosition))
            return;
        auto *bar = m_scroll->verticalScrollBar();
        constexpr int edge = 34;
        constexpr int step = 18;
        if (viewportPosition.y() < edge)
            bar->setValue(bar->value() - step);
        else if (viewportPosition.y() > m_scroll->viewport()->height() - edge)
            bar->setValue(bar->value() + step);
    }

    void syncCardLayoutOrder()
    {
        const auto cards = reorderCards();
        int destination = 0;
        for (const auto &group : m_store->groups()) {
            const auto found = std::find_if(cards.cbegin(), cards.cend(),
                [&group](const CardWidget *card) { return card->groupId() == group.id; });
            if (found == cards.cend())
                continue;
            m_cardsLayout->removeWidget(*found);
            m_cardsLayout->insertWidget(destination++, *found);
        }
        m_cardsLayout->invalidate();
        m_cardsLayout->activate();
    }

    void createHandle()
    {
        m_handle = new HandleFrame;
        m_handle->setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        m_handle->setAttribute(Qt::WA_TranslucentBackground);
        m_handle->setAcceptDrops(true);
        m_handle->setMouseTracking(true);
        m_handle->setFixedSize(kHandleWidth, kHandleHeight);
        m_handle->setWindowIcon(windowIcon());
        m_handle->setObjectName(QStringLiteral("handle"));
        auto *layout = new QVBoxLayout(m_handle);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(5);
        layout->addStretch();
        m_handleArrow = new QLabel(QStringLiteral("‹"), m_handle);
        m_handleArrow->setAlignment(Qt::AlignCenter);
        m_handleArrow->setObjectName(QStringLiteral("handleMark"));
        m_handleArrow->setFixedSize(30, 30);
        layout->addWidget(m_handleArrow, 0, Qt::AlignCenter);
        m_handleCount = new QLabel(QStringLiteral("0"), m_handle);
        m_handleCount->setAlignment(Qt::AlignCenter);
        m_handleCount->setObjectName(QStringLiteral("handleCount"));
        m_handleCount->setFixedSize(26, 22);
        layout->addWidget(m_handleCount, 0, Qt::AlignCenter);
        m_handleLockButton = new QToolButton(m_handle);
        m_handleLockButton->setObjectName(QStringLiteral("handleLockButton"));
        m_handleLockButton->setCheckable(true);
        m_handleLockButton->setAutoRaise(true);
        m_handleLockButton->setFixedSize(28, 28);
        m_handleLockButton->setCursor(Qt::PointingHandCursor);
        connect(m_handleLockButton, &QToolButton::clicked, this, [this](bool locked) {
            setPinnedOpen(locked);
        });
        layout->addWidget(m_handleLockButton, 0, Qt::AlignCenter);
        layout->addStretch();
        updateHandleLockButton();
    }

    void createPanel()
    {
        m_panel = new QFrame(this);
        m_panel->setObjectName(QStringLiteral("panel"));
        auto *root = new QVBoxLayout(m_panel);
        root->setContentsMargins(12, 11, 12, 12);
        root->setSpacing(9);

        auto *header = new QHBoxLayout;
        auto *heading = new QLabel(QStringLiteral("PasteShelf"), m_panel);
        heading->setObjectName(QStringLiteral("heading"));
        header->addWidget(heading);
        auto *badge = new QLabel(QStringLiteral("NATIVE"), m_panel);
        badge->setObjectName(QStringLiteral("nativeBadge"));
        header->addWidget(badge);
        header->addStretch();
        auto *pasteButton = new QToolButton(m_panel);
        pasteButton->setObjectName(QStringLiteral("pasteButton"));
        pasteButton->setText(QStringLiteral("Paste"));
        pasteButton->setIcon(QIcon::fromTheme(QStringLiteral("edit-paste")));
        pasteButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        pasteButton->setShortcut(QKeySequence::Paste);
        pasteButton->setToolTip(QStringLiteral("Add clipboard contents (Ctrl+V)"));
        pasteButton->setFixedHeight(30);
        pasteButton->setCursor(Qt::PointingHandCursor);
        connect(pasteButton, &QToolButton::clicked, this, &ShelfWindow::addClipboardContent);
        header->addWidget(pasteButton);
        auto *menuButton = new QToolButton(m_panel);
        menuButton->setText(QStringLiteral("⋮"));
        menuButton->setObjectName(QStringLiteral("menuButton"));
        menuButton->setFixedSize(30, 30);
        connect(menuButton, &QToolButton::clicked, this, [this, menuButton] {
            showSettingsMenu(menuButton->mapToGlobal(QPoint(0, menuButton->height())));
        });
        header->addWidget(menuButton);
        root->addLayout(header);

        m_dropHint = new QLabel(m_panel);
        m_dropHint->setObjectName(QStringLiteral("dropHint"));
        m_dropHint->setAlignment(Qt::AlignCenter);
        m_dropHint->setFixedHeight(0);
        m_dropHint->hide();
        root->addWidget(m_dropHint);

        m_scroll = new QScrollArea(m_panel);
        m_scroll->setWidgetResizable(true);
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_cardsHost = new QWidget(m_scroll);
        m_cardsHost->setObjectName(QStringLiteral("cardsHost"));
        m_cardsLayout = new QVBoxLayout(m_cardsHost);
        m_cardsLayout->setContentsMargins(1, 1, 4, 1);
        m_cardsLayout->setSpacing(7);
        m_cardsLayout->addStretch();
        m_scroll->setWidget(m_cardsHost);
        root->addWidget(m_scroll, 1);

        m_empty = new QLabel(QStringLiteral("Drag files, folders, text, links,\nor screenshots onto the edge handle."), m_cardsHost);
        m_empty->setObjectName(QStringLiteral("empty"));
        m_empty->setAlignment(Qt::AlignCenter);
        m_empty->setWordWrap(true);
        m_cardsLayout->insertWidget(0, m_empty, 1);

        m_reorderIndicator = new QFrame(m_cardsHost);
        m_reorderIndicator->setObjectName(QStringLiteral("reorderIndicator"));
        m_reorderIndicator->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_reorderIndicator->setFixedHeight(4);
        m_reorderIndicator->hide();

    }

    void applyStyle()
    {
        const QColor window = palette().color(QPalette::Window);
        const QColor base = palette().color(QPalette::Base);
        const QColor text = palette().color(QPalette::Text);
        const QColor muted = palette().color(QPalette::PlaceholderText);
        const QColor accent = palette().color(QPalette::Highlight);
        const QColor accentText = palette().color(QPalette::HighlightedText);
        const QColor border = palette().color(QPalette::Mid);
        const QColor handleTop = accent.lighter(112);
        const QColor handleBottom = accent.darker(118);
        setStyleSheet(QStringLiteral(R"CSS(
            #panel { background: %1; border: 1px solid %6; border-radius: 14px; }
            #heading { color: %3; font-size: 18px; font-weight: 700; }
            #nativeBadge { color: %7; background: %5; border-radius: 7px; padding: 2px 7px; font-size: 8px; font-weight: 700; }
            #pasteButton { color: %3; background: %2; border: 1px solid %6; border-radius: 7px; padding: 2px 8px; font-weight: 600; }
            #pasteButton:hover { border-color: %5; background: %1; }
            #menuButton { border: 1px solid %6; border-radius: 7px; font-size: 19px; }
            #menuButton:hover { background: %2; }
            #dropHint { color: %4; background: %2; border: 1px solid %6; border-radius: 9px; }
            #dropHint[active="true"] { color: %3; border: 2px solid %5; background: %2; font-weight: 600; }
            #reorderIndicator { background: %5; border: none; border-radius: 2px; }
            #card { background: %2; border: 1px solid %6; border-radius: 11px; }
            #card:hover { border: 1px solid %5; background: %1; }
            #cardHeader { background: transparent; border: none; }
            #cardTitle { color: %3; font-weight: 650; }
            #cardDetail, #empty { color: %4; font-size: 11px; }
            #countBadge { color: %7; background: %5; border-radius: 10px; font-size: 10px; font-weight: 700; }
            #cardDetails { background: transparent; border: none; border-top: 1px solid %6; }
            #memberRow { background: %1; border: 1px solid %6; border-radius: 7px; }
            #memberRow:hover { border-color: %5; }
            #memberTitle { color: %3; font-size: 11px; }
            #expandedText { color: %3; background: %1; border-radius: 7px; padding: 9px; }
            #previewPopup { background: %1; border: 1px solid %5; border-radius: 12px; }
            #previewTitle { color: %3; font-size: 14px; font-weight: 700; }
            #previewBody { color: %3; background: transparent; font-size: 12px; }
            #previewScroll, #previewScroll > QWidget > QWidget { background: transparent; }
            QToolButton { border: none; border-radius: 7px; color: %4; }
            QToolButton:hover { background: %2; color: %3; }
            #pinButton[pinned="true"] { color: %7; background: %5; }
            #removeButton:hover { color: #d9534f; }
            QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
            QScrollBar:vertical { background: transparent; width: 7px; margin: 1px; }
            QScrollBar::handle:vertical { background: %6; border-radius: 3px; min-height: 24px; }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        )CSS")
            .arg(window.name(), base.name(), text.name(), muted.name(), accent.name(), border.name(), accentText.name()));
        m_handle->setStyleSheet(QStringLiteral(R"CSS(
            #handle { background: transparent; border: none; }
            #handleMark { color: %2; background: rgba(255, 255, 255, 28); border: 1px solid rgba(255, 255, 255, 42); border-radius: 15px; font-size: 22px; font-weight: 700; }
            #handleCount { color: %1; background: %2; border-radius: 11px; font-size: 11px; font-weight: 750; }
            #handleLockButton { color: %2; background: rgba(255, 255, 255, 20); border: 1px solid rgba(255, 255, 255, 36); border-radius: 9px; }
            #handleLockButton:hover { color: %1; background: %2; }
            #handleLockButton[locked="true"] { color: %1; background: %2; border-color: %2; }
        )CSS").arg(accent.name(), accentText.name()));
        m_handle->setColors(accent.darker(165), handleTop, handleBottom, handleBottom.darker(112));
    }

    void rebuildCards()
    {
        while (m_cardsLayout->count() > 1) {
            QLayoutItem *item = m_cardsLayout->takeAt(0);
            if (item->widget() && item->widget() != m_empty)
                item->widget()->deleteLater();
            delete item;
        }
        if (m_empty) {
            m_cardsLayout->removeWidget(m_empty);
            m_cardsLayout->insertWidget(0, m_empty, 1);
        }
        const bool hasCards = !m_store->groups().isEmpty();
        m_empty->setVisible(!hasCards);
        int totalItems = 0;
        int index = 0;
        for (const auto &group : m_store->groups()) {
            totalItems += group.items.size();
            auto *card = new CardWidget(group, m_store, m_cardsHost,
                [this](const QWidget *widget) { return authoritativeGlobalRect(widget); });
            connect(card, &CardWidget::pinChanged, this, [this](const QString &id, bool pinned) {
                m_store->setPinned(id, pinned);
            });
            connect(card, &CardWidget::removeRequested, this, [this](const QString &id) {
                m_store->remove(id);
                rebuildCards();
            });
            connect(card, &CardWidget::removeItemRequested, this,
                [this](const QString &groupId, const QString &itemId) {
                    m_store->removeItem(groupId, itemId);
                    rebuildCards();
                });
            connect(card, &CardWidget::dragActiveChanged, this, [this](bool active) {
                m_dragActive = active;
                if (active) cancelCloseCountdown();
                else if (!m_pinnedOpen) armCloseCountdown();
            });
            m_cardsLayout->insertWidget(index++, card);
        }
        const QString countText = QString::number(totalItems);
        const QString countTip = QStringLiteral("%1 item%2 on the shelf")
            .arg(totalItems).arg(totalItems == 1 ? QString() : QStringLiteral("s"));
        if (m_handleCount) {
            m_handleCount->setText(countText);
            m_handleCount->setToolTip(countTip);
        }
    }

    void animateTo(bool open)
    {
        if (m_animation) {
            auto *previous = m_animation.data();
            m_animation.clear();
            previous->stop();
            previous->deleteLater();
        }
        m_animating = true;
        const bool wasExpanded = m_expanded;
        if (open) {
            m_expanded = true;
            layoutSurfaces();
            if (!wasExpanded) {
                raise();
                m_handle->raise();
            }
        }
        const qreal start = m_openProgress;
        const qreal end = open ? 1.0 : 0.0;
        auto *animation = new QVariantAnimation(this);
        m_animation = animation;
        animation->setDuration(qMax(65, qRound((open ? 210 : 180) * qAbs(end - start))));
        animation->setStartValue(start);
        animation->setEndValue(end);
        animation->setEasingCurve(open ? QEasingCurve::OutCubic : QEasingCurve::InCubic);
        connect(animation, &QVariantAnimation::valueChanged, this, [this, animation](const QVariant &value) {
            if (m_animation != animation)
                return;
            m_openProgress = value.toReal();
            setFixedSize(surfaceSizeForProgress(m_openProgress));
            updatePositionTitle();
        });
        connect(animation, &QVariantAnimation::finished, this, [this, animation, open] {
            if (m_animation != animation)
                return;
            const qreal finalProgress = open ? 1.0 : 0.0;
            const QSize finalSize = surfaceSizeForProgress(finalProgress);
            const bool needsGeometryCorrection = !qFuzzyCompare(m_openProgress, finalProgress)
                || size() != finalSize;
            m_openProgress = finalProgress;
            if (needsGeometryCorrection)
                setFixedSize(finalSize);
            if (!open) {
                m_expanded = false;
                m_panel->hide();
                if (!m_pinnedOpen)
                    animateHandleFill(0.0, 0);
            }
            m_animating = false;
            m_animation.clear();
            if (needsGeometryCorrection)
                updatePositionTitle();
            animation->deleteLater();
        });
        animation->start();
    }

    void setDropHintActive(bool active)
    {
        m_hintTimer.stop();
        m_dropHint->setText(active ? QStringLiteral("↓  Release to add") : QString());
        m_dropHint->setProperty("active", active);
        m_dropHint->style()->unpolish(m_dropHint);
        m_dropHint->style()->polish(m_dropHint);
        m_dropHint->setFixedHeight(active ? 42 : 0);
        m_dropHint->setVisible(active);
    }

    void showTransientHint(const QString &message, int duration)
    {
        m_dropHint->setProperty("active", false);
        m_dropHint->setText(message);
        m_dropHint->style()->unpolish(m_dropHint);
        m_dropHint->style()->polish(m_dropHint);
        m_dropHint->setFixedHeight(34);
        m_dropHint->show();
        m_hintTimer.start(duration);
    }

    QScreen *selectedScreen() const
    {
        const auto screens = QGuiApplication::screens();
        for (auto *screen : screens) {
            if (screen->name() == m_screenName)
                return screen;
        }
        return QGuiApplication::primaryScreen();
    }

    QPoint desiredHandleTopLeft() const
    {
        QScreen *screen = selectedScreen();
        if (!screen)
            return {};
        const QRect area = screen->availableGeometry();
        int x = area.x() + (area.width() - kHandleWidth) / 2;
        int y = area.y() + (area.height() - kHandleHeight) / 2;
        if (m_placement.contains(QStringLiteral("left"))) x = area.x() + 12;
        if (m_placement.contains(QStringLiteral("right"))) x = area.right() - kHandleWidth - 11;
        if (m_placement.contains(QStringLiteral("top"))) y = area.y() + 12;
        if (m_placement.contains(QStringLiteral("bottom"))) y = area.bottom() - kHandleHeight - 11;
        return {x, y};
    }

    QPoint desiredSurfaceTopLeft() const
    {
        const QPoint handleTopLeft = desiredHandleTopLeft();
        const int finalPanelX = handleIsOnLeft()
            ? handleTopLeft.x() + kHandleWidth + kSurfaceGap
            : handleTopLeft.x() - kSurfaceGap - kPanelWidth;
        int finalPanelY = handleTopLeft.y() + (kHandleHeight - kPanelHeight) / 2;
        if (m_placement.contains(QStringLiteral("top")))
            finalPanelY = handleTopLeft.y();
        if (m_placement.contains(QStringLiteral("bottom")))
            finalPanelY = handleTopLeft.y() + kHandleHeight - kPanelHeight;

        const int x = handleIsOnLeft() ? finalPanelX : finalPanelX + kPanelWidth - width();
        int y = finalPanelY + (kPanelHeight - height()) / 2;
        if (m_placement.contains(QStringLiteral("top")))
            y = finalPanelY;
        if (m_placement.contains(QStringLiteral("bottom")))
            y = finalPanelY + kPanelHeight - height();
        return {x, y};
    }

    QRect authoritativeGlobalRect(const QWidget *widget) const
    {
        if (!widget)
            return {};
        if (widget == m_handle || m_handle->isAncestorOf(widget)) {
            return QRect(desiredHandleTopLeft() + widget->mapTo(m_handle, QPoint(0, 0)),
                widget->size());
        }
        return QRect(desiredSurfaceTopLeft() + widget->mapTo(this, QPoint(0, 0)),
            widget->size());
    }

    void updatePositionTitle()
    {
        QScreen *screen = selectedScreen();
        if (!screen)
            return;
        const QPoint handleTopLeft = desiredHandleTopLeft();
        const int handleX = handleTopLeft.x();
        const int handleY = handleTopLeft.y();

        m_handle->setWindowTitle(QStringLiteral("PasteShelfHandle@%1,%2,%3,%4")
            .arg(handleX).arg(handleY).arg(kHandleWidth).arg(kHandleHeight));
        m_handle->move(handleX, handleY);

        const QPoint surfaceTopLeft = desiredSurfaceTopLeft();
        if (m_handleArrow)
            m_handleArrow->setText(handleIsOnLeft() ? QStringLiteral("›") : QStringLiteral("‹"));
        layoutSurfaces();
        setWindowTitle(QStringLiteral("PasteShelfNative@%1,%2,%3,%4")
            .arg(surfaceTopLeft.x()).arg(surfaceTopLeft.y()).arg(width()).arg(height()));
        move(surfaceTopLeft);
    }

    void showSettingsMenu(const QPoint &position)
    {
        QMenu menu;
        m_menuActive = true;
        cancelCloseCountdown();
        QAction *pin = menu.addAction(QStringLiteral("Pin drawer open"));
        pin->setCheckable(true);
        pin->setChecked(m_pinnedOpen);

        QMenu *placement = menu.addMenu(QStringLiteral("Position"));
        const QList<QPair<QString, QString>> placements = {
            {QStringLiteral("top-left"), QStringLiteral("Top left")},
            {QStringLiteral("top-center"), QStringLiteral("Top center")},
            {QStringLiteral("top-right"), QStringLiteral("Top right")},
            {QStringLiteral("right-center"), QStringLiteral("Right center")},
            {QStringLiteral("bottom-right"), QStringLiteral("Bottom right")},
            {QStringLiteral("bottom-center"), QStringLiteral("Bottom center")},
            {QStringLiteral("bottom-left"), QStringLiteral("Bottom left")},
            {QStringLiteral("left-center"), QStringLiteral("Left center")},
        };
        for (const auto &[id, label] : placements) {
            QAction *action = placement->addAction(label);
            action->setCheckable(true);
            action->setChecked(m_placement == id);
            connect(action, &QAction::triggered, this, [this, id] {
                m_placement = id;
                m_settings.setValue(QStringLiteral("window/placement"), id);
                updatePositionTitle();
            });
        }

        QMenu *screens = menu.addMenu(QStringLiteral("Monitor"));
        for (auto *screen : QGuiApplication::screens()) {
            QAction *action = screens->addAction(screen->name());
            action->setCheckable(true);
            action->setChecked(screen == selectedScreen());
            connect(action, &QAction::triggered, this, [this, screen] {
                m_screenName = screen->name();
                m_settings.setValue(QStringLiteral("window/screen"), m_screenName);
                updatePositionTitle();
            });
        }

        menu.addSeparator();
        QAction *clear = menu.addAction(QStringLiteral("Clear all"));
        if (m_store->groups().isEmpty()) clear->setEnabled(false);
        QAction *chosen = menu.exec(position);
        m_menuActive = false;
        if (chosen == pin) {
            setPinnedOpen(pin->isChecked());
        } else if (chosen == clear) {
            confirmClear();
        }
        if (!m_pinnedOpen)
            scheduleCloseAfterPointerLeave();
    }

    void setPinnedOpen(bool pinned)
    {
        m_pinnedOpen = pinned;
        m_settings.setValue(QStringLiteral("window/pinned"), m_pinnedOpen);
        updateHandleLockButton();
        if (m_pinnedOpen) {
            cancelCloseCountdown();
            openShelf();
        } else {
            scheduleCloseAfterPointerLeave();
        }
    }

    void updateHandleLockButton()
    {
        const QIcon icon = QIcon::fromTheme(m_pinnedOpen
            ? QStringLiteral("object-locked")
            : QStringLiteral("object-unlocked"));
        if (!m_handleLockButton)
            return;
        m_handleLockButton->setChecked(m_pinnedOpen);
        m_handleLockButton->setProperty("locked", m_pinnedOpen);
        m_handleLockButton->setToolTip(m_pinnedOpen
            ? QStringLiteral("Unlock drawer")
            : QStringLiteral("Lock drawer open"));
        if (icon.isNull()) {
            m_handleLockButton->setText(m_pinnedOpen ? QStringLiteral("🔒") : QStringLiteral("🔓"));
        } else {
            m_handleLockButton->setIcon(icon);
            m_handleLockButton->setText(QString());
        }
        m_handleLockButton->style()->unpolish(m_handleLockButton);
        m_handleLockButton->style()->polish(m_handleLockButton);
    }

    void confirmClear()
    {
        if (m_store->groups().isEmpty())
            return;
        QMessageBox confirmation(
            QMessageBox::Question,
            QStringLiteral("Clear PasteShelf Native?"),
            QStringLiteral("Remove every item from the optimized test shelf? Original files will not be deleted."),
            QMessageBox::Cancel,
            this);
        auto *clearButton = confirmation.addButton(QStringLiteral("Clear all"), QMessageBox::DestructiveRole);
        confirmation.exec();
        if (confirmation.clickedButton() != clearButton)
            return;
        m_store->clear();
        rebuildCards();
    }

    void addClipboardContent()
    {
        const QMimeData *mime = QApplication::clipboard()->mimeData(QClipboard::Clipboard);
        if (!mime || !ShelfStore::canImport(mime)) {
            showTransientHint(QStringLiteral("Clipboard has no supported content"), 1500);
            return;
        }
        if (m_store->add(mime)) {
            rebuildCards();
            openShelf();
            showTransientHint(QStringLiteral("✓  Added from clipboard"), 1200);
        }
    }

    void createTray()
    {
        if (!QSystemTrayIcon::isSystemTrayAvailable())
            return;
        m_tray = new QSystemTrayIcon(windowIcon(), this);
        m_tray->setToolTip(QStringLiteral("PasteShelf Native (optimized test build)"));
        auto *menu = new QMenu;
        QAction *clipboard = menu->addAction(QStringLiteral("Add clipboard"));
        connect(clipboard, &QAction::triggered, this, &ShelfWindow::addClipboardContent);
        QAction *show = menu->addAction(QStringLiteral("Show PasteShelf Native"));
        connect(show, &QAction::triggered, this, &ShelfWindow::openShelf);
        QAction *clear = menu->addAction(QStringLiteral("Clear…"));
        connect(clear, &QAction::triggered, this, &ShelfWindow::confirmClear);
        menu->addSeparator();
        QAction *quit = menu->addAction(QStringLiteral("Quit"));
        connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit);
        m_tray->setContextMenu(menu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                openShelf();
        });
        m_tray->show();
    }

    ShelfStore *m_store = nullptr;
    QSettings m_settings;
    HandleFrame *m_handle = nullptr;
    QLabel *m_handleArrow = nullptr;
    QLabel *m_handleCount = nullptr;
    QToolButton *m_handleLockButton = nullptr;
    QWidget *m_panel = nullptr;
    QLabel *m_dropHint = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_cardsHost = nullptr;
    QVBoxLayout *m_cardsLayout = nullptr;
    QLabel *m_empty = nullptr;
    QFrame *m_reorderIndicator = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QPointer<QVariantAnimation> m_animation;
    QPointer<QVariantAnimation> m_handleFillAnimation;
    QSet<QWidget *> m_hoveredWidgets;
    QTimer m_closeTimer;
    QTimer m_drainDelayTimer;
    QTimer m_hintTimer;
    QString m_internalReorderGroupId;
    QString m_placement;
    QString m_screenName;
    bool m_expanded = false;
    bool m_pinnedOpen = false;
    bool m_dragActive = false;
    bool m_menuActive = false;
    bool m_animating = false;
    bool m_ignoreHoverForTest = false;
    qreal m_openProgress = 0.0;
};

class SingleInstance final : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject *parent = nullptr)
        : QObject(parent),
          m_name(QStringLiteral("pasteshelf-native-%1").arg(QString::number(getuid()))),
          m_lockPath(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
                     + QLatin1Char('/') + m_name + QStringLiteral(".lock")),
          m_lock(m_lockPath)
    {
        connect(&m_server, &QLocalServer::newConnection, this, [this] {
            while (m_server.hasPendingConnections()) {
                QLocalSocket *socket = m_server.nextPendingConnection();
                connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                    readSocket(socket);
                });
                connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                QTimer::singleShot(0, socket, [this, socket] { readSocket(socket); });
            }
        });
    }

    bool acquire()
    {
        if (!m_lock.tryLock(150)) {
            QLocalSocket socket;
            socket.connectToServer(m_name);
            if (socket.waitForConnected(400)) {
                socket.write("show\n");
                socket.waitForBytesWritten(200);
                return false;
            }
            bool removed = m_lock.removeStaleLockFile();
            if (!removed) {
                qint64 ownerPid = 0;
                QString ownerApplication;
                QString ownerHost;
                if (m_lock.getLockInfo(&ownerPid, &ownerApplication, &ownerHost)
                    && ownerPid > 0 && ::kill(static_cast<pid_t>(ownerPid), 0) == -1
                    && errno == ESRCH) {
                    removed = QFile::remove(m_lockPath);
                }
            }
            if (!removed || !m_lock.tryLock(150))
                return false;
        }
        QLocalServer::removeServer(m_name);
        return m_server.listen(m_name);
    }

signals:
    void showRequested();

private:
    void readSocket(QLocalSocket *socket)
    {
        if (!socket || socket->bytesAvailable() == 0)
            return;
        if (socket->readAll().contains("show"))
            emit showRequested();
        socket->disconnectFromServer();
    }

    QString m_name;
    QString m_lockPath;
    QLockFile m_lock;
    QLocalServer m_server;
};

bool runSelfTest()
{
    QTemporaryDir directory;
    if (!directory.isValid())
        return false;
    QString textGroupId;
    {
        ShelfStore store(directory.path());
        QMimeData mime;
        mime.setText(QStringLiteral("Native text round trip"));
        mime.setHtml(QStringLiteral("<b>Native text round trip</b>"));
        if (!store.add(&mime) || store.groups().size() != 1)
            return false;
        textGroupId = store.groups().first().id;
        const auto output = store.mimeFor(textGroupId);
        if (!output || output->text() != mime.text() || output->html() != mime.html())
            return false;
        store.setPinned(textGroupId, true);

        const QString firstPath = directory.path() + QStringLiteral("/source-one.txt");
        const QString secondPath = directory.path() + QStringLiteral("/source-two.txt");
        for (const QString &path : {firstPath, secondPath}) {
            QFile source(path);
            if (!source.open(QIODevice::WriteOnly) || source.write("source") != 6)
                return false;
        }
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(firstPath), QUrl::fromLocalFile(secondPath)});
        if (!store.add(&files) || store.groups().first().items.size() != 2)
            return false;
        const QString fileGroupId = store.groups().first().id;
        const QString firstItemId = store.groups().first().items.first().id;
        const QString secondItemId = store.groups().first().items.last().id;
        const auto fileOutput = store.mimeFor(fileGroupId);
        if (!fileOutput || fileOutput->urls().size() != 2)
            return false;
        const auto memberOutput = store.mimeFor(fileGroupId, secondItemId);
        if (!memberOutput || memberOutput->urls().size() != 1
            || memberOutput->urls().first().toLocalFile() != secondPath)
            return false;
        if (!store.removeItem(fileGroupId, firstItemId)
            || store.groups().first().items.size() != 1
            || store.groups().first().items.first().id != secondItemId)
            return false;
        store.remove(fileGroupId);
        if (!QFileInfo::exists(firstPath) || !QFileInfo::exists(secondPath))
            return false;

        QImage image(640, 480, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(QStringLiteral("#e7c844")));
        QMimeData imageMime;
        imageMime.setImageData(image);
        if (!store.add(&imageMime))
            return false;
        const QString imageGroupId = store.groups().first().id;
        const QString ownedImagePath = store.groups().first().items.first().value;
        if (!QFileInfo::exists(ownedImagePath))
            return false;
        store.remove(imageGroupId);
        if (QFileInfo::exists(ownedImagePath))
            return false;

        QMimeData internal;
        internal.setText(QStringLiteral("must not clone"));
        internal.setData(kInternalMime, QByteArrayLiteral("internal"));
        if (ShelfStore::canImport(&internal))
            return false;
    }
    ShelfStore reloaded(directory.path());
    if (reloaded.groups().size() != 1
        || reloaded.groups().first().id != textGroupId
        || !reloaded.groups().first().pinned
        || reloaded.groups().first().items.first().plainText != QStringLiteral("Native text round trip")) {
        return false;
    }

    QTemporaryDir reorderDirectory;
    if (!reorderDirectory.isValid())
        return false;
    QString firstReorderId;
    QString secondReorderId;
    QString thirdReorderId;
    {
        ShelfStore reorderStore(reorderDirectory.path());
        for (const QString &label : {QStringLiteral("First"), QStringLiteral("Second"), QStringLiteral("Third")}) {
            QMimeData item;
            item.setText(label);
            if (!reorderStore.add(&item))
                return false;
            if (label == QLatin1String("First")) firstReorderId = reorderStore.groups().first().id;
            if (label == QLatin1String("Second")) secondReorderId = reorderStore.groups().first().id;
            if (label == QLatin1String("Third")) thirdReorderId = reorderStore.groups().first().id;
        }
        const auto wholeCardMime = reorderStore.mimeFor(firstReorderId);
        const auto memberMime = reorderStore.mimeFor(firstReorderId,
            reorderStore.group(firstReorderId)->items.first().id);
        if (!wholeCardMime || ShelfStore::internalWholeGroupId(wholeCardMime.get()) != firstReorderId
            || !memberMime || !ShelfStore::internalWholeGroupId(memberMime.get()).isEmpty()) {
            return false;
        }
        if (!reorderStore.reorderGroup(firstReorderId, 0)
            || reorderStore.groups().first().id != firstReorderId
            || !reorderStore.reorderGroup(thirdReorderId, 2)
            || reorderStore.groups()[0].id != firstReorderId
            || reorderStore.groups()[1].id != secondReorderId
            || reorderStore.groups()[2].id != thirdReorderId) {
            return false;
        }
    }
    ShelfStore reorderedReloaded(reorderDirectory.path());
    if (reorderedReloaded.groups().size() != 3
        || reorderedReloaded.groups()[0].id != firstReorderId
        || reorderedReloaded.groups()[1].id != secondReorderId
        || reorderedReloaded.groups()[2].id != thirdReorderId) {
        return false;
    }

    // Exercise the compact-card layout with deliberately hostile label widths.
    // This guards against a regression where QLabel's minimum size pushed the
    // pin and remove controls outside the right edge of the card.
    ShelfGroup uiGroup = reloaded.groups().first();
    uiGroup.title = QString(300, QLatin1Char('W'));
    uiGroup.items.first().plainText = QString(500, QLatin1Char('x'));
    CardWidget card(uiGroup, &reloaded);
    card.setFixedWidth(370);
    card.show();
    QApplication::processEvents();
    auto *pinButton = card.findChild<QToolButton *>(QStringLiteral("pinButton"));
    auto *removeButton = card.findChild<QToolButton *>(QStringLiteral("removeButton"));
    const auto buttonFits = [&card](QWidget *button) {
        if (!button || !button->isVisible())
            return false;
        const QRect geometry(button->mapTo(&card, QPoint(0, 0)), button->size());
        return card.rect().contains(geometry.topLeft())
            && card.rect().contains(geometry.bottomRight());
    };
    if (!buttonFits(pinButton) || !buttonFits(removeButton))
        return false;

    bool pinSignalReceived = false;
    bool resultingPinValue = true;
    QObject::connect(&card, &CardWidget::pinChanged,
        [&pinSignalReceived, &resultingPinValue](const QString &, bool pinned) {
            pinSignalReceived = true;
            resultingPinValue = pinned;
        });
    pinButton->click();
    if (!pinSignalReceived || resultingPinValue)
        return false;

    auto *previewZone = card.findChild<QWidget *>(QStringLiteral("previewHoverZone"));
    auto *actionStrip = card.findChild<QWidget *>(QStringLiteral("cardActionStrip"));
    if (!previewZone || !actionStrip)
        return false;
    QEnterEvent previewEnter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QApplication::sendEvent(previewZone, &previewEnter);
    QEventLoop previewWait;
    QTimer::singleShot(470, &previewWait, &QEventLoop::quit);
    previewWait.exec();
    auto *hoverPreview = card.findChild<HoverPreview *>();
    if (!hoverPreview || !hoverPreview->isVisible())
        return false;
    if ((hoverPreview->windowFlags() & Qt::WindowType_Mask) != Qt::Tool
        || !hoverPreview->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus)) {
        return false;
    }
    QEvent previewZoneLeave(QEvent::Leave);
    QApplication::sendEvent(previewZone, &previewZoneLeave);
    QEnterEvent previewPopupEnter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QApplication::sendEvent(hoverPreview, &previewPopupEnter);
    QEventLoop previewBridgeWait;
    QTimer::singleShot(190, &previewBridgeWait, &QEventLoop::quit);
    previewBridgeWait.exec();
    if (!hoverPreview->isVisible())
        return false;
    QEnterEvent actionEnter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QApplication::sendEvent(actionStrip, &actionEnter);
    QApplication::processEvents();
    if (hoverPreview->isVisible())
        return false;

    bool removeSignalReceived = false;
    QObject::connect(&card, &CardWidget::removeRequested,
        [&removeSignalReceived](const QString &) { removeSignalReceived = true; });
    removeButton->click();
    QEventLoop animationWait;
    QTimer::singleShot(260, &animationWait, &QEventLoop::quit);
    animationWait.exec();
    if (!removeSignalReceived)
        return false;

    auto *clipboardMime = new QMimeData;
    clipboardMime->setText(QStringLiteral("Clipboard button insertion"));
    clipboardMime->setHtml(QStringLiteral("<b>Clipboard button insertion</b>"));
    QApplication::clipboard()->setMimeData(clipboardMime, QClipboard::Clipboard);
    {
        ShelfWindow shelf(&reloaded);
        auto *pasteButton = shelf.findChild<QToolButton *>(QStringLiteral("pasteButton"));
        auto *handleLock = shelf.handleLockButtonForTest();
        auto *handle = shelf.handleWidgetForTest();
        auto *dropHint = shelf.findChild<QLabel *>(QStringLiteral("dropHint"));
        if (!pasteButton || !handleLock || !handle || !dropHint || !dropHint->isHidden())
            return false;
        if (handleLock->isChecked())
            handleLock->click();
        handleLock->click();
        if (!shelf.pinnedOpenForTest() || !shelf.expandedForTest() || !handleLock->isChecked())
            return false;
        handleLock->click();
        if (shelf.pinnedOpenForTest() || handleLock->isChecked())
            return false;

        QMimeData draggedText;
        draggedText.setText(QStringLiteral("Handle drag forwarding"));
        QDragEnterEvent dragEnter(QPoint(5, 5), Qt::CopyAction, &draggedText,
            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(handle, &dragEnter);
        QApplication::processEvents();
        if (!dragEnter.isAccepted() || !shelf.expandedForTest()
            || dropHint->isHidden() || dropHint->text() != QStringLiteral("↓  Release to add")) {
            return false;
        }
        QDragLeaveEvent dragLeave;
        QApplication::sendEvent(handle, &dragLeave);
        QApplication::processEvents();
        if (!dragLeave.isAccepted() || !dropHint->isHidden())
            return false;

        pasteButton->click();
        QEventLoop shelfLayoutWait;
        QTimer::singleShot(260, &shelfLayoutWait, &QEventLoop::quit);
        shelfLayoutWait.exec();
        if (dropHint->isHidden() || dropHint->text() != QStringLiteral("✓  Added from clipboard"))
            return false;

        const QString clipboardGroupId = reloaded.groups().first().id;
        auto *reorderIndicator = shelf.findChild<QFrame *>(QStringLiteral("reorderIndicator"));
        auto *cardsHost = shelf.findChild<QWidget *>(QStringLiteral("cardsHost"));
        if (!reorderIndicator || !cardsHost)
            return false;
        QMimeData internalReorder;
        internalReorder.setData(kInternalMime, (clipboardGroupId + QLatin1Char('/')).toUtf8());
        const QPoint reorderPosition(shelf.width() / 2, shelf.height() - 18);
        QDragEnterEvent reorderEnter(reorderPosition, Qt::CopyAction, &internalReorder,
            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&shelf, &reorderEnter);
        QApplication::processEvents();
        if (!reorderEnter.isAccepted() || reorderIndicator->isHidden())
            return false;
        QDropEvent reorderDrop(QPointF(reorderPosition), Qt::CopyAction, &internalReorder,
            Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&shelf, &reorderDrop);
        QApplication::processEvents();
        if (!reorderDrop.isAccepted() || !reorderIndicator->isHidden()
            || reloaded.groups().last().id != clipboardGroupId) {
            return false;
        }
        QStringList cardOrder;
        for (int index = 0; index < cardsHost->layout()->count(); ++index) {
            if (auto *orderedCard = qobject_cast<CardWidget *>(cardsHost->layout()->itemAt(index)->widget()))
                cardOrder.append(orderedCard->groupId());
        }
        if (cardOrder.size() != 2 || cardOrder.last() != clipboardGroupId)
            return false;
        if (!reloaded.reorderGroup(clipboardGroupId, 0))
            return false;
        shelf.showFirstPreviewForTest();
        QApplication::processEvents();
        if (!shelf.firstPreviewAlignedForTest())
            return false;
    }
    QApplication::clipboard()->clear(QClipboard::Clipboard);
    return reloaded.groups().size() == 2
        && reloaded.groups().first().items.first().plainText == QStringLiteral("Clipboard button insertion")
        && reloaded.groups().first().items.first().html == QStringLiteral("<b>Clipboard button insertion</b>");
}

void addVisualTestData(ShelfStore &store)
{
    if (!store.groups().isEmpty())
        return;
    QMimeData files;
    files.setUrls({
        QUrl::fromLocalFile(QStringLiteral("/etc/hosts")),
        QUrl::fromLocalFile(QStringLiteral("/etc/passwd")),
        QUrl::fromLocalFile(QStringLiteral("/etc/fstab")),
    });
    store.add(&files);

    QImage image(960, 540, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#2d6f9f")));
    QPainter painter(&image);
    painter.setPen(QColor(QStringLiteral("#f5d85b")));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 42, QFont::Bold));
    painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("PasteShelf Native"));
    painter.end();
    QMimeData imageMime;
    imageMime.setImageData(image);
    store.add(&imageMime);

    QMimeData text;
    text.setText(QStringLiteral(
        "This is a deliberately long text clipping used to verify the compact card and floating preview. "
        "The card should remain short and readable, while hovering reveals this complete text in a separate panel.\n\n"
        "Pinned cards stay on the shelf after a successful drag. Unpinned cards shrink and fade away. "
        "The preview remains selectable and scrollable for substantially longer snippets."));
    text.setHtml(QStringLiteral("<p><b>Long preview test</b></p><p>%1</p>").arg(text.text().toHtmlEscaped()));
    store.add(&text);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("PasteShelf"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("io.github.pasteshelf"));
    QCoreApplication::setApplicationName(QStringLiteral("PasteShelfNative"));
    QApplication::setApplicationDisplayName(QStringLiteral("PasteShelf Native"));
    QApplication::setDesktopFileName(QString::fromLatin1(kDesktopId));
    QApplication::setQuitOnLastWindowClosed(false);

    const QStringList arguments = app.arguments();
    if (arguments.contains(QStringLiteral("--self-test"))) {
        const bool passed = runSelfTest();
        qInfo("PasteShelf Native self-test: %s", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }

    SingleInstance instance;
    if (!arguments.contains(QStringLiteral("--no-single-instance")) && !instance.acquire())
        return 0;

    const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    ShelfStore store(dataRoot);
    if (arguments.contains(QStringLiteral("--visual-test"))
        || arguments.contains(QStringLiteral("--visual-stack-test")))
        addVisualTestData(store);
    KWinPositioner positioner;
    if (!arguments.contains(QStringLiteral("--no-kwin")))
        positioner.start();
    ShelfWindow window(&store);
    QObject::connect(&instance, &SingleInstance::showRequested, &window, &ShelfWindow::openShelf);
    window.show();

    if (arguments.contains(QStringLiteral("--animation-stress-test"))) {
        window.runAnimationStressTest();
        QTimer::singleShot(700, &app, [&app] {
            app.exit(0);
        });
    } else if (arguments.contains(QStringLiteral("--anchor-animation-test"))) {
        auto anchor = std::make_shared<QPoint>();
        auto anchorStable = std::make_shared<bool>(true);
        auto *anchorSampler = new QTimer(&app);
        anchorSampler->setInterval(8);
        QObject::connect(anchorSampler, &QTimer::timeout, &window, [&window, anchor, anchorStable] {
            if (window.handlePositionForTest() != *anchor)
                *anchorStable = false;
        });
        QTimer::singleShot(120, &window, [&window, anchor, anchorStable, anchorSampler] {
            // Wayland intentionally withholds compositor-assigned global
            // coordinates from clients. The compositor geometry is verified
            // separately; offscreen/X11 can validate the exact edge point.
            *anchor = QGuiApplication::platformName() == QLatin1String("wayland")
                ? window.handlePositionForTest()
                : window.expectedHandlePositionForTest();
            *anchorStable = window.handlePositionForTest() == *anchor;
            anchorSampler->start();
            window.openShelf();
        });
        QTimer::singleShot(430, &app, [&window, &app, anchor, anchorStable] {
            const bool anchoredOpen = window.handleVisibleForTest()
                && window.panelVisibleForTest()
                && window.handlePositionForTest() == *anchor
                && *anchorStable;
            if (!anchoredOpen) {
                app.exit(1);
                return;
            }
            window.closeShelf();
        });
        QTimer::singleShot(730, &app, [&window, &app, anchor, anchorStable, anchorSampler] {
            anchorSampler->stop();
            const bool anchoredClosed = window.handleVisibleForTest()
                && !window.panelVisibleForTest()
                && !window.expandedForTest()
                && window.handlePositionForTest() == *anchor
                && *anchorStable;
            app.exit(anchoredClosed ? 0 : 1);
        });
    } else if (arguments.contains(QStringLiteral("--hover-hold-test"))) {
        window.runHoverHoldTest();
        QTimer::singleShot(1850, &app, [&window, &app] {
            app.exit(window.expandedForTest() && window.handleFillForTest() > 0.99 ? 0 : 1);
        });
    } else if (arguments.contains(QStringLiteral("--hover-leave-test"))) {
        window.runHoverLeaveTest();
        auto holdObserved = std::make_shared<bool>(false);
        auto drainObserved = std::make_shared<bool>(false);
        auto resetObserved = std::make_shared<bool>(false);
        auto *fillSampler = new QTimer(&app);
        fillSampler->setInterval(40);
        QObject::connect(fillSampler, &QTimer::timeout, &window, [&window, drainObserved] {
            const qreal fill = window.handleFillForTest();
            if (fill > 0.05 && fill < 0.95)
                *drainObserved = true;
        });
        QTimer::singleShot(920, &app, [&window, resetObserved] {
            *resetObserved = window.handleFillForTest() > 0.99 && window.expandedForTest();
        });
        QTimer::singleShot(2200, &app, [&window, holdObserved] {
            *holdObserved = window.handleFillForTest() > 0.99 && window.expandedForTest();
        });
        QTimer::singleShot(2500, fillSampler, qOverload<>(&QTimer::start));
        QTimer::singleShot(3850, fillSampler, &QTimer::stop);
        QTimer::singleShot(4300, &app,
            [&window, &app, holdObserved, drainObserved, resetObserved, fillSampler] {
            fillSampler->stop();
            const bool closedAndDrained = !window.expandedForTest()
                && window.handleFillForTest() < 0.01;
            if (!*holdObserved || !*drainObserved || !*resetObserved || !closedAndDrained) {
                qWarning() << "Hover countdown test failed"
                           << "hold=" << *holdObserved
                           << "drain=" << *drainObserved
                           << "reset=" << *resetObserved
                           << "expanded=" << window.expandedForTest()
                           << "fill=" << window.handleFillForTest();
            }
            const int exitCode = !*holdObserved ? 2
                : (!*drainObserved ? 3 : (!*resetObserved ? 4 : (!closedAndDrained ? 5 : 0)));
            app.exit(exitCode);
        });
    } else if (arguments.contains(QStringLiteral("--smoke-test"))) {
        QTimer::singleShot(500, &app, &QCoreApplication::quit);
    } else if (arguments.contains(QStringLiteral("--visual-countdown-test"))) {
        window.runVisualCountdownTest();
        QTimer::singleShot(3800, &app, &QCoreApplication::quit);
    } else if (arguments.contains(QStringLiteral("--visual-test"))) {
        QTimer::singleShot(80, &window, &ShelfWindow::holdOpenForTest);
        QTimer::singleShot(450, &window, &ShelfWindow::showFirstPreviewForTest);
        QTimer::singleShot(650, &window, &ShelfWindow::saveVisualTestImages);
    } else if (arguments.contains(QStringLiteral("--visual-stack-test"))) {
        QTimer::singleShot(80, &window, &ShelfWindow::holdOpenForTest);
        QTimer::singleShot(450, &window, &ShelfWindow::expandFirstStackForTest);
        QTimer::singleShot(650, &window, &ShelfWindow::saveVisualTestImages);
    }
    return app.exec();
}

#include "main.moc"
