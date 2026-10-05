#include "vodruntime.h"
#include "player/mpvplayer.h"
#include <QFutureWatcher>
#include <QtConcurrentRun>

namespace OKILTV::Vod {
void VodRuntime::subtitleWork(const ContentRef &ref, SubtitleOperation operation, const QUrl &file,
    const QString &id, const QJsonObject &selection, bool apply)
{
    if (!m_ready || m_stopped || !m_subtitles || !ref.playable()) return;
    if (!m_subtitleCancellation.contains(ref.profileId)) m_subtitleCancellation.insert(ref.profileId, std::make_shared<std::atomic_bool>(false));
    const auto cancelled = m_subtitleCancellation.value(ref.profileId);
    const auto sequence = ++m_subtitleSequence;
    m_subtitlePending.insert(ref.key(), sequence);
    if (apply) m_subtitleIntents.insert(ref.key());
    m_subtitleError.clear();
    emit subtitlesChanged(ref); emit stateChanged();
    const auto cache = m_subtitles; const auto store = m_store;
    const auto load = active() && m_module->session()->snapshot().ref == ref ? m_module->session()->snapshot().loadToken : QUuid{};
    auto *watcher = new QFutureWatcher<Result<QJsonObject>>(this);
    connect(watcher, &QFutureWatcher<Result<QJsonObject>>::finished, this, [this, watcher, ref, sequence, apply, load]() {
        const auto result = watcher->result(); watcher->deleteLater();
        if (m_stopped || m_subtitlePending.value(ref.key()) != sequence) return;
        m_subtitlePending.remove(ref.key());
        m_subtitleIntents.remove(ref.key());
        if (const auto *error = std::get_if<Error>(&result)) {
            if (!m_subtitleStates.contains(ref.key())) m_subtitleStates.insert(ref.key(), {});
            m_subtitleError = tr("Could not save or read subtitles. Check the file (including the .idx/.sub pair) and available disk space. %1").arg(error->message());
            emit notification(m_subtitleError);
        } else {
            const auto state = std::get<QJsonObject>(result);
            m_subtitleStates.insert(ref.key(), state);
            if (apply && !load.isNull() && active() && m_module->session()->snapshot().loadToken == load) {
                m_module->session()->engine()->updateExternalSubtitles(m_subtitles->playbackFiles(ref, state), state.value(QStringLiteral("selection")).toObject(), load);
            }
        }
        emit subtitlesChanged(ref); emit stateChanged();
    });
    watcher->setFuture(QtConcurrent::run(&m_subtitlePool, [cache, store, ref, operation, file, id, selection, cancelled]() -> Result<QJsonObject> {
        try {
            if (cancelled->load()) return Error{ErrorCode::Cancelled, {}};
            const auto source = store->snapshot(ref.profileId);
            if (const auto *error = std::get_if<Error>(&source)) return *error;
            RequestContext context; context.source = std::get<SourceContext>(source).revision; context.deadline = QDeadlineTimer(60000); context.cancelled = cancelled;
            switch (operation) {
            case SubtitleOperation::Read: return cache->read(ref, context);
            case SubtitleOperation::Import: return cache->importFile(ref, file, context);
            case SubtitleOperation::Remove: return cache->remove(ref, id, context);
            case SubtitleOperation::Select: return cache->select(ref, selection, context);
            }
        } catch (...) { return Error{ErrorCode::StorageUnavailable, {}}; }
        return Error{ErrorCode::StorageUnavailable, {}};
    }));
    // QObject parent owns the watcher; completion schedules deleteLater, shutdown joins its worker.
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks)
void VodRuntime::requestSubtitleState(const ContentRef &ref)
{
    if (!m_subtitleStates.contains(ref.key()) && !m_subtitlePending.contains(ref.key())) subtitleWork(ref, SubtitleOperation::Read);
}
void VodRuntime::beginSubtitleUpload()
{
    if (active()) beginSubtitleUpload(m_module->session()->snapshot().ref);
}
void VodRuntime::beginSubtitleUpload(const ContentRef &ref)
{
    if (!m_ready || m_stopped || !ref.playable() || subtitleDialogOpen() || subtitleBusy(ref)) return;
    m_uploadRef = ref;
    if (active() && !isPaused()) {
        m_uploadPausedSession = m_module->session()->snapshot().sessionToken;
        m_module->session()->pause();
    }
    emit stateChanged(); emit subtitleUploadRequested();
}
void VodRuntime::finishSubtitleUpload(const QUrl &file)
{
    const auto ref = std::exchange(m_uploadRef, {});
    const auto paused = std::exchange(m_uploadPausedSession, {});
    if (!paused.isNull() && active() && isPaused() && m_module->session()->snapshot().sessionToken == paused)
        m_module->session()->resume();
    if (file.isValid() && !file.isEmpty() && ref.playable()) subtitleWork(ref, SubtitleOperation::Import, file, {}, {}, true);
    emit stateChanged();
}
void VodRuntime::selectSubtitlePreference(const ContentRef &ref, const QJsonObject &selection)
{
    subtitleWork(ref, SubtitleOperation::Select, {}, {}, selection, true);
}
void VodRuntime::selectUploadedSubtitle(const QString &id)
{
    if (active()) selectSubtitlePreference(m_module->session()->snapshot().ref,
        {{QStringLiteral("mode"), QStringLiteral("external")}, {QStringLiteral("externalId"), id}, {QStringLiteral("ordinal"), 0}});
}
void VodRuntime::removeUploadedSubtitle(const QString &id)
{
    if (active()) removeUploadedSubtitle(m_module->session()->snapshot().ref, id);
}
void VodRuntime::removeUploadedSubtitle(const ContentRef &ref, const QString &id)
{
    if (subtitleBusy(ref) || id.isEmpty()) return;
    if (active() && m_module->session()->snapshot().ref == ref) {
        const auto load = m_module->session()->snapshot().loadToken;
        m_subtitlePending.insert(ref.key(), ++m_subtitleSequence);
        emit subtitlesChanged(ref); emit stateChanged();
        m_module->session()->engine()->removeExternalSubtitle(id, load, [this, ref, id, load](bool success) {
            if (m_stopped) return;
            m_subtitlePending.remove(ref.key());
            if (success && active() && m_module->session()->snapshot().loadToken == load) subtitleWork(ref, SubtitleOperation::Remove, {}, id, {}, true);
            else { m_subtitleError = tr("Could not remove subtitles from the player. Try again."); emit subtitlesChanged(ref); emit stateChanged(); }
        });
    } else subtitleWork(ref, SubtitleOperation::Remove, {}, id);
}
void VodRuntime::observeSubtitles(const SessionSnapshot &snapshot)
{
    if (snapshot.end || snapshot.sessionToken != m_uploadPausedSession) m_uploadPausedSession = QUuid{};
    if (!snapshot.ref.playable() || snapshot.end) return;
    const auto selection = snapshot.trackPreferences.value(QStringLiteral("sub")).toObject();
    if (m_observedSubtitleLoad != snapshot.loadToken) {
        m_observedSubtitleLoad = snapshot.loadToken; m_observedSubtitlePreference = selection;
        requestSubtitleState(snapshot.ref);
    } else if (selection != m_observedSubtitlePreference) {
        m_observedSubtitlePreference = selection;
        if (!m_subtitleIntents.contains(snapshot.ref.key())) subtitleWork(snapshot.ref, SubtitleOperation::Select, {}, {}, selection);
    }
    auto *backend = qobject_cast<Player::MpvPlayer *>(playerObject());
    if (backend && backend != m_subtitleBackend) {
        m_subtitleBackend = backend;
        connect(backend, &Player::MpvPlayer::externalSubtitleError, this, [this](const QUuid &load) {
            if (!active() || m_module->session()->snapshot().loadToken != load) return;
            m_subtitleError = tr("The subtitle file could not be loaded by mpv. Choose another file or remove it.");
            emit notification(m_subtitleError); emit subtitlesChanged(m_module->session()->snapshot().ref); emit stateChanged();
        });
    }
}
}
