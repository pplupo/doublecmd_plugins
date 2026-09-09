#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

namespace archiveview {

/// Hands a passphrase from the UI thread to a libarchive worker thread.
///
/// Shared by Scanner and Extractor because getting it wrong is a hang, not a
/// wrong answer, and one correct implementation is worth more than two
/// plausible ones.
///
/// The rule that shapes this: **the UI thread must never block on the
/// worker.** A design where the worker blocks inside a synchronous callback
/// while the UI runs the dialog deadlocks the moment the UI thread is also
/// inside cancelAndWait() — which Double Commander triggers merely by
/// arrowing to the next file. So instead:
///
///   * the worker waits on a condition variable with a timeout and rechecks
///     the cancel flag each time round,
///   * the UI thread only ever calls provide(), which returns immediately,
///   * cancel() wakes the worker, so cancellation always wins.
class PassphraseBroker {
public:
    /// How many times a wrong passphrase may be re-prompted before giving up.
    /// libarchive only calls back when it still needs one, so a repeat call
    /// means the last attempt failed; without a cap a wrong password loops
    /// against the prompt forever.
    static constexpr int kMaxAttempts = 5;

    /// Called on the worker thread. `askUi` must not block — it is expected
    /// to post a request to the UI thread and return. Returns nullptr to tell
    /// libarchive to stop asking; the returned pointer stays valid until the
    /// next request.
    const char *request(const std::function<void(int attempt)> &askUi,
                        const std::atomic<bool> &cancelled);

    /// Called on the UI thread in response to askUi.
    void provide(const std::string &passphrase, bool accepted);

    /// Supply a passphrase up front, so the first request does not prompt.
    void preset(const std::string &passphrase);

    /// Wake a waiting worker so it can observe a cancel flag.
    void wake();

    /// Forget state between archives.
    void reset();

    bool wasDeclined() const;
    /// The passphrase last accepted, for handing to a second worker (the
    /// extractor) without prompting the user twice for one archive.
    std::string accepted() const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::string m_passphrase;   ///< kept alive: libarchive retains the pointer
    int m_attempts = 0;
    bool m_answered = false;
    bool m_accepted = false;
    bool m_declined = false;
};

} // namespace archiveview
