#include "ArchiveModel.h"

#include <QDateTime>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QLocale>
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
    default:                                  text = QStringLiteral("-"); break;
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
}

ArchiveModel::~ArchiveModel() = default;

// ---------------------------------------------------------------------------
// Node <-> index plumbing
// ---------------------------------------------------------------------------

ArchiveModel::Node *ArchiveModel::nodeFor(const QModelIndex &index) const
{
    if (!index.isValid())
        return const_cast<Node *>(m_tree.root());
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex ArchiveModel::indexForNode(const Node *node, int column) const
{
    if (!node || node == m_tree.root())
        return QModelIndex();
    return createIndex(node->row, column, const_cast<Node *>(node));
}

// ---------------------------------------------------------------------------
// Population
// ---------------------------------------------------------------------------

void ArchiveModel::beforeInsert(const Node *parent, int first, int count)
{
    beginInsertRows(indexForNode(parent), first, first + count - 1);
}

void ArchiveModel::afterInsert(const Node *, int, int)
{
    endInsertRows();
}

void ArchiveModel::appendEntries(const archiveview::EntryBatch &batch)
{
    if (batch.empty())
        return;

    if (m_flat) {
        // In flat mode the tree is still built (so toggling is instantaneous),
        // but its structure is not visible: the core's per-parent insertions
        // describe rows nobody is showing. Build silently, then announce the
        // one contiguous run that did appear.
        const int firstRow = m_tree.entryCount();
        m_tree.addEntries(batch, nullptr);
        const int lastRow = m_tree.entryCount() - 1;
        if (lastRow >= firstRow) {
            beginInsertRows(QModelIndex(), firstRow, lastRow);
            endInsertRows();
        }
        return;
    }

    m_tree.addEntries(batch, this);
}

void ArchiveModel::clearEntries()
{
    beginResetModel();
    m_tree.clear();
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
    const Node *node = nodeFor(index);
    if (!node || node == m_tree.root() || !node->hasEntry)
        return nullptr;
    return &node->entry;
}

QString ArchiveModel::pathAt(const QModelIndex &index) const
{
    const Node *node = nodeFor(index);
    return (node && node != m_tree.root())
               ? QString::fromStdString(node->fullPath) : QString();
}

QModelIndex ArchiveModel::flatIndex(int row, int column) const
{
    const auto &flat = m_tree.flat();
    if (row < 0 || row >= static_cast<int>(flat.size())
        || column < 0 || column >= ColumnCount) {
        return QModelIndex();
    }
    return createIndex(row, column, flat[static_cast<size_t>(row)]);
}

// ---------------------------------------------------------------------------
// QAbstractItemModel
// ---------------------------------------------------------------------------

QModelIndex ArchiveModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column < 0 || column >= ColumnCount)
        return QModelIndex();

    if (m_flat) {
        const auto &flat = m_tree.flat();
        if (parent.isValid() || row >= static_cast<int>(flat.size()))
            return QModelIndex();
        return createIndex(row, column, flat[static_cast<size_t>(row)]);
    }

    Node *parentNode = nodeFor(parent);
    if (!parentNode || row >= static_cast<int>(parentNode->children.size()))
        return QModelIndex();
    return createIndex(row, column, parentNode->children[static_cast<size_t>(row)]);
}

QModelIndex ArchiveModel::parent(const QModelIndex &child) const
{
    if (m_flat || !child.isValid())
        return QModelIndex();

    const Node *node = nodeFor(child);
    if (!node || !node->parent || node->parent == m_tree.root())
        return QModelIndex();
    return indexForNode(node->parent);
}

int ArchiveModel::rowCount(const QModelIndex &parent) const
{
    if (m_flat)
        return parent.isValid() ? 0 : m_tree.entryCount();

    if (parent.column() > 0)
        return 0;
    const Node *node = nodeFor(parent);
    return node ? static_cast<int>(node->children.size()) : 0;
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
        return QString::fromStdString(m_flat ? node->fullPath : node->name);

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
        // A directory's stored CRC is always zero; printing 00000000 for
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
    const QString name = QString::fromStdString(node->name);
    const QString suffix = QFileInfo(name).suffix().toLower();
    auto cached = m_iconCache.constFind(suffix);
    if (cached != m_iconCache.constEnd())
        return cached.value();

    static QMimeDatabase mimeDb;
    const QMimeType mime = mimeDb.mimeTypeForFile(name, QMimeDatabase::MatchExtension);
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

    const Node *node = nodeFor(index);
    if (!node || node == m_tree.root())
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
                 "uses this exact path.").arg(QString::fromStdString(node->fullPath))
            : QVariant(QString::fromStdString(node->fullPath));

    default:
        return QVariant();
    }
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

Qt::ItemFlags ArchiveModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    // ItemIsDragEnabled is not decoration: QAbstractItemView checks it before
    // it will call startDrag() at all. Without it the view silently refuses
    // to begin a drag, however the view itself is configured.
    return QAbstractItemModel::flags(index) | Qt::ItemIsDragEnabled;
}
