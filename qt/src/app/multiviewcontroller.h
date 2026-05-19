#pragma once

#include "../core/models.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

#include <memory>
#include <optional>
#include <vector>

namespace OKILTV::Core {
class SettingsManager;
}

namespace OKILTV::Player {
class MpvPlayer;
}

namespace OKILTV::App {

class ChannelListModel;
class PlayerController;

class MultiViewController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString layoutMode READ layoutMode NOTIFY layoutModeChanged)
    Q_PROPERTY(int tileCount READ tileCount NOTIFY tilesChanged)
    Q_PROPERTY(int focusedTileIndex READ focusedTileIndex NOTIFY focusedTileIndexChanged)
    Q_PROPERTY(int maxTiles READ maxTiles NOTIFY layoutModeChanged)
    Q_PROPERTY(int layoutColumns READ layoutColumns NOTIFY layoutModeChanged)
    Q_PROPERTY(int layoutRows READ layoutRows NOTIFY layoutModeChanged)
    Q_PROPERTY(QVariantList tiles READ tiles NOTIFY tilesChanged)
    Q_PROPERTY(bool retainedSelectionActive READ retainedSelectionActive NOTIFY retainedSelectionActiveChanged)
    Q_PROPERTY(bool degradePromptVisible READ degradePromptVisible NOTIFY degradePromptVisibleChanged)
    Q_PROPERTY(QString pendingDegradeLayout READ pendingDegradeLayout NOTIFY degradePromptVisibleChanged)

public:
    explicit MultiViewController(
        Core::SettingsManager *settings,
        ChannelListModel *channelListModel,
        PlayerController *playerController,
        QObject *parent = nullptr);
    ~MultiViewController() override;

    QString layoutMode() const;
    int tileCount() const;
    int focusedTileIndex() const;
    int maxTiles() const;
    int layoutColumns() const;
    int layoutRows() const;
    QVariantList tiles() const;
    bool retainedSelectionActive() const;
    bool degradePromptVisible() const;
    QString pendingDegradeLayout() const;

    bool isActive() const;
    bool focusedTileIsPrimary() const;
    bool assignResolvedChannel(const Core::Channel &channel);

    Q_INVOKABLE void cycleLayout();
    Q_INVOKABLE void setLayoutMode(const QString &mode);
    Q_INVOKABLE bool togglePictureInPicture(int channelId);
    Q_INVOKABLE bool toggleGrid();
    Q_INVOKABLE bool stopRetainedPromotedAndRestoreGrid();
    Q_INVOKABLE bool swapPrimaryWithPictureInPicture();
    Q_INVOKABLE void assignChannelToFocusedTile(int channelId);
    Q_INVOKABLE void focusNextTile();
    Q_INVOKABLE void focusTile(int tileIndex);
    Q_INVOKABLE void closeFocusedTile();
    Q_INVOKABLE void exitMultiView();
    Q_INVOKABLE bool fullPromoteAndExit();
    Q_INVOKABLE void acceptPendingDegrade();
    Q_INVOKABLE void declinePendingDegrade();

public slots:
    void applySettings();

signals:
    void layoutModeChanged();
    void focusedTileIndexChanged();
    void tilesChanged();
    void retainedSelectionActiveChanged();
    void degradePromptVisibleChanged();
    void primaryTileAssignmentRequested(int channelId);
    void statusMessageRequested(const QString &message);

private:
    enum class ExitIntent
    {
        SoftRetain,
        FullPromotion,
        DegradeOff,
        ForcedOff
    };

    struct SecondarySlot
    {
        int slotIndex { 1 };
        std::unique_ptr<Player::MpvPlayer> player;
        QPointer<Player::MpvPlayer> borrowedPlayer;
        std::optional<Core::Channel> channel;
        QString playerState { QStringLiteral("empty") };
        bool hasError { false };
        QString errorText;

        [[nodiscard]] Player::MpvPlayer *playbackPlayer() const
        {
            return player ? player.get() : borrowedPlayer.data();
        }
    };

    static QString normalizedLayoutMode(const QString &mode);
    static bool isGridLayout(const QString &mode);
    static QString layoutLabel(const QString &mode);
    static QString gridLayoutModeForTileCount(int tileCount);
    bool exitMultiViewWithIntent(ExitIntent intent);
    bool hasRetainableSecondaryChannels() const;
    bool shouldRetainSelectionOnGridPromotion() const;
    void setRetainedSelectionActive(bool active);
    void clearRetainedSelectionTracking();
    void pruneRetainedSecondaryDuplicatesForPrimary();

    int configuredMaxTiles() const;
    int layoutCapacity(const QString &mode) const;
    int layoutColumnsForMode(const QString &mode) const;
    int layoutRowsForMode(const QString &mode) const;
    int visibleSlotCount() const;
    int occupiedTileCount() const;
    int normalizedFocusedTileIndex() const;
    int slotIndexForPromotion() const;
    int desiredFocusedTileVolume() const;
    QString primaryTileState() const;
    QString degradeSignature() const;
    bool multiviewEnabled() const;
    bool slotHasDuplicateChannel(const Core::Channel &channel, int targetSlotIndex) const;
    bool assignChannelToSecondarySlot(int slotIndex, const Core::Channel &channel);
    bool promoteSecondarySlotToPrimary(int slotIndex);
    bool swapPrimaryWithSecondarySlotNoReconnect(int slotIndex);
    bool stopFocusedTileInActiveGrid();
    void setFocusedTileIndexInternal(int index, bool logChange);
    void setLayoutModeInternal(const QString &mode);
    void ensureSecondarySlotsForCurrentLayout();
    void trimSecondarySlotsForCurrentLayout();
    void clearSecondarySlot(int slotIndex);
    void clearAllSecondarySlots();
    void configureSecondaryPlayer(Player::MpvPlayer &player) const;
    void connectSecondaryPlayerSignals(Player::MpvPlayer *player, int slotIndex);
    bool secondarySlotOwnsPlaybackPlayer(int slotIndex, const Player::MpvPlayer *player) const;
    void updateSecondarySlotState(int slotIndex, const Player::MpvPlayer *player, const QString &state);
    void updateSecondarySlotError(int slotIndex, const Player::MpvPlayer *player, const QString &message);
    void clearDegradePrompt(bool clearSuppression);
    void resetDegradeMonitor();
    void applyFocusedAudioOwnership();
    void handlePrimaryChannelChanged();
    void handleDecodePressureTick();
    void retireDetachedPlayer(std::unique_ptr<Player::MpvPlayer> player);
    void flushRetiredPlayers();
    void scheduleFocusedAudioOwnershipRefresh();
    void emitTilesChanged();
    void logLayoutChange(const QString &previousMode, const QString &nextMode) const;

    Core::SettingsManager *m_settings;
    ChannelListModel *m_channelListModel;
    PlayerController *m_playerController;
    QString m_layoutMode { QStringLiteral("off") };
    std::vector<SecondarySlot> m_secondarySlots;
    std::unique_ptr<Player::MpvPlayer> m_adoptedPrimaryPlayer;
    bool m_retainedSelectionActive { false };
    std::optional<int> m_softRetainOriginSlotIndex;
    std::optional<Core::Channel> m_softRetainPromotedChannel;
    int m_focusedTileIndex { 0 };
    std::optional<int> m_lastFocusedSecondarySlotIndex;
    QTimer m_decodePressureTimer;
    QTimer m_audioOwnershipRefreshTimer;
    QTimer m_retiredPlayerCleanupTimer;
    std::optional<int> m_lastFocusedDroppedFrames;
    int m_decodePressureConsecutiveTicks { 0 };
    bool m_degradePromptVisible { false };
    QString m_pendingDegradeLayout;
    QString m_declinedDegradeSignature;
    bool m_promotingSecondaryToPrimary { false };
    bool m_swappingPrimaryAndSecondary { false };
    bool m_skipPrimaryAutoPromotionOnce { false };
    std::vector<std::unique_ptr<Player::MpvPlayer>> m_retiredPlayers;
};

} // namespace OKILTV::App
