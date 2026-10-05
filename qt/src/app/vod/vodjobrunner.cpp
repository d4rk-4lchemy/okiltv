#include "vodjobrunner.h"
#include <QFutureWatcher>
#include <QEventLoop>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>

namespace OKILTV::Vod {
VodJobRunner::VodJobRunner(int concurrency, int perSource, QObject *parent)
    : QObject(parent), m_limit(std::clamp(concurrency, 1, 4)), m_perSource(std::clamp(perSource, 1, 2)) {}
VodJobRunner::~VodJobRunner() { shutdown(); }
void VodJobRunner::submit(const QUuid &profile, RequestContext request, Work work, Completion completion)
{
    if (m_stopped) {
        completion(Error{ErrorCode::Cancelled, request.operationId});
        return;
    }
    m_queue.push_back({profile, std::move(request), std::move(work), std::move(completion)});
    pump();
}
void VodJobRunner::cancel(const QUuid &operation)
{
    for (auto &job : m_queue) if (job.request.operationId == operation) job.request.cancelled->store(true);
    for (auto &job : m_active) if (job.request.operationId == operation) job.request.cancelled->store(true);
}
void VodJobRunner::cancelSource(const QUuid &profile)
{
    for (auto &job : m_queue) if (job.profile == profile) job.request.cancelled->store(true);
    for (auto &job : m_active) if (job.profile == profile) job.request.cancelled->store(true);
}
bool VodJobRunner::drain(int deadlineMs)
{
    if (pending() == 0) return true;
    QEventLoop loop;
    QTimer timer;
    timer.setInterval(5);
    connect(&timer, &QTimer::timeout, &loop, [&]() { if (pending() == 0) loop.quit(); });
    QTimer::singleShot(std::clamp(deadlineMs, 0, 1000), &loop, &QEventLoop::quit);
    timer.start();
    loop.exec();
    return pending() == 0;
}
void VodJobRunner::shutdown()
{
    m_stopped = true;
    for (auto &job : m_queue) job.request.cancelled->store(true);
    for (auto &job : m_active) job.request.cancelled->store(true);
    m_queue.clear();
    // Workers own only value snapshots/shared ports. Destroying watchers drops
    // delivery safely; the GUI never waits for a network call to finish.
}
int VodJobRunner::pending() const { return static_cast<int>(m_queue.size()) + static_cast<int>(m_active.size()); }
void VodJobRunner::pump()
{
    if (m_stopped) return;
    // Watchers belong to this QObject and deleteLater on completion. The analyzer
    // cannot follow QObject's child ownership across the asynchronous callback.
    while (!m_queue.empty() && m_active.size() < m_limit) { // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
        const auto next = std::find_if(m_queue.begin(), m_queue.end(), [this](const Job &candidate) {
            int count = 0;
            for (const auto &job : m_active) if (job.profile == candidate.profile) ++count;
            return count < m_perSource;
        });
        if (next == m_queue.end()) break;
        auto job = std::move(*next);
        m_queue.erase(next);
        const auto id = job.request.operationId;
        m_active.insert(id, job);
        auto *watcher = new QFutureWatcher<Result<JobReply>>(this);
        connect(watcher, &QFutureWatcher<Result<JobReply>>::finished, this, [this, watcher, id]() {
            auto completed = m_active.take(id);
            auto result = watcher->result();
            watcher->deleteLater();
            if (!m_stopped) {
                if (const auto interrupted = completed.request.interruption()) result = *interrupted;
                completed.completion(std::move(result));
            }
            pump();
        });
        watcher->setFuture(QtConcurrent::run([work = std::move(job.work), request = job.request]() -> Result<JobReply> {
            if (const auto interrupted = request.interruption()) return *interrupted;
            try { return work(request); }
            catch (...) { return Error{ErrorCode::ProviderUnavailable, request.operationId}; }
        }));
    }
}
}
