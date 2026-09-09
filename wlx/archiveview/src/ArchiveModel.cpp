#include "ArchiveModel.h"

#include <QFileIconProvider>
#include <QFileInfo>
#include <QLocale>
#include <QDateTime>
#include <QMimeDatabase>

namespace {

QString formatMode(quint32 mode, archiveview::Entry::Type type)
{
    static const char *bits[] = { "---", "--x", "-w-", "-wx",
                                  "r--", "r-x", "rw-", "rwx" };
    QString text;
    switch (type) {
    case archiveview::Entry::Type::Directory: text = QStringLiteral("d"); break;
    case archiveview::Entry::Type::Symlink:   text = QStringLiteral("l"); break;
    case archiveview::Entry::Type::Hardlink:  text = QStringLiteral("h"); break;
    default:                      text = QStringLiteral("-"); break;
    }
    text += QLatin1String(bits[(mode >> 6) & 7]);
    text += QLatin1String(bits[(mode >> 3) & 7]);
    text += QLatin1String(bits[mode & 7]);
    return text;
}

} // namespace

ArchiveModel::ArchiveModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    m_root.isDir = true;
    m_root.attached = true;
}

ArchiveModel::~ArchiveModel() = default;

// ---------------------------------------------------------------------------
// Node plumbing
// ---------------------------------------------------------------------------

ArchiveModel::Node *ArchiveModel::nodeFor(const QModelIndex &index) const
{
    if (!index.isValid())
        return const_cast<Node *>(&m_root);
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex ArchiveModel::indexForNode(Node *node, int column) const
{
    if (!node || node == &m_root)
        return QModelIndex();
    return createIndex(node->row, column, node);
}

ArchiveModel::Node *ArchiveModel::makeNode(const QString &name,
                                           const QString &fullPath, Node *parent,
                                           bool registerPath)
{
    m_storage.push_back(std::make_unique<Node>());
    Node *node = m_storage.back().get();
    node->name = name;
    node->fullPath = fullPath;
    node->parent = parent;
    if (registerPath)
        m_byPath.insert(fullPath, node);
    return node;
}

void ArchiveModel::attachNode(Node *node, Node *parent,
                              QHash<Node *, QVector<Node *>> *pending)
{
    if (pending && parent->attached) {
        // Parent is already on screen, so this node's arrival must be
        // announced with begin/endInsertRows. Hold it back until the whole
        // batch is processed, then attach every sibling group in one call —
        // otherwise a 512-entry batch means hundreds of view updates.
        (*pending)[parent].append(node);
    } else {
        // Parent is itself pending (or we are in flat mode, where tree
        // structure is invisible), so this node rides along inside its
        // parent's insertion and needs no separate notification.
        node->row = parent->children.size();
        node->attached = parent->attached;
        parent->children.append(node);
    }
}

ArchiveModel::Node *ArchiveModel::ensureNode(const QString &path, bool isDir,
                                             QHash<Node *, QVector<Node *>> *pending)
{
    if (Node *existing = m_byPath.value(path, nullptr)) {
        if (isDir)
            existing->isDir = true;
        return existing;
    }

    const int slash = path.lastIndexOf(QLatin1Char('/'));
    Node *parent = &m_root;
    QString name = path;
    if (slash > 0) {
        // Ancestors are always directories, whether or not the archive said so.
        parent = ensureNode(path.left(slash), true, pending);
        name = path.mid(slash + 1);
    }
    // slash == 0 means an absolute member path such as "/absolute/file.txt".
    // Its first component keeps the leading slash and sits at the top level,
    // so an absolute path stays visibly absolute instead of being quietly
    // rehomed under a blank-named root node.

    Node *node = makeNode(name, path, parent, /*registerPath=*/true);
    node->isDir = isDir;
    attachNode(node, parent, pending);
    return node;
}

ArchiveModel::Node *ArchiveModel::addDuplicate(Node *original,
                                               QHash<Node *, QVector<Node *>> *pending)
{
    // An archive may store two members under the same path. Folding them into
    // one row would hide the discrepancy, and a duplicate path is exactly the
    // kind of thing someone previewing an untrusted archive needs to see —
    // it is a known trick for showing one file and extracting another. The
    // duplicate gets its own row and is deliberately not registered in the
    // path index, so later children still resolve to the first node.
    Node *parent = original->parent ? original->parent : &m_root;
    Node *node = makeNode(original->name, original->fullPath, parent,
                          /*registerPath=*/false);
    node->isDuplicate = true;
    ++m_duplicateCount;
    attachNode(node, parent, pending);
    return node;
}

void ArchiveModel::attachPending(QHash<Node *, QVector<Node *>> *pending)
{
    for (auto it = pending->begin(); it != pending->end(); ++it) {
        Node *parent = it.key();
        QVector<Node *> &group = it.value();
        if (group.isEmpty())
            continue;

        const int first = parent->children.size();
        beginInsertRows(indexForNode(parent), first, first + group.size() - 1);
        for (Node *node : group) {
            node->row = parent->children.size();
            parent->children.append(node);
            node->attached = true;
            // Descendants created inside this batch are attached implicitly
            // by their ancestor becoming visible.
            QVector<Node *> stack = node->children;
            while (!stack.isEmpty()) {
                Node *descendant = stack.takeLast();
                descendant->attached = true;
                stack += descendant->children;
            }
        }
        endInsertRows();
    }
    pending->clear();
}

// ---------------------------------------------------------------------------
// Population
// ---------------------------------------------------------------------------

void ArchiveModel::appendEntries(const archiveview::EntryBatch &batch)
{
    if (batch.empty())
        return;

    const int firstFlatRow = static_cast<int>(m_flatNodes.size());

    // In flat mode the tree is still built (so toggling is instantaneous),
    // but its structure is not visible, so nodes attach silently and a
    // single row insertion covers the whole batch.
    QHash<Node *, QVector<Node *>> pending;
    QHash<Node *, QVector<Node *>> *pendingPtr = m_flat ? nullptr : &pending;

    std::vector<Node *> newFlat;
    newFlat.reserve(batch.size());
    QVector<Node *> updatedExisting;

    for (const archiveview::Entry &entry : batch) {
        // std::string -> QString happens once, here, rather than per data() call.
        Node *node = ensureNode(QString::fromStdString(entry.path),
                                entry.isDir(), pendingPtr);

        if (node->hasEntry)
            node = addDuplicate(node, pendingPtr);
        else if (node->attached)
            updatedExisting.append(node);   // synthesised dir gaining its record

        node->hasEntry = true;
        node->entry = entry;
        newFlat.push_back(node);
    }

    if (m_flat) {
        if (!newFlat.empty()) {
            beginInsertRows(QModelIndex(), firstFlatRow,
                            firstFlatRow + static_cast<int>(newFlat.size()) - 1);
            m_flatNodes.insert(m_flatNodes.end(), newFlat.begin(), newFlat.end());
            endInsertRows();
        }
        return;
    }

    m_flatNodes.insert(m_flatNodes.end(), newFlat.begin(), newFlat.end());
    attachPending(&pending);

    // A directory synthesised by an earlier batch and only now given its own
    // entry record (zip stores directory entries after their contents in
    // some writers) is already on screen — its columns changed in place.
    for (Node *node : updatedExisting) {
        emit dataChanged(indexForNode(node, 0),
                         indexForNode(node, ColumnCount - 1));
    }
}

void ArchiveModel::clearEntries()
{
    beginResetModel();
    m_root.children.clear();
    m_byPath.clear();
    m_flatNodes.clear();
    m_storage.clear();
    m_duplicateCount = 0;
    endResetModel();
}

void ArchiveModel::setFlat(bool flat)
{
    if (m_flat == flat)
        return;

    beginResetModel();
    m_flat = flat;
    endResetModel();
}

const archiveview::Entry *ArchiveModel::entryAt(const QModelIndex &index) const
{
    Node *node = nodeFor(index);
    if (!node || node == &m_root || !node->hasEntry)
        return nullptr;
    return &node->entry;
}

QModelIndex ArchiveModel::flatIndex(int row, int column) const
{
    if (row < 0 || row >= static_cast<int>(m_flatNodes.size())
        || column < 0 || column >= ColumnCount) {
        return QModelIndex();
    }
    return createIndex(row, column, m_flatNodes[row]);
}

QString ArchiveModel::pathAt(const QModelIndex &index) const
{
    Node *node = nodeFor(index);
    return (node && node != &m_root) ? node->fullPath : QString();
}

// ---------------------------------------------------------------------------
// QAbstractItemModel
// ---------------------------------------------------------------------------

QModelIndex ArchiveModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount)
        return QModelIndex();

    if (m_flat) {
        if (parent.isValid() || row >= static_cast<int>(m_flatNodes.size()))
            return QModelIndex();
        return createIndex(row, column, m_flatNodes[row]);
    }

    Node *parentNode = nodeFor(parent);
    if (!parentNode || row >= parentNode->children.size())
        return QModelIndex();
    return createIndex(row, column, parentNode->children.at(row));
}

QModelIndex ArchiveModel::parent(const QModelIndex &child) const
{
    if (m_flat || !child.isValid())
        return QModelIndex();

    Node *node = nodeFor(child);
    if (!node || !node->parent || node->parent == &m_root)
        return QModelIndex();
    return indexForNode(node->parent);
}

int ArchiveModel::rowCount(const QModelIndex &parent) const
{
    if (m_flat)
        return parent.isValid() ? 0 : static_cast<int>(m_flatNodes.size());

    if (parent.column() > 0)
        return 0;
    Node *node = nodeFor(parent);
    return node ? node->children.size() : 0;
}

int ArchiveModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

QVariant ArchiveModel::displayText(const Node *node, int column) const
{
    // A synthesised directory has no entry record; every column but the name
    // is genuinely unknown for it, and blank beats a fabricated zero.
    if (!node->hasEntry && column != NameColumn)
        return QVariant();

    const archiveview::Entry &entry = node->entry;
    const QLocale locale;

    switch (column) {
    case LockColumn:
        return QVariant();   // icon only, see iconFor()

    case NameColumn:
        return m_flat ? node->fullPath : node->name;

    case SizeColumn:
        if (entry.isDir() || entry.size < 0)
            return QVariant();
        return locale.formattedDataSize(entry.size);

    case PackedColumn:
        // libarchive exposes no per-entry compressed size; this is populated
        // from the ZIP central directory only, and stays blank elsewhere.
        //
        // Note this is the size the archive records, which for an encrypted
        // entry includes the per-entry encryption overhead (a 12-byte
        // ZipCrypto header, or an AES salt plus password verifier). unzip -v
        // subtracts that to show payload size; the stored figure is the
        // honest answer for a column labelled "Packed", and it is what makes
        // the ratio reflect real on-disk cost.
        if (entry.isDir() || entry.compressedSize < 0)
            return QVariant();
        return locale.formattedDataSize(entry.compressedSize);

    case RatioColumn: {
        if (entry.isDir() || entry.compressedSize < 0 || entry.size <= 0)
            return QVariant();
        const double ratio = 100.0 * double(entry.compressedSize) / double(entry.size);
        return QStringLiteral("%1%").arg(ratio, 0, 'f', 1);
    }

    case CrcColumn:
        // Also from the ZIP central directory — libarchive stores no CRC.
        // Zero-padded hex, because that is how every other tool prints it.
        // A directory's stored CRC is always zero — printing 00000000 for
        // every folder is noise, not information.
        if (!entry.hasCrc || entry.isDir())
            return QVariant();
        return QStringLiteral("%1").arg(entry.crc32, 8, 16, QLatin1Char('0')).toUpper();

    case ModifiedColumn:
        if (!entry.hasModified)
            return QVariant();
        return locale.toString(QDateTime::fromSecsSinceEpoch(entry.modified),
                               QLocale::ShortFormat);

    case ModeColumn:
        return entry.mode ? formatMode(entry.mode, entry.type) : QVariant();

    case OwnerColumn:
        if (entry.owner.empty() && entry.group.empty())
            return QVariant();
        return QStringLiteral("%1/%2").arg(QString::fromStdString(entry.owner),
                                           QString::fromStdString(entry.group));

    case LinkColumn:
        return entry.linkTarget.empty()
                   ? QVariant() : QString::fromStdString(entry.linkTarget);

    default:
        return QVariant();
    }
}

QIcon ArchiveModel::iconFor(const Node *node) const
{
    static QFileIconProvider provider;

    if (node->isDir || !node->hasEntry) {
        static const QIcon folder = provider.icon(QFileIconProvider::Folder);
        return folder;
    }

    // Cached by suffix. A per-row QMimeDatabase lookup plus theme icon
    // resolution is the other half of why the item-widget version could not
    // scale; caching makes it one lookup per distinct file type.
    const QString suffix = QFileInfo(node->name).suffix().toLower();
    auto cached = m_iconCache.constFind(suffix);
    if (cached != m_iconCache.constEnd())
        return cached.value();

    static QMimeDatabase mimeDb;
    const QMimeType mime = mimeDb.mimeTypeForFile(node->name,
                                                  QMimeDatabase::MatchExtension);
    QIcon icon = QIcon::fromTheme(mime.iconName());
    if (icon.isNull())
        icon = QIcon::fromTheme(mime.genericIconName());
    if (icon.isNull())
        icon = provider.icon(QFileIconProvider::File);

    m_iconCache.insert(suffix, icon);
    return icon;
}

QVariant ArchiveModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    Node *node = nodeFor(index);
    if (!node || node == &m_root)
        return QVariant();

    switch (role) {
    case Qt::DisplayRole:
        return displayText(node, index.column());

    case Qt::DecorationRole:
        if (index.column() == NameColumn)
            return QVariant(iconFor(node));
        if (index.column() == LockColumn && node->hasEntry
            && (node->entry.encrypted || node->entry.metadataEncrypted)) {
            static const QIcon lock = QIcon::fromTheme(QStringLiteral("object-locked"),
                                          QIcon::fromTheme(QStringLiteral("changes-prevent")));
            return QVariant(lock);
        }
        return QVariant();

    case Qt::TextAlignmentRole:
        switch (index.column()) {
        case SizeColumn:
        case PackedColumn:
        case RatioColumn:
        case CrcColumn:
            return QVariant(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return QVariant();
        }

    case Qt::ToolTipRole:
        if (index.column() == LockColumn && node->hasEntry) {
            if (node->entry.metadataEncrypted)
                return tr("Encrypted, including its metadata");
            if (node->entry.encrypted)
                return tr("Contents are encrypted");
            return QVariant();
        }
        return node->isDuplicate
            ? tr("%1\n\nDuplicate: another member of this archive already "
                 "uses this exact path.").arg(node->fullPath)
            : QVariant(node->fullPath);

    default:
        return QVariant();
    }
}

Qt::ItemFlags ArchiveModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    // ItemIsDragEnabled is not decoration: QAbstractItemView checks it before
    // it will call startDrag() at all. Without it the view silently refuses
    // to begin a drag, however the view itself is configured — which is
    // exactly why dragging members out did nothing.
    return QAbstractItemModel::flags(index) | Qt::ItemIsDragEnabled;
}

QVariant ArchiveModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    switch (section) {
    case LockColumn:     return QString();
    case NameColumn:     return tr("Name");
    case SizeColumn:     return tr("Size");
    case PackedColumn:   return tr("Packed");
    case RatioColumn:    return tr("Ratio");
    case CrcColumn:      return tr("CRC-32");
    case ModifiedColumn: return tr("Modified");
    case ModeColumn:     return tr("Mode");
    case OwnerColumn:    return tr("Owner");
    case LinkColumn:     return tr("Link target");
    default:             return QVariant();
    }
}
