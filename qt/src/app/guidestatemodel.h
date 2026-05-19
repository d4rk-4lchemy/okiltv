#pragma once

#include "../core/epgservice.h"
#include "../core/settingsmanager.h"

#include <QFutureSynchronizer>
#include <QObject>
#include <QVariantList>

#include <optional>

namespace OKILTV::App {

class GuideStateModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString selectedGroupId READ selectedGroupId WRITE setSelectedGroupId NOTIFY selectedGroupIdChanged)
    Q_PROPERTY(int selectedChannelId READ selectedChannelId NOTIFY selectedChannelIdChanged)
    Q_PROPERTY(QVariantMap selectedChannel READ selectedChannel NOTIFY selectedChannelChanged)
    Q_PROPERTY(QVariantMap selectedProgram READ selectedProgram NOTIFY selectedProgramChanged)
    Q_PROPERTY(QVariantList channelPrograms READ channelPrograms NOTIFY channelProgramsChanged)
    Q_PROPERTY(bool previewEnabled READ previewEnabled WRITE setPreviewEnabled NOTIFY previewEnabledChanged)
    Q_PROPERTY(bool detailsExpanded READ detailsExpanded WRITE setDetailsExpanded NOTIFY detailsExpandedChanged)

public:
    ~GuideStateModel() override;

    GuideStateModel(Core::EpgService *epg, Core::SettingsManager *settings, QObject *parent = nullptr);

    QString selectedGroupId() const;
    int selectedChannelId() const;
    QVariantMap selectedChannel() const;
    QVariantMap selectedProgram() const;
    QVariantList channelPrograms() const;
    bool previewEnabled() const;
    bool detailsExpanded() const;

    void setChannels(const QList<Core::Channel> &channels);

    Q_INVOKABLE void selectChannel(int channelId);
    Q_INVOKABLE void selectProgram(const QVariantMap &program);
    Q_INVOKABLE void selectProgramByStart(const QString &startIso);

public slots:
    void setSelectedGroupId(const QString &value);
    void setPreviewEnabled(bool value);
    void setDetailsExpanded(bool value);
    void refresh();
    void clear();

signals:
    void selectedGroupIdChanged();
    void selectedChannelIdChanged();
    void selectedChannelChanged();
    void selectedProgramChanged();
    void channelProgramsChanged();
    void previewEnabledChanged();
    void detailsExpandedChanged();

private:
    struct ProgramsUpdate
    {
        std::optional<Core::EpgEntry> selectedProgram;
        QList<Core::EpgEntry> channelPrograms;
        QVariantMap selectedProgramVariant;
        QVariantList channelProgramsVariant;
    };

    void startProgramsUpdateJob(quint64 generation, const Core::Channel &selectedChannel, int lookAheadHours);
    void updatePrograms();
    void applyProgramsUpdate(quint64 generation, ProgramsUpdate result);

    Core::EpgService *m_epg;
    Core::SettingsManager *m_settings;
    QList<Core::Channel> m_channels;
    QString m_selectedGroupId;
    std::optional<Core::Channel> m_selectedChannel;
    std::optional<Core::EpgEntry> m_selectedProgram;
    QList<Core::EpgEntry> m_channelPrograms;
    QVariantMap m_selectedProgramVariant;
    QVariantList m_channelProgramsVariant;
    QString m_preferredProgramStart;
    bool m_previewEnabled { true };
    bool m_detailsExpanded { true };
    QFutureSynchronizer<void> m_backgroundTasks;
    bool m_updateInFlight { false };
    bool m_updateQueued { false };
    quint64 m_queuedGeneration { 0 };
    std::optional<Core::Channel> m_queuedChannel;
    int m_queuedLookAheadHours { 24 };
    quint64 m_updateGeneration { 0 };
};

} // namespace OKILTV::App
