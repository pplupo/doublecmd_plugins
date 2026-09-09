#include "PassphraseBroker.h"

#include <QDeadlineTimer>
#include <QMutexLocker>

const char *PassphraseBroker::request(
    const std::function<void(int attempt)> &askGui,
    const std::atomic<bool> &cancelled)
{
    QMutexLocker lock(&m_mutex);

    if (m_declined || m_attempts >= kMaxAttempts)
        return nullptr;

    // A passphrase supplied in advance is used once without prompting.
    if (m_answered && m_accepted && m_attempts == 0) {
        ++m_attempts;
        return m_passphrase.constData();
    }

    m_answered = false;
    ++m_attempts;

    // Released around the callback: askGui only emits a queued signal, but
    // holding the lock while calling out would invite a deadlock if that ever
    // stopped being true.
    lock.unlock();
    askGui(m_attempts);
    lock.relock();

    while (!m_answered && !cancelled.load(std::memory_order_relaxed)) {
        // Timed rather than indefinite: a signal that never gets answered
        // degrades into a cancellation check instead of a hang.
        m_ready.wait(&m_mutex, QDeadlineTimer(100));
    }

    if (cancelled.load(std::memory_order_relaxed) || !m_accepted) {
        m_declined = true;
        return nullptr;
    }

    return m_passphrase.constData();
}

void PassphraseBroker::provide(const QString &passphrase, bool accepted)
{
    QMutexLocker lock(&m_mutex);
    m_passphrase = passphrase.toUtf8();
    m_accepted = accepted;
    m_answered = true;
    m_ready.wakeAll();
}

void PassphraseBroker::preset(const QString &passphrase)
{
    QMutexLocker lock(&m_mutex);
    m_passphrase = passphrase.toUtf8();
    m_accepted = true;
    m_answered = true;
    m_attempts = 0;
    m_declined = false;
}

void PassphraseBroker::wake()
{
    m_ready.wakeAll();
}

void PassphraseBroker::reset()
{
    QMutexLocker lock(&m_mutex);
    m_passphrase.clear();
    m_attempts = 0;
    m_answered = false;
    m_accepted = false;
    m_declined = false;
}

bool PassphraseBroker::wasDeclined() const
{
    QMutexLocker lock(&m_mutex);
    return m_declined;
}

QString PassphraseBroker::accepted() const
{
    QMutexLocker lock(&m_mutex);
    return (m_accepted && !m_passphrase.isEmpty())
               ? QString::fromUtf8(m_passphrase) : QString();
}
