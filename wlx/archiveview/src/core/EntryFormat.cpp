#include "core/EntryFormat.h"

#include <cstdio>
#include <ctime>

namespace archiveview::format {

std::string size(int64_t bytes)
{
    if (bytes < 0)
        return {};

    static const char *units[] = { "bytes", "KiB", "MiB", "GiB", "TiB", "PiB" };
    if (bytes < 1024) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld %s",
                      static_cast<long long>(bytes), units[0]);
        return buffer;
    }

    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unit]);
    return buffer;
}

std::string mode(uint32_t mode, Entry::Type type)
{
    static const char *bits[] = { "---", "--x", "-w-", "-wx",
                                  "r--", "r-x", "rw-", "rwx" };
    std::string text;
    switch (type) {
    case Entry::Type::Directory: text = "d"; break;
    case Entry::Type::Symlink:   text = "l"; break;
    case Entry::Type::Hardlink:  text = "h"; break;
    default:                     text = "-"; break;
    }
    text += bits[(mode >> 6) & 7];
    text += bits[(mode >> 3) & 7];
    text += bits[mode & 7];
    return text;
}

std::string ratio(int64_t packed, int64_t original)
{
    if (packed < 0 || original <= 0)
        return {};
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f%%",
                  100.0 * static_cast<double>(packed) / static_cast<double>(original));
    return buffer;
}

std::string crc(uint32_t value, bool has)
{
    if (!has)
        return {};
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%08X", value);
    return buffer;
}

std::string modified(int64_t secondsSinceEpoch, bool has)
{
    if (!has)
        return {};

    const std::time_t when = static_cast<std::time_t>(secondsSinceEpoch);
    std::tm parts{};
    // localtime_r rather than localtime: this runs on a worker thread in one
    // variant and the UI thread in the other.
    if (!::localtime_r(&when, &parts))
        return {};

    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &parts) == 0)
        return {};
    return buffer;
}

std::string owner(const Entry &entry)
{
    if (entry.owner.empty() && entry.group.empty())
        return {};
    return entry.owner + "/" + entry.group;
}

} // namespace archiveview::format
