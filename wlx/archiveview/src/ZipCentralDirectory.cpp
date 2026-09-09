#include "ZipCentralDirectory.h"

#include <QFile>

#include "ArchiveNames.h"

namespace {

// Record signatures, little-endian on disk.
constexpr quint32 kEndOfCentralDirectory   = 0x06054b50;  // "PK\5\6"
constexpr quint32 kZip64EocdLocator        = 0x07064b50;  // "PK\6\7"
constexpr quint32 kZip64Eocd               = 0x06064b50;  // "PK\6\6"
constexpr quint32 kCentralDirectoryEntry   = 0x02014b50;  // "PK\1\2"

// The EOCD is 22 bytes plus a comment of at most 65535, so it cannot begin
// more than this far from the end of the file.
constexpr qint64 kMaxEocdSearch = 22 + 65535;

// A hostile file can declare an enormous central directory. Refuse to
// allocate for one rather than trusting the header.
constexpr qint64 kMaxCentralDirectoryBytes = 256LL * 1024 * 1024;

// General purpose bit flags.
constexpr quint16 kFlagEncrypted = 0x0001;
constexpr quint16 kFlagUtf8Names = 0x0800;

// Sentinels meaning "the real value is in the ZIP64 extra field".
constexpr quint32 kMask32 = 0xFFFFFFFFu;
constexpr quint16 kMask16 = 0xFFFFu;

/// Bounds-checked little-endian reader over a QByteArray.
///
/// Every read validates against the buffer that was actually read from disk,
/// not against a length the file claimed. `ok()` goes false on the first
/// overrun and stays false, so callers can check once at the end of a record
/// rather than after every field.
class Cursor {
public:
    explicit Cursor(const QByteArray &buffer, qint64 offset = 0)
        : m_buffer(buffer), m_offset(offset) {}

    bool ok() const { return m_ok; }
    qint64 offset() const { return m_offset; }
    void seek(qint64 offset) { m_offset = offset; }

    bool has(qint64 bytes) const
    {
        return m_ok && bytes >= 0 && m_offset + bytes <= m_buffer.size();
    }

    quint16 u16()
    {
        if (!has(2)) { m_ok = false; return 0; }
        const auto *p = reinterpret_cast<const uchar *>(m_buffer.constData() + m_offset);
        m_offset += 2;
        return quint16(p[0]) | quint16(p[1]) << 8;
    }

    quint32 u32()
    {
        if (!has(4)) { m_ok = false; return 0; }
        const auto *p = reinterpret_cast<const uchar *>(m_buffer.constData() + m_offset);
        m_offset += 4;
        return quint32(p[0]) | quint32(p[1]) << 8
             | quint32(p[2]) << 16 | quint32(p[3]) << 24;
    }

    quint64 u64()
    {
        const quint64 low = u32();
        const quint64 high = u32();
        return low | high << 32;
    }

    QByteArray bytes(qint64 length)
    {
        if (!has(length)) { m_ok = false; return QByteArray(); }
        QByteArray result = m_buffer.mid(m_offset, length);
        m_offset += length;
        return result;
    }

    void skip(qint64 length)
    {
        if (!has(length)) { m_ok = false; return; }
        m_offset += length;
    }

private:
    const QByteArray &m_buffer;
    qint64 m_offset = 0;
    bool m_ok = true;
};

/// Locate the End of Central Directory record in the file's tail.
///
/// Scans backwards and takes the last candidate whose declared comment length
/// exactly matches the bytes remaining after it. The signature is only four
/// bytes, so it can occur inside an archive comment or inside stored data —
/// the length check is what distinguishes the real record from a coincidence.
qint64 findEocd(const QByteArray &tail)
{
    for (qint64 i = tail.size() - 22; i >= 0; --i) {
        Cursor cursor(tail, i);
        if (cursor.u32() != kEndOfCentralDirectory)
            continue;

        Cursor check(tail, i + 20);
        const quint16 commentLength = check.u16();
        if (!check.ok())
            continue;
        if (i + 22 + commentLength == tail.size())
            return i;
    }
    return -1;
}

/// Pull the real sizes out of a ZIP64 extended information extra field.
///
/// The 32-bit size fields in a central directory record hold 0xFFFFFFFF when
/// the true value does not fit, and the actual 64-bit values live in an extra
/// field with header id 0x0001. Its contents are positional and only include
/// the fields that were masked, so which values are present depends on the
/// record that precedes it.
void applyZip64Extra(const QByteArray &extra, bool needUncompressed,
                     bool needCompressed, qint64 *uncompressed,
                     qint64 *compressed)
{
    Cursor cursor(extra);
    while (cursor.has(4)) {
        const quint16 headerId = cursor.u16();
        const quint16 dataSize = cursor.u16();
        if (!cursor.ok())
            return;

        if (headerId != 0x0001) {
            cursor.skip(dataSize);
            if (!cursor.ok())
                return;
            continue;
        }

        const qint64 end = cursor.offset() + dataSize;
        if (needUncompressed && cursor.has(8)) {
            const quint64 value = cursor.u64();
            if (cursor.ok() && value <= quint64(std::numeric_limits<qint64>::max()))
                *uncompressed = qint64(value);
        }
        if (needCompressed && cursor.has(8)) {
            const quint64 value = cursor.u64();
            if (cursor.ok() && value <= quint64(std::numeric_limits<qint64>::max()))
                *compressed = qint64(value);
        }
        cursor.seek(end);
        return;
    }
}

} // namespace

const ZipCentralDirectory::EntryDetail *
ZipCentralDirectory::detailFor(const QString &path, int occurrence) const
{
    auto it = m_details.constFind(path);
    if (it == m_details.constEnd())
        return nullptr;
    if (occurrence < 0 || occurrence >= it->size())
        return nullptr;
    return &it->at(occurrence);
}

bool ZipCentralDirectory::read(const QString &archivePath)
{
    m_details.clear();
    m_comment.clear();
    m_entryCount = 0;
    m_zip64 = false;

    QFile file(archivePath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const qint64 fileSize = file.size();
    if (fileSize < 22)
        return false;

    // --- End of Central Directory -----------------------------------------
    const qint64 tailSize = qMin(fileSize, kMaxEocdSearch);
    if (!file.seek(fileSize - tailSize))
        return false;
    const QByteArray tail = file.read(tailSize);

    const qint64 eocdAt = findEocd(tail);
    if (eocdAt < 0)
        return false;   // not a ZIP, or the record is unlocatable

    Cursor eocd(tail, eocdAt + 4);
    eocd.skip(4);                                   // disk numbers
    eocd.skip(2);                                   // entries on this disk
    const quint16 entries16 = eocd.u16();
    const quint32 cdSize32 = eocd.u32();
    const quint32 cdOffset32 = eocd.u32();
    const quint16 commentLength = eocd.u16();
    if (!eocd.ok())
        return false;

    // The comment libarchive cannot give us, and the reason a shell was
    // involved at all. It is raw bytes with no declared encoding, so it goes
    // through the same fallback chain as member names.
    if (commentLength > 0)
        m_comment = ArchiveNames::decode(eocd.bytes(commentLength));

    qint64 totalEntries = entries16;
    qint64 cdSize = cdSize32;
    qint64 cdOffset = cdOffset32;

    // --- ZIP64, when any 32-bit field is saturated -------------------------
    // Not an exotic case: an archive with more than 65535 members hits this,
    // regardless of how small it is.
    if (entries16 == kMask16 || cdSize32 == kMask32 || cdOffset32 == kMask32) {
        const qint64 locatorAt = eocdAt - 20;
        if (locatorAt >= 0) {
            Cursor locator(tail, locatorAt);
            if (locator.u32() == kZip64EocdLocator) {
                locator.skip(4);                    // disk with ZIP64 EOCD
                const quint64 zip64At = locator.u64();

                if (locator.ok() && zip64At + 56 <= quint64(fileSize)
                    && file.seek(qint64(zip64At))) {
                    const QByteArray record = file.read(56);
                    Cursor zip64(record);
                    if (zip64.u32() == kZip64Eocd) {
                        zip64.skip(8);              // size of this record
                        zip64.skip(4);              // version made by / needed
                        zip64.skip(8);              // disk numbers
                        zip64.skip(8);              // entries on this disk
                        const quint64 total = zip64.u64();
                        const quint64 size = zip64.u64();
                        const quint64 offset = zip64.u64();
                        if (zip64.ok()) {
                            m_zip64 = true;
                            totalEntries = qint64(total);
                            cdSize = qint64(size);
                            cdOffset = qint64(offset);
                        }
                    }
                }
            }
        }
    }

    // --- Central directory -------------------------------------------------
    if (cdOffset < 0 || cdSize <= 0 || cdOffset >= fileSize)
        return false;
    if (cdSize > kMaxCentralDirectoryBytes)
        return false;
    // Never trust the declared size past the end of the file.
    cdSize = qMin(cdSize, fileSize - cdOffset);

    if (!file.seek(cdOffset))
        return false;
    const QByteArray directory = file.read(cdSize);
    if (directory.isEmpty())
        return false;

    if (totalEntries > 0)
        m_details.reserve(int(qMin(totalEntries, qint64(1) << 20)));

    Cursor cursor(directory);
    while (cursor.has(46)) {
        if (cursor.u32() != kCentralDirectoryEntry)
            break;   // end of the directory, or damage — either way, stop

        cursor.skip(4);                             // versions
        const quint16 flags = cursor.u16();
        cursor.skip(2);                             // compression method
        cursor.skip(4);                             // modification time/date
        const quint32 crc = cursor.u32();
        const quint32 compressed32 = cursor.u32();
        const quint32 uncompressed32 = cursor.u32();
        const quint16 nameLength = cursor.u16();
        const quint16 extraLength = cursor.u16();
        const quint16 entryCommentLength = cursor.u16();
        cursor.skip(2);                             // disk number start
        cursor.skip(2);                             // internal attributes
        cursor.skip(4);                             // external attributes
        cursor.skip(4);                             // local header offset

        const QByteArray rawName = cursor.bytes(nameLength);
        const QByteArray extra = cursor.bytes(extraLength);
        cursor.skip(entryCommentLength);

        if (!cursor.ok())
            break;

        EntryDetail detail;
        detail.crc32 = crc;
        // A directory entry and a genuinely empty file both store CRC 0 with
        // zero length; reporting 00000000 for them is correct, not a stub.
        detail.hasCrc = true;
        detail.encrypted = (flags & kFlagEncrypted) != 0;
        detail.compressedSize = compressed32;
        detail.uncompressedSize = uncompressed32;

        if (compressed32 == kMask32 || uncompressed32 == kMask32) {
            applyZip64Extra(extra, uncompressed32 == kMask32,
                            compressed32 == kMask32,
                            &detail.uncompressedSize, &detail.compressedSize);
        }

        const QString path = ArchiveNames::normalize(
            ArchiveNames::decode(rawName, (flags & kFlagUtf8Names) != 0));
        if (path.isEmpty())
            continue;

        m_details[path].append(detail);
        ++m_entryCount;
    }

    return m_entryCount > 0 || !m_comment.isEmpty();
}
