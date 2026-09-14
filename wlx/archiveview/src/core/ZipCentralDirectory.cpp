#include "core/ZipCentralDirectory.h"

#include <algorithm>
#include <fstream>
#include <limits>

#include "core/ArchiveNames.h"

namespace archiveview {
namespace {

// Record signatures, little-endian on disk.
constexpr uint32_t kEndOfCentralDirectory = 0x06054b50;  // "PK\5\6"
constexpr uint32_t kZip64EocdLocator      = 0x07064b50;  // "PK\6\7"
constexpr uint32_t kZip64Eocd             = 0x06064b50;  // "PK\6\6"
constexpr uint32_t kCentralDirectoryEntry = 0x02014b50;  // "PK\1\2"

// The EOCD is 22 bytes plus a comment of at most 65535, so it cannot begin
// more than this far from the end of the file.
constexpr int64_t kMaxEocdSearch = 22 + 65535;

// A hostile file can declare an enormous central directory. Refuse to
// allocate for one rather than trusting the header.
constexpr int64_t kMaxCentralDirectoryBytes = 256LL * 1024 * 1024;

constexpr uint16_t kFlagEncrypted = 0x0001;
constexpr uint16_t kFlagUtf8Names = 0x0800;

// Sentinels meaning "the real value is in the ZIP64 extra field".
constexpr uint32_t kMask32 = 0xFFFFFFFFu;
constexpr uint16_t kMask16 = 0xFFFFu;

/// Bounds-checked little-endian reader over a byte buffer.
///
/// Every read validates against the buffer that was actually read from disk,
/// not against a length the file claimed. `ok()` goes false on the first
/// overrun and stays false, so callers can check once at the end of a record
/// rather than after every field.
class Cursor {
public:
    explicit Cursor(const std::string &buffer, int64_t offset = 0)
        : m_buffer(buffer), m_offset(offset) {}

    bool ok() const { return m_ok; }
    int64_t offset() const { return m_offset; }
    void seek(int64_t offset) { m_offset = offset; }

    bool has(int64_t bytes) const
    {
        return m_ok && bytes >= 0
            && m_offset + bytes <= static_cast<int64_t>(m_buffer.size());
    }

    uint16_t u16()
    {
        if (!has(2)) { m_ok = false; return 0; }
        const auto *p = reinterpret_cast<const unsigned char *>(m_buffer.data()) + m_offset;
        m_offset += 2;
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    uint32_t u32()
    {
        if (!has(4)) { m_ok = false; return 0; }
        const auto *p = reinterpret_cast<const unsigned char *>(m_buffer.data()) + m_offset;
        m_offset += 4;
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
             | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    uint64_t u64()
    {
        const uint64_t low = u32();
        const uint64_t high = u32();
        return low | (high << 32);
    }

    std::string bytes(int64_t length)
    {
        if (!has(length)) { m_ok = false; return {}; }
        std::string result = m_buffer.substr(static_cast<size_t>(m_offset),
                                             static_cast<size_t>(length));
        m_offset += length;
        return result;
    }

    void skip(int64_t length)
    {
        if (!has(length)) { m_ok = false; return; }
        m_offset += length;
    }

private:
    const std::string &m_buffer;
    int64_t m_offset = 0;
    bool m_ok = true;
};

/// Locate the End of Central Directory record in the file's tail.
///
/// Scans backwards and takes the last candidate whose declared comment length
/// exactly matches the bytes remaining after it. The signature is only four
/// bytes, so it can occur inside an archive comment or inside stored data —
/// the length check is what distinguishes the real record from a coincidence.
/// (Both unzip(1) and libarchive can be fooled by a decoy signature in a
/// comment; this is stricter on purpose.)
int64_t findEocd(const std::string &tail)
{
    for (int64_t i = static_cast<int64_t>(tail.size()) - 22; i >= 0; --i) {
        Cursor cursor(tail, i);
        if (cursor.u32() != kEndOfCentralDirectory)
            continue;

        Cursor check(tail, i + 20);
        const uint16_t commentLength = check.u16();
        if (!check.ok())
            continue;
        if (i + 22 + commentLength == static_cast<int64_t>(tail.size()))
            return i;
    }
    return -1;
}

/// Pull the real sizes out of a ZIP64 extended information extra field.
///
/// The 32-bit size fields hold 0xFFFFFFFF when the true value does not fit,
/// and the actual 64-bit values live in an extra field with header id 0x0001.
/// Its contents are positional and only include the fields that were masked,
/// so which values are present depends on the record that precedes it.
void applyZip64Extra(const std::string &extra, bool needUncompressed,
                     bool needCompressed, int64_t *uncompressed,
                     int64_t *compressed)
{
    Cursor cursor(extra);
    while (cursor.has(4)) {
        const uint16_t headerId = cursor.u16();
        const uint16_t dataSize = cursor.u16();
        if (!cursor.ok())
            return;

        if (headerId != 0x0001) {
            cursor.skip(dataSize);
            if (!cursor.ok())
                return;
            continue;
        }

        const int64_t end = cursor.offset() + dataSize;
        const auto fits = [](uint64_t value) {
            return value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        };
        if (needUncompressed && cursor.has(8)) {
            const uint64_t value = cursor.u64();
            if (cursor.ok() && fits(value))
                *uncompressed = static_cast<int64_t>(value);
        }
        if (needCompressed && cursor.has(8)) {
            const uint64_t value = cursor.u64();
            if (cursor.ok() && fits(value))
                *compressed = static_cast<int64_t>(value);
        }
        cursor.seek(end);
        return;
    }
}

std::string readRegion(std::ifstream &file, int64_t offset, int64_t length)
{
    std::string buffer;
    if (length <= 0)
        return buffer;
    buffer.resize(static_cast<size_t>(length));
    file.seekg(offset, std::ios::beg);
    file.read(buffer.data(), length);
    buffer.resize(static_cast<size_t>(file.gcount()));
    return buffer;
}

} // namespace

const ZipCentralDirectory::EntryDetail *
ZipCentralDirectory::detailFor(const std::string &path, int occurrence) const
{
    const auto it = m_details.find(path);
    if (it == m_details.end())
        return nullptr;
    if (occurrence < 0 || occurrence >= static_cast<int>(it->second.size()))
        return nullptr;
    return &it->second[static_cast<size_t>(occurrence)];
}

bool ZipCentralDirectory::read(const std::string &archivePath)
{
    m_details.clear();
    m_comment.clear();
    m_entryCount = 0;
    m_zip64 = false;

    std::ifstream file(archivePath, std::ios::binary);
    if (!file)
        return false;

    file.seekg(0, std::ios::end);
    const int64_t fileSize = static_cast<int64_t>(file.tellg());
    if (fileSize < 22)
        return false;

    // --- End of Central Directory -----------------------------------------
    const int64_t tailSize = std::min(fileSize, kMaxEocdSearch);
    const std::string tail = readRegion(file, fileSize - tailSize, tailSize);

    const int64_t eocdAt = findEocd(tail);
    if (eocdAt < 0)
        return false;   // not a ZIP, or the record is unlocatable

    Cursor eocd(tail, eocdAt + 4);
    eocd.skip(4);                                   // disk numbers
    eocd.skip(2);                                   // entries on this disk
    const uint16_t entries16 = eocd.u16();
    const uint32_t cdSize32 = eocd.u32();
    const uint32_t cdOffset32 = eocd.u32();
    const uint16_t commentLength = eocd.u16();
    if (!eocd.ok())
        return false;

    // The comment libarchive cannot give us, and the reason a shell was
    // involved at all. It is raw bytes with no declared encoding, so it goes
    // through the same fallback chain as member names.
    if (commentLength > 0)
        m_comment = names::decode(eocd.bytes(commentLength));

    int64_t totalEntries = entries16;
    int64_t cdSize = cdSize32;
    int64_t cdOffset = cdOffset32;

    // --- ZIP64, when any 32-bit field is saturated -------------------------
    // Not an exotic case: an archive with more than 65535 members hits this,
    // regardless of how small it is.
    if (entries16 == kMask16 || cdSize32 == kMask32 || cdOffset32 == kMask32) {
        const int64_t locatorAt = eocdAt - 20;
        if (locatorAt >= 0) {
            Cursor locator(tail, locatorAt);
            if (locator.u32() == kZip64EocdLocator) {
                locator.skip(4);                    // disk with ZIP64 EOCD
                const uint64_t zip64At = locator.u64();

                if (locator.ok()
                    && zip64At + 56 <= static_cast<uint64_t>(fileSize)) {
                    const std::string record =
                        readRegion(file, static_cast<int64_t>(zip64At), 56);
                    Cursor zip64(record);
                    if (zip64.u32() == kZip64Eocd) {
                        zip64.skip(8);              // size of this record
                        zip64.skip(4);              // version made by / needed
                        zip64.skip(8);              // disk numbers
                        zip64.skip(8);              // entries on this disk
                        const uint64_t total = zip64.u64();
                        const uint64_t size = zip64.u64();
                        const uint64_t offset = zip64.u64();
                        if (zip64.ok()) {
                            m_zip64 = true;
                            totalEntries = static_cast<int64_t>(total);
                            cdSize = static_cast<int64_t>(size);
                            cdOffset = static_cast<int64_t>(offset);
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
    cdSize = std::min(cdSize, fileSize - cdOffset);

    const std::string directory = readRegion(file, cdOffset, cdSize);
    if (directory.empty())
        return false;

    if (totalEntries > 0) {
        m_details.reserve(static_cast<size_t>(
            std::min<int64_t>(totalEntries, 1 << 20)));
    }

    Cursor cursor(directory);
    while (cursor.has(46)) {
        if (cursor.u32() != kCentralDirectoryEntry)
            break;   // end of the directory, or damage — either way, stop

        cursor.skip(4);                             // versions
        const uint16_t flags = cursor.u16();
        cursor.skip(2);                             // compression method
        cursor.skip(4);                             // modification time/date
        const uint32_t crc = cursor.u32();
        const uint32_t compressed32 = cursor.u32();
        const uint32_t uncompressed32 = cursor.u32();
        const uint16_t nameLength = cursor.u16();
        const uint16_t extraLength = cursor.u16();
        const uint16_t entryCommentLength = cursor.u16();
        cursor.skip(2);                             // disk number start
        cursor.skip(2);                             // internal attributes
        cursor.skip(4);                             // external attributes
        cursor.skip(4);                             // local header offset

        const std::string rawName = cursor.bytes(nameLength);
        const std::string extra = cursor.bytes(extraLength);
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

        const std::string path = names::normalize(
            names::decode(rawName, (flags & kFlagUtf8Names) != 0));
        if (path.empty())
            continue;

        m_details[path].push_back(detail);
        ++m_entryCount;
    }

    return m_entryCount > 0 || !m_comment.empty();
}

} // namespace archiveview
