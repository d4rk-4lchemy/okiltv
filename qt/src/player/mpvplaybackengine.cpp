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
        const auto request = std::exchange(m_waitingForRender, {});
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
            if (preference.isEmpty()) m_trackPreferences.remove(type);
            else m_trackPreferences.insert(type, preference);
            sample();
        });
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, [this]() { sample(); });
    connect(m_player.get(), &MpvPlayer::mediaLoaded, this, [this](const QUuid &token) {
        if (token != m_token || m_token.isNull()) return;
        m_loaded = true;
        // MpvPlayer refreshes telemetry on its event thread before this signal.
        sample(true);
    });
    connect(m_player.get(), &MpvPlayer::mediaEnded, this, [this](const QUuid &token, EndReason reason, bool retryable) { finish(token, reason, retryable); });
    connect(m_player.get(), &MpvPlayer::mediaSeekCompleted, this, [this](const QUuid &token) {
        if (token == m_token && m_loaded) sample(false, true);
    });
}
MpvPlaybackEngine::~MpvPlaybackEngine() { m_listener = {}; m_pending.reset(); m_player->stop(); }
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
    m_trackPreferences = request.trackPreferences;
    // Load-scoped identity fences confirmations; persistence belongs to the VOD
    // session. Keep unavailable choices and remember explicit baseline choices.
    m_player->configureTrackPreferences(token.toString(), QStringLiteral("vod"), m_trackPreferences, false, true);
    m_loaded = false;
    m_capabilitiesDelivered = false;
    m_stopReason.reset();
    m_last = {token, EngineState::Loading, -1, {}, false, false, {}};
    if (m_listener) m_listener(m_last);
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
void MpvPlaybackEngine::pause() { if (!m_token.isNull()) m_player->setPaused(true); }
void MpvPlaybackEngine::resume() { if (!m_token.isNull()) m_player->setPaused(false); }
void MpvPlaybackEngine::seek(qint64 position)
{
    if (m_loaded && m_player->seekable().value_or(false)) m_player->seekAbsoluteExact(static_cast<double>(std::max(qint64(0), position)) / 1000);
}
void MpvPlaybackEngine::stop(EndReason reason)
{
    if (m_token.isNull()) return;
    if (m_waitingForRender) { finish(m_token, reason); return; }
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
    event.trackPreferences = m_trackPreferences;
    m_last = event;
    if (m_listener) m_listener(event);
}
void MpvPlaybackEngine::finish(const QUuid &token, EndReason reason, bool retryable)
{
    if (token.isNull() || token != m_token) return;
    auto event = m_last;
    event.end = reason == EndReason::UserStop ? m_stopReason.value_or(EndReason::UserStop) : reason;
    event.state = EngineState::Stopped;
    event.seekCompleted = false;
    event.retryable = retryable && !m_stopReason;
    m_timer.stop();
    m_waitingForRender.reset();
    m_token = QUuid{};
    m_loaded = false;
    m_stopReason.reset();
    auto pending = std::move(m_pending);
    m_pending.reset();
    if (m_listener) m_listener(event);
    if (pending && m_token.isNull()) load(pending->first, pending->second);
}
}
