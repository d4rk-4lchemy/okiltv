#include "shellcontroller.h"

namespace OKILTV::App {

ShellController::ShellController(Core::SettingsManager *settings, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
{
    m_overlaysVisible = false;
    m_activeOverlay = QStringLiteral("none");
    m_overlaySection = QStringLiteral("appearance");
    m_lastSettingsSection = QStringLiteral("appearance");
}

bool ShellController::overlaysVisible() const
{
    return m_overlaysVisible;
}

bool ShellController::fullscreen() const
{
    return m_fullscreen;
}

QString ShellController::layoutBand() const
{
    return m_layoutBand;
}

QString ShellController::focusedZone() const
{
    return m_focusedZone;
}

QString ShellController::activeOverlay() const
{
    return m_activeOverlay;
}

QString ShellController::overlaySection() const
{
    return m_overlaySection;
}

void ShellController::restoreLastView()
{
    setOverlaysVisible(false);
    clearOverlay();
}

void ShellController::setOverlaysVisible(const bool value)
{
    if (m_overlaysVisible == value) {
        return;
    }

    m_overlaysVisible = value;
    emit overlaysVisibleChanged();
}

void ShellController::setFullscreen(const bool value)
{
    if (m_fullscreen == value) {
        return;
    }

    m_fullscreen = value;
    const auto nextBand = computeLayoutBand(m_fullscreen ? 1920 : 1440, m_fullscreen);
    const auto layoutChanged = m_layoutBand != nextBand;
    m_layoutBand = nextBand;
    emit fullscreenChanged();
    if (layoutChanged) {
        emit layoutBandChanged();
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ShellController::updateWindowMetrics(const int width, const int height)
{
    Q_UNUSED(height);

    const auto nextBand = computeLayoutBand(width, m_fullscreen);
    if (m_layoutBand == nextBand) {
        return;
    }

    m_layoutBand = nextBand;
    emit layoutBandChanged();
}

void ShellController::setFocusedZone(const QString &value)
{
    const auto normalized = value.trimmed().isEmpty() ? QStringLiteral("content") : value.trimmed().toLower();
    if (m_focusedZone == normalized) {
        return;
    }

    m_focusedZone = normalized;
    emit focusedZoneChanged();
}

void ShellController::setActiveOverlay(const QString &value)
{
    const auto normalized = normalizeOverlayName(value);
    if (m_activeOverlay == normalized) {
        return;
    }

    m_activeOverlay = normalized;
    if (m_activeOverlay != QStringLiteral("none")) {
        setOverlaysVisible(true);
    }
    emit activeOverlayChanged();
}

void ShellController::setOverlaySection(const QString &value)
{
    const auto normalized = normalizeOverlaySection(value);
    if (m_overlaySection == normalized) {
        return;
    }

    m_overlaySection = normalized;
    if (m_overlaySection != QStringLiteral("sources")) {
        m_lastSettingsSection = m_overlaySection;
    }
    emit overlaySectionChanged();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ShellController::openOverlay(const QString &overlay, const QString &section)
{
    const auto normalizedOverlay = normalizeOverlayName(overlay);
    if (normalizedOverlay == QStringLiteral("settings")) {
        if (section.trimmed().isEmpty()) {
            setOverlaySection(m_lastSettingsSection);
        } else {
            setOverlaySection(section);
        }
    }
    setActiveOverlay(normalizedOverlay);
}

void ShellController::clearOverlay()
{
    setActiveOverlay(QStringLiteral("none"));
}

void ShellController::showOverlaysTemporary()
{
    setOverlaysVisible(true);
}

bool ShellController::reopenMaximizedOnLaunch() const
{
    return m_settings->current().reopenMaximizedOnLaunch;
}

void ShellController::setReopenMaximizedOnLaunch(const bool value)
{
    if (m_settings->current().reopenMaximizedOnLaunch == value) {
        return;
    }

    m_settings->current().reopenMaximizedOnLaunch = value;
    m_settings->save();
}

QString ShellController::computeLayoutBand(const int width, const bool fullscreen) const
{
    if (fullscreen || width >= 1920) {
        return QStringLiteral("expanded");
    }
    if (width >= 1440) {
        return QStringLiteral("standard");
    }
    return QStringLiteral("compact");
}

QString ShellController::normalizeOverlayName(const QString &value)
{
    auto normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("guide") || normalized == QStringLiteral("settings")) {
        return normalized;
    }
    return QStringLiteral("none");
}

QString ShellController::normalizeOverlaySection(const QString &value)
{
    const auto normalized = value.trimmed().toLower();
    return normalized.isEmpty() ? QStringLiteral("appearance") : normalized;
}

} // namespace OKILTV::App
