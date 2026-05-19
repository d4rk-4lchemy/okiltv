#pragma once

#include "../core/epgservice.h"

#include <QFutureSynchronizer>
#include <QHash>
#include <QObject>
#include <QVariantList>

#include <optional>

namespace OKILTV::App {

class NowNextModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString channelName READ channelName NOTIFY channelChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY dataChanged)
    Q_PROPERTY(QVariantMap currentProgram READ currentProgram NOTIFY dataChanged)
    Q_PROPERTY(QVariantMap nextProgram READ nextProgram NOTIFY dataChanged)
    Q_PROPERTY(QVariantList upcomingPrograms READ upcomingPrograms NOTIFY dataChanged)

public:
    ~NowNextModel() override;

    explicit NowNextModel(Core::EpgService *epg, QObject *parent = nullptr);

    QString channelName() const;
    bool loading() const;
    QVariantMap currentProgram() const;
    QVariantMap nextProgram() const;
    QVariantList upcomingPrograms() const;

    void setChannel(const std::optional<Core::Channel> &channel);

public slots:
    void refresh();
    void clear();

signals:
    void channelChanged();
    void dataChanged();

private:
    struct RefreshResult
    {
        std::optional<Core::EpgEntry> currentProgram;
        std::optional<Core::EpgEntry> nextProgram;
        QList<Core::EpgEntry> upcomingPrograms;
        QVariantMap currentProgramVariant;
        QVariantMap nextProgramVariant;
        QVariantList upcomingProgramsVariant;
    };

    struct CachedResult
    {
        QVariantMap currentProgramVariant;
        QVariantMap nextProgramVariant;
        QVariantList upcomingProgramsVariant;
    };

    void startRefreshJob(quint64 generation, const Core::Channel &channel);
    void applyRefreshResult(quint64 generation, RefreshResult result);

    Core::EpgService *m_epg;
    std::optional<Core::Channel> m_channel;
    std::optional<Core::EpgEntry> m_currentProgram;
    std::optional<Core::EpgEntry> m_nextProgram;
    QList<Core::EpgEntry> m_upcomingPrograms;
    QVariantMap m_currentProgramVariant;
    QVariantMap m_nextProgramVariant;
    QVariantList m_upcomingProgramsVariant;
    bool m_loading { false };
    bool m_skipNextLoadingState { false };
    QHash<QString, CachedResult> m_resultCache;
    QFutureSynchronizer<void> m_backgroundTasks;
    quint64 m_refreshGeneration { 0 };
    bool m_refreshInFlight { false };
    bool m_refreshQueued { false };
    quint64 m_queuedGeneration { 0 };
    std::optional<Core::Channel> m_queuedChannel;
};

} // namespace OKILTV::App
