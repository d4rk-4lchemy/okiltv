#pragma once
#include "playbackrequest.h"
#include <functional>

namespace OKILTV::Player {
class IPlaybackEngine {
public:
    using Listener = std::function<void(const PlaybackEvent &)>;
    virtual ~IPlaybackEngine() = default;
    // Commands/events on the owner thread. Tokens originate at actual loads,
    // never by relabelling an old untagged mpv signal with the current token.
    virtual void setListener(Listener) = 0;
    virtual void load(const PlaybackRequest &, const QUuid &loadToken) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void seek(qint64 positionMs) = 0;
    virtual void stop(EndReason) = 0;
    virtual void setVolume(double percent) = 0;
    // Opaque borrowed handle, only the future renderer adapter may interpret it.
    virtual void updateExternalSubtitles(const QVariantList &, const QJsonObject &, const QUuid &) {}
    // NOLINTNEXTLINE(performance-unnecessary-value-param) -- Async adapters take ownership of the completion callable.
    virtual void removeExternalSubtitle(const QString &, const QUuid &, std::function<void(bool)> done) { done(false); }
    virtual void *renderHandle() const = 0;
};
}
