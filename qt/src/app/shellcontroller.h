#pragma once

#include "../core/settingsmanager.h"

#include <QObject>

namespace OKILTV::App {

class ShellController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool overlaysVisible READ overlaysVisible WRITE setOverlaysVisible NOTIFY overlaysVisibleChanged)
    Q_PROPERTY(bool fullscreen READ fullscreen WRITE setFullscreen NOTIFY fullscreenChanged)
    Q_PROPERTY(QString layoutBand READ layoutBand NOTIFY layoutBandChanged)
    Q_PROPERTY(QString focusedZone READ focusedZone WRITE setFocusedZone NOTIFY focusedZoneChanged)
    Q_PROPERTY(QString activeOverlay READ activeOverlay WRITE setActiveOverlay NOTIFY activeOverlayChanged)
    Q_PROPERTY(QString overlaySection READ overlaySection WRITE setOverlaySection NOTIFY overlaySectionChanged)

public:
    explicit ShellController(Core::SettingsManager *settings, QObject *parent = nullptr);

    bool overlaysVisible() const;
    bool fullscreen() const;
    QString layoutBand() const;
    QString focusedZone() const;
    QString activeOverlay() const;
    QString overlaySection() const;

public slots:
    void restoreLastView();
    void setOverlaysVisible(bool value);
    void setFullscreen(bool value);
    void updateWindowMetrics(int width, int height);
    void setFocusedZone(const QString &value);
    void setActiveOverlay(const QString &value);
    void setOverlaySection(const QString &value);
    void openOverlay(const QString &overlay, const QString &section = QString {});
    void clearOverlay();
    void showOverlaysTemporary();
    Q_INVOKABLE bool reopenMaximizedOnLaunch() const;
    Q_INVOKABLE void setReopenMaximizedOnLaunch(bool value);

signals:
    void overlaysVisibleChanged();
    void fullscreenChanged();
    void layoutBandChanged();
    void focusedZoneChanged();
    void activeOverlayChanged();
    void overlaySectionChanged();

private:
    QString computeLayoutBand(int width, bool fullscreen) const;
    static QString normalizeOverlayName(const QString &value);
    static QString normalizeOverlaySection(const QString &value);

    Core::SettingsManager *m_settings;
    bool m_overlaysVisible { false };
    bool m_fullscreen { false };
    QString m_layoutBand { QStringLiteral("standard") };
    QString m_focusedZone { QStringLiteral("content") };
    QString m_activeOverlay { QStringLiteral("none") };
    QString m_overlaySection { QStringLiteral("appearance") };
    QString m_lastSettingsSection { QStringLiteral("appearance") };
};

} // namespace OKILTV::App
