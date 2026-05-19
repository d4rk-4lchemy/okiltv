#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>

class QQuickWindow;

namespace OKILTV::App {

class UiTestCaptureController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled CONSTANT)

public:
    explicit UiTestCaptureController(QObject *parent = nullptr);

    bool enabled() const;

    Q_INVOKABLE void setWindow(QObject *windowObject);
    Q_INVOKABLE void requestCapture(const QString &outputPath);
    Q_INVOKABLE void notifyCaptureSaved(const QString &outputPath, bool ok);

signals:
    void captureRequested(const QString &outputPath);
    void captureFinished(const QString &outputPath, bool ok);

private:
    void pollRequests();

    QString m_requestFilePath;
    QTimer m_pollTimer;
    QPointer<QQuickWindow> m_window;
};

} // namespace OKILTV::App
