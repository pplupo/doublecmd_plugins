#include "core/PassphraseBroker.h"

#include <chrono>

namespace archiveview {

const char *PassphraseBroker::request(
    const std::function<void(int attempt)> &askUi,
    const std::atomic<bool> &cancelled)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_declined || m_attempts >= kMaxAttempts)
        return nullptr;

    // A passphrase supplied in advance is used once without prompting.
    if (m_answered && m_accepted && m_attempts == 0) {
        ++m_attempts;
        return m_passphrase.c_str();
    }

    m_answered = false;
    ++m_attempts;

    // Released around the callback: askUi only posts to the UI thread, but
    // holding the lock while calling out would invite a deadlock if that ever
    // stopped being true.
    const int attempt = m_attempts;
    lock.unlock();
    askUi(attempt);
    lock.lock();

    while (!m_answered && !cancelled.load(std::memory_order_relaxed)) {
        // Timed rather than indefinite: a request that never gets answered
        // degrades into a cancellation check instead of a hang.
        m_ready.wait_for(lock, std::chrono::milliseconds(100));
    }

    if (cancelled.load(std::memory_order_relaxed) || !m_accepted) {
        m_declined = true;
        return nullptr;
    }

    return m_passphrase.c_str();
}

void PassphraseBroker::provide(const std::string &passphrase, bool accepted)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_passphrase = passphrase;
        m_accepted = accepted;
        m_answered = true;
    }
    m_ready.notify_all();
}

void PassphraseBroker::preset(const std::string &passphrase)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_passphrase = passphrase;
    m_accepted = true;
    m_answered = true;
    m_attempts = 0;
    m_declined = false;
}

void PassphraseBroker::wake()
{
    m_ready.notify_all();
}

void PassphraseBroker::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_passphrase.clear();
    m_attempts = 0;
    m_answered = false;
    m_accepted = false;
    m_declined = false;
}

bool PassphraseBroker::wasDeclined() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_declined;
}

std::string PassphraseBroker::accepted() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_accepted && !m_passphrase.empty()) ? m_passphrase : std::string();
}

} // namespace archiveview
