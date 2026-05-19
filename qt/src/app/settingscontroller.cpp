#include "settingscontroller.h"

#include "multiviewcontroller.h"
#include "playercontroller.h"
#include "profilesmodel.h"
#include "../core/models.h"
#include "../core/processutils.h"

#include <algorithm>
#include <cmath>

#include <QFileInfo>

namespace OKILTV::App {

namespace {

bool differs(const double left, const double right)
{
    return std::abs(left - right) > 0.0001;
}

QString mpvPathValidationStatusFor(const QString &normalizedPath)
{
    if (normalizedPath.trimmed().isEmpty()) {
        return QStringLiteral("Bundled mpv will be used.");
    }
    if (QFileInfo::exists(normalizedPath)) {
        return QStringLiteral("Override file exists. Runtime loading is deferred until playback.");
    }
    return QStringLiteral("Configured override is missing (%1). Bundled fallback will be used.").arg(normalizedPath);
}

bool ffmpegToolsAvailableNow()
{
    return Core::ffmpegToolsAvailable();
}

} // namespace

SettingsController::SettingsController(
    Core::SettingsManager *settings,
    PlayerController *playerController,
    MultiViewController *multiViewController,
    ProfilesModel *profilesModel,
    QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_playerController(playerController)
    , m_multiViewController(multiViewController)
    , m_profilesModel(profilesModel)
{
    reload();
}

QString SettingsController::theme() const
{
    return m_theme;
}

void SettingsController::setTheme(const QString &value)
{
    const auto wasDirty = dirty();
    if (m_theme == value) {
        return;
    }
    m_theme = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::showOnTopModeIndicator() const
{
    return m_showOnTopModeIndicator;
}

void SettingsController::setShowOnTopModeIndicator(const bool value)
{
    const auto wasDirty = dirty();
    if (m_showOnTopModeIndicator == value) {
        return;
    }
    m_showOnTopModeIndicator = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::preventDisplaySleep() const
{
    return m_preventDisplaySleep;
}

void SettingsController::setPreventDisplaySleep(const bool value)
{
    const auto wasDirty = dirty();
    if (m_preventDisplaySleep == value) {
        return;
    }
    m_preventDisplaySleep = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::guidePreviewEnabled() const
{
    return m_guidePreviewEnabled;
}

void SettingsController::setGuidePreviewEnabled(const bool value)
{
    const auto wasDirty = dirty();
    if (m_guidePreviewEnabled == value) {
        return;
    }
    m_guidePreviewEnabled = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::overlayAutoHide() const
{
    return m_overlayAutoHide;
}

void SettingsController::setOverlayAutoHide(const bool value)
{
    const auto wasDirty = dirty();
    if (m_overlayAutoHide == value) {
        return;
    }
    m_overlayAutoHide = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

int SettingsController::overlayAutoHideSeconds() const
{
    return m_overlayAutoHideSeconds;
}

void SettingsController::setOverlayAutoHideSeconds(const int value)
{
    const auto wasDirty = dirty();
    if (m_overlayAutoHideSeconds == value) {
        return;
    }
    m_overlayAutoHideSeconds = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

int SettingsController::refreshIntervalMinutes() const
{
    return m_refreshIntervalMinutes;
}

void SettingsController::setRefreshIntervalMinutes(const int value)
{
    const auto wasDirty = dirty();
    if (m_refreshIntervalMinutes == value) {
        return;
    }
    m_refreshIntervalMinutes = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::autoRefreshEpg() const
{
    return m_autoRefreshEpg;
}

void SettingsController::setAutoRefreshEpg(const bool value)
{
    const auto wasDirty = dirty();
    if (m_autoRefreshEpg == value) {
        return;
    }
    m_autoRefreshEpg = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

int SettingsController::guidePastHours() const
{
    return m_guidePastHours;
}

void SettingsController::setGuidePastHours(const int value)
{
    const auto wasDirty = dirty();
    const auto normalized = Core::normalizeGuideHours(value);
    if (m_guidePastHours == normalized) {
        return;
    }
    m_guidePastHours = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

int SettingsController::epgLookAheadHours() const
{
    return m_epgLookAheadHours;
}

void SettingsController::setEpgLookAheadHours(const int value)
{
    const auto wasDirty = dirty();
    const auto normalized = Core::normalizeGuideHours(value);
    if (m_epgLookAheadHours == normalized) {
        return;
    }
    m_epgLookAheadHours = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

double SettingsController::waitForDataStreamSeconds() const
{
    return m_waitForDataStreamSeconds;
}

void SettingsController::setWaitForDataStreamSeconds(const double value)
{
    const auto wasDirty = dirty();
    if (!differs(m_waitForDataStreamSeconds, value)) {
        return;
    }
    m_waitForDataStreamSeconds = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::deinterlaceEnabled() const
{
    return m_deinterlaceEnabled;
}

void SettingsController::setDeinterlaceEnabled(const bool value)
{
    const auto wasDirty = dirty();
    if (m_deinterlaceEnabled == value) {
        return;
    }
    m_deinterlaceEnabled = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

double SettingsController::bufferSizeSeconds() const
{
    return m_bufferSizeSeconds;
}

void SettingsController::setBufferSizeSeconds(const double value)
{
    const auto wasDirty = dirty();
    if (!differs(m_bufferSizeSeconds, value)) {
        return;
    }
    m_bufferSizeSeconds = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

QString SettingsController::playerUserAgent() const
{
    return m_playerUserAgent;
}

void SettingsController::setPlayerUserAgent(const QString &value)
{
    const auto wasDirty = dirty();
    const auto normalized = value.trimmed();
    if (m_playerUserAgent == normalized) {
        return;
    }
    m_playerUserAgent = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::timeshiftEnabled() const
{
    return ffmpegToolsAvailable() && m_timeshiftEnabled;
}

void SettingsController::setTimeshiftEnabled(const bool value)
{
    const auto wasDirty = dirty();
    const auto normalized = ffmpegToolsAvailable() && value;
    if (m_timeshiftEnabled == normalized) {
        return;
    }
    m_timeshiftEnabled = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

int SettingsController::timeshiftWindowMinutes() const
{
    return m_timeshiftWindowMinutes;
}

void SettingsController::setTimeshiftWindowMinutes(const int value)
{
    const auto wasDirty = dirty();
    const auto normalized = Core::normalizeTimeshiftWindowMinutes(value);
    if (m_timeshiftWindowMinutes == normalized) {
        return;
    }
    m_timeshiftWindowMinutes = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

int SettingsController::timeshiftSegmentSeconds() const
{
    return m_timeshiftSegmentSeconds;
}

void SettingsController::setTimeshiftSegmentSeconds(const int value)
{
    const auto wasDirty = dirty();
    const auto normalized = Core::normalizeTimeshiftSegmentSeconds(value);
    if (m_timeshiftSegmentSeconds == normalized) {
        return;
    }
    m_timeshiftSegmentSeconds = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

QString SettingsController::timeshiftStorageDirectory() const
{
    return m_timeshiftStorageDirectory;
}

void SettingsController::setTimeshiftStorageDirectory(const QString &value)
{
    const auto wasDirty = dirty();
    const auto normalized = normalizeOverridePath(value);
    if (m_timeshiftStorageDirectory == normalized) {
        return;
    }
    m_timeshiftStorageDirectory = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

int SettingsController::timeshiftMaxDiskGb() const
{
    return m_timeshiftMaxDiskGb;
}

void SettingsController::setTimeshiftMaxDiskGb(const int value)
{
    const auto wasDirty = dirty();
    const auto normalized = Core::normalizeTimeshiftMaxDiskGb(value);
    if (m_timeshiftMaxDiskGb == normalized) {
        return;
    }
    m_timeshiftMaxDiskGb = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

QString SettingsController::mpvDllPath() const
{
    return m_mpvDllPath;
}

void SettingsController::setMpvDllPath(const QString &value)
{
    const auto wasDirty = dirty();
    const auto normalized = normalizeOverridePath(value);
    if (m_mpvDllPath == normalized) {
        return;
    }
    m_mpvDllPath = normalized;
    validateMpvPath();
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

QString SettingsController::mpvPathValidationStatus() const
{
    return m_mpvPathValidationStatus;
}

bool SettingsController::ffmpegToolsAvailable() const
{
    return ffmpegToolsAvailableNow();
}

bool SettingsController::multiviewEnabled() const
{
    return m_multiviewEnabled;
}

void SettingsController::setMultiviewEnabled(const bool value)
{
    const auto wasDirty = dirty();
    if (m_multiviewEnabled == value) {
        return;
    }
    m_multiviewEnabled = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

int SettingsController::multiviewMaxTiles() const
{
    return m_multiviewMaxTiles;
}

void SettingsController::setMultiviewMaxTiles(const int value)
{
    const auto wasDirty = dirty();
    const auto normalized = Core::normalizeMultiviewMaxTiles(value);
    if (m_multiviewMaxTiles == normalized) {
        return;
    }
    m_multiviewMaxTiles = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::multiviewPreferHwdec() const
{
    return m_multiviewPreferHwdec;
}

void SettingsController::setMultiviewPreferHwdec(const bool value)
{
    const auto wasDirty = dirty();
    if (m_multiviewPreferHwdec == value) {
        return;
    }
    m_multiviewPreferHwdec = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::multiviewRetainSelectionOnPromotion() const
{
    return m_multiviewRetainSelectionOnPromotion;
}

void SettingsController::setMultiviewRetainSelectionOnPromotion(const bool value)
{
    const auto wasDirty = dirty();
    if (m_multiviewRetainSelectionOnPromotion == value) {
        return;
    }
    m_multiviewRetainSelectionOnPromotion = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

QString SettingsController::screenshotsDirectory() const
{
    return m_screenshotsDirectory;
}

void SettingsController::setScreenshotsDirectory(const QString &value)
{
    const auto wasDirty = dirty();
    const auto normalized = normalizeOverridePath(value);
    if (m_screenshotsDirectory == normalized) {
        return;
    }
    m_screenshotsDirectory = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

QString SettingsController::recordingsDirectory() const
{
    return m_recordingsDirectory;
}

void SettingsController::setRecordingsDirectory(const QString &value)
{
    const auto wasDirty = dirty();
    const auto normalized = normalizeOverridePath(value);
    if (m_recordingsDirectory == normalized) {
        return;
    }
    m_recordingsDirectory = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::remuxRecordingsToMkv() const
{
    return ffmpegToolsAvailable() && m_remuxRecordingsToMkv;
}

void SettingsController::setRemuxRecordingsToMkv(bool value)
{
    const auto wasDirty = dirty();
    const auto normalized = ffmpegToolsAvailable() && value;
    if (m_remuxRecordingsToMkv == normalized) {
        return;
    }
    m_remuxRecordingsToMkv = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::minimizeToTrayOnMinimize() const
{
    return m_minimizeToTrayOnMinimize;
}

void SettingsController::setMinimizeToTrayOnMinimize(const bool value)
{
    if (m_minimizeToTrayOnMinimize == value) {
        return;
    }
    const auto wasDirty = dirty();
    m_minimizeToTrayOnMinimize = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

QString SettingsController::dvrRecordingsDirectory() const
{
    return m_dvrRecordingsDirectory;
}

void SettingsController::setDvrRecordingsDirectory(const QString &value)
{
    const auto wasDirty = dirty();
    const auto normalized = normalizeOverridePath(value);
    if (m_dvrRecordingsDirectory == normalized) {
        return;
    }
    m_dvrRecordingsDirectory = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::dvrRemuxToMkv() const
{
    return ffmpegToolsAvailable() && m_dvrRemuxToMkv;
}

void SettingsController::setDvrRemuxToMkv(const bool value)
{
    const auto wasDirty = dirty();
    const auto normalized = ffmpegToolsAvailable() && value;
    if (m_dvrRemuxToMkv == normalized) {
        return;
    }
    m_dvrRemuxToMkv = normalized;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

int SettingsController::dvrStartOffsetMinutes() const
{
    return m_dvrStartOffsetMinutes;
}

void SettingsController::setDvrStartOffsetMinutes(const int value)
{
    if (m_dvrStartOffsetMinutes == value) {
        return;
    }
    const auto wasDirty = dirty();
    m_dvrStartOffsetMinutes = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

int SettingsController::dvrEndOffsetMinutes() const
{
    return m_dvrEndOffsetMinutes;
}

void SettingsController::setDvrEndOffsetMinutes(const int value)
{
    if (m_dvrEndOffsetMinutes == value) {
        return;
    }
    const auto wasDirty = dirty();
    m_dvrEndOffsetMinutes = value;
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

bool SettingsController::dirty() const
{
    const auto &settings = m_settings->current();
    const auto ffmpegAvailable = ffmpegToolsAvailable();
    const auto effectiveSettingsTimeshiftEnabled = ffmpegAvailable ? settings.timeshiftEnabled : false;
    const auto effectiveSettingsRemuxRecordings = ffmpegAvailable ? settings.remuxRecordingsToMkv : false;
    const auto effectiveSettingsDvrRemux = ffmpegAvailable ? settings.dvrRemuxToMkv : false;
    return normalizedThemeDraft() != settings.theme
        || m_showOnTopModeIndicator != settings.showOnTopModeIndicator
        || m_preventDisplaySleep != settings.preventDisplaySleep
        || m_guidePreviewEnabled != settings.guidePreviewEnabled
        || m_overlayAutoHide != settings.overlayAutoHide
        || normalizedOverlayAutoHideSecondsDraft() != settings.overlayAutoHideSeconds
        || normalizedRefreshIntervalDraft() != settings.refreshIntervalMinutes
        || m_autoRefreshEpg != settings.autoRefreshEpg
        || normalizedGuidePastHoursDraft() != settings.guidePastHours
        || normalizedLookAheadDraft() != settings.epgLookAheadHours
        || differs(normalizedWaitForDataStreamDraft(), settings.playerWaitForStreamSeconds)
        || m_deinterlaceEnabled != settings.playerDeinterlaceEnabled
        || differs(normalizedBufferSizeDraft(), settings.playerBufferSeconds)
        || m_playerUserAgent.trimmed() != settings.playerUserAgent.trimmed()
        || timeshiftEnabled() != effectiveSettingsTimeshiftEnabled
        || normalizedTimeshiftWindowMinutesDraft() != settings.timeshiftWindowMinutes
        || normalizedTimeshiftSegmentSecondsDraft() != settings.timeshiftSegmentSeconds
        || normalizeOverridePath(m_timeshiftStorageDirectory) != normalizeOverridePath(settings.timeshiftStorageDirectory)
        || normalizedTimeshiftMaxDiskGbDraft() != settings.timeshiftMaxDiskGb
        || normalizeOverridePath(m_mpvDllPath) != normalizeOverridePath(settings.mpvDllPath)
        || m_multiviewEnabled != settings.multiviewEnabled
        || m_multiviewMaxTiles != Core::normalizeMultiviewMaxTiles(settings.multiviewMaxTiles)
        || m_multiviewPreferHwdec != settings.multiviewPreferHwdec
        || m_multiviewRetainSelectionOnPromotion != settings.multiviewRetainSelectionOnPromotion
        || normalizeOverridePath(m_screenshotsDirectory) != normalizeOverridePath(settings.screenshotsDirectory)
        || normalizeOverridePath(m_recordingsDirectory) != normalizeOverridePath(settings.recordingsDirectory)
        || remuxRecordingsToMkv() != effectiveSettingsRemuxRecordings
        || m_minimizeToTrayOnMinimize != settings.minimizeToTrayOnMinimize
        || normalizeOverridePath(m_dvrRecordingsDirectory) != normalizeOverridePath(settings.dvrRecordingsDirectory)
        || dvrRemuxToMkv() != effectiveSettingsDvrRemux
        || m_dvrStartOffsetMinutes != settings.dvrStartOffsetMinutes
        || m_dvrEndOffsetMinutes != settings.dvrEndOffsetMinutes;
}

void SettingsController::reload()
{
    const auto wasDirty = dirty();
    const auto &settings = m_settings->current();
    m_theme = settings.theme;
    m_showOnTopModeIndicator = settings.showOnTopModeIndicator;
    m_preventDisplaySleep = settings.preventDisplaySleep;
    m_guidePreviewEnabled = settings.guidePreviewEnabled;
    m_overlayAutoHide = settings.overlayAutoHide;
    m_overlayAutoHideSeconds = settings.overlayAutoHideSeconds;
    m_refreshIntervalMinutes = settings.refreshIntervalMinutes;
    m_autoRefreshEpg = settings.autoRefreshEpg;
    m_guidePastHours = settings.guidePastHours;
    m_epgLookAheadHours = settings.epgLookAheadHours;
    m_waitForDataStreamSeconds = settings.playerWaitForStreamSeconds;
    m_deinterlaceEnabled = settings.playerDeinterlaceEnabled;
    m_bufferSizeSeconds = settings.playerBufferSeconds;
    m_playerUserAgent = settings.playerUserAgent.trimmed();
    m_timeshiftEnabled = settings.timeshiftEnabled;
    m_timeshiftWindowMinutes = settings.timeshiftWindowMinutes;
    m_timeshiftSegmentSeconds = settings.timeshiftSegmentSeconds;
    m_timeshiftStorageDirectory = normalizeOverridePath(settings.timeshiftStorageDirectory);
    m_timeshiftMaxDiskGb = settings.timeshiftMaxDiskGb;
    m_mpvDllPath = normalizeOverridePath(settings.mpvDllPath);
    m_mpvPathValidationStatus = mpvPathValidationStatusFor(m_mpvDllPath);
    m_multiviewEnabled = settings.multiviewEnabled;
    m_multiviewMaxTiles = Core::normalizeMultiviewMaxTiles(settings.multiviewMaxTiles);
    m_multiviewPreferHwdec = settings.multiviewPreferHwdec;
    m_multiviewRetainSelectionOnPromotion = settings.multiviewRetainSelectionOnPromotion;
    m_screenshotsDirectory = normalizeOverridePath(settings.screenshotsDirectory);
    m_recordingsDirectory = normalizeOverridePath(settings.recordingsDirectory);
    m_remuxRecordingsToMkv = settings.remuxRecordingsToMkv;
    m_minimizeToTrayOnMinimize = settings.minimizeToTrayOnMinimize;
    m_dvrRecordingsDirectory = normalizeOverridePath(settings.dvrRecordingsDirectory);
    m_dvrRemuxToMkv = settings.dvrRemuxToMkv;
    m_dvrStartOffsetMinutes = settings.dvrStartOffsetMinutes;
    m_dvrEndOffsetMinutes = settings.dvrEndOffsetMinutes;
    emit validationChanged();
    emit settingsChanged();
    emitDirtyChangedIfNeeded(wasDirty);
}

void SettingsController::save()
{
    const auto wasDirty = dirty();
    auto &settings = m_settings->current();
    settings.theme = normalizedThemeDraft();
    settings.showOnTopModeIndicator = m_showOnTopModeIndicator;
    settings.preventDisplaySleep = m_preventDisplaySleep;
    settings.guidePreviewEnabled = m_guidePreviewEnabled;
    settings.overlayAutoHide = m_overlayAutoHide;
    settings.overlayAutoHideSeconds = normalizedOverlayAutoHideSecondsDraft();
    settings.refreshIntervalMinutes = normalizedRefreshIntervalDraft();
    settings.autoRefreshEpg = m_autoRefreshEpg;
    settings.guidePastHours = normalizedGuidePastHoursDraft();
    settings.epgLookAheadHours = normalizedLookAheadDraft();
    settings.playerWaitForStreamSeconds = normalizedWaitForDataStreamDraft();
    settings.playerDeinterlaceEnabled = m_deinterlaceEnabled;
    settings.playerBufferSeconds = normalizedBufferSizeDraft();
    settings.playerUserAgent = m_playerUserAgent.trimmed();
    settings.timeshiftEnabled = timeshiftEnabled();
    settings.timeshiftWindowMinutes = normalizedTimeshiftWindowMinutesDraft();
    settings.timeshiftSegmentSeconds = normalizedTimeshiftSegmentSecondsDraft();
    settings.timeshiftStorageDirectory = normalizeOverridePath(m_timeshiftStorageDirectory);
    settings.timeshiftMaxDiskGb = normalizedTimeshiftMaxDiskGbDraft();
    settings.mpvDllPath = normalizeOverridePath(m_mpvDllPath);
    settings.multiviewEnabled = m_multiviewEnabled;
    settings.multiviewMaxTiles = Core::normalizeMultiviewMaxTiles(m_multiviewMaxTiles);
    settings.multiviewPreferHwdec = m_multiviewPreferHwdec;
    settings.multiviewRetainSelectionOnPromotion = m_multiviewRetainSelectionOnPromotion;
    settings.screenshotsDirectory = normalizeOverridePath(m_screenshotsDirectory);
    settings.recordingsDirectory = normalizeOverridePath(m_recordingsDirectory);
    settings.remuxRecordingsToMkv = remuxRecordingsToMkv();
    settings.minimizeToTrayOnMinimize = m_minimizeToTrayOnMinimize;
    settings.dvrRecordingsDirectory = normalizeOverridePath(m_dvrRecordingsDirectory);
    settings.dvrRemuxToMkv = dvrRemuxToMkv();
    settings.dvrStartOffsetMinutes = m_dvrStartOffsetMinutes;
    settings.dvrEndOffsetMinutes = m_dvrEndOffsetMinutes;
    m_settings->save();

    m_theme = settings.theme;
    m_showOnTopModeIndicator = settings.showOnTopModeIndicator;
    m_preventDisplaySleep = settings.preventDisplaySleep;
    m_overlayAutoHideSeconds = settings.overlayAutoHideSeconds;
    m_refreshIntervalMinutes = settings.refreshIntervalMinutes;
    m_guidePastHours = settings.guidePastHours;
    m_epgLookAheadHours = settings.epgLookAheadHours;
    m_waitForDataStreamSeconds = settings.playerWaitForStreamSeconds;
    m_deinterlaceEnabled = settings.playerDeinterlaceEnabled;
    m_bufferSizeSeconds = settings.playerBufferSeconds;
    m_playerUserAgent = settings.playerUserAgent.trimmed();
    m_timeshiftEnabled = settings.timeshiftEnabled;
    m_timeshiftWindowMinutes = settings.timeshiftWindowMinutes;
    m_timeshiftSegmentSeconds = settings.timeshiftSegmentSeconds;
    m_timeshiftStorageDirectory = settings.timeshiftStorageDirectory;
    m_timeshiftMaxDiskGb = settings.timeshiftMaxDiskGb;
    m_mpvDllPath = settings.mpvDllPath;
    m_mpvPathValidationStatus = mpvPathValidationStatusFor(m_mpvDllPath);
    m_multiviewEnabled = settings.multiviewEnabled;
    m_multiviewMaxTiles = Core::normalizeMultiviewMaxTiles(settings.multiviewMaxTiles);
    m_multiviewPreferHwdec = settings.multiviewPreferHwdec;
    m_multiviewRetainSelectionOnPromotion = settings.multiviewRetainSelectionOnPromotion;
    m_screenshotsDirectory = settings.screenshotsDirectory;
    m_recordingsDirectory = settings.recordingsDirectory;
    m_remuxRecordingsToMkv = settings.remuxRecordingsToMkv;
    m_minimizeToTrayOnMinimize = settings.minimizeToTrayOnMinimize;
    m_dvrRecordingsDirectory = settings.dvrRecordingsDirectory;
    m_dvrRemuxToMkv = settings.dvrRemuxToMkv;
    m_dvrStartOffsetMinutes = settings.dvrStartOffsetMinutes;
    m_dvrEndOffsetMinutes = settings.dvrEndOffsetMinutes;

    emit validationChanged();
    emit settingsChanged();
    m_playerController->applySettings(
        settings.mpvDllPath,
        settings.mpvOptions,
        settings.playerWaitForStreamSeconds,
        settings.playerDeinterlaceEnabled,
        settings.playerBufferSeconds,
        settings.playerUserAgent,
        remuxRecordingsToMkv());
    m_multiViewController->applySettings();
    m_profilesModel->reload();
    emitDirtyChangedIfNeeded(wasDirty);
    emit saved();
}

void SettingsController::cancel()
{
    reload();
}

void SettingsController::useBundledMpv()
{
    setMpvDllPath({});
    validateMpvPath();
}

void SettingsController::validateMpvPath()
{
    m_mpvPathValidationStatus = mpvPathValidationStatusFor(normalizeOverridePath(m_mpvDllPath));
    emit validationChanged();
}

QString SettingsController::normalizeOverridePath(const QString &rawPath)
{
    auto trimmed = rawPath.trimmed();
    if (trimmed.compare(QStringLiteral("mpv-2.dll"), Qt::CaseInsensitive) == 0) {
        return {};
    }
    return trimmed;
}

QString SettingsController::normalizedThemeDraft() const
{
    const auto trimmed = m_theme.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("Dark") : trimmed;
}

int SettingsController::normalizedRefreshIntervalDraft() const
{
    return std::max(1, m_refreshIntervalMinutes);
}

int SettingsController::normalizedOverlayAutoHideSecondsDraft() const
{
    return std::max(1, m_overlayAutoHideSeconds);
}

int SettingsController::normalizedGuidePastHoursDraft() const
{
    return Core::normalizeGuideHours(m_guidePastHours);
}

int SettingsController::normalizedLookAheadDraft() const
{
    return Core::normalizeGuideHours(m_epgLookAheadHours);
}

double SettingsController::normalizedWaitForDataStreamDraft() const
{
    return Core::normalizePlayerWaitForStreamSeconds(m_waitForDataStreamSeconds);
}

double SettingsController::normalizedBufferSizeDraft() const
{
    return Core::normalizePlayerBufferSeconds(m_bufferSizeSeconds);
}

int SettingsController::normalizedTimeshiftWindowMinutesDraft() const
{
    return Core::normalizeTimeshiftWindowMinutes(m_timeshiftWindowMinutes);
}

int SettingsController::normalizedTimeshiftSegmentSecondsDraft() const
{
    return Core::normalizeTimeshiftSegmentSeconds(m_timeshiftSegmentSeconds);
}

int SettingsController::normalizedTimeshiftMaxDiskGbDraft() const
{
    return Core::normalizeTimeshiftMaxDiskGb(m_timeshiftMaxDiskGb);
}

void SettingsController::emitDirtyChangedIfNeeded(const bool wasDirty)
{
    if (wasDirty != dirty()) {
        emit dirtyChanged();
    }
}

} // namespace OKILTV::App
