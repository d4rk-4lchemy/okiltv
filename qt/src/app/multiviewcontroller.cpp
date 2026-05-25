#include "multiviewcontroller.h"

#include "channellistmodel.h"
#include "playercontroller.h"

#include "../core/debuglogger.h"
#include "../core/settingsmanager.h"
#include "../player/mpvplayer.h"

#include <QVariantMap>

#include <algorithm>
#include <cmath>

namespace OKILTV::App {

using namespace Core;
using namespace Player;

namespace {

constexpr int kDecodePressurePollIntervalMs = 1000;
constexpr int kDecodePressureDeltaThreshold = 15;
constexpr int kDecodePressureConsecutiveTickThreshold = 3;

QString normalizedLayoutModeValue(QString value)
{
    value = value.trimmed().toLower();
    if (value == QStringLiteral("pip")) {
        return QStringLiteral("pip");
    }
    if (value == QStringLiteral("grid")
        || value == QStringLiteral("grid2x1")
        || value == QStringLiteral("2x1")) {
        return value == QStringLiteral("grid") ? QStringLiteral("grid") : QStringLiteral("grid2x1");
    }
    if (value == QStringLiteral("grid2x2") || value == QStringLiteral("2x2")) {
        return QStringLiteral("grid2x2");
    }
    if (value == QStringLiteral("grid3x2") || value == QStringLiteral("3x2")) {
        return QStringLiteral("grid3x2");
    }
    if (value == QStringLiteral("grid3x3") || value == QStringLiteral("3x3")) {
        return QStringLiteral("grid3x3");
    }
    if (value == QStringLiteral("grid4x3") || value == QStringLiteral("4x3")) {
        return QStringLiteral("grid4x3");
    }
    return QStringLiteral("off");
}

QString layoutLabelValue(const QString &mode)
{
    if (mode == QStringLiteral("pip")) {
        return QStringLiteral("PiP");
    }
    if (mode == QStringLiteral("grid2x1")) {
        return QStringLiteral("2x1");
    }
    if (mode == QStringLiteral("grid2x2")) {
        return QStringLiteral("2x2");
    }
    if (mode == QStringLiteral("grid3x2")) {
        return QStringLiteral("3x2");
    }
    if (mode == QStringLiteral("grid3x3")) {
        return QStringLiteral("3x3");
    }
    if (mode == QStringLiteral("grid4x3")) {
        return QStringLiteral("4x3");
    }
    return QStringLiteral("Off");
}

int normalizedMultiviewMaxTiles(const AppSettings &settings)
{
    return Core::normalizeMultiviewMaxTiles(settings.multiviewMaxTiles);
}

bool isGridLayoutValue(const QString &mode)
{
    return mode == QStringLiteral("grid2x1")
        || mode == QStringLiteral("grid2x2")
        || mode == QStringLiteral("grid3x2")
        || mode == QStringLiteral("grid3x3")
        || mode == QStringLiteral("grid4x3");
}

QString gridLayoutModeForTileCountValue(const int tileCount)
{
    switch (tileCount) {
    case 2:
        return QStringLiteral("grid2x1");
    case 4:
        return QStringLiteral("grid2x2");
    case 6:
        return QStringLiteral("grid3x2");
    case 9:
        return QStringLiteral("grid3x3");
    case 12:
    default:
        return QStringLiteral("grid4x3");
    }
}

int layoutColumnsForModeValue(const QString &mode)
{
    if (mode == QStringLiteral("grid2x1") || mode == QStringLiteral("grid2x2")) {
        return 2;
    }
    if (mode == QStringLiteral("grid3x2") || mode == QStringLiteral("grid3x3")) {
        return 3;
    }
    if (mode == QStringLiteral("grid4x3")) {
        return 4;
    }
    return 1;
}

int layoutRowsForModeValue(const QString &mode)
{
    if (mode == QStringLiteral("grid2x1")) {
        return 1;
    }
    if (mode == QStringLiteral("grid2x2")) {
        return 2;
    }
    if (mode == QStringLiteral("grid3x2") || mode == QStringLiteral("grid4x3")) {
        return 2 + (mode == QStringLiteral("grid4x3") ? 1 : 0);
    }
    if (mode == QStringLiteral("grid3x3")) {
        return 3;
    }
    return 1;
}

bool catchupMultiviewBlocked(PlayerController *playerController)
{
    return playerController != nullptr && playerController->playbackMode() == QStringLiteral("catchup");
}

} // namespace

MultiViewController::MultiViewController(
    SettingsManager *settings,
    ChannelListModel *channelListModel,
    PlayerController *playerController,
    QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_channelListModel(channelListModel)
    , m_playerController(playerController)
{
    m_decodePressureTimer.setInterval(kDecodePressurePollIntervalMs);
    connect(&m_decodePressureTimer, &QTimer::timeout, this, &MultiViewController::handleDecodePressureTick);
    m_audioOwnershipRefreshTimer.setSingleShot(true);
    m_audioOwnershipRefreshTimer.setInterval(150);
    connect(
        &m_audioOwnershipRefreshTimer,
        &QTimer::timeout,
        this,
        &MultiViewController::applyFocusedAudioOwnership);
    m_retiredPlayerCleanupTimer.setSingleShot(true);
    connect(&m_retiredPlayerCleanupTimer, &QTimer::timeout, this, &MultiViewController::flushRetiredPlayers);
    connect(m_playerController, &PlayerController::volumeChanged, this, &MultiViewController::applyFocusedAudioOwnership);
    connect(m_playerController, &PlayerController::mutedChanged, this, &MultiViewController::applyFocusedAudioOwnership);
    connect(m_playerController, &PlayerController::currentChannelChanged, this, &MultiViewController::handlePrimaryChannelChanged);
}

MultiViewController::~MultiViewController()
{
    m_decodePressureTimer.stop();
    m_audioOwnershipRefreshTimer.stop();
    m_retiredPlayerCleanupTimer.stop();
    if (m_playerController != nullptr) {
        disconnect(m_playerController, nullptr, this, nullptr);
    }
    if (m_playerController != nullptr
        && m_adoptedPrimaryPlayer
        && m_playerController->isSharedPlaybackPlayer(m_adoptedPrimaryPlayer.get())) {
        m_playerController->detachSharedPlayback();
    }
    clearAllSecondarySlots();
    if (m_adoptedPrimaryPlayer) {
        m_adoptedPrimaryPlayer->stop();
        m_adoptedPrimaryPlayer.reset();
    }
    flushRetiredPlayers();
}

QString MultiViewController::layoutMode() const
{
    return m_layoutMode;
}

int MultiViewController::tileCount() const
{
    return occupiedTileCount();
}

int MultiViewController::focusedTileIndex() const
{
    return normalizedFocusedTileIndex();
}

int MultiViewController::maxTiles() const
{
    return layoutCapacity(m_layoutMode);
}

int MultiViewController::layoutColumns() const
{
    return layoutColumnsForMode(m_layoutMode);
}

int MultiViewController::layoutRows() const
{
    return layoutRowsForMode(m_layoutMode);
}

QVariantList MultiViewController::tiles() const
{
    QVariantList result;
    const auto primaryChannel = m_playerController->currentChannelValue();
    const auto slotCount = visibleSlotCount();
    if (slotCount <= 0) {
        return result;
    }

    const auto focusIndex = normalizedFocusedTileIndex();
    for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex) {
        QVariantMap tile;
        tile.insert(QStringLiteral("tileIndex"), slotIndex);
        tile.insert(QStringLiteral("tileId"), slotIndex == 0 ? QStringLiteral("primary") : QStringLiteral("secondary-%1").arg(slotIndex));
        tile.insert(QStringLiteral("isFocused"), slotIndex == focusIndex);
        tile.insert(QStringLiteral("isPrimary"), slotIndex == 0);

        if (slotIndex == 0) {
            tile.insert(QStringLiteral("isEmpty"), !primaryChannel.has_value());
            tile.insert(QStringLiteral("channelId"), primaryChannel.has_value() ? primaryChannel->id : -1);
            tile.insert(QStringLiteral("channelName"), primaryChannel.has_value() ? primaryChannel->name : QStringLiteral("Primary tile"));
            tile.insert(QStringLiteral("playerState"), primaryTileState());
            tile.insert(QStringLiteral("hasError"), m_playerController->channelLoadFailed());
            tile.insert(
                QStringLiteral("errorText"),
                m_playerController->channelLoadFailed() ? QStringLiteral("Channel couldn't be loaded") : QString {});
            tile.insert(QStringLiteral("playerObject"), QVariant::fromValue(m_playerController->playbackPlayerObject()));
        } else {
            const auto secondaryIndex = static_cast<std::size_t>(slotIndex - 1);
            if (secondaryIndex < m_secondarySlots.size()) {
                const auto &slot = m_secondarySlots.at(secondaryIndex);
                tile.insert(QStringLiteral("isEmpty"), !slot.channel.has_value());
                tile.insert(QStringLiteral("channelId"), slot.channel.has_value() ? slot.channel->id : -1);
                tile.insert(QStringLiteral("channelName"), slot.channel.has_value() ? slot.channel->name : QStringLiteral("Empty tile"));
                tile.insert(QStringLiteral("playerState"), slot.playerState);
                tile.insert(QStringLiteral("hasError"), slot.hasError);
                tile.insert(QStringLiteral("errorText"), slot.errorText);
                tile.insert(
                    QStringLiteral("playerObject"),
                    QVariant::fromValue(static_cast<QObject *>(slot.playbackPlayer())));
            } else {
                tile.insert(QStringLiteral("isEmpty"), true);
                tile.insert(QStringLiteral("channelId"), -1);
                tile.insert(QStringLiteral("channelName"), QStringLiteral("Empty tile"));
                tile.insert(QStringLiteral("playerState"), QStringLiteral("empty"));
                tile.insert(QStringLiteral("hasError"), false);
                tile.insert(QStringLiteral("errorText"), QString {});
                tile.insert(QStringLiteral("playerObject"), QVariant::fromValue(static_cast<QObject *>(nullptr)));
            }
        }

        result.push_back(tile);
    }

    return result;
}

bool MultiViewController::retainedSelectionActive() const
{
    return m_retainedSelectionActive;
}

bool MultiViewController::degradePromptVisible() const
{
    return m_degradePromptVisible;
}

QString MultiViewController::pendingDegradeLayout() const
{
    return m_pendingDegradeLayout;
}

bool MultiViewController::isActive() const
{
    return m_layoutMode != QStringLiteral("off");
}

bool MultiViewController::focusedTileIsPrimary() const
{
    return normalizedFocusedTileIndex() == 0;
}

bool MultiViewController::assignResolvedChannel(const Channel &channel)
{
    if (!isActive()) {
        return false;
    }

    if (focusedTileIsPrimary()) {
        const auto primaryChannel = m_playerController->currentChannelValue();
        if (primaryChannel.has_value()
            && primaryChannel->profileId == channel.profileId
            && primaryChannel->id == channel.id) {
            return true;
        }
        if (slotHasDuplicateChannel(channel, 0)) {
            emit statusMessageRequested(QStringLiteral("Channel already open in another tile"));
            return false;
        }
        emit primaryTileAssignmentRequested(channel.id);
        return true;
    }

    return assignChannelToSecondarySlot(normalizedFocusedTileIndex(), channel);
}

void MultiViewController::cycleLayout()
{
    if (catchupMultiviewBlocked(m_playerController)) {
        emit statusMessageRequested(QStringLiteral("Multiview is unavailable during catch-up playback."));
        return;
    }
    if (!multiviewEnabled()) {
        emit statusMessageRequested(QStringLiteral("Multiview is disabled in Settings."));
        return;
    }

    QString nextMode = QStringLiteral("off");
    if (m_layoutMode == QStringLiteral("off")) {
        nextMode = QStringLiteral("pip");
    } else if (m_layoutMode == QStringLiteral("pip")) {
        nextMode = gridLayoutModeForTileCount(configuredMaxTiles());
    }

    setLayoutMode(nextMode);
}

void MultiViewController::setLayoutMode(const QString &mode)
{
    if (catchupMultiviewBlocked(m_playerController) && normalizedLayoutModeValue(mode) != QStringLiteral("off")) {
        emit statusMessageRequested(QStringLiteral("Multiview is unavailable during catch-up playback."));
        return;
    }
    auto normalized = normalizedLayoutModeValue(mode);
    if (normalized != QStringLiteral("off") && !multiviewEnabled()) {
        emit statusMessageRequested(QStringLiteral("Multiview is disabled in Settings."));
        return;
    }

    if (normalized == QStringLiteral("grid")) {
        normalized = gridLayoutModeForTileCount(configuredMaxTiles());
    }

    if (normalized == QStringLiteral("off")) {
        exitMultiView();
        return;
    }

    setLayoutModeInternal(normalized);
}

bool MultiViewController::togglePictureInPicture(const int channelId)
{
    if (catchupMultiviewBlocked(m_playerController)) {
        emit statusMessageRequested(QStringLiteral("Multiview is unavailable during catch-up playback."));
        return false;
    }
    if (!multiviewEnabled()) {
        emit statusMessageRequested(QStringLiteral("Multiview is disabled in Settings."));
        return false;
    }

    if (m_layoutMode == QStringLiteral("pip")) {
        const auto selectedChannel = m_channelListModel->channelById(channelId);
        if (!selectedChannel.has_value()) {
            exitMultiView();
            return true;
        }
        if (slotHasDuplicateChannel(selectedChannel.value(), 1)) {
            emit statusMessageRequested(QStringLiteral("Channel already open in another tile"));
            return false;
        }

        setLayoutModeInternal(QStringLiteral("pip"));
        setFocusedTileIndexInternal(0, false);
        return assignChannelToSecondarySlot(1, selectedChannel.value());
    }

    const auto primaryChannel = m_playerController->currentChannelValue();
    if (!primaryChannel.has_value()) {
        emit statusMessageRequested(QStringLiteral("Start playback before opening PiP."));
        return false;
    }

    const auto selectedChannel = m_channelListModel->channelById(channelId);
    if (!selectedChannel.has_value()) {
        setLayoutModeInternal(QStringLiteral("pip"));
        setFocusedTileIndexInternal(1, false);
        emitTilesChanged();
        return true;
    }
    if (slotHasDuplicateChannel(selectedChannel.value(), 1)) {
        emit statusMessageRequested(QStringLiteral("Channel already open in another tile"));
        return false;
    }

    setLayoutModeInternal(QStringLiteral("pip"));
    setFocusedTileIndexInternal(0, false);
    const auto assigned = assignChannelToSecondarySlot(1, selectedChannel.value());
    const auto secondaryChannelOpen = !m_secondarySlots.empty() && m_secondarySlots.front().channel.has_value();
    if (!assigned && !secondaryChannelOpen) {
        exitMultiView();
    }
    return assigned;
}

bool MultiViewController::toggleGrid()
{
    if (catchupMultiviewBlocked(m_playerController)) {
        emit statusMessageRequested(QStringLiteral("Multiview is unavailable during catch-up playback."));
        return false;
    }
    if (!multiviewEnabled()) {
        emit statusMessageRequested(QStringLiteral("Multiview is disabled in Settings."));
        return false;
    }

    if (isGridLayout(m_layoutMode)) {
        return exitMultiViewWithIntent(
            shouldRetainSelectionOnGridPromotion()
                ? ExitIntent::SoftRetain
                : ExitIntent::ForcedOff);
    }

    if (!m_playerController->currentChannelValue().has_value()) {
        emit statusMessageRequested(QStringLiteral("Start playback before opening multiview."));
        return false;
    }

    setLayoutModeInternal(gridLayoutModeForTileCount(configuredMaxTiles()));
    return true;
}

bool MultiViewController::stopRetainedPromotedAndRestoreGrid()
{
    if (isGridLayout(m_layoutMode)) {
        return stopFocusedTileInActiveGrid();
    }

    if (m_layoutMode != QStringLiteral("off") || !m_retainedSelectionActive) {
        return false;
    }

    const auto promotedChannel = m_playerController->currentChannelValue();
    if (!promotedChannel.has_value()) {
        return false;
    }

    setLayoutModeInternal(gridLayoutModeForTileCount(configuredMaxTiles()));

    int promotedSlotIndex = -1;
    const auto primaryChannel = m_playerController->currentChannelValue();
    if (primaryChannel.has_value()
        && primaryChannel->profileId == promotedChannel->profileId
        && primaryChannel->id == promotedChannel->id) {
        promotedSlotIndex = 0;
    } else {
        for (int slotIndex = 1; slotIndex <= static_cast<int>(m_secondarySlots.size()); ++slotIndex) {
            const auto &slot = m_secondarySlots.at(static_cast<std::size_t>(slotIndex - 1));
            if (!slot.channel.has_value()) {
                continue;
            }
            if (slot.channel->profileId == promotedChannel->profileId
                && slot.channel->id == promotedChannel->id) {
                promotedSlotIndex = slotIndex;
                break;
            }
        }
    }

    if (promotedSlotIndex == 0) {
        int replacementSlotIndex = -1;
        for (int slotIndex = 1; slotIndex <= static_cast<int>(m_secondarySlots.size()); ++slotIndex) {
            const auto &slot = m_secondarySlots.at(static_cast<std::size_t>(slotIndex - 1));
            if (!slot.channel.has_value()) {
                continue;
            }
            replacementSlotIndex = slotIndex;
            break;
        }

        if (replacementSlotIndex > 0) {
            if (!swapPrimaryWithSecondarySlotNoReconnect(replacementSlotIndex)) {
                (void)promoteSecondarySlotToPrimary(replacementSlotIndex);
            }
            promotedSlotIndex = replacementSlotIndex;
        } else {
            m_playerController->stop();
            promotedSlotIndex = -1;
        }
    }

    if (promotedSlotIndex > 0) {
        clearSecondarySlot(promotedSlotIndex);
    }

    int focusIndex = 0;
    if (!m_playerController->currentChannelValue().has_value()) {
        for (int slotIndex = 1; slotIndex <= static_cast<int>(m_secondarySlots.size()); ++slotIndex) {
            const auto &slot = m_secondarySlots.at(static_cast<std::size_t>(slotIndex - 1));
            if (!slot.channel.has_value()) {
                continue;
            }
            focusIndex = slotIndex;
            break;
        }
    }
    setFocusedTileIndexInternal(focusIndex, false);
    emitTilesChanged();
    return true;
}

bool MultiViewController::stopFocusedTileInActiveGrid()
{
    if (!isGridLayout(m_layoutMode)) {
        return false;
    }

    const auto focusIndex = normalizedFocusedTileIndex();
    if (focusIndex <= 0) {
        if (m_playerController->currentChannelValue().has_value()) {
            m_skipPrimaryAutoPromotionOnce = true;
            m_playerController->stop();
        }
    } else {
        clearSecondarySlot(focusIndex);
    }

    clearDegradePrompt(true);
    applyFocusedAudioOwnership();
    emitTilesChanged();

    if (occupiedTileCount() == 0) {
        return exitMultiViewWithIntent(ExitIntent::ForcedOff);
    }
    return true;
}

bool MultiViewController::swapPrimaryWithPictureInPicture()
{
    if (m_layoutMode != QStringLiteral("pip")) {
        emit statusMessageRequested(QStringLiteral("PiP is not active."));
        return false;
    }

    const auto primaryChannel = m_playerController->currentChannelValue();
    if (!primaryChannel.has_value()) {
        return false;
    }
    if (m_secondarySlots.empty() || !m_secondarySlots.front().channel.has_value()) {
        emit statusMessageRequested(QStringLiteral("PiP has no secondary channel to swap."));
        return false;
    }

    if (!swapPrimaryWithSecondarySlotNoReconnect(1)) {
        return false;
    }
    setFocusedTileIndexInternal(0, false);
    applyFocusedAudioOwnership();
    scheduleFocusedAudioOwnershipRefresh();
    Core::DebugLogger::instance().log(
        QStringLiteral("multiview.tile.audio"),
        QStringLiteral("PiP swap applied warm-audio policy (secondary decode on, volume muted)."));
    emitTilesChanged();
    return true;
}

void MultiViewController::assignChannelToFocusedTile(const int channelId)
{
    const auto channel = m_channelListModel->channelById(channelId);
    if (!channel.has_value()) {
        return;
    }

    assignResolvedChannel(channel.value());
}

void MultiViewController::focusNextTile()
{
    if (!isActive()) {
        return;
    }

    const auto slotCount = visibleSlotCount();
    if (slotCount <= 1) {
        return;
    }

    setFocusedTileIndexInternal((normalizedFocusedTileIndex() + 1) % slotCount, true);
}

void MultiViewController::focusTile(const int tileIndex)
{
    if (!isActive()) {
        return;
    }

    if (tileIndex < 0 || tileIndex >= visibleSlotCount()) {
        return;
    }

    setFocusedTileIndexInternal(tileIndex, true);
}

void MultiViewController::closeFocusedTile()
{
    if (!isActive() || focusedTileIsPrimary()) {
        return;
    }

    clearSecondarySlot(normalizedFocusedTileIndex());
    setFocusedTileIndexInternal(0, false);
    clearDegradePrompt(true);
    applyFocusedAudioOwnership();
    emitTilesChanged();
}

void MultiViewController::exitMultiView()
{
    (void)exitMultiViewWithIntent(ExitIntent::ForcedOff);
}

bool MultiViewController::fullPromoteAndExit()
{
    return exitMultiViewWithIntent(ExitIntent::FullPromotion);
}

void MultiViewController::acceptPendingDegrade()
{
    if (!m_degradePromptVisible || m_pendingDegradeLayout.isEmpty()) {
        return;
    }

    const auto nextMode = m_pendingDegradeLayout;
    clearDegradePrompt(true);
    Core::DebugLogger::instance().log(
        QStringLiteral("multiview.degrade"),
        QStringLiteral("Accepted degrade to %1.").arg(nextMode));
    if (nextMode == QStringLiteral("off")) {
        (void)exitMultiViewWithIntent(ExitIntent::DegradeOff);
        return;
    }
    setLayoutMode(nextMode);
}

void MultiViewController::declinePendingDegrade()
{
    if (!m_degradePromptVisible) {
        return;
    }

    m_declinedDegradeSignature = degradeSignature();
    clearDegradePrompt(false);
    Core::DebugLogger::instance().log(
        QStringLiteral("multiview.degrade"),
        QStringLiteral("Declined degrade prompt for %1.").arg(m_layoutMode));
}

void MultiViewController::applySettings()
{
    if (!multiviewEnabled() && (isActive() || retainedSelectionActive())) {
        (void)exitMultiViewWithIntent(ExitIntent::ForcedOff);
        return;
    }

    if (isActive()) {
        if (isGridLayout(m_layoutMode)) {
            const auto desiredGridMode = gridLayoutModeForTileCount(configuredMaxTiles());
            if (m_layoutMode != desiredGridMode) {
                m_layoutMode = desiredGridMode;
                emit layoutModeChanged();
            }
        } else if (m_layoutMode != QStringLiteral("pip")) {
            m_layoutMode = QStringLiteral("off");
            emit layoutModeChanged();
        }
        ensureSecondarySlotsForCurrentLayout();
        trimSecondarySlotsForCurrentLayout();
    }

    for (auto &slot : m_secondarySlots) {
        if (!slot.player) {
            continue;
        }
        configureSecondaryPlayer(*slot.player);
    }

    clearDegradePrompt(true);
    applyFocusedAudioOwnership();
    emitTilesChanged();
}

QString MultiViewController::normalizedLayoutMode(const QString &mode)
{
    return normalizedLayoutModeValue(mode);
}

bool MultiViewController::isGridLayout(const QString &mode)
{
    return isGridLayoutValue(mode);
}

QString MultiViewController::layoutLabel(const QString &mode)
{
    return layoutLabelValue(mode);
}

QString MultiViewController::gridLayoutModeForTileCount(const int tileCount)
{
    return gridLayoutModeForTileCountValue(tileCount);
}

bool MultiViewController::exitMultiViewWithIntent(const ExitIntent intent)
{
    const auto hadActiveLayout = isActive();
    const auto hadRetainedSelection = retainedSelectionActive();
    if (!hadActiveLayout && !hadRetainedSelection) {
        return false;
    }

    const auto previousMode = m_layoutMode;
    int focusedPromotionSlot = -1;
    std::optional<Channel> focusedPromotionChannel;
    if (hadActiveLayout) {
        if (!focusedTileIsPrimary()) {
            const auto candidateSlot = normalizedFocusedTileIndex();
            if (candidateSlot > 0
                && candidateSlot <= static_cast<int>(m_secondarySlots.size())
                && m_secondarySlots.at(static_cast<std::size_t>(candidateSlot - 1)).channel.has_value()) {
                focusedPromotionSlot = candidateSlot;
                focusedPromotionChannel = m_secondarySlots.at(static_cast<std::size_t>(candidateSlot - 1)).channel;
            }
        }

        if (intent == ExitIntent::SoftRetain) {
            m_softRetainOriginSlotIndex.reset();
            m_softRetainPromotedChannel.reset();
            if (focusedPromotionSlot > 0) {
                if (swapPrimaryWithSecondarySlotNoReconnect(focusedPromotionSlot)) {
                    m_softRetainOriginSlotIndex = focusedPromotionSlot;
                    m_softRetainPromotedChannel = focusedPromotionChannel;
                } else {
                    (void)promoteSecondarySlotToPrimary(focusedPromotionSlot);
                }
            }
        } else if (intent == ExitIntent::FullPromotion) {
            if (focusedPromotionSlot > 0) {
                if (!swapPrimaryWithSecondarySlotNoReconnect(focusedPromotionSlot)) {
                    (void)promoteSecondarySlotToPrimary(focusedPromotionSlot);
                }
            }
        } else if (intent == ExitIntent::ForcedOff) {
            if (focusedPromotionSlot > 0) {
                (void)promoteSecondarySlotToPrimary(focusedPromotionSlot);
            }
        }
    }

    const auto primaryChannel = m_playerController->currentChannelValue();
    const auto restorePrimaryToBasePlayer = m_playerController->usingSharedPlayback();
    m_layoutMode = QStringLiteral("off");
    setFocusedTileIndexInternal(0, false);

    if (intent == ExitIntent::SoftRetain) {
        const auto hasRetained = hasRetainableSecondaryChannels();
        setRetainedSelectionActive(hasRetained);
        if (!hasRetained) {
            clearRetainedSelectionTracking();
        }
    } else {
        clearAllSecondarySlots();
        clearRetainedSelectionTracking();
    }

    if (restorePrimaryToBasePlayer && intent != ExitIntent::SoftRetain) {
        if (!primaryChannel.has_value()) {
            m_playerController->detachSharedPlayback();
        } else if (auto *basePlayer = m_playerController->primaryBasePlayer()) {
            basePlayer->setAudioEnabled(false);
            basePlayer->setVolume(0);
            basePlayer->stop();
        }
    }

    clearDegradePrompt(true);
    resetDegradeMonitor();
    applyFocusedAudioOwnership();
    scheduleFocusedAudioOwnershipRefresh();
    if (hadActiveLayout) {
        logLayoutChange(previousMode, m_layoutMode);
        emit layoutModeChanged();
    }
    emitTilesChanged();
    return true;
}

bool MultiViewController::hasRetainableSecondaryChannels() const
{
    for (const auto &slot : m_secondarySlots) {
        if (slot.channel.has_value()) {
            return true;
        }
    }
    return false;
}

bool MultiViewController::shouldRetainSelectionOnGridPromotion() const
{
    return m_settings->current().multiviewRetainSelectionOnPromotion;
}

void MultiViewController::setRetainedSelectionActive(const bool active)
{
    if (m_retainedSelectionActive == active) {
        return;
    }
    m_retainedSelectionActive = active;
    emit retainedSelectionActiveChanged();
}

void MultiViewController::clearRetainedSelectionTracking()
{
    m_softRetainOriginSlotIndex.reset();
    m_softRetainPromotedChannel.reset();
    setRetainedSelectionActive(false);
}

void MultiViewController::pruneRetainedSecondaryDuplicatesForPrimary()
{
    const auto primaryChannel = m_playerController->currentChannelValue();
    if (!primaryChannel.has_value()) {
        return;
    }

    for (int slotIndex = 1; slotIndex <= static_cast<int>(m_secondarySlots.size()); ++slotIndex) {
        const auto &slot = m_secondarySlots.at(static_cast<std::size_t>(slotIndex - 1));
        if (!slot.channel.has_value()) {
            continue;
        }
        if (slot.channel->profileId == primaryChannel->profileId
            && slot.channel->id == primaryChannel->id) {
            clearSecondarySlot(slotIndex);
        }
    }
}

int MultiViewController::configuredMaxTiles() const
{
    return normalizedMultiviewMaxTiles(m_settings->current());
}

int MultiViewController::layoutCapacity(const QString &mode) const
{
    if (mode == QStringLiteral("pip")) {
        return 2;
    }
    if (mode == QStringLiteral("grid2x1")) {
        return 2;
    }
    if (mode == QStringLiteral("grid2x2")) {
        return 4;
    }
    if (mode == QStringLiteral("grid3x2")) {
        return 6;
    }
    if (mode == QStringLiteral("grid3x3")) {
        return 9;
    }
    if (mode == QStringLiteral("grid4x3")) {
        return 12;
    }
    return 1;
}

int MultiViewController::layoutColumnsForMode(const QString &mode) const
{
    return layoutColumnsForModeValue(mode);
}

int MultiViewController::layoutRowsForMode(const QString &mode) const
{
    return layoutRowsForModeValue(mode);
}

int MultiViewController::visibleSlotCount() const
{
    if (isActive()) {
        return std::max(1, layoutCapacity(m_layoutMode));
    }

    return m_playerController->currentChannelValue().has_value() ? 1 : 0;
}

int MultiViewController::occupiedTileCount() const
{
    auto count = m_playerController->currentChannelValue().has_value() ? 1 : 0;
    for (const auto &slot : m_secondarySlots) {
        if (slot.channel.has_value()) {
            ++count;
        }
    }
    return count;
}

int MultiViewController::normalizedFocusedTileIndex() const
{
    const auto slotCount = visibleSlotCount();
    if (slotCount <= 0) {
        return 0;
    }
    return std::clamp(m_focusedTileIndex, 0, slotCount - 1);
}

int MultiViewController::slotIndexForPromotion() const
{
    if (!isActive()) {
        return -1;
    }

    if (!focusedTileIsPrimary() && normalizedFocusedTileIndex() < visibleSlotCount()) {
        const auto focusedSlot = normalizedFocusedTileIndex();
        if (focusedSlot > 0
            && focusedSlot - 1 < static_cast<int>(m_secondarySlots.size())
            && m_secondarySlots.at(static_cast<std::size_t>(focusedSlot - 1)).channel.has_value()) {
            return focusedSlot;
        }
    }

    if (m_lastFocusedSecondarySlotIndex.has_value()) {
        const auto slotIndex = m_lastFocusedSecondarySlotIndex.value();
        if (slotIndex > 0
            && slotIndex - 1 < static_cast<int>(m_secondarySlots.size())
            && m_secondarySlots.at(static_cast<std::size_t>(slotIndex - 1)).channel.has_value()) {
            return slotIndex;
        }
    }

    for (int slotIndex = 1; slotIndex <= static_cast<int>(m_secondarySlots.size()); ++slotIndex) {
        if (m_secondarySlots.at(static_cast<std::size_t>(slotIndex - 1)).channel.has_value()) {
            return slotIndex;
        }
    }

    return -1;
}

int MultiViewController::desiredFocusedTileVolume() const
{
    return (m_playerController->muted() || m_playerController->volume() <= 0.0)
        ? 0
        : static_cast<int>(std::round(std::clamp(m_playerController->volume(), 0.0, 100.0)));
}

QString MultiViewController::primaryTileState() const
{
    const auto hasPrimaryChannel = m_playerController->currentChannelValue().has_value();
    if (!hasPrimaryChannel) {
        return QStringLiteral("empty");
    }
    if (m_playerController->channelLoadFailed()) {
        return QStringLiteral("error");
    }
    if (m_playerController->isLoading()) {
        return QStringLiteral("loading");
    }
    if (m_playerController->isBuffering()) {
        return QStringLiteral("buffering");
    }
    if (m_playerController->isPlaying()) {
        return QStringLiteral("playing");
    }
    return QStringLiteral("idle");
}

QString MultiViewController::degradeSignature() const
{
    QStringList parts;
    parts << m_layoutMode << QString::number(normalizedFocusedTileIndex());
    const auto primaryChannel = m_playerController->currentChannelValue();
    parts << (primaryChannel.has_value() ? QString::number(primaryChannel->id) : QStringLiteral("-1"));
    for (const auto &slot : m_secondarySlots) {
        parts << (slot.channel.has_value() ? QString::number(slot.channel->id) : QStringLiteral("-1"));
    }
    return parts.join(u'|');
}

bool MultiViewController::multiviewEnabled() const
{
    return m_settings->current().multiviewEnabled;
}

bool MultiViewController::slotHasDuplicateChannel(const Channel &channel, const int targetSlotIndex) const
{
    const auto primaryChannel = m_playerController->currentChannelValue();
    if (targetSlotIndex != 0
        && primaryChannel.has_value()
        && primaryChannel->profileId == channel.profileId
        && primaryChannel->id == channel.id) {
        return true;
    }

    for (int index = 0; index < static_cast<int>(m_secondarySlots.size()); ++index) {
        if (index + 1 == targetSlotIndex) {
            continue;
        }
        const auto &slot = m_secondarySlots.at(static_cast<std::size_t>(index));
        if (slot.channel.has_value()
            && slot.channel->profileId == channel.profileId
            && slot.channel->id == channel.id) {
            return true;
        }
    }

    return false;
}

bool MultiViewController::assignChannelToSecondarySlot(const int slotIndex, const Channel &channel)
{
    if (slotIndex <= 0 || slotIndex > static_cast<int>(m_secondarySlots.size())) {
        return false;
    }

    auto &slot = m_secondarySlots[static_cast<std::size_t>(slotIndex - 1)];
    if (slot.channel.has_value()
        && slot.channel->profileId == channel.profileId
        && slot.channel->id == channel.id) {
        return true;
    }

    if (slotHasDuplicateChannel(channel, slotIndex)) {
        emit statusMessageRequested(QStringLiteral("Channel already open in another tile"));
        return false;
    }

    if (!slot.player) {
        slot.player = std::make_unique<MpvPlayer>();
        connectSecondaryPlayerSignals(slot.player.get(), slotIndex);
    }
    if (slot.borrowedPlayer) {
        if (slot.borrowedPlayer.data() != m_playerController->primaryBasePlayer()) {
            slot.borrowedPlayer->stop();
        }
    }
    slot.borrowedPlayer.clear();
    configureSecondaryPlayer(*slot.player);

    slot.channel = channel;
    slot.playerState = QStringLiteral("loading");
    slot.hasError = false;
    slot.errorText.clear();
    if (auto *slotPlayer = slot.playbackPlayer()) {
        slotPlayer->setAudioEnabled(true);
        slotPlayer->setVolume(0);
        slotPlayer->play(channel.streamUrl);
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("multiview.tile.add"),
        QStringLiteral("Assigned channel %1 to tile %2.").arg(channel.name).arg(slotIndex));

    clearDegradePrompt(true);
    applyFocusedAudioOwnership();
    emitTilesChanged();
    return true;
}

bool MultiViewController::promoteSecondarySlotToPrimary(const int slotIndex)
{
    if (slotIndex <= 0 || slotIndex > static_cast<int>(m_secondarySlots.size())) {
        return false;
    }

    auto &slot = m_secondarySlots[static_cast<std::size_t>(slotIndex - 1)];
    if (!slot.channel.has_value() || !slot.playbackPlayer()) {
        return false;
    }

    m_promotingSecondaryToPrimary = true;
    const auto promotedChannel = slot.channel.value();
    auto *primaryBasePlayer = m_playerController->primaryBasePlayer();
    if (slot.player) {
        m_adoptedPrimaryPlayer = std::move(slot.player);
        m_playerController->attachSharedPlayback(
            m_adoptedPrimaryPlayer.get(),
            promotedChannel,
            true);
    } else if (slot.borrowedPlayer && slot.borrowedPlayer.data() == primaryBasePlayer) {
        m_playerController->detachSharedPlayback(false);
        m_playerController->adoptExistingPlaybackChannel(promotedChannel);
    } else {
        m_promotingSecondaryToPrimary = false;
        return false;
    }
    slot.borrowedPlayer.clear();
    slot.channel.reset();
    slot.playerState = QStringLiteral("empty");
    slot.hasError = false;
    slot.errorText.clear();
    m_promotingSecondaryToPrimary = false;
    setFocusedTileIndexInternal(0, false);
    emitTilesChanged();
    return true;
}

bool MultiViewController::swapPrimaryWithSecondarySlotNoReconnect(const int slotIndex)
{
    if (slotIndex <= 0 || slotIndex > static_cast<int>(m_secondarySlots.size())) {
        return false;
    }

    auto &slot = m_secondarySlots[static_cast<std::size_t>(slotIndex - 1)];
    if (!slot.channel.has_value() || !slot.playbackPlayer()) {
        return false;
    }

    const auto primaryChannel = m_playerController->currentChannelValue();
    if (!primaryChannel.has_value()) {
        return false;
    }

    const auto &previousPrimaryChannel = *primaryChannel;
    const auto promotedChannel = slot.channel.value();
    auto *promotedPlayer = slot.playbackPlayer();
    auto *demotedPrimaryPlayer = m_playerController->player();
    if (promotedPlayer == nullptr || demotedPrimaryPlayer == nullptr || promotedPlayer == demotedPrimaryPlayer) {
        return false;
    }

    auto *primaryBasePlayer = m_playerController->primaryBasePlayer();

    std::unique_ptr<MpvPlayer> demotedOwnedPrimaryPlayer;
    const auto demotedWasBasePlayer = demotedPrimaryPlayer == primaryBasePlayer;
    if (!demotedWasBasePlayer) {
        if (m_adoptedPrimaryPlayer && demotedPrimaryPlayer == m_adoptedPrimaryPlayer.get()) {
            demotedOwnedPrimaryPlayer = std::move(m_adoptedPrimaryPlayer);
        } else {
            return false;
        }
    }

    const auto promotedIsBasePlayer = promotedPlayer == primaryBasePlayer;
    const auto promotedIsOwnedSlotPlayer = slot.player && promotedPlayer == slot.player.get();
    if (!promotedIsBasePlayer && !promotedIsOwnedSlotPlayer) {
        return false;
    }

    m_swappingPrimaryAndSecondary = true;
    if (promotedIsBasePlayer) {
        m_playerController->detachSharedPlayback(false);
        m_playerController->adoptExistingPlaybackChannel(promotedChannel);
    } else {
        m_adoptedPrimaryPlayer = std::move(slot.player);
        slot.borrowedPlayer.clear();
        m_playerController->attachSharedPlayback(
            m_adoptedPrimaryPlayer.get(),
            promotedChannel,
            true,
            false);
    }

    if (demotedWasBasePlayer) {
        if (slot.player) {
            slot.player->stop();
            slot.player.reset();
        }
        slot.borrowedPlayer = demotedPrimaryPlayer;
    } else {
        slot.player = std::move(demotedOwnedPrimaryPlayer);
        slot.borrowedPlayer.clear();
    }
    m_swappingPrimaryAndSecondary = false;

    slot.channel = previousPrimaryChannel;
    slot.playerState = QStringLiteral("ready");
    slot.hasError = false;
    slot.errorText.clear();
    return true;
}

void MultiViewController::setFocusedTileIndexInternal(const int index, const bool logChange)
{
    const auto slotCount = visibleSlotCount();
    const auto normalized = slotCount <= 0 ? 0 : std::clamp(index, 0, slotCount - 1);
    if (m_focusedTileIndex == normalized) {
        applyFocusedAudioOwnership();
        return;
    }

    m_focusedTileIndex = normalized;
    if (m_focusedTileIndex > 0) {
        m_lastFocusedSecondarySlotIndex = m_focusedTileIndex;
    }
    if (logChange) {
        Core::DebugLogger::instance().log(
            QStringLiteral("multiview.tile.focus"),
            QStringLiteral("Focused tile changed to %1.").arg(m_focusedTileIndex));
    }
    clearDegradePrompt(true);
    resetDegradeMonitor();
    applyFocusedAudioOwnership();
    emit focusedTileIndexChanged();
    emitTilesChanged();
}

void MultiViewController::setLayoutModeInternal(const QString &mode)
{
    auto normalized = normalizedLayoutModeValue(mode);
    if (normalized == QStringLiteral("grid")) {
        normalized = gridLayoutModeForTileCount(configuredMaxTiles());
    }

    const auto openingRetainedGrid = m_retainedSelectionActive
        && normalized != QStringLiteral("off")
        && isGridLayout(normalized);
    if (m_retainedSelectionActive && !openingRetainedGrid) {
        (void)exitMultiViewWithIntent(ExitIntent::FullPromotion);
    }

    if (normalized == m_layoutMode) {
        ensureSecondarySlotsForCurrentLayout();
        trimSecondarySlotsForCurrentLayout();
        applyFocusedAudioOwnership();
        emitTilesChanged();
        return;
    }

    const auto previousMode = m_layoutMode;
    m_layoutMode = normalized;
    std::optional<Channel> reopenViewedChannel;
    int reopenFocusIndex = -1;
    if (openingRetainedGrid) {
        reopenViewedChannel = m_playerController->currentChannelValue();
        if (m_softRetainOriginSlotIndex.has_value() && m_softRetainPromotedChannel.has_value()) {
            const auto primaryChannel = m_playerController->currentChannelValue();
            const auto &retainedPromoted = m_softRetainPromotedChannel.value();
            const auto originSlotIndex = m_softRetainOriginSlotIndex.value();
            if (primaryChannel.has_value()
                && primaryChannel->profileId == retainedPromoted.profileId
                && primaryChannel->id == retainedPromoted.id
                && originSlotIndex > 0
                && originSlotIndex <= static_cast<int>(m_secondarySlots.size())
                && m_secondarySlots.at(static_cast<std::size_t>(originSlotIndex - 1)).channel.has_value()) {
                (void)swapPrimaryWithSecondarySlotNoReconnect(originSlotIndex);
            }
        }
        pruneRetainedSecondaryDuplicatesForPrimary();
        if (reopenViewedChannel.has_value()) {
            const auto primaryChannel = m_playerController->currentChannelValue();
            if (primaryChannel.has_value()
                && primaryChannel->profileId == reopenViewedChannel->profileId
                && primaryChannel->id == reopenViewedChannel->id) {
                reopenFocusIndex = 0;
            } else {
                for (int slotIndex = 1; slotIndex <= static_cast<int>(m_secondarySlots.size()); ++slotIndex) {
                    const auto &slot = m_secondarySlots.at(static_cast<std::size_t>(slotIndex - 1));
                    if (!slot.channel.has_value()) {
                        continue;
                    }
                    if (slot.channel->profileId == reopenViewedChannel->profileId
                        && slot.channel->id == reopenViewedChannel->id) {
                        reopenFocusIndex = slotIndex;
                        break;
                    }
                }
            }
        }
        clearRetainedSelectionTracking();
    }
    ensureSecondarySlotsForCurrentLayout();
    trimSecondarySlotsForCurrentLayout();
    const auto desiredFocusIndex = openingRetainedGrid && reopenFocusIndex >= 0
        ? reopenFocusIndex
        : std::min(normalizedFocusedTileIndex(), visibleSlotCount() - 1);
    setFocusedTileIndexInternal(desiredFocusIndex, false);
    clearDegradePrompt(true);
    resetDegradeMonitor();
    applyFocusedAudioOwnership();
    if (openingRetainedGrid) {
        scheduleFocusedAudioOwnershipRefresh();
    }
    logLayoutChange(previousMode, m_layoutMode);
    emit layoutModeChanged();
    emitTilesChanged();
}

void MultiViewController::ensureSecondarySlotsForCurrentLayout()
{
    const auto desiredSecondarySlots = std::max(0, layoutCapacity(m_layoutMode) - 1);
    while (static_cast<int>(m_secondarySlots.size()) < desiredSecondarySlots) {
        SecondarySlot slot;
        slot.slotIndex = static_cast<int>(m_secondarySlots.size()) + 1;
        m_secondarySlots.push_back(std::move(slot));
    }
    for (int index = 0; index < static_cast<int>(m_secondarySlots.size()); ++index) {
        m_secondarySlots[static_cast<std::size_t>(index)].slotIndex = index + 1;
    }
}

void MultiViewController::trimSecondarySlotsForCurrentLayout()
{
    const auto desiredSecondarySlots = std::max(0, layoutCapacity(m_layoutMode) - 1);
    while (static_cast<int>(m_secondarySlots.size()) > desiredSecondarySlots) {
        clearSecondarySlot(static_cast<int>(m_secondarySlots.size()));
        m_secondarySlots.pop_back();
    }
    if (m_lastFocusedSecondarySlotIndex.has_value()
        && m_lastFocusedSecondarySlotIndex.value() > static_cast<int>(m_secondarySlots.size())) {
        m_lastFocusedSecondarySlotIndex.reset();
    }
}

void MultiViewController::clearSecondarySlot(const int slotIndex)
{
    if (slotIndex <= 0 || slotIndex > static_cast<int>(m_secondarySlots.size())) {
        return;
    }

    auto &slot = m_secondarySlots[static_cast<std::size_t>(slotIndex - 1)];
    if (slot.player) {
        disconnect(slot.player.get(), nullptr, this, nullptr);
        retireDetachedPlayer(std::move(slot.player));
    }
    if (slot.borrowedPlayer) {
        // After a no-reconnect PiP swap, a secondary slot can temporarily reference the
        // primary base player. Clearing the slot must not stop that shared base player.
        if (slot.borrowedPlayer.data() != m_playerController->primaryBasePlayer()) {
            slot.borrowedPlayer->stop();
        }
        slot.borrowedPlayer.clear();
    }
    if (slot.channel.has_value()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("multiview.tile.remove"),
            QStringLiteral("Closed tile %1 (%2).").arg(slotIndex).arg(slot.channel->name));
    }
    slot.channel.reset();
    slot.playerState = QStringLiteral("empty");
    slot.hasError = false;
    slot.errorText.clear();
    if (m_lastFocusedSecondarySlotIndex == slotIndex) {
        m_lastFocusedSecondarySlotIndex.reset();
    }
}

void MultiViewController::clearAllSecondarySlots()
{
    for (int slotIndex = 1; slotIndex <= static_cast<int>(m_secondarySlots.size()); ++slotIndex) {
        clearSecondarySlot(slotIndex);
    }
    m_secondarySlots.clear();
    m_lastFocusedSecondarySlotIndex.reset();
}

void MultiViewController::configureSecondaryPlayer(MpvPlayer &player) const
{
    auto options = m_settings->current().mpvOptions;
    if (!options.contains(QStringLiteral("hwdec"))) {
        options.insert(
            QStringLiteral("hwdec"),
            m_settings->current().multiviewPreferHwdec ? QStringLiteral("auto-safe") : QStringLiteral("no"));
    }

    player.configureLibraryPath(m_settings->current().mpvDllPath);
    player.configureOptions(options);
    player.configurePlaybackTuning(
        m_settings->current().playerWaitForStreamSeconds,
        m_settings->current().playerDeinterlaceEnabled,
        m_settings->current().playerBufferSeconds);
    player.configureUserAgent(m_settings->current().playerUserAgent);
}

void MultiViewController::connectSecondaryPlayerSignals(MpvPlayer *player, const int slotIndex)
{
    connect(player, &MpvPlayer::fileLoaded, this, [this, player, slotIndex]() {
        updateSecondarySlotState(slotIndex, player, QStringLiteral("ready"));
    });
    connect(player, &MpvPlayer::pauseStateChanged, this, [this, player, slotIndex](const bool paused) {
        updateSecondarySlotState(slotIndex, player, paused ? QStringLiteral("paused") : QStringLiteral("playing"));
    });
    connect(player, &MpvPlayer::bufferingStateChanged, this, [this, player, slotIndex](const bool buffering) {
        updateSecondarySlotState(slotIndex, player, buffering ? QStringLiteral("buffering") : QStringLiteral("ready"));
    });
    connect(player, &MpvPlayer::playbackEnded, this, [this, player, slotIndex]() {
        updateSecondarySlotState(slotIndex, player, QStringLiteral("ended"));
    });
    connect(player, &MpvPlayer::errorOccurred, this, [this, player, slotIndex](const QString &message) {
        updateSecondarySlotError(slotIndex, player, message);
    });
}

bool MultiViewController::secondarySlotOwnsPlaybackPlayer(const int slotIndex, const MpvPlayer *player) const
{
    if (slotIndex <= 0 || slotIndex > static_cast<int>(m_secondarySlots.size())) {
        return false;
    }
    if (player == nullptr) {
        return false;
    }
    const auto &slot = m_secondarySlots.at(static_cast<std::size_t>(slotIndex - 1));
    return slot.playbackPlayer() == player;
}

void MultiViewController::updateSecondarySlotState(
    const int slotIndex,
    const MpvPlayer *player,
    const QString &state)
{
    if (!secondarySlotOwnsPlaybackPlayer(slotIndex, player)) {
        return;
    }

    auto &slot = m_secondarySlots[static_cast<std::size_t>(slotIndex - 1)];
    slot.playerState = state;
    if (state != QStringLiteral("error")) {
        slot.hasError = false;
        slot.errorText.clear();
    }
    emitTilesChanged();
}

void MultiViewController::updateSecondarySlotError(
    const int slotIndex,
    const MpvPlayer *player,
    const QString &message)
{
    if (!secondarySlotOwnsPlaybackPlayer(slotIndex, player)) {
        return;
    }

    auto &slot = m_secondarySlots[static_cast<std::size_t>(slotIndex - 1)];
    slot.playerState = QStringLiteral("error");
    slot.hasError = true;
    slot.errorText = message.trimmed().isEmpty()
        ? QStringLiteral("Channel couldn't be loaded")
        : message.trimmed();
    Core::DebugLogger::instance().log(
        QStringLiteral("multiview.tile.error"),
        QStringLiteral("Tile %1 failed: %2").arg(slotIndex).arg(slot.errorText));
    emitTilesChanged();
}

void MultiViewController::clearDegradePrompt(const bool clearSuppression)
{
    const auto visibilityChanged = m_degradePromptVisible || !m_pendingDegradeLayout.isEmpty();
    m_degradePromptVisible = false;
    m_pendingDegradeLayout.clear();
    if (clearSuppression) {
        m_declinedDegradeSignature.clear();
    }
    if (visibilityChanged) {
        emit degradePromptVisibleChanged();
    }
}

void MultiViewController::resetDegradeMonitor()
{
    m_lastFocusedDroppedFrames.reset();
    m_decodePressureConsecutiveTicks = 0;
    if (isActive()) {
        m_decodePressureTimer.start();
    } else {
        m_decodePressureTimer.stop();
    }
}

void MultiViewController::applyFocusedAudioOwnership()
{
    const auto focusedVolume = desiredFocusedTileVolume();
    auto *primaryPlayer = m_playerController->player();
    if (!isActive()) {
        if (primaryPlayer != nullptr) {
            primaryPlayer->setAudioEnabled(true);
            primaryPlayer->setVolume(focusedVolume);
        }
        for (auto &slot : m_secondarySlots) {
            if (auto *slotPlayer = slot.playbackPlayer()) {
                slotPlayer->setAudioEnabled(m_retainedSelectionActive);
                slotPlayer->setVolume(0);
            }
        }
        return;
    }

    const auto focusIndex = normalizedFocusedTileIndex();
    if (primaryPlayer != nullptr) {
        primaryPlayer->setAudioEnabled(true);
        primaryPlayer->setVolume(m_layoutMode == QStringLiteral("pip")
            ? focusedVolume
            : (focusIndex == 0 ? focusedVolume : 0));
    }

    for (int index = 0; index < static_cast<int>(m_secondarySlots.size()); ++index) {
        auto &slot = m_secondarySlots[static_cast<std::size_t>(index)];
        auto *slotPlayer = slot.playbackPlayer();
        if (slotPlayer == nullptr) {
            continue;
        }
        if (m_layoutMode == QStringLiteral("pip")) {
            // Keep PiP audio decoding warm for instant audio when this tile is promoted to main.
            slotPlayer->setAudioEnabled(true);
            slotPlayer->setVolume(0);
            continue;
        }
        slotPlayer->setAudioEnabled(true);
        slotPlayer->setVolume((index + 1) == focusIndex ? focusedVolume : 0);
    }
}

void MultiViewController::handlePrimaryChannelChanged()
{
    if (m_swappingPrimaryAndSecondary) {
        emitTilesChanged();
        return;
    }

    if (!m_playerController->usingSharedPlayback() && m_adoptedPrimaryPlayer) {
        retireDetachedPlayer(std::move(m_adoptedPrimaryPlayer));
    }

    if (m_promotingSecondaryToPrimary) {
        emitTilesChanged();
        return;
    }

    if (isActive() && !m_playerController->currentChannelValue().has_value()) {
        if (m_skipPrimaryAutoPromotionOnce) {
            m_skipPrimaryAutoPromotionOnce = false;
        } else {
            const auto promotionSlotIndex = slotIndexForPromotion();
            if (promotionSlotIndex > 0) {
                promoteSecondarySlotToPrimary(promotionSlotIndex);
            }
        }
    } else {
        m_skipPrimaryAutoPromotionOnce = false;
    }

    if (m_retainedSelectionActive) {
        const auto currentChannel = m_playerController->currentChannelValue();
        if (currentChannel.has_value()) {
            for (const auto &slot : m_secondarySlots) {
                if (!slot.channel.has_value()) {
                    continue;
                }
                if (slot.channel->profileId != currentChannel->profileId) {
                    (void)exitMultiViewWithIntent(ExitIntent::FullPromotion);
                    return;
                }
            }
        }
    }

    clearDegradePrompt(true);
    resetDegradeMonitor();
    applyFocusedAudioOwnership();
    emitTilesChanged();
}

void MultiViewController::retireDetachedPlayer(std::unique_ptr<MpvPlayer> player)
{
    if (!player) {
        return;
    }

    player->setRenderUpdateTarget(nullptr);
    player->setAudioEnabled(false);
    player->setVolume(0);
    player->stop();
    m_retiredPlayers.push_back(std::move(player));
    m_retiredPlayerCleanupTimer.start(250);
}

void MultiViewController::flushRetiredPlayers()
{
    m_retiredPlayers.clear();
}

void MultiViewController::scheduleFocusedAudioOwnershipRefresh()
{
    m_audioOwnershipRefreshTimer.start();
}

void MultiViewController::handleDecodePressureTick()
{
    if (!isActive() || m_degradePromptVisible || occupiedTileCount() <= 1) {
        return;
    }

    if (m_declinedDegradeSignature == degradeSignature()) {
        return;
    }

    MpvPlayer *focusedPlayer = nullptr;
    const auto focusIndex = normalizedFocusedTileIndex();
    if (focusIndex == 0) {
        focusedPlayer = m_playerController->player();
    } else if (focusIndex - 1 < static_cast<int>(m_secondarySlots.size())) {
        focusedPlayer = m_secondarySlots[static_cast<std::size_t>(focusIndex - 1)].playbackPlayer();
    }

    if (focusedPlayer == nullptr) {
        resetDegradeMonitor();
        return;
    }

    const auto droppedFrames = focusedPlayer->droppedFrameCount();
    if (!droppedFrames.has_value()) {
        resetDegradeMonitor();
        return;
    }

    if (m_lastFocusedDroppedFrames.has_value()) {
        const auto delta = droppedFrames.value() - m_lastFocusedDroppedFrames.value();
        if (delta >= kDecodePressureDeltaThreshold) {
            ++m_decodePressureConsecutiveTicks;
        } else {
            m_decodePressureConsecutiveTicks = 0;
        }
    }
    m_lastFocusedDroppedFrames = droppedFrames;

    if (m_decodePressureConsecutiveTicks < kDecodePressureConsecutiveTickThreshold) {
        return;
    }

    m_pendingDegradeLayout = isGridLayout(m_layoutMode)
        ? QStringLiteral("pip")
        : QStringLiteral("off");
    m_degradePromptVisible = true;
    m_decodePressureConsecutiveTicks = 0;
    Core::DebugLogger::instance().log(
        QStringLiteral("multiview.degrade"),
        QStringLiteral("Decode pressure detected on focused tile; proposing degrade to %1.")
            .arg(m_pendingDegradeLayout));
    emit degradePromptVisibleChanged();
}

void MultiViewController::emitTilesChanged()
{
    emit tilesChanged();
}

void MultiViewController::logLayoutChange(const QString &previousMode, const QString &nextMode) const
{
    Core::DebugLogger::instance().log(
        QStringLiteral("multiview.layout.change"),
        QStringLiteral("Layout changed from %1 to %2.")
            .arg(layoutLabelValue(previousMode), layoutLabelValue(nextMode)));
}

} // namespace OKILTV::App
