#include "core/ArchiveSettings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace archiveview {
namespace {

std::string trim(std::string text)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

std::string toLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return text;
}

/// Accepts the spellings people actually write in an ini by hand.
bool parseBool(const std::string &value, bool fallback)
{
    const std::string lowered = toLower(trim(value));
    if (lowered == "true" || lowered == "1" || lowered == "yes" || lowered == "on")
        return true;
    if (lowered == "false" || lowered == "0" || lowered == "no" || lowered == "off")
        return false;
    return fallback;
}

std::vector<std::string> splitList(const std::string &value)
{
    std::vector<std::string> items;
    std::string::size_type start = 0;
    while (start <= value.size()) {
        const auto comma = value.find(',', start);
        const auto end = (comma == std::string::npos) ? value.size() : comma;
        const std::string item = trim(value.substr(start, end - start));
        if (!item.empty())
            items.push_back(item);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return items;
}

} // namespace

Settings Settings::load(const std::string &iniPath)
{
    Settings settings;
    if (iniPath.empty())
        return settings;

    std::ifstream file(iniPath);
    if (!file)
        return settings;

    bool inSection = false;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#')
            continue;

        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close == std::string::npos)
                continue;
            inSection = toLower(trim(line.substr(1, close - 1))) == kSection;
            continue;
        }

        if (!inSection)
            continue;

        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;

        const std::string key = toLower(trim(line.substr(0, equals)));
        const std::string value = trim(line.substr(equals + 1));

        if (key == "flatview") {
            settings.startFlat = parseBool(value, settings.startFlat);
        } else if (key == "detailpanel") {
            settings.showDetailPanel = parseBool(value, settings.showDetailPanel);
        } else if (key == "filterbox") {
            settings.showFilterBox = parseBool(value, settings.showFilterBox);
        } else if (key == "maxentries") {
            // A ceiling of zero or less would mean "no listing at all", which
            // is never what someone editing an ini intends; treat it as unset.
            const long long ceiling = std::strtoll(value.c_str(), nullptr, 10);
            if (ceiling > 0)
                settings.maxEntries = ceiling;
        } else if (key == "namecodec") {
            settings.nameCodec = value;
        } else if (key == "hiddencolumns") {
            settings.hiddenColumns = splitList(value);
        }
    }

    return settings;
}

} // namespace archiveview
