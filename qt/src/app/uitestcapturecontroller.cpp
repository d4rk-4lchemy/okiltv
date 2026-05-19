#include "uitestcapturecontroller.h"

#include "../core/debuglogger.h"

#include <QFile>
#include <QQuickWindow>

namespace OKILTV::App {

UiTestCaptureController::UiTestCaptureController(QObject *parent)
    : QObject(parent)
    , m_requestFilePath(qEnvironmentVariable("OKILTV_UI_TEST_CAPTURE_REQUEST_FILE").trimmed())
{
    if (!m_requestFilePath.isEmpty()) {
        m_pollTimer.setInterval(100);
        connect(&m_pollTimer, &QTimer::timeout, this, &UiTestCaptureController::pollRequests);
        m_pollTimer.start();
        Core::DebugLogger::instance().log(
            QStringLiteral("ui-test"),
            QStringLiteral("UI test capture enabled. request_file=%1").arg(m_requestFilePath));
    }
}

bool UiTestCaptureController::enabled() const
{
    return !m_requestFilePath.isEmpty();
}

void UiTestCaptureController::setWindow(QObject *windowObject)
{
    m_window = qobject_cast<QQuickWindow *>(windowObject);
}

void UiTestCaptureController::requestCapture(const QString &outputPath)
{
    const auto normalizedPath = outputPath.trimmed();
    if (normalizedPath.isEmpty()) {
        notifyCaptureSaved({}, false);
        return;
    }

    Core::DebugLogger::instance().log(
        QStringLiteral("ui-test"),
        QStringLiteral("Capture requested: %1").arg(normalizedPath));
    if (m_window != nullptr) {
        const auto image = m_window->grabWindow();
        const auto ok = !image.isNull() && image.save(normalizedPath);
        notifyCaptureSaved(normalizedPath, ok);
        return;
    }

    emit captureRequested(normalizedPath);
}

void UiTestCaptureController::notifyCaptureSaved(const QString &outputPath, const bool ok)
{
    Core::DebugLogger::instance().log(
        QStringLiteral("ui-test"),
        QStringLiteral("Capture %1: %2")
            .arg(ok ? QStringLiteral("saved") : QStringLiteral("failed"))
            .arg(outputPath));
    emit captureFinished(outputPath, ok);
}

void UiTestCaptureController::pollRequests()
{
    if (m_requestFilePath.isEmpty()) {
        return;
    }

    QFile requestFile(m_requestFilePath);
    if (!requestFile.exists()) {
        return;
    }

    if (!requestFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    const auto outputPath = QString::fromUtf8(requestFile.readAll()).trimmed();
    requestFile.close();
    requestFile.remove();

    if (outputPath.isEmpty()) {
        return;
    }

    requestCapture(outputPath);
}

} // namespace OKILTV::App
