#pragma once

#include "../core/models.h"
#include "../core/processutils.h"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QProcess>
#include <QPointer>
#include <QTimer>
#include <QThread>
#include <QUrl>

class QFileDialog;

namespace OKILTV::Core { class SettingsManager; }

namespace OKILTV::App {

class CatchupArchiveTransfer;

// Owns finite downloads independently of every playback backend. Secrets stay in
// the private job snapshots; only presentation fields are exposed as model roles.
class CatchupDownloadController final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool paused READ paused NOTIFY summaryChanged)
    Q_PROPERTY(bool activeProgressKnown READ activeProgressKnown NOTIFY summaryChanged)
    Q_PROPERTY(bool hasPending READ hasPending NOTIFY summaryChanged)
    Q_PROPERTY(int queuedCount READ queuedCount NOTIFY summaryChanged)
    Q_PROPERTY(double activeProgress READ activeProgress NOTIFY summaryChanged)
    Q_PROPERTY(bool unread READ unread NOTIFY summaryChanged)
    Q_PROPERTY(bool shuttingDown READ shuttingDown NOTIFY summaryChanged)
public:
    enum Role { JobIdRole = Qt::UserRole + 1, TitleRole, ChannelRole, PathRole,
                StateRole, ProgressRole, BytesRole, ErrorRole, ProgressKnownRole, ProgramStartRole };
    explicit CatchupDownloadController(Core::SettingsManager *settings, QObject *parent = nullptr);
    ~CatchupDownloadController() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool paused() const { return m_paused; }
    bool activeProgressKnown() const;
    Q_INVOKABLE void pauseAll();
    Q_INVOKABLE void resumeAll();
    bool hasPending() const;
    int queuedCount() const;
    double activeProgress() const;
    bool unread() const { return m_unread; }
    bool shuttingDown() const { return m_shuttingDown; }
    QString enqueue(const Core::Channel &channel, const Core::EpgEntry &program, const QUrl &destination);
    Q_INVOKABLE QUrl suggestedDestination(const QString &title, const QString &channelName = {}, const QDateTime &start = {});
    Q_INVOKABLE void chooseDestination(QObject *parentWindow, const QUrl &suggested);
    Q_INVOKABLE void cancelDestination();
    Q_INVOKABLE void cancel(const QString &jobId);
    Q_INVOKABLE void restart(const QString &jobId);
    Q_INVOKABLE void dismiss(const QString &jobId);
    Q_INVOKABLE void markRead();
    Q_INVOKABLE void shutdown();
    static QString safeFileName(QString title);
signals:
    void destinationChosen(const QUrl &destination);
    void destinationCancelled();
    void summaryChanged();
    void notification(const QString &message);
    void shutdownFinished();
private:
    struct Job {
        QString id;
        Core::Channel channel;
        Core::EpgEntry program;
        QString path;
        QString destinationPath; // Original output target, retained when path points to partial data.
        QString temporaryPath;
        QString sourcePath;
        qint64 trimStartSeconds { 0 };
        qint64 durationSeconds { 0 };
        bool progressKnown { false };
        bool hls { false };
        QString state { QStringLiteral("queued") };
        QString error;
        double progress { 0 };
        qint64 bytes { 0 };
        bool discardPartial { false };
    };
    void startNext();
    void startTransfer();
    void finalizeDownload();
    void launch(const QString &binary, const QStringList &arguments, bool probing);
    void readProgress();
    void processFinished(int code, QProcess::ExitStatus status);
    void stopActive(const QString &state, const QString &reason = {});
    void finish(const QString &state, const QString &reason = {});
    void cleanupAndFinish(int attempt = 0);
    bool preservePartial(Job &job);
    void publishActive();
    bool pathReserved(const QString &path) const;
    static bool pending(const Job &job);
    Core::SettingsManager *m_settings;
    QList<Job> m_jobs;
    int m_active { -1 };
    QProcess *m_process { nullptr };
    QPointer<QFileDialog> m_saveDialog;
#if defined(Q_OS_WIN)
    std::unique_ptr<Core::WindowsProcessJob> m_processJob;
#endif
    QTimer m_watchdog;
    QElapsedTimer m_lastProgress;
    QByteArray m_stdout;
    QByteArray m_probeOutput;
    QString m_terminalState;
    QString m_terminalReason;
    QThread m_transferThread;
    CatchupArchiveTransfer *m_transfer { nullptr };
    quint64 m_transferToken { 0 };
    bool m_transferActive { false };
    bool m_paused { false };
    bool m_probing { false };
    bool m_unread { false };
    bool m_shuttingDown { false };
};

} // namespace OKILTV::App
