#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/PassphraseBroker.h"

struct archive;

namespace archiveview {

/// Extracts selected members to a directory on a worker thread.
///
/// This is the one place in the plugin that writes to the filesystem, so it
/// is also the one place where a hostile member *name* becomes dangerous
/// rather than merely informative. The viewer shows "../../../../etc/passwd"
/// verbatim on purpose; extraction must refuse to write it.
///
/// Three independent layers stop that, because any single one of them could
/// be defeated by a case nobody thought of:
///
///  1. Every member path is checked before extraction: a ".." component or a
///     leading "/" is refused outright and counted.
///  2. The resolved destination path is required to stay inside the
///     destination directory, compared after canonicalisation.
///  3. libarchive's own ARCHIVE_EXTRACT_SECURE_* flags are enabled, and
///     stored symlink and hardlink targets are themselves checked, so a
///     symlink pointing out of the tree is refused before it can be created.
///
/// Note the process working directory is never changed. bsdtar chdir()s into
/// the destination, which is fine for a standalone tool and unacceptable
/// inside a file manager's process — so absolute target paths are built
/// explicitly instead.
class Extractor {
public:
    struct Callbacks {
        std::function<void(int done, int total, const std::string &member)> progress;
        /// `refused` counts members rejected by the path checks above.
        std::function<void(bool ok, const std::string &error,
                           int extracted, int refused)> finished;
        /// Posted when a passphrase is needed. Must not block.
        std::function<void(int attempt)> passphraseNeeded;
    };

    Extractor() = default;
    ~Extractor();

    Extractor(const Extractor &) = delete;
    Extractor &operator=(const Extractor &) = delete;

    void setCallbacks(Callbacks callbacks) { m_callbacks = std::move(callbacks); }

    /// Extract `members` (normalised in-archive paths; empty means all) from
    /// `archivePath` into `destination`.
    void extract(const std::string &archivePath,
                 const std::vector<std::string> &members,
                 const std::string &destination);

    /// Reuse a passphrase already accepted for this archive, so extracting
    /// does not prompt again after listing.
    void setPassphrase(const std::string &passphrase);
    void providePassphrase(const std::string &passphrase, bool accepted);

    void cancel();
    void cancelAndWait();

    /// Where the last extraction actually wrote each member, in order.
    /// Used to build the drag payload and to open a previewed entry.
    std::vector<std::string> writtenPaths() const;

    /// Exposed for testing: layer 1 of the containment checks.
    static bool isSafeMemberPath(const std::string &path);
    /// Exposed for testing: layer 2.
    static bool isInsideDestination(const std::string &canonicalDestination,
                                    const std::string &target);

private:
    void run(std::string archivePath, std::vector<std::string> members,
             std::string destination);
    const char *requestPassphrase();
    static const char *passphraseTrampoline(::archive *a, void *client);

    Callbacks m_callbacks;
    std::thread m_thread;
    std::vector<std::string> m_written;
    mutable std::mutex m_writtenMutex;
    std::atomic<bool> m_cancel{false};
    PassphraseBroker m_broker;
};

} // namespace archiveview
