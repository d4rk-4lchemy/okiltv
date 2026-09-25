#pragma once

#include "../core/epgservice.h"
#include "../core/settingsmanager.h"

#include <QFutureSynchronizer>
#include <QHash>
#include <QObject>
#include <QVariantList>

#include <optional>

namespace OKILTV::App {

class NowNextModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap channel READ channel NOTIFY channelChanged)
    Q_PROPERTY(QVariantList pastPrograms READ pastPrograms NOTIFY dataChanged)
    Q_PROPERTY(QString channelName READ channelName NOTIFY channelChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY dataChanged)
    Q_PROPERTY(QVariantMap currentProgram READ currentProgram NOTIFY dataChanged)
    Q_PROPERTY(QVariantMap nextProgram READ nextProgram NOTIFY dataChanged)
    Q_PROPERTY(QVariantList upcomingPrograms READ upcomingPrograms NOTIFY dataChanged)

public:
    ~NowNextModel() override;

    NowNextModel(Core::EpgService *epg, Core::SettingsManager *settings, QObject *parent = nullptr);

    QVariantMap channel() const;
    QVariantList pastPrograms() const;
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
        QVariantList pastProgramsVariant;
    };

    void startRefreshJob(quint64 generation, const Core::Channel &channel, int lookAheadHours);
    void applyRefreshResult(quint64 generation, RefreshResult result);

    Core::EpgService *m_epg;
    Core::SettingsManager *m_settings;
    std::optional<Core::Channel> m_channel;
    std::optional<Core::EpgEntry> m_currentProgram;
    std::optional<Core::EpgEntry> m_nextProgram;
    QList<Core::EpgEntry> m_upcomingPrograms;
    QVariantMap m_currentProgramVariant;
    QVariantMap m_nextProgramVariant;
    QVariantList m_upcomingProgramsVariant;
    QVariantList m_pastProgramsVariant;
    bool m_loading { false };
    bool m_skipNextLoadingState { false };
    QFutureSynchronizer<void> m_backgroundTasks;
    quint64 m_refreshGeneration { 0 };
    bool m_refreshInFlight { false };
    bool m_refreshQueued { false };
    quint64 m_queuedGeneration { 0 };
    std::optional<Core::Channel> m_queuedChannel;
    int m_queuedLookAheadHours { 24 };
};

} // namespace OKILTV::App
