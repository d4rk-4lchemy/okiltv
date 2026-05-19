#pragma once

#include "models.h"

#include <QDateTime>
#include <QHash>
#include <QIODevice>
#include <QList>
#include <QReadWriteLock>

#include <memory>
#include <optional>

class QXmlStreamReader;

namespace OKILTV::Core {

class EpgService
{
public:
    struct Snapshot
    {
        QHash<QString, QList<EpgEntry>> index;
        QHash<QString, QDateTime> maxStopByChannelId;
        QList<EpgEntry> allEntries;
        int totalEntries { 0 };
    };

    static QList<EpgEntry> parseEntries(const QByteArray &payload);
    static QList<EpgEntry> parseEntries(QIODevice *device);
    static Snapshot buildSnapshot(const QList<EpgEntry> &entries);

    void loadFromBytes(const QByteArray &payload);
    void loadFromEntries(const QList<EpgEntry> &entries);
    void applySnapshot(Snapshot snapshot);
    void applySnapshot(std::shared_ptr<const Snapshot> snapshot);
    void clear();

    std::optional<EpgEntry> currentProgram(const QString &tvgId) const;
    std::optional<EpgEntry> nextProgram(const QString &tvgId) const;
    QList<EpgEntry> programsInRange(const QString &tvgId, const QDateTime &from, const QDateTime &to) const;
    QDateTime channelMaxStop(const QString &tvgId) const;
    QList<EpgEntry> allEntries() const;

    int totalEntries() const;

private:
    static std::optional<EpgEntry> parseProgramme(QXmlStreamReader &reader);
    static bool tryParseDate(const QString &value, QDateTime *result);
    static QString normalizeKey(const QString &channelId);

    mutable QReadWriteLock m_lock;
    std::shared_ptr<const Snapshot> m_snapshot { std::make_shared<Snapshot>() };
};

} // namespace OKILTV::Core
