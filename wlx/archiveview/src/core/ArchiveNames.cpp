#include "core/ArchiveNames.h"

#include <iconv.h>
#include <langinfo.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace archiveview::names {
namespace {

std::string g_fallbackCodec;

/// Convert `input` from `fromCodec` to UTF-8. Returns false on any invalid
/// byte, rather than substituting replacement characters — a name that only
/// partly decodes is worse than falling through to the next candidate.
///
/// iconv rather than glib: it is what glib uses underneath, it is in libc on
/// the platforms this repository targets, and it keeps the core free of a
/// dependency that the Qt build would otherwise pull in solely for this.
bool convertToUtf8(const std::string &input, const char *fromCodec,
                   std::string *output)
{
    if (!fromCodec || !*fromCodec)
        return false;

    const iconv_t handle = iconv_open("UTF-8", fromCodec);
    if (handle == reinterpret_cast<iconv_t>(-1))
        return false;

    // Worst case for any codepage into UTF-8 is 4 bytes per input byte.
    std::vector<char> buffer(input.size() * 4 + 4);

    char *inPtr = const_cast<char *>(input.data());
    size_t inLeft = input.size();
    char *outPtr = buffer.data();
    size_t outLeft = buffer.size();

    const size_t rc = iconv(handle, &inPtr, &inLeft, &outPtr, &outLeft);
    iconv_close(handle);

    if (rc == static_cast<size_t>(-1) || inLeft != 0)
        return false;

    output->assign(buffer.data(), buffer.size() - outLeft);
    return true;
}

/// Latin-1 is a total function: every byte maps to a code point, so this
/// never fails and always round-trips.
std::string latin1ToUtf8(const std::string &input)
{
    std::string output;
    output.reserve(input.size() * 2);
    for (const unsigned char byte : input) {
        if (byte < 0x80) {
            output.push_back(static_cast<char>(byte));
        } else {
            output.push_back(static_cast<char>(0xC0 | (byte >> 6)));
            output.push_back(static_cast<char>(0x80 | (byte & 0x3F)));
        }
    }
    return output;
}

} // namespace

bool isValidUtf8(const std::string &bytes)
{
    const auto *p = reinterpret_cast<const unsigned char *>(bytes.data());
    const auto *end = p + bytes.size();

    while (p < end) {
        const unsigned char lead = *p;
        int trailing = 0;
        unsigned int codePoint = 0;

        if (lead < 0x80) {
            ++p;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            trailing = 1;
            codePoint = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            trailing = 2;
            codePoint = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            trailing = 3;
            codePoint = lead & 0x07;
        } else {
            return false;   // continuation byte or 5/6-byte form
        }

        if (p + trailing >= end)
            return false;

        for (int i = 1; i <= trailing; ++i) {
            const unsigned char continuation = p[i];
            if ((continuation & 0xC0) != 0x80)
                return false;
            codePoint = (codePoint << 6) | (continuation & 0x3F);
        }

        // Reject overlong forms, surrogates and out-of-range code points:
        // all three are ways to smuggle bytes past a naive validator.
        if ((trailing == 1 && codePoint < 0x80)
            || (trailing == 2 && codePoint < 0x800)
            || (trailing == 3 && codePoint < 0x10000)
            || (codePoint >= 0xD800 && codePoint <= 0xDFFF)
            || codePoint > 0x10FFFF) {
            return false;
        }

        p += trailing + 1;
    }
    return true;
}

std::string decode(const std::string &raw, bool declaredUtf8)
{
    if (raw.empty())
        return {};

    if (declaredUtf8 || isValidUtf8(raw))
        return raw;

    std::string converted;

    // The locale's codeset. On a UTF-8 system this fails for exactly the
    // names that reached here, which is the point of trying the next one.
    if (const char *codeset = nl_langinfo(CODESET)) {
        if (std::strcmp(codeset, "UTF-8") != 0
            && convertToUtf8(raw, codeset, &converted)) {
            return converted;
        }
    }

    // An explicitly configured legacy codepage. This is the only way to get
    // these names right: the archive does not record its encoding, so no
    // amount of guessing beats a user who knows where the file came from.
    if (!g_fallbackCodec.empty()
        && convertToUtf8(raw, g_fallbackCodec.c_str(), &converted)) {
        return converted;
    }

    return latin1ToUtf8(raw);
}

std::string normalize(std::string path)
{
    while (path.rfind("./", 0) == 0)
        path.erase(0, 2);

    for (std::string::size_type at = path.find("//");
         at != std::string::npos;
         at = path.find("//", at)) {
        path.erase(at, 1);
    }

    while (!path.empty() && path.back() == '/')
        path.pop_back();

    return path;
}

void setFallbackCodec(const std::string &codec)
{
    g_fallbackCodec = codec;
}

const std::string &fallbackCodec()
{
    return g_fallbackCodec;
}

} // namespace archiveview::names
