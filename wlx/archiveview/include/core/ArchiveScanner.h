#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "core/ArchiveEntry.h"
#include "core/PassphraseBroker.h"

/// libarchive's opaque handle, forward-declared at global scope on purpose:
/// naming it inside the namespace below would declare a distinct
/// archiveview::archive that nothing can convert to the real one.
struct archive;

namespace archiveview {

/// Walks an archive with libarchive on a worker thread and streams entries to
/// the UI in batches.
///
/// Three properties drive the design:
///
///  1. **The walk must never run on the host's UI thread.** ListLoad is
///     called on Double Commander's UI thread; listing a solid .tar.xz means
///     decompressing the whole stream, which would freeze the file manager.
///
///  2. **Cancellation must interrupt libarchive from the inside.** libarchive
///     has no cancel API, and a flag checked between entries is useless while
///     archive_read_next_header() is blocked decompressing a large solid
///     block. So the archive is opened through archive_read_open2() with our
///     own read callback: when the cancel flag is set the callback reports a
///     fatal read error, which unwinds the decompressor immediately.
///
///  3. **Callbacks arrive on the worker thread.** They are deliberately not
///     marshalled here — the toolkit adapters know how to hop threads
///     (Qt::QueuedConnection, g_idle_add) and the core does not need to.
///     Every callback implementation must therefore be thread-aware.
class Scanner {
public:
    /// Number of entries accumulated before a batch is handed to the UI. A
    /// per-entry callback across 100k entries costs more than the walk.
    static constexpr int kBatchSize = 512;
    /// Default ceiling on entries; a hostile archive can declare far more
    /// members than there is memory to model them.
    static constexpr int64_t kDefaultMaxEntries = 500000;

    struct Callbacks {
        std::function<void(const std::string &format,
                           const std::string &filters)> format;
        std::function<void(const std::string &comment)> comment;
        std::function<void(const EntryBatch &batch)> entries;
        std::function<void(int64_t bytesRead, int64_t totalBytes)> progress;
        /// Terminal callback. `ok` is false only for a genuine read failure —
        /// a cancelled scan reports ok=true with summary.cancelled set.
        std::function<void(bool ok, const std::string &error,
                           const Summary &summary)> finished;
        /// Posted when a passphrase is needed. Must not block; answer with
        /// providePassphrase().
        std::function<void(int attempt)> passphraseNeeded;
    };

    Scanner() = default;
    ~Scanner();

    Scanner(const Scanner &) = delete;
    Scanner &operator=(const Scanner &) = delete;

    void setCallbacks(Callbacks callbacks) { m_callbacks = std::move(callbacks); }

    /// Bounded synchronous check that libarchive recognises this file.
    ///
    /// ListLoad must answer "can you show this?" before returning, so that DC
    /// falls through to another viewer when the answer is no. The full walk
    /// cannot answer that — it only finds out at the end — so this reads just
    /// far enough to identify the format. An archive whose format is
    /// recognised but whose first header cannot be read (encrypted, damaged)
    /// still counts as readable: the view has something useful to say.
    static bool canRead(const std::string &archivePath);

    /// Begin scanning. Safe to call only when not already running.
    void scan(const std::string &archivePath,
              int64_t maxEntries = kDefaultMaxEntries);

    /// Request cancellation. Returns immediately; `finished` still fires.
    void cancel();
    /// Request cancellation and block until the worker has exited. Must be
    /// called before destroying anything the callbacks reference.
    void cancelAndWait();
    bool running() const { return m_running.load(std::memory_order_relaxed); }

    void providePassphrase(const std::string &passphrase, bool accepted);
    void setPassphrase(const std::string &passphrase);
    /// The passphrase the user supplied for this archive, so extraction does
    /// not prompt a second time for the same file.
    std::string acceptedPassphrase() const { return m_broker.accepted(); }

private:
    void run(std::string archivePath, int64_t maxEntries);
    const char *requestPassphrase();
    static const char *passphraseTrampoline(::archive *a, void *client);

    Callbacks m_callbacks;
    std::thread m_thread;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
    PassphraseBroker m_broker;
};

} // namespace archiveview
