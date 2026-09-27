#pragma once
#include <QMap>
#include <QJsonObject>
#include <QUrl>
#include <QUuid>
#include <optional>

namespace OKILTV::Player {
enum class TransportPolicy { NativeMedia, LiveMpegTsNormalized };
enum class PlaybackPolicy { OnDemand, ContinuousLive };
enum class EndReason { NaturalEnd, UserStop, Replaced, Redirected, Error, Shutdown, Unknown };
enum class EngineState { Loading, Loaded, Playing, Paused, Buffering, Stopped };
struct PlaybackRequest {
    QUrl mediaUri;
    QMap<QByteArray, QByteArray> allowedHeaders;
    TransportPolicy transport = TransportPolicy::NativeMedia;
    PlaybackPolicy policy = PlaybackPolicy::OnDemand;
    std::optional<qint64> initialPositionMs;
    // Application invariants only; provider JSON cannot fill this map. The mpv
    // adapter rejects unknown/conflicting entries. Global user options are
    // inherited through configureOptions(), beneath per-load invariants.
    QMap<QString, QString> validatedEngineOptions;
    bool startPaused = false;
    QJsonObject trackPreferences;
};
struct PlaybackEvent {
    QUuid loadToken;
    EngineState state = EngineState::Loading;
    qint64 positionMs = 0;
    std::optional<qint64> durationMs;
    bool seekable = false;
    bool seekCompleted = false;
    std::optional<EndReason> end;
    bool retryable = false;
    std::optional<QJsonObject> trackPreferences;
};
}
