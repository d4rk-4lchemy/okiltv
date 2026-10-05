#pragma once
#include "iplaybackengine.h"
#include "mpvplayer.h"
#include "vodrangestream.h"
#include <QTimer>

namespace OKILTV::Player {
class MpvPlaybackEngine final : public QObject, public IPlaybackEngine {
public:
    explicit MpvPlaybackEngine(std::unique_ptr<MpvPlayer> player = std::make_unique<MpvPlayer>(), bool requireRenderSurface = false);
    ~MpvPlaybackEngine() override;
    void setListener(Listener listener) override;
    void load(const PlaybackRequest &, const QUuid &) override;
    void pause() override;
    void resume() override;
    void seek(qint64) override;
    void stop(EndReason) override;
    void setVolume(double) override;
    void *renderHandle() const override { return m_player.get(); }
    void updateExternalSubtitles(const QVariantList &, const QJsonObject &, const QUuid &) override;
    void removeExternalSubtitle(const QString &, const QUuid &, std::function<void(bool)>) override;
    MpvPlayer *player() const { return m_player.get(); }
private:
    void startLoad(const PlaybackRequest &request);
    void sample(bool loaded = false, bool seekCompleted = false);
    void finish(const QUuid &, EndReason, bool retryable = false);
    std::unique_ptr<MpvPlayer> m_player;
    Listener m_listener;
    QTimer m_timer;
    QUuid m_token;
    bool m_loaded = false;
    bool m_capabilitiesDelivered = false;
    std::optional<EndReason> m_stopReason;
    std::optional<std::pair<PlaybackRequest, QUuid>> m_pending;
    PlaybackEvent m_last;
    QJsonObject m_trackPreferences;
    QVariantList m_externalSubtitles;
    QJsonObject m_requestedSubtitleSelection;
    bool m_subtitleSelectionConfirmed = true;
    bool m_requireRenderSurface = false;
    std::optional<PlaybackRequest> m_waitingForRender;
    VodRangeStream::Ptr m_rangeStream;
    bool m_preparingRange = false;
    bool m_requestedPause = false;
};
}
