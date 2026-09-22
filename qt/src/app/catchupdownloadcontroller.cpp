#include "catchupdownloadcontroller.h"
#include "catchuparchivetransfer.h"

#include "../core/appdatapaths.h"
#include "../core/datetimeformat.h"
#include "../core/catchupurlresolver.h"
#include "../core/settingsmanager.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QWindow>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>
#include <utility>

namespace OKILTV::App {

CatchupDownloadController::CatchupDownloadController(Core::SettingsManager *settings, QObject *parent)
    : QAbstractListModel(parent), m_settings(settings)
{
    m_transfer = new CatchupArchiveTransfer;
    m_transfer->moveToThread(&m_transferThread);
    connect(&m_transferThread, &QThread::finished, m_transfer, &QObject::deleteLater);
    connect(m_transfer, &CatchupArchiveTransfer::progress, this,
        // Order is fixed by CatchupArchiveTransfer::progress; token guards stale worker events.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        [this](quint64 token, qint64 bytes, qint64 total, bool comparing, double fraction) {
            if (token != m_transferToken || m_active < 0 || !m_terminalState.isEmpty())
                return;
            auto &job = m_jobs[m_active];
            job.bytes = bytes;
            job.progressKnown = total > 0 || fraction >= 0;
            if (fraction >= 0)
                job.progress = std::max(job.progress, std::clamp(99.0 * fraction, 0.0, 99.0));
            else if (total > 0)
                job.progress = std::max(job.progress, std::clamp(99.0 * static_cast<double>(bytes) / static_cast<double>(total), 0.0, 99.0));
            if (!m_paused)
                job.state = comparing ? QStringLiteral("resuming") : QStringLiteral("downloading");
            publishActive();
        });
    connect(m_transfer, &CatchupArchiveTransfer::paused, this, [this](quint64 token, const QString &reason) {
        if (token != m_transferToken || m_active < 0 || !m_terminalState.isEmpty())
            return;
        m_paused = true;
        m_jobs[m_active].state = QStringLiteral("paused");
        m_jobs[m_active].error = reason;
        publishActive();
        if (!reason.isEmpty())
            emit notification(reason);
    });
    connect(m_transfer, &CatchupArchiveTransfer::failed, this, [this](quint64 token, const QString &reason) {
        if (token != m_transferToken || m_active < 0 || !m_terminalState.isEmpty())
            return;
        m_transferActive = false;
        finish(QStringLiteral("failed"), reason);
    });
    connect(m_transfer, &CatchupArchiveTransfer::completed, this, [this](quint64 token, bool hls) {
        if (token != m_transferToken || m_active < 0 || !m_terminalState.isEmpty())
            return;
        m_transferActive = false;
        m_jobs[m_active].hls = hls;
        finalizeDownload();
    });
    connect(m_transfer, &CatchupArchiveTransfer::stopped, this, [this](quint64 token) {
        if (token != m_transferToken || m_active < 0)
            return;
        m_transferActive = false;
        cleanupAndFinish();
    });
    m_transferThread.start();
    m_watchdog.setInterval(1000);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (m_active < 0 || !m_terminalState.isEmpty())
            return;
        auto &job = m_jobs[m_active];
        const auto size = QFileInfo(job.temporaryPath).size();
        if (!m_probing && size > job.bytes) {
            job.bytes = size;
            m_lastProgress.restart();
            publishActive();
        }
        if (m_lastProgress.elapsed() >= (m_probing ? 10000 : 60000))
            stopActive(QStringLiteral("failed"), QStringLiteral("Download timed out without progress."));
    });
}

CatchupDownloadController::~CatchupDownloadController()
{
    // Normal exits wait asynchronously via shutdown(). This is only a last-resort
    // cleanup for application teardown that bypasses the window close contract.
    m_shuttingDown = true;
    m_transfer->disconnect(this);
    QMetaObject::invokeMethod(m_transfer, &CatchupArchiveTransfer::close, Qt::BlockingQueuedConnection);
    m_transferThread.quit();
    m_transferThread.wait();
    if (m_saveDialog) {
        m_saveDialog->disconnect(this);
        delete m_saveDialog;
    }
    if (m_process)
        m_process->disconnect(this);
    if (m_process && m_process->state() != QProcess::NotRunning) {
#if defined(Q_OS_WIN)
        if (m_processJob)
            m_processJob->terminate();
#endif
        m_process->kill();
        m_process->waitForFinished(3000);
    }
    for (auto &job : m_jobs) {
        if (job.discardPartial) {
            if (!job.temporaryPath.isEmpty())
                QFile::remove(job.temporaryPath);
            if (!job.sourcePath.isEmpty()) {
                QDir(job.sourcePath + QStringLiteral(".hls")).removeRecursively();
                QFile::remove(job.sourcePath);
            }
        } else if (pending(job)) {
            preservePartial(job);
        }
    }
}

bool CatchupDownloadController::pending(const Job &job)
{
    return job.state == QStringLiteral("queued") || job.state == QStringLiteral("downloading")
        || job.state == QStringLiteral("verifying") || job.state == QStringLiteral("stopping")
        || job.state == QStringLiteral("paused") || job.state == QStringLiteral("resuming")
        || job.state == QStringLiteral("finalizing");
}

int CatchupDownloadController::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_jobs.size());
}

QVariant CatchupDownloadController::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_jobs.size())
        return {};
    const auto &job = m_jobs[index.row()];
    switch (role) {
    case JobIdRole: return job.id;
    case TitleRole: return job.program.title;
    case ChannelRole: return job.channel.name;
    case ProgramStartRole: return job.program.start;
    case PathRole: return job.path;
    case StateRole: return job.state;
    case ProgressRole: return job.progress;
    case BytesRole: return job.bytes;
    case ErrorRole: return job.error;
    case ProgressKnownRole: return job.progressKnown;
    default: return {};
    }
}

QHash<int, QByteArray> CatchupDownloadController::roleNames() const
{
    return {{JobIdRole, "jobId"}, {TitleRole, "title"}, {ChannelRole, "channelName"},
            {PathRole, "destination"}, {StateRole, "jobState"}, {ProgressRole, "progress"},
            {BytesRole, "bytes"}, {ErrorRole, "errorText"}, {ProgressKnownRole, "progressKnown"}, {ProgramStartRole, "programStart"}};
}

bool CatchupDownloadController::hasPending() const
{
    return std::any_of(m_jobs.cbegin(), m_jobs.cend(), pending);
}

int CatchupDownloadController::queuedCount() const
{
    return static_cast<int>(std::count_if(m_jobs.cbegin(), m_jobs.cend(), [](const Job &job) {
        return job.state == QStringLiteral("queued");
    }));
}

double CatchupDownloadController::activeProgress() const
{
    return m_active >= 0 ? m_jobs[m_active].progress : 0;
}

bool CatchupDownloadController::activeProgressKnown() const
{
    return m_active >= 0 && m_jobs[m_active].progressKnown;
}

void CatchupDownloadController::pauseAll()
{
    if (m_paused || m_shuttingDown || !hasPending())
        return;
    m_paused = true;
    if (m_transferActive && m_active >= 0 && m_terminalState.isEmpty()) {
        m_jobs[m_active].state = QStringLiteral("paused");
        const auto token = ++m_transferToken;
        QMetaObject::invokeMethod(m_transfer, [transfer = m_transfer, token] { transfer->pause(token); });
    }
    publishActive();
}

void CatchupDownloadController::resumeAll()
{
    if (!m_paused || m_shuttingDown)
        return;
    m_paused = false;
    if (m_transferActive && m_active >= 0 && m_terminalState.isEmpty())
        startTransfer();
    else
        startNext();
    publishActive();
}

QString CatchupDownloadController::safeFileName(QString title)
{
    title.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")), QStringLiteral("_"));
    title = title.trimmed().left(160);
    while (title.endsWith(u'.') || title.endsWith(u' '))
        title.chop(1);
    static const QRegularExpression reserved(QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$))"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(title).hasMatch())
        title.prepend(u'_');
    return title.isEmpty() ? QStringLiteral("Programme") : title;
}

bool CatchupDownloadController::pathReserved(const QString &path) const
{
    const auto parent = QFileInfo(path).dir().canonicalPath();
    const auto normalized = QDir(parent).filePath(QFileInfo(path).fileName());
    return std::any_of(m_jobs.cbegin(), m_jobs.cend(), [&](const Job &job) {
        const auto other = QDir(QFileInfo(job.path).dir().canonicalPath()).filePath(QFileInfo(job.path).fileName());
#if defined(Q_OS_WIN)
        return pending(job) && normalized.compare(other, Qt::CaseInsensitive) == 0;
#else
        return pending(job) && normalized == other;
#endif
    });
}

QUrl CatchupDownloadController::suggestedDestination(const QString &title, const QString &channelName, const QDateTime &start)
{
    auto directory = m_settings->current().catchupDownloadDirectory;
    if (directory.isEmpty())
        directory = m_settings->current().recordingsDirectory;
    if (directory.isEmpty())
        directory = Core::AppDataPaths::recordingsDirectory();
    const auto &settings = m_settings->current();
    const auto format = Core::resolveDateTimeFormat(settings.dateOrder, settings.timeFormat);
    const auto name = safeFileName(start.isValid()
        ? QStringLiteral("%1 - %2 - %3").arg(channelName,
            Core::formatDisplayDateTime(start, format.dateTimePattern()),
            title.trimmed().isEmpty() ? QStringLiteral("Programme") : title)
        : title);
    auto path = QDir(directory).filePath(name + QStringLiteral(".mkv"));
    for (int suffix = 2; QFileInfo::exists(path) || pathReserved(path); ++suffix)
        path = QDir(directory).filePath(QStringLiteral("%1 (%2).mkv").arg(name).arg(suffix));
    return QUrl::fromLocalFile(path);
}

void CatchupDownloadController::chooseDestination(QObject *parentWindow, const QUrl &suggested)
{
    if (m_saveDialog || m_shuttingDown)
        return;
    // QFileDialog supports a suggested non-existent filename in both native and
    // fallback implementations. Qt Quick 6.10's fallback leaves that field empty.
    auto *dialog = new QFileDialog;
    m_saveDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Download programme"));
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setNameFilter(QStringLiteral("Matroska video (*.mkv)"));
    dialog->setDefaultSuffix(QStringLiteral("mkv"));
    dialog->setOption(QFileDialog::DontConfirmOverwrite);
    dialog->setDirectory(QFileInfo(suggested.toLocalFile()).absolutePath());
    dialog->selectFile(suggested.toLocalFile());
    dialog->setWindowModality(Qt::WindowModal);
    // A QQuickWindow cannot be a QWidget parent. Establish native transient
    // ownership explicitly so the picker stays above Windows overlays.
    dialog->winId();
    if (auto *window = qobject_cast<QWindow *>(parentWindow))
        dialog->windowHandle()->setTransientParent(window);
    connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
        m_saveDialog = nullptr;
        const auto files = dialog->selectedUrls();
        if (result == QDialog::Accepted && !files.isEmpty())
            emit destinationChosen(files.first());
        else
            emit destinationCancelled();
    });
    dialog->open();
}

void CatchupDownloadController::cancelDestination()
{
    if (m_saveDialog)
        m_saveDialog->reject();
}

QString CatchupDownloadController::enqueue(const Core::Channel &channel, const Core::EpgEntry &program,
                                         const QUrl &destination)
{
    if (m_shuttingDown)
        return QStringLiteral("Application is shutting down.");
    if (!destination.isLocalFile() || destination.toLocalFile().isEmpty())
        return QStringLiteral("Choose a local MKV file.");
    const auto path = QFileInfo(destination.toLocalFile()).absoluteFilePath();
    if (QFileInfo(path).suffix().compare(QStringLiteral("mkv"), Qt::CaseInsensitive) != 0)
        return QStringLiteral("The filename must end in .mkv.");
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink() || pathReserved(path))
        return QStringLiteral("This filename is already in use. Choose another filename.");
    if (!QFileInfo(QFileInfo(path).absolutePath()).isDir())
        return QStringLiteral("The destination directory does not exist.");
    Job job;
    job.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.channel = channel;
    job.program = program;
    job.path = path;
    job.destinationPath = path;
    m_settings->current().catchupDownloadDirectory = QFileInfo(path).absolutePath();
    m_settings->save();
    const auto row = rowCount();
    beginInsertRows({}, row, row);
    m_jobs.append(job);
    endInsertRows();
    emit summaryChanged();
    QTimer::singleShot(0, this, &CatchupDownloadController::startNext);
    return {};
}

void CatchupDownloadController::publishActive()
{
    if (m_active >= 0)
        emit dataChanged(index(m_active), index(m_active));
    emit summaryChanged();
}

void CatchupDownloadController::startNext()
{
    if (m_active >= 0 || m_shuttingDown || m_paused)
        return;
    for (int row = 0; row < rowCount(); ++row) {
        if (m_jobs[row].state == QStringLiteral("queued")) {
            m_active = row;
            break;
        }
    }
    if (m_active < 0)
        return;
    auto &job = m_jobs[m_active];
    job.state = QStringLiteral("downloading");
    publishActive();
    startTransfer();
}

void CatchupDownloadController::startTransfer()
{
    auto &job = m_jobs[m_active];
    job.error.clear();
    job.state = job.sourcePath.isEmpty() ? QStringLiteral("downloading") : QStringLiteral("resuming");
    publishActive();
    const auto profile = m_settings->profileById(job.channel.profileId);
    QString reason;
    const auto target = profile && m_settings->current().catchupEnabled && Core::ffmpegToolsAvailable()
        ? Core::CatchupUrlResolver(profile).resolveDownload(job.channel, job.program, &reason) : std::nullopt;
    if (!target) {
        stopActive(QStringLiteral("failed"), reason.isEmpty()
            ? QStringLiteral("Source, catch-up or ffmpeg/ffprobe is unavailable.") : reason);
        return;
    }
    if (!CatchupDownloadTransfer::supportedUrl(QUrl(target->url), true)) {
        stopActive(QStringLiteral("failed"), QStringLiteral("Downloads support HTTP/HTTPS media and finite HLS archives; DASH is not supported."));
        return;
    }
    if (QFileInfo::exists(job.path) || QFileInfo(job.path).isSymLink()) {
        stopActive(QStringLiteral("failed"), QStringLiteral("The destination already exists."));
        return;
    }
    if (job.sourcePath.isEmpty()) {
        QTemporaryFile source(QDir(QFileInfo(job.path).absolutePath()).filePath(QStringLiteral(".okiltv-download-XXXXXX.source")));
        if (!source.open()) {
            finish(QStringLiteral("failed"), QStringLiteral("Cannot create a file in the destination directory."));
            return;
        }
        job.sourcePath = source.fileName();
        source.setAutoRemove(false);
    }
    job.trimStartSeconds = target->trimStartSeconds;
    job.durationSeconds = target->durationSeconds;
    m_transferActive = true;
    const auto token = ++m_transferToken;
    const auto url = QUrl(target->url);
    const auto path = job.sourcePath;
    const auto agent = m_settings->current().playerUserAgent;
    DownloadTimeline timeline {target->durationSeconds, target->trimStartSeconds, {}};
    if (job.channel.source == Core::ChannelSource::Xtream) {
        const auto endSeconds = ((target->durationSeconds + 59) / 60 + 1) * 60;
        // Some providers ignore the finite URL's end and keep sending archive
        // data. Retain the programme, leading trim and two seconds for packet/
        // frame reordering, then close HTTP and verify the local remux normally.
        timeline.durationSeconds = target->durationSeconds + target->trimStartSeconds + 2;
        timeline.trimStartSeconds = 0;
        timeline.finishAtMediaEnd = true;
        timeline.resumeUrl = [url, endSeconds](qint64 offset) {
            return QUrl(Core::CatchupUrlResolver::xtreamWindowUrl(url.toString(), offset, endSeconds));
        };
    } else {
        const auto channel = job.channel;
        const auto program = job.program;
        timeline.resumeUrl = [profile, channel, program](qint64 offset) {
            const auto window = Core::CatchupUrlResolver(profile).resolveWindow(
                channel, program.start.addSecs(offset), program.stop);
            return window ? QUrl(window->url) : QUrl {};
        };
    }
    QMetaObject::invokeMethod(m_transfer, [transfer = m_transfer, token, url, path, agent, timeline] {
        transfer->start(token, url, path, agent, timeline);
    });
}

void CatchupDownloadController::finalizeDownload()
{
    auto &job = m_jobs[m_active];
    job.state = QStringLiteral("finalizing");
    job.progressKnown = true;
    job.progress = std::max(99.0, job.progress);
    QTemporaryFile temporary(QDir(QFileInfo(job.path).absolutePath()).filePath(QStringLiteral(".okiltv-download-XXXXXX.part")));
    if (!temporary.open()) {
        finish(QStringLiteral("failed"), QStringLiteral("Cannot create the MKV working file."));
        return;
    }
    job.temporaryPath = temporary.fileName();
    temporary.setAutoRemove(false);
    temporary.close();
    publishActive();
    QStringList args {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-nostats"),
        QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y"),
        QStringLiteral("-progress"), QStringLiteral("pipe:1"),
        QStringLiteral("-protocol_whitelist"), job.hls ? QStringLiteral("file,crypto") : QStringLiteral("file"),
        QStringLiteral("-format_whitelist"), (job.hls ? QStringLiteral("hls,") : QString())
            + QStringLiteral("mpegts,mpeg,mpegvideo,mov,matroska,webm,avi,aac,mp3,flac,ogg,wav,asf,webvtt")};
    if (job.hls)
        args << QStringLiteral("-f") << QStringLiteral("hls") << QStringLiteral("-allowed_extensions") << QStringLiteral("ALL");
    args << QStringList {QStringLiteral("-i"), job.sourcePath,
        QStringLiteral("-ss"), QString::number(job.trimStartSeconds),
        QStringLiteral("-t"), QString::number(job.durationSeconds),
        QStringLiteral("-map"), QStringLiteral("0:v?"), QStringLiteral("-map"), QStringLiteral("0:a?"),
        QStringLiteral("-map"), QStringLiteral("0:s?"), QStringLiteral("-c"), QStringLiteral("copy"),
        QStringLiteral("-avoid_negative_ts"), QStringLiteral("make_zero"),
        QStringLiteral("-map_metadata"), QStringLiteral("-1"),
        QStringLiteral("-metadata"), QStringLiteral("title=") + job.program.title,
        QStringLiteral("-f"), QStringLiteral("matroska"), job.temporaryPath};
    launch(Core::resolveProcessBinary(QStringLiteral("ffmpeg")), args, false);
}

void CatchupDownloadController::launch(const QString &binary, const QStringList &arguments, bool probing)
{
    m_probing = probing;
    m_stdout.clear();
    m_probeOutput.clear();
    m_process = new QProcess(this);
    auto *process = m_process;
#if defined(Q_OS_WIN)
    m_processJob = std::make_unique<Core::WindowsProcessJob>();
    if (!m_processJob->configure(*process)) {
        process->deleteLater();
        m_process = nullptr;
        finish(QStringLiteral("failed"), QStringLiteral("Cannot safely start the download process."));
        return;
    }
#endif
    connect(process, &QProcess::readyReadStandardOutput, this, &CatchupDownloadController::readProgress);
    // Drain diagnostics without exposing provider URLs or arbitrary metadata to
    // QML/logs. User-facing failures use application-owned messages.
    connect(process, &QProcess::readyReadStandardError, this, [process] { process->readAllStandardError(); });
    connect(process, &QProcess::finished, this, &CatchupDownloadController::processFinished);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            processFinished(-1, QProcess::CrashExit);
    });
    m_lastProgress.restart();
    m_watchdog.start();
    connect(process, &QProcess::started, this, [this, process] {
        if (!m_terminalState.isEmpty() && process == m_process)
            process->kill();
    });
    process->start(binary, arguments);
}

void CatchupDownloadController::readProgress()
{
    if (!m_process || m_active < 0)
        return;
    const auto bytes = m_process->readAllStandardOutput();
    if (m_probing) {
        if (m_probeOutput.size() + bytes.size() <= 1024LL * 1024)
            m_probeOutput.append(bytes);
        else
            stopActive(QStringLiteral("failed"), QStringLiteral("Invalid media inspection output."));
        return;
    }
    m_stdout.append(bytes);
    qsizetype newline = -1;
    while ((newline = m_stdout.indexOf('\n')) >= 0) {
        const auto line = m_stdout.left(newline).trimmed();
        m_stdout.remove(0, newline + 1);
        if (!line.startsWith("out_time_us="))
            continue;
        bool ok = false;
        const auto micros = line.mid(12).toLongLong(&ok);
        const auto duration = m_jobs[m_active].program.start.secsTo(m_jobs[m_active].program.stop);
        if (!ok || duration <= 0)
            continue;
        // Reserve 100% for successful verification and publication.
        const double progress = 99.0 + std::clamp(static_cast<double>(micros) / (static_cast<double>(duration) * 1000000.0), 0.0, 1.0) * 0.9;
        if (progress > m_jobs[m_active].progress) {
            m_jobs[m_active].progress = progress;
            m_lastProgress.restart();
            publishActive();
        }
    }
    if (m_stdout.size() > 65536)
        m_stdout.clear();
}

void CatchupDownloadController::processFinished(int code, QProcess::ExitStatus status)
{
    if (!m_process || m_active < 0)
        return;
    readProgress();
    m_watchdog.stop();
    auto *process = m_process;
    process->disconnect(this);
    process->deleteLater();
    m_process = nullptr;
#if defined(Q_OS_WIN)
    // Closing the job also retires descendants before releasing temporary files.
    m_processJob.reset();
#endif
    if (!m_terminalState.isEmpty()) {
        cleanupAndFinish();
        return;
    }
    if (code != 0 || status != QProcess::NormalExit) {
        finish(QStringLiteral("failed"), QStringLiteral("Download failed. Check the connection, archive availability, disk space and MKV codec support."));
        return;
    }
    auto &job = m_jobs[m_active];
    if (!m_probing) {
        job.state = QStringLiteral("verifying");
        job.bytes = QFileInfo(job.temporaryPath).size();
        publishActive();
        launch(Core::resolveProcessBinary(QStringLiteral("ffprobe")),
            {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
             QStringLiteral("format=duration:stream=codec_type"), QStringLiteral("-of"), QStringLiteral("json"), job.temporaryPath}, true);
        return;
    }
    const auto object = QJsonDocument::fromJson(m_probeOutput).object();
    bool durationOk = false;
    const double duration = object.value(QStringLiteral("format")).toObject().value(QStringLiteral("duration")).toString().toDouble(&durationOk);
    bool hasMedia = false;
    for (const auto &stream : object.value(QStringLiteral("streams")).toArray()) {
        const auto type = stream.toObject().value(QStringLiteral("codec_type")).toString();
        hasMedia |= type == QStringLiteral("video") || type == QStringLiteral("audio");
    }
    const auto expected = static_cast<double>(job.program.start.secsTo(job.program.stop));
    if (!durationOk || !std::isfinite(duration) || duration <= 0 || !hasMedia || job.bytes <= 0
        || std::abs(duration - expected) > 5.0) {
        finish(QStringLiteral("failed"), QStringLiteral("The provider returned an incomplete or invalid programme."));
        return;
    }
    if (!QFile::rename(job.temporaryPath, job.path)) {
        finish(QStringLiteral("failed"), QStringLiteral("Cannot save the completed file. The destination may already exist or be inaccessible."));
        return;
    }
    job.temporaryPath.clear();
    finish(QStringLiteral("completed"));
}

void CatchupDownloadController::stopActive(const QString &state, const QString &reason)
{
    if (m_active < 0)
        return;
    if (state == QStringLiteral("cancelled")) {
        m_jobs[m_active].discardPartial = true;
        if (!m_terminalState.isEmpty()) {
            m_terminalState = state;
            m_terminalReason.clear();
            return;
        }
    }
    if (!m_terminalState.isEmpty())
        return;
    m_terminalState = state;
    m_terminalReason = reason;
    m_jobs[m_active].state = QStringLiteral("stopping");
    publishActive();
    m_watchdog.stop();
    if (m_transferActive) {
        const auto token = ++m_transferToken;
        QMetaObject::invokeMethod(m_transfer, [transfer = m_transfer, token] { transfer->stop(token); });
        return;
    }
    if (!m_process) {
        cleanupAndFinish();
        return;
    }
    if (m_process->state() == QProcess::NotRunning) {
        processFinished(-1, QProcess::CrashExit);
        return;
    }
    m_process->terminate();
    const QPointer<QProcess> process(m_process);
    QTimer::singleShot(1500, this, [this, process] {
        if (process && process == m_process && process->state() != QProcess::NotRunning) {
#if defined(Q_OS_WIN)
            if (m_processJob)
                m_processJob->terminate();
#endif
            process->kill();
        }
    });
}

void CatchupDownloadController::finish(const QString &state, const QString &reason)
{
    m_terminalState = state;
    m_terminalReason = reason;
    cleanupAndFinish();
}

bool CatchupDownloadController::preservePartial(Job &job)
{
    const QFileInfo destination(job.path);
    const auto base = destination.completeBaseName();
    auto preserve = [&](QString &working, const QString &extension) {
        if (working.isEmpty() || !QFileInfo::exists(working))
            return true;
        auto partial = destination.dir().filePath(base + QStringLiteral(".partial.") + extension);
        for (int suffix = 2; QFileInfo::exists(partial) || QFileInfo(partial).isSymLink() || pathReserved(partial); ++suffix)
            partial = destination.dir().filePath(QStringLiteral("%1.partial (%2).%3").arg(base).arg(suffix).arg(extension));
        if (!QFile::rename(working, partial))
            return false;
        job.path = partial;
        job.bytes = QFileInfo(partial).size();
        m_terminalReason += QStringLiteral(" Incomplete data retained at: %1").arg(partial);
        working.clear();
        return true;
    };
    job.hls |= !job.sourcePath.isEmpty() && QDir(job.sourcePath + QStringLiteral(".hls")).exists();
    if (job.hls && !job.sourcePath.isEmpty()) {
        QDir graph(job.sourcePath + QStringLiteral(".hls"));
        if (graph.exists() && !graph.removeRecursively())
            return false;
        if (QFileInfo::exists(job.sourcePath) && !QFile::remove(job.sourcePath))
            return false;
        job.sourcePath.clear();
    }
    // Keep the MKV as the primary displayed path when both files exist.
    const bool sourcePreserved = preserve(job.sourcePath, QStringLiteral("download"));
    const bool mkvPreserved = preserve(job.temporaryPath, QStringLiteral("mkv"));
    return sourcePreserved && mkvPreserved;
}

void CatchupDownloadController::cleanupAndFinish(int attempt)
{
    if (m_active < 0)
        return;
    auto &job = m_jobs[m_active];
    const bool removeFiles = job.discardPartial || m_terminalState == QStringLiteral("completed");
    auto remove = [](QString &path) {
        if (path.isEmpty())
            return true;
        if (QFileInfo::exists(path) && !QFile::remove(path))
            return false;
        path.clear();
        return true;
    };
    bool settled = false;
    if (removeFiles) {
        const bool graphRemoved = job.sourcePath.isEmpty()
            || !QDir(job.sourcePath + QStringLiteral(".hls")).exists()
            || QDir(job.sourcePath + QStringLiteral(".hls")).removeRecursively();
        const bool sourceRemoved = graphRemoved && remove(job.sourcePath);
        const bool mkvRemoved = remove(job.temporaryPath);
        settled = sourceRemoved && mkvRemoved;
    } else {
        settled = preservePartial(job);
    }
    if (!settled) {
        job.state = QStringLiteral("stopping");
        publishActive();
        if (attempt < 10) {
            QTimer::singleShot(500, this, [this, attempt] { cleanupAndFinish(attempt + 1); });
            return;
        }
        // A verified published MKV remains successful even if source cleanup
        // failed; report the retained working path instead of losing it.
        if (m_terminalState != QStringLiteral("completed"))
            m_terminalState = QStringLiteral("failed");
        for (const auto &path : {job.sourcePath, job.temporaryPath}) {
            if (path.isEmpty())
                continue;
            m_terminalReason += QStringLiteral(" Working file retained at: %1").arg(path);
            if (!removeFiles) {
                job.path = path;
                job.bytes = QFileInfo(path).size();
            }
        }
    }
    job.state = m_terminalState;
    job.error = m_terminalReason;
    if (job.state == QStringLiteral("completed"))
        job.progress = 100;
    m_unread = true;
    publishActive();
    const auto message = job.state == QStringLiteral("completed")
        ? QStringLiteral("Download complete: %1").arg(job.program.title)
        : (job.state == QStringLiteral("failed") ? QStringLiteral("Download failed: %1. %2").arg(job.program.title, job.error) : QString());
    m_active = -1;
    m_terminalState.clear();
    m_terminalReason.clear();
    if (!hasPending())
        m_paused = false;
    emit summaryChanged();
    if (!message.isEmpty())
        emit notification(message);
    if (m_shuttingDown)
        emit shutdownFinished();
    else
        QTimer::singleShot(0, this, &CatchupDownloadController::startNext);
}

void CatchupDownloadController::cancel(const QString &jobId)
{
    for (int row = 0; row < rowCount(); ++row) {
        auto &job = m_jobs[row];
        if (job.id != jobId || !pending(job))
            continue;
        if (row == m_active) {
            stopActive(QStringLiteral("cancelled"));
        } else {
            job.state = QStringLiteral("cancelled");
            if (!hasPending())
                m_paused = false;
            emit dataChanged(index(row), index(row));
            emit summaryChanged();
        }
        return;
    }
}

void CatchupDownloadController::restart(const QString &jobId)
{
    for (const auto &job : std::as_const(m_jobs)) {
        if (job.id != jobId || (job.state != QStringLiteral("cancelled") && job.state != QStringLiteral("failed")))
            continue;
        // Enqueue can reallocate the model. Copy the original target first and
        // use the normal FIFO/validation flow with fresh progress and media.
        const auto channel = job.channel;
        const auto program = job.program;
        const auto destination = QUrl::fromLocalFile(job.destinationPath);
        const auto originalId = job.id;
        const auto reason = enqueue(channel, program, destination);
        if (!reason.isEmpty()) {
            emit notification(reason);
            return;
        }
        dismiss(originalId);
        return;
    }
}

void CatchupDownloadController::dismiss(const QString &jobId)
{
    for (int row = 0; row < rowCount(); ++row) {
        if (m_jobs[row].id != jobId || pending(m_jobs[row]))
            continue;
        // Dismissal only removes history. Retry deletion solely for jobs that
        // the user cancelled; failed downloads always keep their data.
        for (const auto &temporary : {m_jobs[row].sourcePath, m_jobs[row].temporaryPath}) {
            if (m_jobs[row].discardPartial && !temporary.isEmpty()
                && QFileInfo::exists(temporary) && !QFile::remove(temporary)) {
                emit notification(QStringLiteral("Cannot remove temporary file: %1").arg(temporary));
                return;
            }
        }
        beginRemoveRows({}, row, row);
        m_jobs.removeAt(row);
        if (m_active > row)
            --m_active;
        endRemoveRows();
        emit summaryChanged();
        return;
    }
}

void CatchupDownloadController::markRead()
{
    m_unread = false;
    emit summaryChanged();
}

void CatchupDownloadController::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    for (int row = 0; row < rowCount(); ++row) {
        if (m_jobs[row].state == QStringLiteral("queued")) {
            m_jobs[row].state = QStringLiteral("cancelled");
            emit dataChanged(index(row), index(row));
        }
    }
    emit summaryChanged();
    if (m_active >= 0)
        stopActive(QStringLiteral("cancelled"));
    else
        emit shutdownFinished();
}

} // namespace OKILTV::App
