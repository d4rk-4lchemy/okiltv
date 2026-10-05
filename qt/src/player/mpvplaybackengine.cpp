#include "mpvplaybackengine.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace OKILTV::Player {
namespace {
std::optional<qint64> milliseconds(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0 || seconds >= static_cast<double>(std::numeric_limits<qint64>::max()) / 1000.0) return {};
    return static_cast<qint64>(seconds * 1000);
}
}
MpvPlaybackEngine::MpvPlaybackEngine(std::unique_ptr<MpvPlayer> player, bool requireRenderSurface)
    : m_player(std::move(player)), m_requireRenderSurface(requireRenderSurface)
{
    if (m_requireRenderSurface) m_player->requireNativeRenderSurface();
    connect(m_player.get(), &MpvPlayer::renderContextReady, this, [this]() {
        if (!m_waitingForRender || m_token.isNull() || !m_player->renderContextAvailable()) return;
        auto request = std::exchange(m_waitingForRender, {});
        request->startPaused = m_requestedPause;
        if (!m_player->play(*request, m_token)) finish(m_token, EndReason::Error);
        else m_timer.start();
    });
    connect(m_player.get(), &MpvPlayer::videoReconfigured, this, [this]() {
        if (!m_token.isNull() && !m_stopReason) m_player->detectAndApplyDeinterlace();
    });
    connect(m_player.get(), &MpvPlayer::trackPreferenceChanged, this,
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- Matches the Qt signal signature.
        [this](const QString &profile, const QString &key, const QString &type, const QJsonObject &preference) {
            if (m_token.isNull() || profile != m_token.toString() || key != QStringLiteral("vod") || m_stopReason) return;
            if (type == QLatin1String("sub")) m_subtitleSelectionConfirmed = true;
            if (preference.isEmpty()) m_trackPreferences.remove(type);
            else m_trackPreferences.insert(type, preference);
            sample();
        });
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, [this]() { sample(); });
    connect(m_player.get(), &MpvPlayer::mediaLoaded, this, [this](const QUuid &token) {
        if (token != m_token || m_token.isNull()) return;
        m_loaded = true;
        m_player->updateExternalSubtitles(m_externalSubtitles, m_requestedSubtitleSelection, token);
        // MpvPlayer refreshes telemetry on its event thread before this signal.
        sample(true);
    });
    connect(m_player.get(), &MpvPlayer::mediaEnded, this, [this](const QUuid &token, EndReason reason, bool retryable) { finish(token, reason, retryable); });
    connect(m_player.get(), &MpvPlayer::mediaSeekCompleted, this, [this](const QUuid &token) {
        if (token == m_token && m_loaded) sample(false, true);
    });
}
MpvPlaybackEngine::~MpvPlaybackEngine()
{
    m_listener = {}; m_pending.reset();
    if (m_rangeStream) m_rangeStream->cancel();
    m_player->stop();
    m_player.reset(); // Release mpv callback references before the GUI-owned stream.
    m_rangeStream.reset();
}
void MpvPlaybackEngine::setListener(Listener listener) { m_listener = std::move(listener); }
void MpvPlaybackEngine::load(const PlaybackRequest &request, const QUuid &token)
{
    if (!m_token.isNull()) {
        if (m_pending && m_listener) m_listener({m_pending->second, EngineState::Stopped, -1, {}, false, false, EndReason::Replaced});
        m_pending = std::make_pair(request, token);
        stop(EndReason::Replaced);
        return;
    }
    m_token = token;
    m_requestedPause = request.startPaused;
    m_trackPreferences = request.trackPreferences;
    m_externalSubtitles = request.externalSubtitles;
    m_requestedSubtitleSelection = request.trackPreferences.value(QStringLiteral("sub")).toObject();
    m_subtitleSelectionConfirmed = m_externalSubtitles.isEmpty() && m_requestedSubtitleSelection.value(QStringLiteral("mode")).toString() != QLatin1String("external");
    m_player->resetExternalSubtitles();
    // Load-scoped identity fences confirmations; persistence belongs to the VOD
    // session. Keep unavailable choices and remember explicit baseline choices.
    m_player->configureTrackPreferences(token.toString(), QStringLiteral("vod"), m_trackPreferences, false, true);
    m_loaded = false;
    m_capabilitiesDelivered = false;
    m_stopReason.reset();
    m_last = {token, EngineState::Loading, -1, {}, false, false, {}};
    if (m_listener) m_listener(m_last);
    if (m_token != token) return;
    if (m_player->vodRangeCacheEligible(request)) {
        m_preparingRange = true;
        m_rangeStream = VodRangeStream::create(request.mediaUri,
            request.allowedHeaders.value("User-Agent", request.allowedHeaders.value("user-agent", m_player->vodUserAgent())));
        m_rangeStream->prepare([this, request, token](bool ready) {
            if (token != m_token || !m_preparingRange) return;
            m_preparingRange = false;
            auto effective = request;
            effective.startPaused = m_requestedPause;
            if (ready) effective.mediaUri = QUrl(m_rangeStream->virtualUrl());
            else if (m_rangeStream->nativeFallbackAllowed()) m_rangeStream.reset();
            else { finish(token, EndReason::Error, true); return; }
            startLoad(effective);
        });
        return;
    }
    startLoad(request);
}
void MpvPlaybackEngine::startLoad(const PlaybackRequest &request)
{
    const auto token = m_token;
    if (m_requireRenderSurface && !m_player->renderContextAvailable()) {
        m_waitingForRender = request;
        QTimer::singleShot(5000, this, [this, token]() {
            if (token == m_token && m_waitingForRender) finish(token, EndReason::Error);
        });
        return;
    }
    if (!m_player->play(request, token)) { finish(token, EndReason::Error); return; }
    m_timer.start();
}
void MpvPlaybackEngine::pause()
{
    if (!m_token.isNull()) { m_requestedPause = true; m_player->setPaused(true); }
}
void MpvPlaybackEngine::resume()
{
    if (!m_token.isNull()) { m_requestedPause = false; m_player->setPaused(false); }
}
void MpvPlaybackEngine::seek(qint64 position)
{
    if (m_loaded && m_player->seekable().value_or(false)) m_player->seekAbsoluteExact(static_cast<double>(std::max(qint64(0), position)) / 1000);
}
void MpvPlaybackEngine::stop(EndReason reason)
{
    if (m_token.isNull()) return;
    if (m_waitingForRender || m_preparingRange) { finish(m_token, reason); return; }
    if (m_rangeStream) m_rangeStream->cancel();
    m_stopReason = reason;
    m_player->stop();
}
void MpvPlaybackEngine::setVolume(double volume) { m_player->setVolume(static_cast<int>(std::clamp(volume, 0.0, 100.0))); }
void MpvPlaybackEngine::sample(bool loaded, bool seekCompleted)
{
    if (m_token.isNull() || !m_loaded || m_stopReason || (!loaded && !m_capabilitiesDelivered)) return;
    if (loaded) m_capabilitiesDelivered = true;
    PlaybackEvent event;
    event.loadToken = m_token;
    event.positionMs = milliseconds(m_player->position()).value_or(-1);
    if (const auto duration = m_player->duration()) event.durationMs = milliseconds(*duration);
    event.seekable = m_player->seekable().value_or(false);
    event.state = loaded ? EngineState::Loaded : (m_player->bufferingState().value_or(false) ? EngineState::Buffering
        : (m_player->pauseState().value_or(false) ? EngineState::Paused : EngineState::Playing));
    event.seekCompleted = seekCompleted;
    if (m_subtitleSelectionConfirmed) event.trackPreferences = m_trackPreferences;
    event.videoWidth = m_player->videoWidth();
    event.videoHeight = m_player->videoHeight();
    event.tracks = m_player->trackList();
    m_last = event;
    if (m_listener) m_listener(event);
}
void MpvPlaybackEngine::finish(const QUuid &token, EndReason reason, bool retryable)
{
    if (token.isNull() || token != m_token) return;
    // Some demuxers report a failed read as EOF. Never mark an interrupted
    // remote transfer watched; retain the observed position for bounded recovery.
    if (!m_stopReason && m_rangeStream && m_rangeStream->failed()) {
        reason = EndReason::Error;
        retryable = true;
    }
    auto event = m_last;
    event.end = reason == EndReason::UserStop ? m_stopReason.value_or(EndReason::UserStop) : reason;
    event.state = EngineState::Stopped;
    event.seekCompleted = false;
    event.retryable = retryable && !m_stopReason;
    m_timer.stop();
    if (m_rangeStream) m_rangeStream->cancel();
    m_rangeStream.reset();
    m_preparingRange = false;
    m_waitingForRender.reset();
    m_token = QUuid{};
    m_player->resetExternalSubtitles();
    m_loaded = false;
    m_stopReason.reset();
    auto pending = std::move(m_pending);
    m_pending.reset();
    if (m_listener) m_listener(event);
    if (pending && m_token.isNull()) load(pending->first, pending->second);
}
void MpvPlaybackEngine::updateExternalSubtitles(const QVariantList &files, const QJsonObject &selection, const QUuid &token)
{
    if (token != m_token || m_stopReason) return;
    m_externalSubtitles = files;
    m_requestedSubtitleSelection = selection;
    m_subtitleSelectionConfirmed = false;
    if (m_loaded) m_player->updateExternalSubtitles(files, selection, token);
}
void MpvPlaybackEngine::removeExternalSubtitle(const QString &id, const QUuid &token, std::function<void(bool)> done)
{
    if (token != m_token || !m_loaded || m_stopReason) { done(false); return; }
    m_player->removeExternalSubtitle(id, token, std::move(done));
}

}
