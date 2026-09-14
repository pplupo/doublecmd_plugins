#include "core/ArchiveExtractor.h"

#include <archive.h>
#include <archive_entry.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_set>

#include "core/ArchiveNames.h"

namespace archiveview {
namespace {

constexpr int kReadBlockSize = 256 * 1024;

struct ReadState {
    FILE *file = nullptr;
    std::vector<char> buffer;
    const std::atomic<bool> *cancel = nullptr;
};

la_ssize_t readCallback(struct archive *a, void *clientData, const void **buffer)
{
    auto *state = static_cast<ReadState *>(clientData);
    if (state->cancel->load(std::memory_order_relaxed)) {
        archive_set_error(a, ECANCELED, "extraction cancelled");
        return -1;
    }
    const size_t n = std::fread(state->buffer.data(), 1, state->buffer.size(),
                                state->file);
    if (n == 0 && std::ferror(state->file)) {
        archive_set_error(a, EIO, "%s", std::strerror(errno));
        return -1;
    }
    *buffer = state->buffer.data();
    return static_cast<la_ssize_t>(n);
}

la_int64_t seekCallback(struct archive *a, void *clientData,
                        la_int64_t offset, int whence)
{
    auto *state = static_cast<ReadState *>(clientData);
    if (state->cancel->load(std::memory_order_relaxed)) {
        archive_set_error(a, ECANCELED, "extraction cancelled");
        return -1;
    }
    if (::fseeko(state->file, offset, whence) != 0) {
        archive_set_error(a, EIO, "seek failed");
        return -1;
    }
    return ::ftello(state->file);
}

int closeCallback(struct archive *, void *clientData)
{
    auto *state = static_cast<ReadState *>(clientData);
    if (state->file) {
        std::fclose(state->file);
        state->file = nullptr;
    }
    return ARCHIVE_OK;
}

std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> parts;
    std::string::size_type start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto end = (slash == std::string::npos) ? path.size() : slash;
        if (end > start)
            parts.emplace_back(path, start, end - start);
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return parts;
}

/// Lexical cleanup only — no filesystem access, so it works for paths that do
/// not exist yet. Resolves "." and "..", which is what makes the containment
/// comparison meaningful.
std::string cleanPath(const std::string &path)
{
    const bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> stack;
    for (const std::string &part : splitPath(path)) {
        if (part == ".")
            continue;
        if (part == "..") {
            if (!stack.empty() && stack.back() != "..")
                stack.pop_back();
            else if (!absolute)
                stack.push_back("..");
            continue;
        }
        stack.push_back(part);
    }

    std::string result = absolute ? "/" : "";
    for (size_t i = 0; i < stack.size(); ++i) {
        if (i)
            result += '/';
        result += stack[i];
    }
    if (result.empty())
        result = absolute ? "/" : ".";
    return result;
}

} // namespace

bool Extractor::isSafeMemberPath(const std::string &path)
{
    if (path.empty() || path.front() == '/')
        return false;

    // A Windows-style drive or UNC prefix is equally absolute.
    if (path.find(":\\") != std::string::npos || path.rfind("\\\\", 0) == 0)
        return false;

    // Checked on split components rather than with a substring search, so a
    // legitimate name like "..hidden" or "a..b.txt" is not caught by mistake.
    const std::vector<std::string> parts = splitPath(path);
    for (const std::string &part : parts) {
        if (part == "..")
            return false;
    }
    return !parts.empty();
}

bool Extractor::isInsideDestination(const std::string &canonicalDestination,
                                    const std::string &target)
{
    const std::string clean = cleanPath(target);
    return clean == canonicalDestination
        || clean.rfind(canonicalDestination + "/", 0) == 0;
}

Extractor::~Extractor()
{
    cancelAndWait();
}

void Extractor::extract(const std::string &archivePath,
                        const std::vector<std::string> &members,
                        const std::string &destination)
{
    cancelAndWait();
    {
        std::lock_guard<std::mutex> lock(m_writtenMutex);
        m_written.clear();
    }
    m_cancel.store(false, std::memory_order_relaxed);
    m_thread = std::thread(&Extractor::run, this, archivePath, members, destination);
}

void Extractor::setPassphrase(const std::string &passphrase)
{
    if (!passphrase.empty())
        m_broker.preset(passphrase);
}

void Extractor::providePassphrase(const std::string &passphrase, bool accepted)
{
    m_broker.provide(passphrase, accepted);
}

void Extractor::cancel()
{
    m_cancel.store(true, std::memory_order_relaxed);
    m_broker.wake();
}

void Extractor::cancelAndWait()
{
    cancel();
    if (m_thread.joinable())
        m_thread.join();
}

std::vector<std::string> Extractor::writtenPaths() const
{
    std::lock_guard<std::mutex> lock(m_writtenMutex);
    return m_written;
}

const char *Extractor::passphraseTrampoline(::archive *, void *client)
{
    return static_cast<Extractor *>(client)->requestPassphrase();
}

const char *Extractor::requestPassphrase()
{
    return m_broker.request(
        [this](int attempt) {
            if (m_callbacks.passphraseNeeded)
                m_callbacks.passphraseNeeded(attempt);
        },
        m_cancel);
}

void Extractor::run(std::string archivePath, std::vector<std::string> members,
                    std::string destination)
{
    int extracted = 0;
    int refused = 0;

    const auto finish = [&](bool ok, const std::string &error) {
        if (m_callbacks.finished)
            m_callbacks.finished(ok, error, extracted, refused);
    };

    std::error_code ec;
    std::filesystem::create_directories(destination, ec);

    std::string canonicalDestination =
        std::filesystem::canonical(destination, ec).string();
    if (ec || canonicalDestination.empty())
        canonicalDestination = cleanPath(destination);

    ReadState state;
    state.file = std::fopen(archivePath.c_str(), "rb");
    if (!state.file) {
        finish(false, std::strerror(errno));
        return;
    }
    state.buffer.resize(kReadBlockSize);
    state.cancel = &m_cancel;

    struct archive *reader = archive_read_new();
    struct archive *writer = archive_write_disk_new();
    if (!reader || !writer) {
        if (reader) archive_read_free(reader);
        if (writer) archive_write_free(writer);
        std::fclose(state.file);
        finish(false, "Out of memory");
        return;
    }

    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    archive_read_set_seek_callback(reader, seekCallback);
    archive_read_set_passphrase_callback(reader, this, passphraseTrampoline);

    // Layer 3. NOABSOLUTEPATHS is deliberately absent: the target paths this
    // builds are absolute by construction, having already been validated by
    // layers 1 and 2. SECURE_SYMLINKS is the one that matters here — it stops
    // a member being written through a symlink planted by an earlier member.
    archive_write_disk_set_options(writer,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM
        | ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NODOTDOT);
    archive_write_disk_set_standard_lookup(writer);

    if (archive_read_open2(reader, &state, nullptr, readCallback,
                           nullptr, closeCallback) != ARCHIVE_OK) {
        const char *message = archive_error_string(reader);
        const std::string error = message ? message : "cannot open archive";
        archive_read_free(reader);
        archive_write_free(writer);
        finish(false, error);
        return;
    }

    // Empty selection means the whole archive.
    const std::unordered_set<std::string> wanted(members.begin(), members.end());
    const int total = wanted.empty() ? 0 : static_cast<int>(wanted.size());

    bool ok = true;
    std::string error;
    struct archive_entry *entry = nullptr;

    for (;;) {
        const int rc = archive_read_next_header(reader, &entry);
        if (rc == ARCHIVE_EOF)
            break;
        if (rc < ARCHIVE_WARN) {
            if (!m_cancel.load(std::memory_order_relaxed)) {
                ok = false;
                const char *message = archive_error_string(reader);
                error = message ? message : "read failed";
            }
            break;
        }

        const char *utf8Name = archive_entry_pathname_utf8(entry);
        const char *rawName = archive_entry_pathname(entry);
        const std::string member = names::normalize(
            (utf8Name && *utf8Name) ? names::decode(utf8Name, true)
                                    : (rawName ? names::decode(rawName)
                                               : std::string()));
        if (member.empty())
            continue;

        if (!wanted.empty() && wanted.find(member) == wanted.end())
            continue;

        if (!isSafeMemberPath(member)) {
            // The traversal and absolute-path members the viewer displays
            // verbatim end up here, counted and skipped.
            ++refused;
            continue;
        }

        const std::string target = canonicalDestination + "/" + member;
        if (!isInsideDestination(canonicalDestination, target)) {
            ++refused;
            continue;
        }

        archive_entry_set_pathname(entry, target.c_str());

        // A stored link target is a path too, and an absolute or climbing one
        // is exactly as dangerous as a member name. libarchive's
        // SECURE_SYMLINKS covers traversal *through* existing symlinks; this
        // refuses creating one that points out of the tree in the first place.
        if (const char *link = archive_entry_symlink(entry)) {
            const std::string linkTarget = link;
            const std::string parent =
                target.substr(0, target.find_last_of('/'));
            if (!linkTarget.empty()
                && (linkTarget.front() == '/'
                    || !isInsideDestination(canonicalDestination,
                                            parent + "/" + linkTarget))) {
                ++refused;
                continue;
            }
        }
        if (const char *link = archive_entry_hardlink(entry)) {
            const std::string linkTarget = names::normalize(link);
            if (!isSafeMemberPath(linkTarget)) {
                ++refused;
                continue;
            }
            const std::string resolved = canonicalDestination + "/" + linkTarget;
            archive_entry_set_hardlink(entry, resolved.c_str());
        }

        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            // A refusal from libarchive's own security checks lands here.
            ++refused;
            continue;
        }

        bool entryOk = true;
        const void *block = nullptr;
        size_t blockSize = 0;
        la_int64_t blockOffset = 0;
        for (;;) {
            if (m_cancel.load(std::memory_order_relaxed)) {
                entryOk = false;
                break;
            }
            const int readRc = archive_read_data_block(reader, &block,
                                                       &blockSize, &blockOffset);
            if (readRc == ARCHIVE_EOF)
                break;
            if (readRc < ARCHIVE_WARN) {
                if (!m_cancel.load(std::memory_order_relaxed)) {
                    ok = false;
                    const char *message = archive_error_string(reader);
                    error = message ? message : "read failed";
                }
                entryOk = false;
                break;
            }
            if (archive_write_data_block(writer, block, blockSize,
                                         blockOffset) < ARCHIVE_WARN) {
                ok = false;
                const char *message = archive_error_string(writer);
                error = message ? message : "write failed";
                entryOk = false;
                break;
            }
        }

        archive_write_finish_entry(writer);

        if (!entryOk) {
            if (m_cancel.load(std::memory_order_relaxed))
                break;
            continue;
        }

        ++extracted;
        {
            std::lock_guard<std::mutex> lock(m_writtenMutex);
            m_written.push_back(target);
        }
        if (m_callbacks.progress)
            m_callbacks.progress(extracted, total, member);

        if (!ok)
            break;
    }

    archive_read_free(reader);
    archive_write_free(writer);

    if (m_cancel.load(std::memory_order_relaxed))
        error = "Cancelled";

    finish(ok, error);
}

} // namespace archiveview
