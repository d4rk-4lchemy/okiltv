#pragma once

#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>

namespace OKILTV::Core { class SettingsManager; }
class QNetworkReply;

namespace OKILTV::App {

class UpdateCheckController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(QString errorText READ errorText NOTIFY changed)

public:
    explicit UpdateCheckController(Core::SettingsManager *settings, QObject *parent = nullptr);
    // Explicit dependencies keep network tests independent of GitHub and the build version.
    UpdateCheckController(Core::SettingsManager *settings, QString currentVersion,
                          QUrl endpoint, int timeoutMs, QObject *parent = nullptr);
    ~UpdateCheckController() override;

    QString currentVersion() const { return m_currentVersion; }
    QString latestVersion() const { return m_latestVersion; }
    QString errorText() const { return m_errorText; }
    bool pending() const { return m_pending; }

    void check();
    Q_INVOKABLE void shutdown();
    Q_INVOKABLE void openRelease();
    Q_INVOKABLE void skipVersion();
    Q_INVOKABLE void dismiss();

signals:
    void changed();
    void checkFinished();

private:
    void finishReply();

    Core::SettingsManager *m_settings;
    QString m_currentVersion;
    QString m_latestVersion;
    QString m_releaseTag;
    QString m_errorText;
    QUrl m_endpoint;
    int m_timeoutMs;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    bool m_started { false };
    bool m_stopped { false };
    bool m_pending { false };
};

} // namespace OKILTV::App
