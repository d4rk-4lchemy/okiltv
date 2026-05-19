#include "debuglogger.h"

#include "appdatapaths.h"
#include "redaction.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSaveFile>
#include <QTextStream>
#include <QtGlobal>

#include <cstdio>

namespace OKILTV::Core {

namespace {

constexpr int kMaxLogEntries = 4000;

QString dumpDirectory()
{
    return AppDataPaths::debugDumpDirectory();
}

QString timestampUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString qtLogCategory(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("qt-debug");
    case QtInfoMsg:
        return QStringLiteral("qt-info");
    case QtWarningMsg:
        return QStringLiteral("qt-warning");
    case QtCriticalMsg:
        return QStringLiteral("qt-critical");
    case QtFatalMsg:
        return QStringLiteral("qt-fatal");
    }

    return QStringLiteral("qt");
}

QString formatQtMessage(const QMessageLogContext &context, const QString &message)
{
    QStringList metadata;
    if (context.category != nullptr && *context.category != '\0') {
        metadata.push_back(QStringLiteral("category=%1").arg(QString::fromUtf8(context.category)));
    }
    if (context.file != nullptr && *context.file != '\0') {
        if (context.line > 0) {
            metadata.push_back(
                QStringLiteral("file=%1:%2").arg(QString::fromUtf8(context.file)).arg(context.line));
        } else {
            metadata.push_back(QStringLiteral("file=%1").arg(QString::fromUtf8(context.file)));
        }
    }
    if (context.function != nullptr && *context.function != '\0') {
        metadata.push_back(QStringLiteral("function=%1").arg(QString::fromUtf8(context.function)));
    }

    auto trimmedMessage = message.trimmed();
    if (metadata.isEmpty()) {
        return trimmedMessage;
    }

    return QStringLiteral("%1 (%2)").arg(trimmedMessage, metadata.join(QStringLiteral(", ")));
}

} // namespace

DebugLogger &DebugLogger::instance()
{
    static DebugLogger logger;
    return logger;
}

void DebugLogger::startSessionLogging()
{
    QMutexLocker locker(&m_mutex);
    configureSessionMirrorLocked();
    if (!m_messageHandlerInstalled) {
        m_previousQtMessageHandler = qInstallMessageHandler(&DebugLogger::qtMessageHandler);
        m_messageHandlerInstalled = true;
    }
}

QString DebugLogger::sessionLogPath() const
{
    QMutexLocker locker(&m_mutex);
    if (m_sessionLogPath.isEmpty()) {
        return {};
    }
    return QDir::toNativeSeparators(m_sessionLogPath);
}

void DebugLogger::log(QStringView category, const QString &message)
{
    const auto normalizedCategory = QString(category).trimmed();
    auto normalizedMessage = QString(message).trimmed().replace(u'\n', QStringLiteral("\\n"));
    normalizedMessage = redactSensitiveText(normalizedMessage);
    const auto timestamp = timestampUtc();
    const auto line = QStringLiteral("[%1] [%2] %3").arg(timestamp, normalizedCategory, normalizedMessage);
    static const bool kMirrorToStderr = qEnvironmentVariableIsSet("OKILTV_DEBUG_STDERR");

    QList<Subscriber> subscribers;
    Entry structuredEntry;

    QMutexLocker locker(&m_mutex);
    configureSessionMirrorLocked();
    structuredEntry.cursor = m_nextCursor++;
    structuredEntry.timestampUtc = timestamp;
    structuredEntry.category = normalizedCategory;
    structuredEntry.message = normalizedMessage;
    structuredEntry.line = line;

    m_entries.push_back(line);
    m_structuredEntries.push_back(structuredEntry);
    while (m_entries.size() > kMaxLogEntries) {
        m_entries.pop_front();
    }
    while (m_structuredEntries.size() > kMaxLogEntries) {
        m_structuredEntries.pop_front();
    }
    appendToSessionMirrorLocked(line);
    subscribers = m_subscribers.values();

    if (kMirrorToStderr) {
        const auto utf8 = line.toUtf8();
        std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stderr);
        std::fwrite("\n", 1, 1, stderr);
        std::fflush(stderr);
    }

    locker.unlock();
    for (const auto &subscriber : subscribers) {
        if (subscriber) {
            subscriber(structuredEntry);
        }
    }
}

QString DebugLogger::writeDump(const QString &summary) const
{
    const auto fileName =
        QStringLiteral("iptvplayer-debug-%1.txt").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss")));
    auto filePath = QDir(dumpDirectory()).filePath(fileName);

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    stream << "OKILTV Qt Debug Dump\n";
    stream << "Generated UTC: " << timestampUtc() << "\n\n";
    stream << "Session Log: "
           << (sessionLogPath().isEmpty() ? QStringLiteral("<disabled>") : sessionLogPath())
           << "\n\n";
    stream << "=== Runtime Summary ===\n";
    stream << redactSensitiveText(summary.trimmed()) << "\n\n";
    stream << "=== Recent Log Entries ===\n";

    for (const auto &entry : snapshotEntries()) {
        stream << entry << '\n';
    }

    if (!file.commit()) {
        return {};
    }

    return filePath;
}

QStringList DebugLogger::snapshotEntries() const
{
    QMutexLocker locker(&m_mutex);
    return m_entries;
}

QList<DebugLogger::Entry> DebugLogger::entriesSince(const quint64 cursor) const
{
    QMutexLocker locker(&m_mutex);
    QList<Entry> entries;
    entries.reserve(m_structuredEntries.size());
    for (const auto &entry : m_structuredEntries) {
        if (entry.cursor > cursor) {
            entries.push_back(entry);
        }
    }
    return entries;
}

quint64 DebugLogger::latestCursor() const
{
    QMutexLocker locker(&m_mutex);
    if (m_structuredEntries.isEmpty()) {
        return 0;
    }
    return m_structuredEntries.constLast().cursor;
}

quint64 DebugLogger::subscribe(const Subscriber &subscriber)
{
    if (!subscriber) {
        return 0;
    }

    QMutexLocker locker(&m_mutex);
    const auto subscriberId = m_nextSubscriberId++;
    m_subscribers.insert(subscriberId, subscriber);
    return subscriberId;
}

void DebugLogger::unsubscribe(const quint64 subscriberId)
{
    if (subscriberId == 0) {
        return;
    }

    QMutexLocker locker(&m_mutex);
    m_subscribers.remove(subscriberId);
}

void DebugLogger::configureSessionMirrorLocked()
{
    if (m_sessionMirrorConfigured) {
        return;
    }
    m_sessionMirrorConfigured = true;

    const auto configuredPath = qEnvironmentVariable("OKILTV_DEBUG_FILE").trimmed();
    const auto stderrMirrorEnabled = qEnvironmentVariableIsSet("OKILTV_DEBUG_STDERR");
    if (configuredPath.isEmpty() && !stderrMirrorEnabled) {
        return;
    }

    QString resolvedPath = configuredPath;
    if (resolvedPath.isEmpty()) {
        resolvedPath = QDir(dumpDirectory()).filePath(
            QStringLiteral("iptvplayer-session-%1.log")
                .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss"))));
    }

    const QFileInfo fileInfo(resolvedPath);
    if (!QDir().mkpath(fileInfo.absolutePath())) {
        return;
    }

    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        return;
    }

    m_sessionLogPath = fileInfo.absoluteFilePath();
    QTextStream stream(&file);
    stream << '[' << timestampUtc() << "] [logger] Session log started.\n";
    file.flush();
}

void DebugLogger::appendToSessionMirrorLocked(const QString &entry)
{
    if (m_sessionLogPath.isEmpty()) {
        return;
    }

    QFile file(m_sessionLogPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append)) {
        return;
    }

    QTextStream stream(&file);
    stream << entry << '\n';
    file.flush();
}

void DebugLogger::qtMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    thread_local bool inHandler = false;
    auto &logger = DebugLogger::instance();
    const auto previousHandler = logger.m_previousQtMessageHandler;

    if (!inHandler) {
        inHandler = true;
        logger.log(qtLogCategory(type), formatQtMessage(context, message));
        inHandler = false;
    }

    if (previousHandler != nullptr) {
        previousHandler(type, context, message);
        return;
    }

    if (type == QtFatalMsg) {
        abort();
    }
}

} // namespace OKILTV::Core
