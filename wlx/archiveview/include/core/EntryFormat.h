#pragma once

#include <cstdint>
#include <string>

#include "core/ArchiveEntry.h"

/// Cell text, shared so both variants say the same thing about an entry.
///
/// The Qt variant uses QLocale for sizes and timestamps, which is
/// locale-aware in ways these are not; that difference is cosmetic and
/// deliberate. Everything where a *wrong* string would mislead — the mode
/// bits, the ratio, the CRC, blank-versus-zero — lives here so it cannot
/// drift between toolkits.
namespace archiveview::format {

/// IEC units. Empty for a negative (unset) size or a directory.
std::string size(int64_t bytes);
/// "drwxr-xr-x" style, with 'l' for symlinks and 'h' for hardlinks.
std::string mode(uint32_t mode, Entry::Type type);
/// "37.2%" — empty unless both sizes are known and the original is non-zero.
std::string ratio(int64_t packed, int64_t original);
/// Eight uppercase hex digits, empty when the format records no CRC.
std::string crc(uint32_t value, bool has);
/// "YYYY-MM-DD HH:MM" in local time, empty when unset.
std::string modified(int64_t secondsSinceEpoch, bool has);
/// "owner/group", empty when neither is recorded.
std::string owner(const Entry &entry);

} // namespace archiveview::format
