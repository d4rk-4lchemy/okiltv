#pragma once

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <functional>

namespace OKILTV::Core {

class DebugLogger final
{
public:
    struct Entry
    {
        quint64 cursor { 0 };
        QString timestampUtc;
        QString category;
        QString message;
        QString line;
    };

    using Subscriber = std::function<void(const Entry &)>;

    static DebugLogger &instance();

    void startSessionLogging();
    QString sessionLogPath() const;
    void log(QStringView category, const QString &message);
    QString writeDump(const QString &summary) const;
    QList<Entry> entriesSince(quint64 cursor) const;
    quint64 latestCursor() const;
    quint64 subscribe(const Subscriber &subscriber);
    void unsubscribe(quint64 subscriberId);

private:
    DebugLogger() = default;

    static void qtMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message);
    void configureSessionMirrorLocked();
    void appendToSessionMirrorLocked(const QString &entry);

    QStringList snapshotEntries() const;

    mutable QMutex m_mutex;
    QStringList m_entries;
    QList<Entry> m_structuredEntries;
    QHash<quint64, Subscriber> m_subscribers;
    QString m_sessionLogPath;
    QtMessageHandler m_previousQtMessageHandler = nullptr;
    bool m_messageHandlerInstalled { false };
    bool m_sessionMirrorConfigured { false };
    quint64 m_nextCursor { 1 };
    quint64 m_nextSubscriberId { 1 };
};

} // namespace OKILTV::Core
