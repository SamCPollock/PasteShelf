#include <QApplication>
#include <QBoxLayout>
#include <QBuffer>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
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
#include <QHoverEvent>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
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
#include <QPlainTextEdit>
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
#include <QTextDocument>
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

constexpr auto kInternalMime = "application/x-pastashelf-item";
constexpr auto kDesktopId = "io.github.pastashelf.PastaShelf";
constexpr auto kBundledIcon = ":/assets/io.github.pastashelf.PastaShelf.png";
constexpr qint64 kMaxOwnedPayloadBytes = 100LL * 1024LL * 1024LL;

QString runtimeDirectory()
{
    QString directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(directory);
    return directory;
}

Qt::WindowFlags shelfSurfaceFlags()
{
#ifdef Q_OS_LINUX
    // These are utility surfaces for a tray application, not independent app
    // windows. KWin still positions them through the script below, while the
    // tool type keeps Plasma launchers and window searches from listing them.
    return Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
#else
    // On macOS Qt::Tool maps to an NSPanel that normally disappears whenever
    // the application is inactive, which would hide PastaShelf's edge handle.
    return Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint;
#endif
}

QString migrateLegacyData()
{
    const QString current = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (QFileInfo::exists(current))
        return current;

    const QDir dataHome(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    const QStringList candidates = {
        dataHome.filePath(QStringLiteral("PastaShelf/PastaShelfNative")),
        dataHome.filePath(QStringLiteral("PasteShelf/PasteShelfNative")),
    };
    QString legacy;
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            legacy = candidate;
            break;
        }
    }
    if (legacy.isEmpty())
        return current;

    QDir().mkpath(QFileInfo(current).absolutePath());
    if (!QDir().rename(legacy, current)) {
        qWarning("PastaShelf could not migrate its existing data; using it in place.");
        return legacy;
    }
    QDir().rmdir(QFileInfo(legacy).absolutePath());
    return current;
}

void migrateLegacySettings()
{
    QSettings current(QStringLiteral("PastaShelf"), QStringLiteral("PastaShelf"));
    if (!current.allKeys().isEmpty())
        return;

    const QList<QPair<QString, QString>> candidates = {
        {QStringLiteral("PastaShelf"), QStringLiteral("PastaShelfNative")},
        {QStringLiteral("PasteShelf"), QStringLiteral("PasteShelfNative")},
    };
    for (const auto &[organization, application] : candidates) {
        QSettings legacy(organization, application);
        const QStringList keys = legacy.allKeys();
        if (keys.isEmpty())
            continue;
        for (const QString &key : keys)
            current.setValue(key, legacy.value(key));
        current.sync();
        return;
    }
}

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
        QDir().mkpath(m_root);
        QDir().mkpath(m_content);
        const auto directoryPermissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner;
        QFile::setPermissions(m_root, directoryPermissions);
        QFile::setPermissions(m_content, directoryPermissions);
        if (QFileInfo::exists(m_statePath)) {
            QFile::setPermissions(m_statePath,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
        load();
        if (m_storageWritable)
            cleanupOrphans();
    }

    const QList<ShelfGroup> &groups() const { return m_groups; }

    const ShelfGroup *group(const QString &id) const
    {
        const auto it = std::find_if(m_groups.cbegin(), m_groups.cend(),
            [&](const ShelfGroup &entry) { return entry.id == id; });
        return it == m_groups.cend() ? nullptr : &*it;
    }

    static bool hasImageOffer(const QMimeData *mime)
    {
        if (!mime)
            return false;
        if (mime->hasImage())
            return true;
        const QStringList formats = mime->formats();
        return std::any_of(formats.cbegin(), formats.cend(),
            [](const QString &format) { return format.startsWith(QLatin1String("image/")); });
    }

    static QImage imageFromMime(const QMimeData *mime)
    {
        if (!mime)
            return {};
        if (mime->hasImage()) {
            QImage image = qvariant_cast<QImage>(mime->imageData());
            if (!image.isNull())
                return image;
            const QPixmap pixmap = qvariant_cast<QPixmap>(mime->imageData());
            if (!pixmap.isNull())
                return pixmap.toImage();
        }
        for (const QString &format : mime->formats()) {
            if (!format.startsWith(QLatin1String("image/")))
                continue;
            const QImage image = QImage::fromData(mime->data(format));
            if (!image.isNull())
                return image;
        }
        return {};
    }

    static bool canImport(const QMimeData *mime)
    {
        return mime && !mime->hasFormat(kInternalMime)
            && (mime->hasUrls() || hasImageOffer(mime) || mime->hasHtml() || mime->hasText());
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

    static bool isImportableUrl(const QUrl &url)
    {
        if (!url.isValid())
            return false;
        if (url.isLocalFile())
            return QFileInfo(url.toLocalFile()).isAbsolute();
        if (url.isRelative() || url.scheme().isEmpty())
            return false;
        if (!url.host().isEmpty())
            return true;

        // Opaque URLs such as mailto:, magnet:, urn:, and custom app links do
        // not have a host. They still cannot contain unescaped whitespace.
        // This distinction is important for browser drags: Qt permissively
        // parses page titles such as "Video: Funny Cats - YouTube" as a URL
        // with the invented scheme "video".
        const QString display = url.toDisplayString();
        const qsizetype separator = display.indexOf(QLatin1Char(':'));
        if (separator < 1 || separator == display.size() - 1)
            return false;
        const QString payload = display.sliced(separator + 1);
        return std::none_of(payload.cbegin(), payload.cend(),
            [](QChar character) { return character.isSpace(); });
    }

    static QList<QUrl> sanitizedUrls(const QList<QUrl> &offeredUrls, QString *companionTitle = nullptr)
    {
        QList<QUrl> urls;
        QString title;
        for (const auto &url : offeredUrls) {
            if (isImportableUrl(url)) {
                if (!urls.contains(url))
                    urls.append(url);
                continue;
            }
            const QString candidate = url.toDisplayString().trimmed();
            if (title.isEmpty() && !candidate.isEmpty() && !candidate.contains(QLatin1Char('\n')))
                title = candidate;
        }
        if (companionTitle)
            *companionTitle = title;
        return urls;
    }

    bool add(const QMimeData *mime)
    {
        if (!canImport(mime))
            return false;
        if ((mime->hasText() && mime->text().toUtf8().size() > kMaxOwnedPayloadBytes)
            || (mime->hasHtml() && mime->html().toUtf8().size() > kMaxOwnedPayloadBytes)) {
            return false;
        }

        ShelfGroup group;
        group.id = QUuid::createUuid().toString(QUuid::WithoutBraces);

        const QImage offeredImage = imageFromMime(mime);
        QString companionTitle;
        const QList<QUrl> offeredUrls = offeredImage.isNull() && mime->hasUrls()
            ? sanitizedUrls(mime->urls(), &companionTitle)
            : QList<QUrl>();

        if (!offeredUrls.isEmpty()) {
            for (const auto &url : offeredUrls) {
                ShelfItem item;
                item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                if (url.isLocalFile()) {
                    const QFileInfo info(url.toLocalFile());
                    item.kind = info.isDir() ? QStringLiteral("folder") : QStringLiteral("file");
                    item.title = info.fileName().isEmpty() ? info.absoluteFilePath() : info.fileName();
                    item.value = info.absoluteFilePath();
                    if (info.isFile() && isEphemeralPath(item.value)) {
                        QString ownedPath;
                        if (!materializeFile(info, item.id, &ownedPath)) {
                            removeOwned(group);
                            return false;
                        }
                        item.value = ownedPath;
                        item.owned = true;
                    }
                } else {
                    item.kind = QStringLiteral("url");
                    item.title = url.fileName().isEmpty() ? url.host() : url.fileName();
                    if (item.title.isEmpty()) item.title = compactText(url.toDisplayString());
                    item.value = url.toString();
                }
                group.items.append(item);
            }
            if (group.items.size() == 1 && group.items.first().kind == QLatin1String("url")
                && !companionTitle.isEmpty()) {
                group.items.first().title = compactText(companionTitle);
            }
            if (group.items.size() == 1
                && group.items.first().kind == QLatin1String("url")) {
                auto &item = group.items.first();
                if (mime->hasText())
                    item.plainText = mime->text();
                if (mime->hasHtml())
                    item.html = mime->html();
            }
        } else if (!offeredImage.isNull()) {
            QByteArray pngPayload;
            QBuffer buffer(&pngPayload);
            if (!buffer.open(QIODevice::WriteOnly) || !offeredImage.save(&buffer, "PNG")
                || pngPayload.size() > kMaxOwnedPayloadBytes) {
                return false;
            }
            const QString fileName = group.id + QStringLiteral(".png");
            const QString path = m_content + QLatin1Char('/') + fileName;
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return false;
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            if (file.write(pngPayload) != pngPayload.size() || !file.commit())
                return false;
            ShelfItem item;
            item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            item.kind = QStringLiteral("image");
            item.title = QStringLiteral("Screenshot %1 × %2")
                .arg(offeredImage.width()).arg(offeredImage.height());
            item.value = path;
            item.owned = true;
            if (mime->hasText())
                item.plainText = mime->text();
            if (mime->hasHtml())
                item.html = mime->html();
            group.items.append(item);
        } else if (mime->hasHtml() || mime->hasText()) {
            ShelfItem item;
            item.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            item.kind = mime->hasHtml() ? QStringLiteral("html") : QStringLiteral("text");
            item.plainText = mime->text();
            item.html = mime->hasHtml() ? mime->html() : QString();
            if (item.plainText.isEmpty() && !item.html.isEmpty()) {
                QTextDocument document;
                document.setHtml(item.html);
                item.plainText = document.toPlainText();
            }
            item.title = compactText(item.plainText.isEmpty() ? QStringLiteral("Rich text") : item.plainText);
            group.items.append(item);
        } else {
            return false;
        }

        if (group.items.isEmpty())
            return false;
        group.title = group.items.size() == 1
            ? group.items.first().title
            : QStringLiteral("%1 items").arg(group.items.size());
        m_groups.prepend(group);
        if (save())
            return true;
        m_groups.removeFirst();
        removeOwned(group);
        return false;
    }

    bool setPinned(const QString &id, bool pinned)
    {
        for (auto &group : m_groups) {
            if (group.id == id) {
                if (group.pinned == pinned)
                    return true;
                const bool previous = group.pinned;
                group.pinned = pinned;
                if (save())
                    return true;
                group.pinned = previous;
                return false;
            }
        }
        return false;
    }

    bool updateText(const QString &id, const QString &text)
    {
        if (text.trimmed().isEmpty())
            return false;
        for (auto &group : m_groups) {
            if (group.id != id)
                continue;
            if (group.items.size() != 1)
                return false;
            auto &item = group.items.first();
            if (item.kind != QLatin1String("text") && item.kind != QLatin1String("html"))
                return false;

            const ShelfGroup original = group;
            item.kind = QStringLiteral("text");
            item.title = compactText(text);
            item.value.clear();
            item.plainText = text;
            item.html.clear();
            item.owned = false;
            group.title = item.title;
            if (save())
                return true;
            group = original;
            return false;
        }
        return false;
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
            const ShelfGroup removed = m_groups.takeAt(index);
            if (!save()) {
                m_groups.insert(index, removed);
                return false;
            }
            removeOwned(removed);
            return true;
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
                const ShelfGroup original = group;
                const ShelfItem removed = group.items[itemIndex];
                group.items.removeAt(itemIndex);
                group.title = group.items.size() == 1
                    ? group.items.first().title
                    : QStringLiteral("%1 items").arg(group.items.size());
                if (!save()) {
                    group = original;
                    return false;
                }
                removeOwned(removed);
                return true;
            }
        }
        return false;
    }

    bool clear()
    {
        const auto removed = m_groups;
        m_groups.clear();
        if (!save()) {
            m_groups = removed;
            return false;
        }
        for (const auto &group : removed)
            removeOwned(group);
        return true;
    }

    std::unique_ptr<QMimeData> mimeFor(const QString &id, const QString &itemId = {}) const
    {
        const ShelfGroup *entry = group(id);
        if (!entry)
            return {};
        auto mime = std::make_unique<QMimeData>();
        mime->setData(kInternalMime, (id + QLatin1Char('/') + itemId).toUtf8());

        QList<QUrl> urls;
        bool hasPayload = false;
        for (const auto &item : entry->items) {
            if (!itemId.isEmpty() && item.id != itemId)
                continue;
            if (item.kind == QLatin1String("file") || item.kind == QLatin1String("folder")) {
                if (QFileInfo::exists(item.value)) {
                    urls.append(QUrl::fromLocalFile(item.value));
                    hasPayload = true;
                }
            } else if (item.kind == QLatin1String("url")) {
                const QUrl url(item.value);
                if (isImportableUrl(url)) {
                    urls.append(url);
                    hasPayload = true;
                }
            }
            else if (item.kind == QLatin1String("image")) {
                QImageReader reader(item.value);
                const QImage image = reader.read();
                if (!image.isNull()) {
                    mime->setImageData(image);
                    urls.append(QUrl::fromLocalFile(item.value));
                    hasPayload = true;
                }
            } else {
                if (!item.plainText.isEmpty()) {
                    mime->setText(item.plainText);
                    hasPayload = true;
                }
                if (!item.html.isEmpty()) {
                    mime->setHtml(item.html);
                    hasPayload = true;
                }
            }
            if (item.kind != QLatin1String("text") && item.kind != QLatin1String("html")) {
                if (!item.plainText.isEmpty()) {
                    mime->setText(item.plainText);
                    hasPayload = true;
                }
                if (!item.html.isEmpty()) {
                    mime->setHtml(item.html);
                    hasPayload = true;
                }
            }
        }
        if (!urls.isEmpty())
            mime->setUrls(urls);
        if (!hasPayload)
            return {};
        return mime;
    }

    std::unique_ptr<QMimeData> clipboardMimeFor(const QString &id, const QString &itemId = {}) const
    {
        auto mime = mimeFor(id, itemId);
        if (mime)
            mime->removeFormat(QString::fromLatin1(kInternalMime));
        return mime;
    }

    Qt::DropActions outboundDragActions(const QString &id, const QString &itemId = {}) const
    {
        const ShelfGroup *entry = group(id);
        if (!entry)
            return Qt::IgnoreAction;

        // Moving a pinned card would immediately make the retained reference
        // stale. Likewise, PastaShelf-owned payloads must remain in its private
        // content directory until their card is removed. Only advertise Move
        // when every selected item is an existing, ordinary file-system entry.
        bool selectedAny = false;
        bool canMove = !entry->pinned;
        for (const auto &item : entry->items) {
            if (!itemId.isEmpty() && item.id != itemId)
                continue;
            selectedAny = true;
            if (item.owned
                || (item.kind != QLatin1String("file") && item.kind != QLatin1String("folder"))
                || !QFileInfo::exists(item.value)) {
                canMove = false;
            }
        }
        if (!selectedAny)
            return Qt::IgnoreAction;
        return canMove ? Qt::DropActions(Qt::CopyAction | Qt::MoveAction)
                       : Qt::DropActions(Qt::CopyAction);
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
    static bool pathWithin(const QString &path, const QString &root)
    {
        if (path.isEmpty() || root.isEmpty())
            return false;
        const QString cleanPath = QDir::cleanPath(path);
        const QString cleanRoot = QDir::cleanPath(root);
        return cleanPath == cleanRoot
            || cleanPath.startsWith(cleanRoot + QDir::separator());
    }

    bool isEphemeralPath(const QString &path) const
    {
        const QString canonicalPath = QFileInfo(path).canonicalFilePath();
        if (canonicalPath.isEmpty())
            return false;
        const QString ownRoot = QFileInfo(m_root).canonicalFilePath();
        if (pathWithin(canonicalPath, ownRoot))
            return false;

        QStringList ephemeralRoots = {QFileInfo(QDir::tempPath()).canonicalFilePath()};
        const QString runtimeRoot = QStandardPaths::writableLocation(
            QStandardPaths::RuntimeLocation);
        if (!runtimeRoot.isEmpty())
            ephemeralRoots.append(QFileInfo(runtimeRoot).canonicalFilePath());
        return std::any_of(ephemeralRoots.cbegin(), ephemeralRoots.cend(),
            [&canonicalPath](const QString &root) { return pathWithin(canonicalPath, root); });
    }

    bool materializeFile(const QFileInfo &sourceInfo, const QString &itemId,
        QString *ownedPath) const
    {
        if (!ownedPath || sourceInfo.size() < 0
            || sourceInfo.size() > kMaxOwnedPayloadBytes) {
            return false;
        }
        QFile source(sourceInfo.absoluteFilePath());
        if (!source.open(QIODevice::ReadOnly))
            return false;

        const QString safeName = sourceInfo.fileName().isEmpty()
            ? QStringLiteral("payload") : sourceInfo.fileName();
        const QString destination = m_content + QLatin1Char('/') + itemId
            + QLatin1Char('-') + safeName;
        QSaveFile output(destination);
        if (!output.open(QIODevice::WriteOnly))
            return false;
        output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        constexpr qint64 chunkSize = 1024 * 1024;
        qint64 copiedBytes = 0;
        while (!source.atEnd()) {
            const QByteArray chunk = source.read(chunkSize);
            if (chunk.isEmpty() && source.error() != QFileDevice::NoError)
                return false;
            copiedBytes += chunk.size();
            if (copiedBytes > kMaxOwnedPayloadBytes)
                return false;
            if (output.write(chunk) != chunk.size())
                return false;
        }
        if (!output.commit())
            return false;
        *ownedPath = destination;
        return true;
    }

    void load()
    {
        QFile file(m_statePath);
        if (!file.open(QIODevice::ReadOnly))
            return;
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || document.object().value(QStringLiteral("version")).toInt(-1) != 1) {
            m_storageWritable = false;
            qWarning("PastaShelf left an invalid or unsupported state file untouched.");
            return;
        }
        bool normalizedLegacyUrls = false;
        for (const auto &value : document.object().value(QStringLiteral("groups")).toArray()) {
            auto group = ShelfGroup::fromJson(value.toObject());
            normalizedLegacyUrls = normalizeLegacyUrlGroup(group) || normalizedLegacyUrls;
            if (!group.id.isEmpty() && !group.items.isEmpty())
                m_groups.append(std::move(group));
        }
        if (normalizedLegacyUrls)
            save();
    }

    void cleanupOrphans() const
    {
        QSet<QString> referenced;
        for (const auto &group : m_groups) {
            for (const auto &item : group.items) {
                if (item.owned && isOwnedPayloadPath(item.value))
                    referenced.insert(QFileInfo(item.value).absoluteFilePath());
            }
        }
        const QFileInfoList contentFiles = QDir(m_content).entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo &file : contentFiles) {
            if (!referenced.contains(file.absoluteFilePath()))
                QFile::remove(file.absoluteFilePath());
        }
    }

    bool save() const
    {
        if (!m_storageWritable)
            return false;
        QJsonArray array;
        for (const auto &group : m_groups)
            array.append(group.toJson());
        QSaveFile file(m_statePath);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        const QByteArray payload = QJsonDocument(QJsonObject{
            {QStringLiteral("version"), 1},
            {QStringLiteral("groups"), array},
        }).toJson(QJsonDocument::Compact);
        if (file.write(payload) != payload.size())
            return false;
        return file.commit();
    }

    bool isOwnedPayloadPath(const QString &path) const
    {
        if (path.isEmpty())
            return false;
        const QFileInfo candidate(path);
        return candidate.isAbsolute()
            && candidate.absolutePath() == QDir(m_content).absolutePath();
    }

    void removeOwned(const ShelfItem &item) const
    {
        if (item.owned && isOwnedPayloadPath(item.value))
            QFile::remove(item.value);
    }

    void removeOwned(const ShelfGroup &group) const
    {
        for (const auto &item : group.items) {
            removeOwned(item);
        }
    }

    static bool normalizeLegacyUrlGroup(ShelfGroup &group)
    {
        if (group.items.size() < 2)
            return false;
        QList<ShelfItem> validItems;
        QString companionTitle;
        for (const auto &item : std::as_const(group.items)) {
            if (item.kind != QLatin1String("url"))
                return false;
            if (isImportableUrl(QUrl(item.value))) {
                validItems.append(item);
            } else if (companionTitle.isEmpty()) {
                companionTitle = item.value.trimmed();
            }
        }
        if (validItems.isEmpty() || validItems.size() == group.items.size())
            return false;
        group.items = std::move(validItems);
        if (group.items.size() == 1) {
            if (!companionTitle.isEmpty())
                group.items.first().title = compactText(companionTitle);
            group.title = group.items.first().title;
        } else {
            group.title = QStringLiteral("%1 items").arg(group.items.size());
        }
        return true;
    }

    QString m_root;
    QString m_content;
    QString m_statePath;
    QList<ShelfGroup> m_groups;
    bool m_storageWritable = true;
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
#ifdef Q_OS_MACOS
    if (QProcess::startDetached(QStringLiteral("/usr/bin/open"),
            {QStringLiteral("-R"), info.absoluteFilePath()})) {
        return;
    }
#endif
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
        setWindowTitle(QStringLiteral("PastaShelfPreview@%1,%2,%3,%4")
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
        auto *copy = actionButton(QStringLiteral("edit-copy"), QStringLiteral("C"),
            QStringLiteral("Copy this item"));
        copy->setObjectName(QStringLiteral("memberCopyButton"));
        connect(copy, &QToolButton::clicked, this, [this] {
            emit copyRequested(m_groupId, m_item.id);
        });
        layout->addWidget(copy);
        auto *remove = actionButton(QStringLiteral("edit-delete"), QStringLiteral("×"), QStringLiteral("Remove this item"));
        connect(remove, &QToolButton::clicked, this, [this] { poof(); });
        layout->addWidget(remove);
    }

signals:
    void removeRequested(const QString &groupId, const QString &itemId);
    void copyRequested(const QString &groupId, const QString &itemId);
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
        const auto action = drag->exec(
            m_store->outboundDragActions(m_groupId, m_item.id), Qt::CopyAction);
        emit dragActiveChanged(false);
        drag->deleteLater();
        setCursor(Qt::OpenHandCursor);
        if ((action == Qt::CopyAction || action == Qt::MoveAction) && !m_isPinned())
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

        auto *copyButton = actionButton(m_actionStrip, QStringLiteral("edit-copy"), QStringLiteral("C"),
            QStringLiteral("Copy to clipboard"));
        copyButton->setObjectName(QStringLiteral("copyButton"));
        connect(copyButton, &QToolButton::clicked, this, [this] {
            emit copyRequested(m_group.id, {});
        });
        actionLayout->addWidget(copyButton);

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
        m_singleClickTimer.setSingleShot(true);
        m_singleClickTimer.setInterval(QApplication::doubleClickInterval());
        connect(&m_singleClickTimer, &QTimer::timeout, this, [this] {
            if (m_expandable)
                setExpanded(!m_expanded);
        });
        if (isEditableText())
            m_previewHoverZone->setToolTip(QStringLiteral("Double-click to edit"));
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

    void setExpandedForTest(bool expanded)
    {
        setExpanded(expanded);
    }

    bool detailsExpandedForTest() const
    {
        return m_expanded && m_details && !m_details->isHidden()
            && m_details->maximumHeight() == m_detailsExpandedHeight;
    }

    void savePreviewForTest(const QString &path)
    {
        if (!m_previewPopup)
            showHoverPreview();
        if (m_previewPopup) {
            m_previewPopup->grab().save(path);
            qInfo() << "PastaShelf visual test card=" << frameGeometry()
                    << "preview=" << m_previewPopup->frameGeometry();
        }
    }

    QRect previewPlacementForTest() const
    {
        return m_previewPopup ? m_previewPopup->authoritativeGeometry() : QRect();
    }

signals:
    void pinChanged(const QString &id, bool pinned);
    void copyRequested(const QString &groupId, const QString &itemId);
    void removeRequested(const QString &id);
    void removeItemRequested(const QString &groupId, const QString &itemId);
    void dragActiveChanged(bool active);
    void editTextRequested(const QString &id, const QString &text);

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
            m_singleClickTimer.stop();
            m_pressPosition = event->position().toPoint();
            m_clickCandidate = true;
            setCursor(Qt::ClosedHandCursor);
        }
        QFrame::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        setCursor(Qt::OpenHandCursor);
        if (event->button() == Qt::LeftButton && m_clickCandidate && m_expandable) {
            if (isEditableText())
                m_singleClickTimer.start();
            else
                setExpanded(!m_expanded);
        }
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
        m_singleClickTimer.stop();
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
        const auto action = drag->exec(
            m_store->outboundDragActions(m_group.id), Qt::CopyAction);
        emit dragActiveChanged(false);
        drag->deleteLater();
        setCursor(Qt::OpenHandCursor);
        if ((action == Qt::CopyAction || action == Qt::MoveAction)
            && !m_internalReorderAccepted && !m_pinned) {
            poof(true);
        }
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && isEditableText()) {
            m_clickCandidate = false;
            m_singleClickTimer.stop();
            m_showPreviewTimer.stop();
            if (m_previewPopup)
                m_previewPopup->hide();
            emit editTextRequested(m_group.id, m_group.items.first().plainText);
            event->accept();
            return;
        }
        QFrame::mouseDoubleClickEvent(event);
    }

private:
    bool isEditableText() const
    {
        return m_group.items.size() == 1
            && (m_group.items.first().kind == QLatin1String("text")
                || m_group.items.first().kind == QLatin1String("html"));
    }

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
                connect(row, &MemberRow::copyRequested, this, &CardWidget::copyRequested);
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
        if (m_detailsAnimation) {
            auto *previous = m_detailsAnimation.data();
            m_detailsAnimation.clear();
            previous->stop();
            previous->deleteLater();
        }
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
        m_detailsAnimation = animation;
        connect(animation, &QPropertyAnimation::finished, this, [this, animation, expanded] {
            if (m_detailsAnimation != animation)
                return;
            m_detailsAnimation.clear();
            if (!expanded)
                m_details->hide();
            animation->deleteLater();
        });
        animation->start();
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
    QPointer<QPropertyAnimation> m_detailsAnimation;
    QTimer m_showPreviewTimer;
    QTimer m_hidePreviewTimer;
    QTimer m_singleClickTimer;
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
        m_scriptPath = runtime + QStringLiteral("/pastashelf-positioner.js");
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
    static QString pluginName() { return QStringLiteral("pastashelf-positioner"); }

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
    const match = caption.match(/^PastaShelf(?:Preview|Handle)?@(-?\d+),(-?\d+),(\d+),(\d+)/);
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
        : m_store(store), m_settings(QStringLiteral("PastaShelf"), QStringLiteral("PastaShelf"))
    {
        setAcceptDrops(true);
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setWindowFlags(shelfSurfaceFlags());
        setAttribute(Qt::WA_TranslucentBackground);
        setWindowIcon(QApplication::windowIcon());

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
        m_hoverOpenTimer.setSingleShot(true);
        m_hoverOpenTimer.setInterval(kHoverOpenDelayMs);
        connect(&m_hoverOpenTimer, &QTimer::timeout, this, [this] {
            if (!m_expanded && !m_hoveredWidgets.isEmpty() && !m_dragActive && !m_manualCollapse)
                openShelf();
        });
        m_toastTimer.setSingleShot(true);
        connect(&m_toastTimer, &QTimer::timeout, m_toast, &QWidget::hide);
#ifdef Q_OS_MACOS
        // Cocoa can occasionally omit an enter/leave transition for an
        // inactive borderless window, especially while another application
        // owns a native drag. Reconcile only the small edge handle at a low
        // frequency so hover-to-open remains dependable in those cases.
        m_pointerPresenceTimer.setInterval(100);
        connect(&m_pointerPresenceTimer, &QTimer::timeout,
            this, &ShelfWindow::reconcileMacPointerPresence);
        m_pointerPresenceTimer.start();
#endif

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
        // QObject disconnects receivers during its base destructor, but that
        // happens after ShelfWindow's members have already been destroyed.
        // Disconnect widget-destruction callbacks while both tracking sets
        // are still alive, including callbacks for children that are no
        // longer currently hovered.
        for (auto *widget : std::as_const(m_hoverTrackedWidgets))
            QObject::disconnect(widget, nullptr, this, nullptr);
        m_hoverTrackedWidgets.clear();
        m_hoveredWidgets.clear();
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
    static QPoint handleTopLeftForTest(const QRect &area, const QString &placement)
    {
        return handleTopLeftForArea(area, placement);
    }
    bool addTextNoteForTest(const QString &text) { return addTextNote(text); }
    bool editTextNoteForTest(const QString &id, const QString &text) { return editTextNote(id, text); }
    bool copyToClipboardForTest(const QString &groupId, const QString &itemId = {})
    {
        return copyToClipboard(groupId, itemId);
    }
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
        m_manualCollapse = false;
        m_hoverOpenTimer.stop();
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

    void shrinkShelf()
    {
        m_manualCollapse = true;
        m_hoverOpenTimer.stop();
        m_closeTimer.stop();
        m_drainDelayTimer.stop();
        if (m_pinnedOpen)
            setPinnedOpen(false);
        if (m_expanded)
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
        grab().save(QStringLiteral("/tmp/pastashelf-self-main.png"));
        m_handle->grab().save(QStringLiteral("/tmp/pastashelf-self-handle.png"));
        if (auto *card = findChild<CardWidget *>())
            card->savePreviewForTest(QStringLiteral("/tmp/pastashelf-self-preview.png"));
    }

    void runVisualCountdownTest()
    {
        m_ignoreHoverForTest = true;
        openShelf();
        QTimer::singleShot(300, this, [this] {
            m_hoveredWidgets.clear();
            m_handle->grab().save(QStringLiteral("/tmp/pastashelf-handle-full.png"));
            armCloseCountdown();
        });
        QTimer::singleShot(2550, this, [this] {
            m_handle->grab().save(QStringLiteral("/tmp/pastashelf-handle-half.png"));
        });
        QTimer::singleShot(3550, this, [this] {
            m_handle->grab().save(QStringLiteral("/tmp/pastashelf-handle-empty.png"));
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
            if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter
                || event->type() == QEvent::HoverMove) {
                notePointerEntered(widget);
            } else if (event->type() == QEvent::Leave
                || event->type() == QEvent::HoverLeave || event->type() == QEvent::Hide) {
                notePointerLeft(widget);
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void enterEvent(QEnterEvent *event) override
    {
        notePointerEntered(this);
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        notePointerLeft(this);
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
        // Open on drag presence even if a platform has not exposed its full
        // MIME payload yet. A later move/drop is still accepted only after
        // ShelfStore confirms that the data can be imported.
        openShelf();
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
    static constexpr int kHorizontalEdgeInset = 12;
    static constexpr int kCornerVerticalInset = 48;
    static constexpr int kCloseHoldMs = 1500;
    static constexpr int kCloseDrainMs = 1500;
    static constexpr int kHoverOpenDelayMs = 300;
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

    static QPoint handleTopLeftForArea(const QRect &area, const QString &placement)
    {
        int x = area.x() + (area.width() - kHandleWidth) / 2;
        int y = area.y() + (area.height() - kHandleHeight) / 2;
        if (placement.contains(QStringLiteral("left")))
            x = area.x() + kHorizontalEdgeInset;
        if (placement.contains(QStringLiteral("right")))
            x = area.x() + area.width() - kHandleWidth - kHorizontalEdgeInset;
        if (placement.contains(QStringLiteral("top")))
            y = area.y() + kCornerVerticalInset;
        if (placement.contains(QStringLiteral("bottom")))
            y = area.y() + area.height() - kHandleHeight - kCornerVerticalInset;
        return {x, y};
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

    void notePointerEntered(QWidget *widget)
    {
        if (!widget || !ownsWidget(widget))
            return;
        if (!widget->property("pastashelfHoverTracked").toBool()) {
            widget->setProperty("pastashelfHoverTracked", true);
            m_hoverTrackedWidgets.insert(widget);
            connect(widget, &QObject::destroyed, this, [this, widget] {
                m_hoverTrackedWidgets.remove(widget);
                m_hoveredWidgets.remove(widget);
                scheduleCloseAfterPointerLeave();
            });
        }
        const bool wasHovered = m_hoveredWidgets.contains(widget);
        m_hoveredWidgets.insert(widget);
        if (m_manualCollapse)
            return;
        if (!m_expanded) {
            if (!wasHovered && !m_hoverOpenTimer.isActive())
                m_hoverOpenTimer.start();
        } else {
            cancelCloseCountdown();
            if (m_animating)
                openShelf();
        }
    }

    void notePointerLeft(QWidget *widget)
    {
        if (!widget)
            return;
        m_hoveredWidgets.remove(widget);
        scheduleCloseAfterPointerLeave();
    }

    void reconcileMacPointerPresence()
    {
#ifdef Q_OS_MACOS
        if (!m_handle || !m_handle->isVisible())
            return;
        const QRect handleGeometry(m_handle->mapToGlobal(QPoint(0, 0)), m_handle->size());
        const bool pointerInside = handleGeometry.contains(QCursor::pos());
        if (pointerInside && QGuiApplication::mouseButtons() != Qt::NoButton && !m_expanded)
            openShelf();
        if (pointerInside == m_macPointerInsideHandle)
            return;
        m_macPointerInsideHandle = pointerInside;
        if (pointerInside) {
            notePointerEntered(m_handle);
            return;
        }
        for (auto it = m_hoveredWidgets.begin(); it != m_hoveredWidgets.end();) {
            QWidget *widget = *it;
            if (widget == m_handle || (widget && m_handle->isAncestorOf(widget)))
                it = m_hoveredWidgets.erase(it);
            else
                ++it;
        }
        scheduleCloseAfterPointerLeave();
#endif
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
            if (m_hoveredWidgets.isEmpty())
                m_hoverOpenTimer.stop();
            if (m_hoveredWidgets.isEmpty())
                m_manualCollapse = false;
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
        m_handle->setWindowFlags(shelfSurfaceFlags());
        m_handle->setAttribute(Qt::WA_TranslucentBackground);
        m_handle->setAttribute(Qt::WA_Hover);
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
        auto *heading = new QLabel(QStringLiteral("PastaShelf"), m_panel);
        heading->setObjectName(QStringLiteral("heading"));
        header->addWidget(heading);
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
        auto *newTextButton = new QToolButton(m_panel);
        newTextButton->setObjectName(QStringLiteral("newTextButton"));
        const QIcon addIcon = QIcon::fromTheme(QStringLiteral("list-add"));
        if (addIcon.isNull())
            newTextButton->setText(QStringLiteral("+"));
        else
            newTextButton->setIcon(addIcon);
        newTextButton->setToolTip(QStringLiteral("New text note"));
        newTextButton->setFixedSize(30, 30);
        newTextButton->setCursor(Qt::PointingHandCursor);
        connect(newTextButton, &QToolButton::clicked, this, &ShelfWindow::showNewTextNoteDialog);
        header->addWidget(newTextButton);
        m_shrinkButton = new QToolButton(m_panel);
        m_shrinkButton->setObjectName(QStringLiteral("shrinkButton"));
        m_shrinkButton->setToolTip(QStringLiteral("Shrink shelf"));
        m_shrinkButton->setFixedSize(30, 30);
        m_shrinkButton->setCursor(Qt::PointingHandCursor);
        connect(m_shrinkButton, &QToolButton::clicked, this, &ShelfWindow::shrinkShelf);
        header->addWidget(m_shrinkButton);
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

        m_toast = new QLabel(m_panel);
        m_toast->setObjectName(QStringLiteral("toast"));
        m_toast->setAlignment(Qt::AlignCenter);
        m_toast->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_toast->setGeometry(16, kPanelHeight - 52, kPanelWidth - 32, 36);
        m_toast->hide();

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
            #pasteButton { color: %3; background: %2; border: 1px solid %6; border-radius: 7px; padding: 2px 8px; font-weight: 600; }
            #pasteButton:hover { border-color: %5; background: %1; }
            #newTextButton { color: %3; background: %2; border: 1px solid %6; border-radius: 7px; font-size: 19px; font-weight: 600; }
            #newTextButton:hover { border-color: %5; background: %1; }
            #shrinkButton { color: %3; background: %2; border: 1px solid %6; border-radius: 7px; font-size: 19px; font-weight: 600; }
            #shrinkButton:hover { border-color: %5; background: %1; }
            #menuButton { border: 1px solid %6; border-radius: 7px; font-size: 19px; }
            #menuButton:hover { background: %2; }
            #dropHint { color: %4; background: %2; border: 1px solid %6; border-radius: 9px; }
            #dropHint[active="true"] { color: %3; border: 2px solid %5; background: %2; font-weight: 600; }
            #toast { color: %7; background: %5; border: 1px solid %6; border-radius: 9px; padding: 5px 10px; font-weight: 600; }
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
            #noteEditor { color: %3; background: %2; border: 1px solid %6; border-radius: 9px; padding: 8px; selection-background-color: %5; }
            #noteEditor:focus { border: 2px solid %5; }
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
                if (!m_store->setPinned(id, pinned)) {
                    rebuildCards();
                    showTransientHint(QStringLiteral("Could not save pin state"), 1500);
                }
            });
            connect(card, &CardWidget::copyRequested, this,
                [this](const QString &groupId, const QString &itemId) {
                    copyToClipboard(groupId, itemId);
                });
            connect(card, &CardWidget::editTextRequested,
                this, &ShelfWindow::showEditTextDialog);
            connect(card, &CardWidget::removeRequested, this, [this](const QString &id) {
                if (!m_store->remove(id))
                    showTransientHint(QStringLiteral("Could not remove item"), 1500);
                rebuildCards();
            });
            connect(card, &CardWidget::removeItemRequested, this,
                [this](const QString &groupId, const QString &itemId) {
                    if (!m_store->removeItem(groupId, itemId))
                        showTransientHint(QStringLiteral("Could not remove stack item"), 1500);
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
        m_dropHint->setText(active ? QStringLiteral("↓  Release to add") : QString());
        m_dropHint->setProperty("active", active);
        m_dropHint->style()->unpolish(m_dropHint);
        m_dropHint->style()->polish(m_dropHint);
        m_dropHint->setFixedHeight(active ? 42 : 0);
        m_dropHint->setVisible(active);
    }

    void showTransientHint(const QString &message, int duration)
    {
        m_toastTimer.stop();
        m_toast->setText(message);
        m_toast->show();
        m_toast->raise();
        m_toastTimer.start(duration);
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
        return handleTopLeftForArea(screen->availableGeometry(), m_placement);
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

        m_handle->setWindowTitle(QStringLiteral("PastaShelfHandle@%1,%2,%3,%4")
            .arg(handleX).arg(handleY).arg(kHandleWidth).arg(kHandleHeight));
        m_handle->move(handleX, handleY);

        const QPoint surfaceTopLeft = desiredSurfaceTopLeft();
        if (m_handleArrow)
            m_handleArrow->setText(handleIsOnLeft() ? QStringLiteral("›") : QStringLiteral("‹"));
        if (m_shrinkButton) {
            const QIcon shrinkIcon = QIcon::fromTheme(handleIsOnLeft()
                    ? QStringLiteral("go-previous")
                    : QStringLiteral("go-next"));
            if (shrinkIcon.isNull()) {
                m_shrinkButton->setIcon({});
                m_shrinkButton->setText(handleIsOnLeft() ? QStringLiteral("‹") : QStringLiteral("›"));
            } else {
                m_shrinkButton->setIcon(shrinkIcon);
                m_shrinkButton->setText(QString());
            }
        }
        layoutSurfaces();
        setWindowTitle(QStringLiteral("PastaShelf@%1,%2,%3,%4")
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
            QStringLiteral("Clear PastaShelf?"),
            QStringLiteral("Remove every item from the optimized test shelf? Original files will not be deleted."),
            QMessageBox::Cancel,
            this);
        auto *clearButton = confirmation.addButton(QStringLiteral("Clear all"), QMessageBox::DestructiveRole);
        confirmation.exec();
        if (confirmation.clickedButton() != clearButton)
            return;
        if (m_store->clear()) {
            rebuildCards();
        } else {
            showTransientHint(QStringLiteral("Could not clear the shelf"), 1500);
        }
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

    bool copyToClipboard(const QString &groupId, const QString &itemId = {})
    {
        auto mime = m_store->clipboardMimeFor(groupId, itemId);
        if (!mime) {
            showTransientHint(QStringLiteral("Could not copy item"), 1500);
            return false;
        }
        QApplication::clipboard()->setMimeData(mime.release(), QClipboard::Clipboard);
        showTransientHint(itemId.isEmpty()
                ? QStringLiteral("✓  Copied to clipboard")
                : QStringLiteral("✓  Item copied to clipboard"),
            1200);
        return true;
    }

    bool addTextNote(const QString &text)
    {
        if (text.trimmed().isEmpty())
            return false;
        QMimeData mime;
        mime.setText(text);
        if (!m_store->add(&mime)) {
            showTransientHint(QStringLiteral("Could not save text note"), 1500);
            return false;
        }
        rebuildCards();
        openShelf();
        showTransientHint(QStringLiteral("✓  Text note added"), 1200);
        return true;
    }

    bool editTextNote(const QString &id, const QString &text)
    {
        if (!m_store->updateText(id, text)) {
            showTransientHint(text.trimmed().isEmpty()
                    ? QStringLiteral("Text notes cannot be empty")
                    : QStringLiteral("Could not update text note"),
                1500);
            return false;
        }
        rebuildCards();
        openShelf();
        showTransientHint(QStringLiteral("✓  Text note updated"), 1200);
        return true;
    }

    void showNewTextNoteDialog()
    {
        showTextDialog({}, {});
    }

    void showEditTextDialog(const QString &id, const QString &text)
    {
        showTextDialog(id, text);
    }

    void showTextDialog(const QString &editingId, const QString &initialText)
    {
        const bool editing = !editingId.isEmpty();
        m_menuActive = true;
        cancelCloseCountdown();

        QDialog dialog(this);
        dialog.setWindowTitle(editing ? QStringLiteral("Edit text note") : QStringLiteral("New text note"));
        dialog.setModal(true);
        dialog.resize(430, 285);
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(10);

        auto *editor = new QPlainTextEdit(&dialog);
        editor->setObjectName(QStringLiteral("noteEditor"));
        editor->setPlaceholderText(QStringLiteral("Type a note to keep on the shelf…"));
        editor->setPlainText(initialText);
        editor->setTabChangesFocus(false);
        layout->addWidget(editor, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        auto *saveButton = buttons->addButton(
            editing ? QStringLiteral("Save changes") : QStringLiteral("Add note"),
            QDialogButtonBox::AcceptRole);
        saveButton->setDefault(true);
        saveButton->setEnabled(!initialText.trimmed().isEmpty());
        connect(editor, &QPlainTextEdit::textChanged, &dialog, [editor, saveButton] {
            saveButton->setEnabled(!editor->toPlainText().trimmed().isEmpty());
        });
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);

        editor->setFocus();
        const int result = dialog.exec();
        if (result == QDialog::Accepted) {
            if (editing)
                editTextNote(editingId, editor->toPlainText());
            else
                addTextNote(editor->toPlainText());
        }

        m_menuActive = false;
        if (!m_pinnedOpen)
            scheduleCloseAfterPointerLeave();
    }

    void createTray()
    {
        if (!QSystemTrayIcon::isSystemTrayAvailable())
            return;
        m_tray = new QSystemTrayIcon(windowIcon(), this);
        m_tray->setToolTip(QStringLiteral("PastaShelf"));
        auto *menu = new QMenu(this);
        QAction *newText = menu->addAction(QStringLiteral("New text note…"));
        connect(newText, &QAction::triggered, this, &ShelfWindow::showNewTextNoteDialog);
        QAction *clipboard = menu->addAction(QStringLiteral("Add clipboard"));
        connect(clipboard, &QAction::triggered, this, &ShelfWindow::addClipboardContent);
        QAction *show = menu->addAction(QStringLiteral("Show PastaShelf"));
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
    QToolButton *m_shrinkButton = nullptr;
    QWidget *m_panel = nullptr;
    QLabel *m_dropHint = nullptr;
    QLabel *m_toast = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_cardsHost = nullptr;
    QVBoxLayout *m_cardsLayout = nullptr;
    QLabel *m_empty = nullptr;
    QFrame *m_reorderIndicator = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    QPointer<QVariantAnimation> m_animation;
    QPointer<QVariantAnimation> m_handleFillAnimation;
    QSet<QWidget *> m_hoveredWidgets;
    QSet<QWidget *> m_hoverTrackedWidgets;
    QTimer m_closeTimer;
    QTimer m_drainDelayTimer;
    QTimer m_hoverOpenTimer;
    QTimer m_toastTimer;
    QTimer m_pointerPresenceTimer;
    QString m_internalReorderGroupId;
    QString m_placement;
    QString m_screenName;
    bool m_expanded = false;
    bool m_pinnedOpen = false;
    bool m_dragActive = false;
    bool m_menuActive = false;
    bool m_animating = false;
    bool m_manualCollapse = false;
    bool m_ignoreHoverForTest = false;
    bool m_macPointerInsideHandle = false;
    qreal m_openProgress = 0.0;
};

class SingleInstance final : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject *parent = nullptr)
        : QObject(parent),
          m_name(QStringLiteral("pastashelf-%1").arg(QString::number(getuid()))),
          m_lockPath(runtimeDirectory() + QLatin1Char('/') + m_name
                     + QStringLiteral(".lock")),
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
    const QRect placementArea(100, 50, 1920, 1080);
    if (ShelfWindow::handleTopLeftForTest(placementArea, QStringLiteral("top-left")) != QPoint(112, 98)
        || ShelfWindow::handleTopLeftForTest(placementArea, QStringLiteral("top-right")) != QPoint(1962, 98)
        || ShelfWindow::handleTopLeftForTest(placementArea, QStringLiteral("bottom-left")) != QPoint(112, 934)
        || ShelfWindow::handleTopLeftForTest(placementArea, QStringLiteral("bottom-right")) != QPoint(1962, 934)) {
        return false;
    }

    QTemporaryDir directory;
    if (!directory.isValid())
        return false;
    QString textGroupId;
    {
        ShelfStore store(directory.path());
        QMimeData mime;
        mime.setText(QStringLiteral("Text round trip"));
        mime.setHtml(QStringLiteral("<b>Text round trip</b>"));
        if (!store.add(&mime) || store.groups().size() != 1)
            return false;
        textGroupId = store.groups().first().id;
        const auto publicPermissionBits = QFileDevice::ReadGroup | QFileDevice::WriteGroup
            | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther
            | QFileDevice::ExeOther;
        if ((QFileInfo(directory.path() + QStringLiteral("/state.json")).permissions()
                & publicPermissionBits) != QFileDevice::Permissions{}) {
            return false;
        }
        const auto output = store.mimeFor(textGroupId);
        if (!output || output->text() != mime.text() || output->html() != mime.html())
            return false;
        const auto clipboardOutput = store.clipboardMimeFor(textGroupId);
        if (!clipboardOutput || clipboardOutput->text() != mime.text()
            || clipboardOutput->html() != mime.html()
            || clipboardOutput->hasFormat(QString::fromLatin1(kInternalMime))) {
            return false;
        }
        if (!store.setPinned(textGroupId, true))
            return false;
        if (!store.updateText(textGroupId, QStringLiteral("Edited copied text")))
            return false;
        const auto editedOutput = store.mimeFor(textGroupId);
        if (!editedOutput || editedOutput->text() != QStringLiteral("Edited copied text")
            || editedOutput->hasHtml() || !store.group(textGroupId)->pinned) {
            return false;
        }

        QMimeData htmlOnly;
        htmlOnly.setHtml(QStringLiteral("<p>HTML <b>only</b></p>"));
        if (!store.add(&htmlOnly))
            return false;
        const QString htmlOnlyGroupId = store.groups().first().id;
        const auto htmlOnlyOutput = store.mimeFor(htmlOnlyGroupId);
        if (!htmlOnlyOutput || htmlOnlyOutput->text() != QStringLiteral("HTML only")
            || htmlOnlyOutput->html() != htmlOnly.html()
            || !store.remove(htmlOnlyGroupId)) {
            return false;
        }

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
        const Qt::DropActions copyAndMove = Qt::CopyAction | Qt::MoveAction;
        if (store.outboundDragActions(fileGroupId) != copyAndMove
            || store.outboundDragActions(fileGroupId, secondItemId) != copyAndMove
            || store.outboundDragActions(textGroupId) != Qt::CopyAction
            || !store.setPinned(fileGroupId, true)
            || store.outboundDragActions(fileGroupId) != Qt::CopyAction
            || !store.setPinned(fileGroupId, false)) {
            return false;
        }
        CardWidget stackCard(*store.group(fileGroupId), &store);
        stackCard.setFixedWidth(370);
        stackCard.expandForTest();
        bool memberCopySignalReceived = false;
        QString copiedMemberGroupId;
        QString copiedMemberItemId;
        QObject::connect(&stackCard, &CardWidget::copyRequested,
            [&memberCopySignalReceived, &copiedMemberGroupId, &copiedMemberItemId](
                const QString &groupId, const QString &itemId) {
                memberCopySignalReceived = true;
                copiedMemberGroupId = groupId;
                copiedMemberItemId = itemId;
            });
        const auto memberCopyButtons = stackCard.findChildren<QToolButton *>(
            QStringLiteral("memberCopyButton"));
        if (memberCopyButtons.size() != 2)
            return false;
        memberCopyButtons.last()->click();
        if (!memberCopySignalReceived || copiedMemberGroupId != fileGroupId
            || copiedMemberItemId != secondItemId) {
            return false;
        }
        stackCard.show();
        QApplication::processEvents();
        stackCard.setExpandedForTest(false);
        stackCard.setExpandedForTest(true);
        stackCard.setExpandedForTest(false);
        stackCard.setExpandedForTest(true);
        QEventLoop detailsAnimationWait;
        QTimer::singleShot(220, &detailsAnimationWait, &QEventLoop::quit);
        detailsAnimationWait.exec();
        if (!stackCard.detailsExpandedForTest())
            return false;
        if (!store.removeItem(fileGroupId, firstItemId)
            || store.groups().first().items.size() != 1
            || store.groups().first().items.first().id != secondItemId
            || store.groups().first().title != QStringLiteral("source-two.txt"))
            return false;
        store.remove(fileGroupId);
        if (!QFileInfo::exists(firstPath) || !QFileInfo::exists(secondPath))
            return false;

        const QString vanishingPath = directory.path() + QStringLiteral("/vanishing.txt");
        QFile vanishingFile(vanishingPath);
        if (!vanishingFile.open(QIODevice::WriteOnly) || vanishingFile.write("gone") != 4)
            return false;
        vanishingFile.close();
        QMimeData vanishingMime;
        vanishingMime.setUrls({QUrl::fromLocalFile(vanishingPath)});
        if (!store.add(&vanishingMime))
            return false;
        const QString vanishingGroupId = store.groups().first().id;
        if (!QFile::remove(vanishingPath) || store.mimeFor(vanishingGroupId)
            || store.clipboardMimeFor(vanishingGroupId)
            || !store.remove(vanishingGroupId)) {
            return false;
        }

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

        QByteArray encodedImage;
        QBuffer encodedImageBuffer(&encodedImage);
        if (!encodedImageBuffer.open(QIODevice::WriteOnly)
            || !image.save(&encodedImageBuffer, "PNG")) {
            return false;
        }
        QMimeData rawImageMime;
        rawImageMime.setData(QStringLiteral("image/png"), encodedImage);
        rawImageMime.setUrls({QUrl(QStringLiteral("https://example.com/incidental-image-link"))});
        if (!ShelfStore::canImport(&rawImageMime) || !store.add(&rawImageMime)
            || store.groups().first().items.size() != 1
            || store.groups().first().items.first().kind != QLatin1String("image")) {
            return false;
        }
        const QString rawImageGroupId = store.groups().first().id;
        const QString rawImagePath = store.groups().first().items.first().value;
        if (!store.remove(rawImageGroupId) || QFileInfo::exists(rawImagePath))
            return false;

        QMimeData invalidImageMime;
        invalidImageMime.setData(QStringLiteral("image/png"), QByteArrayLiteral("not an image"));
        if (!ShelfStore::canImport(&invalidImageMime) || store.add(&invalidImageMime)
            || store.groups().size() != 1) {
            return false;
        }

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
        || reloaded.groups().first().items.first().plainText != QStringLiteral("Edited copied text")
        || reloaded.groups().first().items.first().kind != QLatin1String("text")) {
        return false;
    }

    QTemporaryDir ephemeralSourceDirectory;
    QTemporaryDir ephemeralStoreDirectory;
    if (!ephemeralSourceDirectory.isValid() || !ephemeralStoreDirectory.isValid())
        return false;
    {
        ShelfStore ephemeralStore(ephemeralStoreDirectory.path());
        const QString temporaryPath = ephemeralSourceDirectory.path()
            + QStringLiteral("/temporary-export.txt");
        QFile temporarySource(temporaryPath);
        if (!temporarySource.open(QIODevice::WriteOnly)
            || temporarySource.write("survive restart") != 15) {
            return false;
        }
        temporarySource.close();
        QMimeData temporaryFileMime;
        temporaryFileMime.setUrls({QUrl::fromLocalFile(temporaryPath)});
        if (!ephemeralStore.add(&temporaryFileMime))
            return false;
        const ShelfGroup temporaryGroup = ephemeralStore.groups().first();
        const QString materializedPath = temporaryGroup.items.first().value;
        const auto ephemeralPublicPermissionBits = QFileDevice::ReadGroup
            | QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther
            | QFileDevice::WriteOther | QFileDevice::ExeOther;
        if (!temporaryGroup.items.first().owned || materializedPath == temporaryPath
            || !QFileInfo::exists(materializedPath)
            || (QFileInfo(materializedPath).permissions() & ephemeralPublicPermissionBits)
                != QFileDevice::Permissions{}) {
            return false;
        }
        if (!QFile::remove(temporaryPath))
            return false;
        const auto durableMime = ephemeralStore.mimeFor(temporaryGroup.id);
        if (!durableMime || durableMime->urls().size() != 1
            || durableMime->urls().first().toLocalFile() != materializedPath)
            return false;
        if (!ephemeralStore.remove(temporaryGroup.id) || QFileInfo::exists(materializedPath))
            return false;

        const QString oversizedPath = ephemeralSourceDirectory.path()
            + QStringLiteral("/oversized-payload.bin");
        QFile oversized(oversizedPath);
        if (!oversized.open(QIODevice::WriteOnly)
            || !oversized.resize(kMaxOwnedPayloadBytes + 1)) {
            return false;
        }
        oversized.close();
        QMimeData oversizedMime;
        oversizedMime.setUrls({QUrl::fromLocalFile(oversizedPath)});
        if (ephemeralStore.add(&oversizedMime) || !ephemeralStore.groups().isEmpty())
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

    QTemporaryDir browserUrlDirectory;
    if (!browserUrlDirectory.isValid())
        return false;
    {
        ShelfStore browserUrlStore(browserUrlDirectory.path());
        if (ShelfStore::isImportableUrl(QUrl(QStringLiteral("Video: Funny Cats - YouTube")))
            || ShelfStore::isImportableUrl(QUrl(QStringLiteral("C++: A Tour - YouTube")))
            || ShelfStore::isImportableUrl(QUrl(QStringLiteral("File: quarterly report")))
            || !ShelfStore::isImportableUrl(QUrl(QStringLiteral("mailto:person@example.com")))
            || !ShelfStore::isImportableUrl(QUrl(QStringLiteral("custom://remote-host/item")))) {
            return false;
        }
        QMimeData browserUrl;
        browserUrl.setUrls({
            QUrl(QStringLiteral("https://www.youtube.com/watch?v=example")),
            QUrl(QStringLiteral("Video: Funny Cats - YouTube")),
        });
        browserUrl.setText(QStringLiteral("Funny Cats video"));
        browserUrl.setHtml(QStringLiteral("<b>Funny Cats video</b>"));
        if (!browserUrlStore.add(&browserUrl) || browserUrlStore.groups().first().items.size() != 1
            || browserUrlStore.groups().first().items.first().value
                != QStringLiteral("https://www.youtube.com/watch?v=example")
            || browserUrlStore.groups().first().title != QStringLiteral("video: Funny Cats - YouTube")) {
            return false;
        }
        const auto browserUrlOutput = browserUrlStore.mimeFor(
            browserUrlStore.groups().first().id);
        if (!browserUrlOutput || browserUrlOutput->urls().size() != 1
            || browserUrlOutput->text() != browserUrl.text()
            || browserUrlOutput->html() != browserUrl.html()) {
            return false;
        }

        QMimeData genuineMultiUrl;
        genuineMultiUrl.setUrls({
            QUrl(QStringLiteral("https://example.com/one")),
            QUrl(QStringLiteral("https://example.com/two")),
        });
        if (!browserUrlStore.add(&genuineMultiUrl)
            || browserUrlStore.groups().first().items.size() != 2) {
            return false;
        }
    }

    QTemporaryDir legacyUrlDirectory;
    if (!legacyUrlDirectory.isValid())
        return false;
    ShelfGroup malformedUrlGroup;
    malformedUrlGroup.id = QStringLiteral("legacy-browser-url");
    malformedUrlGroup.title = QStringLiteral("2 items");
    malformedUrlGroup.items = {
        {QStringLiteral("real-url"), QStringLiteral("url"), QStringLiteral("watch"),
            QStringLiteral("https://www.youtube.com/watch?v=legacy"), {}, {}, false},
        {QStringLiteral("browser-title"), QStringLiteral("url"), QStringLiteral("Video: Legacy video - YouTube"),
            QStringLiteral("Video: Legacy video - YouTube"), {}, {}, false},
    };
    QSaveFile legacyState(legacyUrlDirectory.path() + QStringLiteral("/state.json"));
    if (!legacyState.open(QIODevice::WriteOnly))
        return false;
    legacyState.write(QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("groups"), QJsonArray{malformedUrlGroup.toJson()}},
    }).toJson(QJsonDocument::Compact));
    if (!legacyState.commit())
        return false;
    ShelfStore migratedUrlStore(legacyUrlDirectory.path());
    if (migratedUrlStore.groups().size() != 1
        || migratedUrlStore.groups().first().items.size() != 1
        || migratedUrlStore.groups().first().title != QStringLiteral("Video: Legacy video - YouTube")) {
        return false;
    }
    ShelfStore persistedMigratedUrlStore(legacyUrlDirectory.path());
    if (persistedMigratedUrlStore.groups().first().items.size() != 1)
        return false;

    QTemporaryDir failingStorageDirectory;
    if (!failingStorageDirectory.isValid())
        return false;
    {
        ShelfStore failingStore(failingStorageDirectory.path());
        QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::cyan);
        QMimeData imageMime;
        imageMime.setImageData(image);
        if (!failingStore.add(&imageMime))
            return false;
        const QString originalId = failingStore.groups().first().id;
        const QString originalImagePath = failingStore.groups().first().items.first().value;
        const QString statePath = failingStorageDirectory.path() + QStringLiteral("/state.json");
        if (!QFile::remove(statePath) || !QDir().mkdir(statePath))
            return false;

        if (failingStore.setPinned(originalId, true)
            || failingStore.group(originalId)->pinned) {
            return false;
        }
        QMimeData unsavedText;
        unsavedText.setText(QStringLiteral("must roll back"));
        if (failingStore.add(&unsavedText) || failingStore.groups().size() != 1)
            return false;

        const int ownedCountBeforeFailedAdd = QDir(
            failingStorageDirectory.path() + QStringLiteral("/content"))
            .entryList(QDir::Files).size();
        QImage secondImage(20, 20, QImage::Format_ARGB32_Premultiplied);
        secondImage.fill(Qt::magenta);
        QMimeData unsavedImage;
        unsavedImage.setImageData(secondImage);
        if (failingStore.add(&unsavedImage)
            || QDir(failingStorageDirectory.path() + QStringLiteral("/content"))
                    .entryList(QDir::Files).size() != ownedCountBeforeFailedAdd) {
            return false;
        }
        if (failingStore.remove(originalId) || failingStore.groups().size() != 1
            || !QFileInfo::exists(originalImagePath)) {
            return false;
        }
        if (failingStore.clear() || failingStore.groups().size() != 1
            || !QFileInfo::exists(originalImagePath)) {
            return false;
        }
    }

    QTemporaryDir confinedCleanupDirectory;
    if (!confinedCleanupDirectory.isValid())
        return false;
    const QString outsideOwnedPath = confinedCleanupDirectory.path()
        + QStringLiteral("/must-not-delete.txt");
    QFile outsideOwned(outsideOwnedPath);
    if (!outsideOwned.open(QIODevice::WriteOnly) || outsideOwned.write("keep") != 4)
        return false;
    outsideOwned.close();
    ShelfGroup unsafeOwnedGroup;
    unsafeOwnedGroup.id = QStringLiteral("unsafe-owned-group");
    unsafeOwnedGroup.title = QStringLiteral("Unsafe owned path");
    unsafeOwnedGroup.items = {
        {QStringLiteral("unsafe-owned-item"), QStringLiteral("image"),
            QStringLiteral("Unsafe owned path"), outsideOwnedPath, {}, {}, true},
    };
    const QString confinedRoot = confinedCleanupDirectory.path() + QStringLiteral("/store");
    QDir().mkpath(confinedRoot);
    QSaveFile unsafeState(confinedRoot + QStringLiteral("/state.json"));
    if (!unsafeState.open(QIODevice::WriteOnly))
        return false;
    unsafeState.write(QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("groups"), QJsonArray{unsafeOwnedGroup.toJson()}},
    }).toJson(QJsonDocument::Compact));
    if (!unsafeState.commit())
        return false;
    ShelfStore confinedStore(confinedRoot);
    if (!confinedStore.remove(unsafeOwnedGroup.id) || !QFileInfo::exists(outsideOwnedPath))
        return false;

    QTemporaryDir orphanDirectory;
    if (!orphanDirectory.isValid())
        return false;
    const QString orphanContent = orphanDirectory.path() + QStringLiteral("/content");
    QDir().mkpath(orphanContent);
    const QString orphanPath = orphanContent + QStringLiteral("/unfinished-payload");
    QFile orphan(orphanPath);
    if (!orphan.open(QIODevice::WriteOnly) || orphan.write("orphan") != 6)
        return false;
    orphan.close();
    ShelfStore orphanCleaningStore(orphanDirectory.path());
    if (QFileInfo::exists(orphanPath))
        return false;

    QTemporaryDir corruptStateDirectory;
    if (!corruptStateDirectory.isValid())
        return false;
    const QString corruptStatePath = corruptStateDirectory.path() + QStringLiteral("/state.json");
    const QByteArray corruptStateContents = QByteArrayLiteral("{not valid json");
    QFile corruptState(corruptStatePath);
    if (!corruptState.open(QIODevice::WriteOnly)
        || corruptState.write(corruptStateContents) != corruptStateContents.size()) {
        return false;
    }
    corruptState.close();
    const QString preservedContent = corruptStateDirectory.path() + QStringLiteral("/content");
    QDir().mkpath(preservedContent);
    const QString preservedPayloadPath = preservedContent + QStringLiteral("/preserved-payload");
    QFile preservedPayload(preservedPayloadPath);
    if (!preservedPayload.open(QIODevice::WriteOnly) || preservedPayload.write("preserve") != 8)
        return false;
    preservedPayload.close();
    ShelfStore corruptStore(corruptStateDirectory.path());
    QMimeData refusedAfterCorruption;
    refusedAfterCorruption.setText(QStringLiteral("do not overwrite the damaged state"));
    if (corruptStore.add(&refusedAfterCorruption))
        return false;
    if (!QFileInfo::exists(preservedPayloadPath) || !corruptState.open(QIODevice::ReadOnly)
        || corruptState.readAll() != corruptStateContents) {
        return false;
    }
    corruptState.close();

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
    auto *copyButton = card.findChild<QToolButton *>(QStringLiteral("copyButton"));
    auto *removeButton = card.findChild<QToolButton *>(QStringLiteral("removeButton"));
    const auto buttonFits = [&card](QWidget *button) {
        if (!button || !button->isVisible())
            return false;
        const QRect geometry(button->mapTo(&card, QPoint(0, 0)), button->size());
        return card.rect().contains(geometry.topLeft())
            && card.rect().contains(geometry.bottomRight());
    };
    if (!buttonFits(copyButton) || !buttonFits(pinButton) || !buttonFits(removeButton))
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

    bool editSignalReceived = false;
    QString editedGroupId;
    QString editInitialText;
    QObject::connect(&card, &CardWidget::editTextRequested,
        [&editSignalReceived, &editedGroupId, &editInitialText](const QString &id, const QString &text) {
            editSignalReceived = true;
            editedGroupId = id;
            editInitialText = text;
        });
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPointF(12, 12),
        QPointF(card.mapToGlobal(QPoint(12, 12))),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&card, &doubleClick);
    if (!editSignalReceived || editedGroupId != uiGroup.id
        || editInitialText != uiGroup.items.first().plainText) {
        return false;
    }

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
        auto *newTextButton = shelf.findChild<QToolButton *>(QStringLiteral("newTextButton"));
        auto *shrinkButton = shelf.findChild<QToolButton *>(QStringLiteral("shrinkButton"));
        auto *handleLock = shelf.handleLockButtonForTest();
        auto *handle = shelf.handleWidgetForTest();
        auto *dropHint = shelf.findChild<QLabel *>(QStringLiteral("dropHint"));
        auto *toast = shelf.findChild<QLabel *>(QStringLiteral("toast"));
        auto *panel = shelf.findChild<QWidget *>(QStringLiteral("panel"));
        if (!pasteButton || !newTextButton || newTextButton->toolTip() != QStringLiteral("New text note")
            || !shrinkButton || shrinkButton->toolTip() != QStringLiteral("Shrink shelf")
            || !handleLock || !handle || !shelf.testAttribute(Qt::WA_Hover)
            || !handle->testAttribute(Qt::WA_Hover) || !dropHint || !dropHint->isHidden()
            || !toast || !toast->isHidden() || !panel || panel->layout()->indexOf(toast) >= 0
            || toast->geometry().center().y() < panel->height() / 2) {
            return false;
        }
        // Register a normal child in the hover-tracking callbacks. Destroying
        // ShelfWindow at the end of this scope must disconnect that callback
        // before its tracking sets are torn down.
        QEnterEvent trackedControlEnter(QPointF(2, 2), QPointF(2, 2), QPointF(2, 2));
        QApplication::sendEvent(pasteButton, &trackedControlEnter);
        QApplication::processEvents();
        if (handleLock->isChecked())
            handleLock->click();
        handleLock->click();
        if (!shelf.pinnedOpenForTest() || !shelf.expandedForTest() || !handleLock->isChecked())
            return false;
        shrinkButton->click();
        QEventLoop shrinkWait;
        QTimer::singleShot(260, &shrinkWait, &QEventLoop::quit);
        shrinkWait.exec();
        if (shelf.pinnedOpenForTest() || handleLock->isChecked()
            || shelf.expandedForTest() || shelf.panelVisibleForTest()) {
            return false;
        }

        QMimeData deferredDragPayload;
        deferredDragPayload.setData("application/x-pastashelf-deferred", "pending");
        QDragEnterEvent deferredDragEnter(QPoint(5, 5), Qt::CopyAction,
            &deferredDragPayload, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(handle, &deferredDragEnter);
        QApplication::processEvents();
        if (deferredDragEnter.isAccepted() || !shelf.expandedForTest())
            return false;
        shrinkButton->click();
        QEventLoop deferredDragCloseWait;
        QTimer::singleShot(260, &deferredDragCloseWait, &QEventLoop::quit);
        deferredDragCloseWait.exec();
        if (shelf.expandedForTest() || shelf.panelVisibleForTest())
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
        if (!dropHint->isHidden() || toast->isHidden()
            || toast->text() != QStringLiteral("✓  Added from clipboard")) {
            return false;
        }

        const QString clipboardGroupId = reloaded.groups().first().id;
        CardWidget *clipboardCard = nullptr;
        for (auto *candidate : shelf.findChildren<CardWidget *>()) {
            if (candidate->groupId() == clipboardGroupId) {
                clipboardCard = candidate;
                break;
            }
        }
        auto *clipboardCopyButton = clipboardCard
            ? clipboardCard->findChild<QToolButton *>(QStringLiteral("copyButton"))
            : nullptr;
        if (!clipboardCopyButton)
            return false;
        clipboardCopyButton->click();
        QApplication::processEvents();
        const QMimeData *copiedMime = QApplication::clipboard()->mimeData(QClipboard::Clipboard);
        if (!copiedMime || copiedMime->text() != QStringLiteral("Clipboard button insertion")
            || copiedMime->html() != QStringLiteral("<b>Clipboard button insertion</b>")
            || copiedMime->hasFormat(QString::fromLatin1(kInternalMime))
            || toast->isHidden() || toast->text() != QStringLiteral("✓  Copied to clipboard")) {
            return false;
        }

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
        const int groupCountBeforeNote = reloaded.groups().size();
        if (shelf.addTextNoteForTest(QStringLiteral(" \n\t"))
            || reloaded.groups().size() != groupCountBeforeNote
            || !shelf.addTextNoteForTest(QStringLiteral("A quick shelf note\nwith a second line"))) {
            return false;
        }
        const QString noteId = reloaded.groups().first().id;
        if (!reloaded.setPinned(noteId, true))
            return false;
        if (!shelf.editTextNoteForTest(noteId, QStringLiteral("An edited quick note\nwith new text")))
            return false;
        if (toast->isHidden() || toast->text() != QStringLiteral("✓  Text note updated"))
            return false;
        const ShelfGroup *note = reloaded.group(noteId);
        if (!note || note->items.size() != 1 || note->items.first().kind != QLatin1String("text")
            || note->items.first().plainText != QStringLiteral("An edited quick note\nwith new text")
            || !note->pinned || reloaded.groups().size() != groupCountBeforeNote + 1) {
            return false;
        }
        if (!reloaded.remove(noteId))
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
    painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("PastaShelf"));
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
    QCoreApplication::setOrganizationName(QStringLiteral("PastaShelf"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("io.github.pastashelf"));
    QCoreApplication::setApplicationName(QStringLiteral("PastaShelf"));
    QApplication::setApplicationDisplayName(QStringLiteral("PastaShelf"));
    QApplication::setDesktopFileName(QString::fromLatin1(kDesktopId));
    QApplication::setWindowIcon(QIcon(QString::fromLatin1(kBundledIcon)));
    QApplication::setQuitOnLastWindowClosed(false);
    migrateLegacySettings();

    const QStringList arguments = app.arguments();
    if (arguments.contains(QStringLiteral("--self-test"))) {
        const bool passed = runSelfTest();
        qInfo("PastaShelf self-test: %s", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }

    SingleInstance instance;
    if (!arguments.contains(QStringLiteral("--no-single-instance")) && !instance.acquire())
        return 0;

    const QString dataRoot = migrateLegacyData();
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
    } else if (arguments.contains(QStringLiteral("--hover-open-delay-test"))) {
        auto quickPassStayedClosed = std::make_shared<bool>(false);
        auto deliberateHoverStayedClosedInitially = std::make_shared<bool>(false);
        QTimer::singleShot(80, &window, [&window] {
            QEnterEvent enter(QPointF(4, 4), QPointF(4, 4), QPointF(4, 4));
            QApplication::sendEvent(window.handleWidgetForTest(), &enter);
        });
        QTimer::singleShot(200, &window, [&window] {
            QEvent leave(QEvent::Leave);
            QApplication::sendEvent(window.handleWidgetForTest(), &leave);
        });
        QTimer::singleShot(460, &window, [&window, quickPassStayedClosed] {
            *quickPassStayedClosed = !window.expandedForTest();
        });
        QTimer::singleShot(520, &window, [&window] {
            QHoverEvent hoverEnter(QEvent::HoverEnter, QPointF(4, 4), QPointF(4, 4),
                QPointF(-1, -1), Qt::NoModifier);
            QApplication::sendEvent(window.handleWidgetForTest(), &hoverEnter);
        });
        QTimer::singleShot(700, &window, [&window, deliberateHoverStayedClosedInitially] {
            *deliberateHoverStayedClosedInitially = !window.expandedForTest();
        });
        // Leave enough scheduling headroom for cold CI runners. The earlier
        // assertion still verifies that the shelf does not open before the
        // configured 300 ms delay.
        QTimer::singleShot(1250, &app,
            [&window, &app, quickPassStayedClosed, deliberateHoverStayedClosedInitially] {
            const bool openedAfterDelay = window.expandedForTest();
            const bool passed = *quickPassStayedClosed
                && *deliberateHoverStayedClosedInitially && openedAfterDelay;
            if (!passed) {
                qWarning("Hover delay test failed: quick=%d initial=%d opened=%d",
                    *quickPassStayedClosed, *deliberateHoverStayedClosedInitially,
                    openedAfterDelay);
            }
            app.exit(passed ? 0 : 1);
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
        // Leave ample room after the 180 ms close animation. Busy offscreen
        // CI/event loops can deliver the 3-second timer a few frames late.
        QTimer::singleShot(4800, &app,
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
