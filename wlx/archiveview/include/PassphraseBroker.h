#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QWaitCondition>
#include <atomic>
#include <functional>

/// Hands a passphrase from the GUI thread to a libarchive worker thread.
///
/// Shared by ArchiveScanner and ArchiveExtractor because getting it wrong is
/// a hang, not a wrong answer, and one correct implementation is worth more
/// than two plausible ones.
///
/// Deliberately **not** a BlockingQueuedConnection. That would park the
/// worker inside the signal while the GUI thread ran the dialog — and when
/// the GUI thread is simultaneously inside cancelAndWait() (which Double
/// Commander triggers merely by arrowing to the next file), the two block on
/// each other forever. Here:
///
///   * the worker waits on a condition with a timeout and rechecks cancel,
///   * the GUI thread never blocks on the worker,
///   * cancel() wakes the worker, so cancellation always wins.
class PassphraseBroker {
public:
    /// How many times a wrong passphrase may be re-prompted before giving up.
    /// libarchive only calls back when it still needs one, so a repeat call
    /// means the last attempt failed; without a cap a wrong password loops
    /// against the prompt forever.
    static constexpr int kMaxAttempts = 5;

    /// Called on the worker thread. `askGui` must be non-blocking — it is
    /// expected to emit a queued signal and return. Returns nullptr to tell
    /// libarchive to stop asking; the returned pointer stays valid until the
    /// next request.
    const char *request(const std::function<void(int attempt)> &askGui,
                        const std::atomic<bool> &cancelled);

    /// Called on the GUI thread in response to askGui.
    void provide(const QString &passphrase, bool accepted);

    /// Supply a passphrase up front, so the first request does not prompt.
    void preset(const QString &passphrase);

    /// Wake a waiting worker so it can observe a cancel flag.
    void wake();

    /// Forget state between archives.
    void reset();

    bool wasDeclined() const;
    /// The passphrase last accepted, for handing to a second worker (the
    /// extractor) without prompting the user twice for one archive.
    QString accepted() const;

private:
    mutable QMutex m_mutex;
    QWaitCondition m_ready;
    QByteArray m_passphrase;   ///< kept alive: libarchive retains the pointer
    int m_attempts = 0;
    bool m_answered = false;
    bool m_accepted = false;
    bool m_declined = false;
};
