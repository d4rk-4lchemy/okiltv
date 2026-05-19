#pragma once

#include "../core/settingsmanager.h"

#include <QObject>

namespace OKILTV::App {

class PlayerController;
class ProfilesModel;
class MultiViewController;

class SettingsController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY settingsChanged)
    Q_PROPERTY(bool showOnTopModeIndicator READ showOnTopModeIndicator WRITE setShowOnTopModeIndicator NOTIFY settingsChanged)
    Q_PROPERTY(bool preventDisplaySleep READ preventDisplaySleep WRITE setPreventDisplaySleep NOTIFY settingsChanged)
    Q_PROPERTY(bool guidePreviewEnabled READ guidePreviewEnabled WRITE setGuidePreviewEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool overlayAutoHide READ overlayAutoHide WRITE setOverlayAutoHide NOTIFY settingsChanged)
    Q_PROPERTY(int overlayAutoHideSeconds READ overlayAutoHideSeconds WRITE setOverlayAutoHideSeconds NOTIFY settingsChanged)
    Q_PROPERTY(int refreshIntervalMinutes READ refreshIntervalMinutes WRITE setRefreshIntervalMinutes NOTIFY settingsChanged)
    Q_PROPERTY(bool autoRefreshEpg READ autoRefreshEpg WRITE setAutoRefreshEpg NOTIFY settingsChanged)
    Q_PROPERTY(int guidePastHours READ guidePastHours WRITE setGuidePastHours NOTIFY settingsChanged)
    Q_PROPERTY(int epgLookAheadHours READ epgLookAheadHours WRITE setEpgLookAheadHours NOTIFY settingsChanged)
    Q_PROPERTY(double waitForDataStreamSeconds READ waitForDataStreamSeconds WRITE setWaitForDataStreamSeconds NOTIFY settingsChanged)
    Q_PROPERTY(bool deinterlaceEnabled READ deinterlaceEnabled WRITE setDeinterlaceEnabled NOTIFY settingsChanged)
    Q_PROPERTY(double bufferSizeSeconds READ bufferSizeSeconds WRITE setBufferSizeSeconds NOTIFY settingsChanged)
    Q_PROPERTY(QString playerUserAgent READ playerUserAgent WRITE setPlayerUserAgent NOTIFY settingsChanged)
    Q_PROPERTY(bool timeshiftEnabled READ timeshiftEnabled WRITE setTimeshiftEnabled NOTIFY settingsChanged)
    Q_PROPERTY(int timeshiftWindowMinutes READ timeshiftWindowMinutes WRITE setTimeshiftWindowMinutes NOTIFY settingsChanged)
    Q_PROPERTY(int timeshiftSegmentSeconds READ timeshiftSegmentSeconds WRITE setTimeshiftSegmentSeconds NOTIFY settingsChanged)
    Q_PROPERTY(QString timeshiftStorageDirectory READ timeshiftStorageDirectory WRITE setTimeshiftStorageDirectory NOTIFY settingsChanged)
    Q_PROPERTY(int timeshiftMaxDiskGb READ timeshiftMaxDiskGb WRITE setTimeshiftMaxDiskGb NOTIFY settingsChanged)
    Q_PROPERTY(QString mpvDllPath READ mpvDllPath WRITE setMpvDllPath NOTIFY settingsChanged)
    Q_PROPERTY(QString mpvPathValidationStatus READ mpvPathValidationStatus NOTIFY validationChanged)
    Q_PROPERTY(bool ffmpegToolsAvailable READ ffmpegToolsAvailable NOTIFY validationChanged)
    Q_PROPERTY(bool multiviewEnabled READ multiviewEnabled WRITE setMultiviewEnabled NOTIFY settingsChanged)
    Q_PROPERTY(int multiviewMaxTiles READ multiviewMaxTiles WRITE setMultiviewMaxTiles NOTIFY settingsChanged)
    Q_PROPERTY(bool multiviewPreferHwdec READ multiviewPreferHwdec WRITE setMultiviewPreferHwdec NOTIFY settingsChanged)
    Q_PROPERTY(bool multiviewRetainSelectionOnPromotion READ multiviewRetainSelectionOnPromotion WRITE setMultiviewRetainSelectionOnPromotion NOTIFY settingsChanged)
    Q_PROPERTY(QString screenshotsDirectory READ screenshotsDirectory WRITE setScreenshotsDirectory NOTIFY settingsChanged)
    Q_PROPERTY(QString recordingsDirectory READ recordingsDirectory WRITE setRecordingsDirectory NOTIFY settingsChanged)
    Q_PROPERTY(bool remuxRecordingsToMkv READ remuxRecordingsToMkv WRITE setRemuxRecordingsToMkv NOTIFY settingsChanged)
    Q_PROPERTY(bool minimizeToTrayOnMinimize READ minimizeToTrayOnMinimize WRITE setMinimizeToTrayOnMinimize NOTIFY settingsChanged)
    Q_PROPERTY(QString dvrRecordingsDirectory READ dvrRecordingsDirectory WRITE setDvrRecordingsDirectory NOTIFY settingsChanged)
    Q_PROPERTY(bool dvrRemuxToMkv READ dvrRemuxToMkv WRITE setDvrRemuxToMkv NOTIFY settingsChanged)
    Q_PROPERTY(int dvrStartOffsetMinutes READ dvrStartOffsetMinutes WRITE setDvrStartOffsetMinutes NOTIFY settingsChanged)
    Q_PROPERTY(int dvrEndOffsetMinutes READ dvrEndOffsetMinutes WRITE setDvrEndOffsetMinutes NOTIFY settingsChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)

public:
    SettingsController(
        Core::SettingsManager *settings,
        PlayerController *playerController,
        MultiViewController *multiViewController,
        ProfilesModel *profilesModel,
        QObject *parent = nullptr);

    QString theme() const;
    void setTheme(const QString &value);

    bool showOnTopModeIndicator() const;
    void setShowOnTopModeIndicator(bool value);

    bool preventDisplaySleep() const;
    void setPreventDisplaySleep(bool value);

    bool guidePreviewEnabled() const;
    void setGuidePreviewEnabled(bool value);

    bool overlayAutoHide() const;
    void setOverlayAutoHide(bool value);

    int overlayAutoHideSeconds() const;
    void setOverlayAutoHideSeconds(int value);

    int refreshIntervalMinutes() const;
    void setRefreshIntervalMinutes(int value);

    bool autoRefreshEpg() const;
    void setAutoRefreshEpg(bool value);

    int guidePastHours() const;
    void setGuidePastHours(int value);

    int epgLookAheadHours() const;
    void setEpgLookAheadHours(int value);

    double waitForDataStreamSeconds() const;
    void setWaitForDataStreamSeconds(double value);

    bool deinterlaceEnabled() const;
    void setDeinterlaceEnabled(bool value);

    double bufferSizeSeconds() const;
    void setBufferSizeSeconds(double value);

    QString playerUserAgent() const;
    void setPlayerUserAgent(const QString &value);

    bool timeshiftEnabled() const;
    void setTimeshiftEnabled(bool value);

    int timeshiftWindowMinutes() const;
    void setTimeshiftWindowMinutes(int value);

    int timeshiftSegmentSeconds() const;
    void setTimeshiftSegmentSeconds(int value);

    QString timeshiftStorageDirectory() const;
    void setTimeshiftStorageDirectory(const QString &value);

    int timeshiftMaxDiskGb() const;
    void setTimeshiftMaxDiskGb(int value);

    QString mpvDllPath() const;
    void setMpvDllPath(const QString &value);

    QString mpvPathValidationStatus() const;
    bool ffmpegToolsAvailable() const;

    bool multiviewEnabled() const;
    void setMultiviewEnabled(bool value);

    int multiviewMaxTiles() const;
    void setMultiviewMaxTiles(int value);

    bool multiviewPreferHwdec() const;
    void setMultiviewPreferHwdec(bool value);
    bool multiviewRetainSelectionOnPromotion() const;
    void setMultiviewRetainSelectionOnPromotion(bool value);

    QString screenshotsDirectory() const;
    void setScreenshotsDirectory(const QString &value);

    QString recordingsDirectory() const;
    void setRecordingsDirectory(const QString &value);

    bool remuxRecordingsToMkv() const;
    void setRemuxRecordingsToMkv(bool value);

    bool minimizeToTrayOnMinimize() const;
    void setMinimizeToTrayOnMinimize(bool value);

    QString dvrRecordingsDirectory() const;
    void setDvrRecordingsDirectory(const QString &value);

    bool dvrRemuxToMkv() const;
    void setDvrRemuxToMkv(bool value);

    int dvrStartOffsetMinutes() const;
    void setDvrStartOffsetMinutes(int value);

    int dvrEndOffsetMinutes() const;
    void setDvrEndOffsetMinutes(int value);

    bool dirty() const;

    Q_INVOKABLE void reload();
    Q_INVOKABLE void save();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void useBundledMpv();
    Q_INVOKABLE void validateMpvPath();

signals:
    void settingsChanged();
    void validationChanged();
    void dirtyChanged();
    void saved();

private:
    static QString normalizeOverridePath(const QString &rawPath);
    QString normalizedThemeDraft() const;
    int normalizedOverlayAutoHideSecondsDraft() const;
    int normalizedRefreshIntervalDraft() const;
    int normalizedGuidePastHoursDraft() const;
    int normalizedLookAheadDraft() const;
    double normalizedWaitForDataStreamDraft() const;
    double normalizedBufferSizeDraft() const;
    int normalizedTimeshiftWindowMinutesDraft() const;
    int normalizedTimeshiftSegmentSecondsDraft() const;
    int normalizedTimeshiftMaxDiskGbDraft() const;
    void emitDirtyChangedIfNeeded(bool wasDirty);

    Core::SettingsManager *m_settings;
    PlayerController *m_playerController;
    MultiViewController *m_multiViewController;
    ProfilesModel *m_profilesModel;
    QString m_theme { QStringLiteral("Dark") };
    bool m_showOnTopModeIndicator { true };
    bool m_preventDisplaySleep { true };
    bool m_guidePreviewEnabled { true };
    bool m_overlayAutoHide { true };
    int m_overlayAutoHideSeconds { 3 };
    int m_refreshIntervalMinutes { 360 };
    bool m_autoRefreshEpg { true };
    int m_guidePastHours { 6 };
    int m_epgLookAheadHours { 24 };
    double m_waitForDataStreamSeconds { 5.0 };
    bool m_deinterlaceEnabled { true };
    double m_bufferSizeSeconds { 3.0 };
    QString m_playerUserAgent;
    bool m_timeshiftEnabled { false };
    int m_timeshiftWindowMinutes { 90 };
    int m_timeshiftSegmentSeconds { 2 };
    QString m_timeshiftStorageDirectory;
    int m_timeshiftMaxDiskGb { 8 };
    QString m_mpvDllPath;
    QString m_mpvPathValidationStatus { QStringLiteral("Bundled mpv will be used.") };
    bool m_multiviewEnabled { true };
    int m_multiviewMaxTiles { 4 };
    bool m_multiviewPreferHwdec { true };
    bool m_multiviewRetainSelectionOnPromotion { false };
    QString m_screenshotsDirectory;
    QString m_recordingsDirectory;
    bool m_remuxRecordingsToMkv { true };
    bool m_minimizeToTrayOnMinimize { true };
    QString m_dvrRecordingsDirectory;
    bool m_dvrRemuxToMkv { true };
    int m_dvrStartOffsetMinutes { 2 };
    int m_dvrEndOffsetMinutes { 2 };
};

} // namespace OKILTV::App
