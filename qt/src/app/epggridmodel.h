#pragma once

#include "../core/epgservice.h"

#include <QAbstractListModel>
#include <QFutureSynchronizer>
#include <QHash>

namespace OKILTV::App {

class EpgGridModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QVariantList timeSlots READ timeSlots NOTIFY timeSlotsChanged)
    Q_PROPERTY(QVariantList visibleTimeSlots READ visibleTimeSlots NOTIFY visibleTimeSlotsChanged)
    Q_PROPERTY(QString windowStartLabel READ windowStartLabel NOTIFY windowChanged)
    Q_PROPERTY(QString windowEndLabel READ windowEndLabel NOTIFY windowChanged)
    Q_PROPERTY(int windowSpanMinutes READ windowSpanMinutes NOTIFY windowChanged)
    Q_PROPERTY(int selectedChannelId READ selectedChannelId WRITE setSelectedChannelId NOTIFY selectedChannelIdChanged)
    Q_PROPERTY(QString selectedProgramStart READ selectedProgramStart WRITE setSelectedProgramStart NOTIFY selectedProgramStartChanged)
    Q_PROPERTY(QVariantMap selectedProgram READ selectedProgram NOTIFY selectedProgramChanged)
    Q_PROPERTY(double currentTimeOffsetMinutes READ currentTimeOffsetMinutes NOTIFY timeSlotsChanged)

public:
    ~EpgGridModel() override;

    enum Roles
    {
        ChannelIdRole = Qt::UserRole + 1,
        ChannelNameRole,
        ChannelIconPathRole,
        ChannelTvgIdRole,
        ChannelProfileIdRole,
        ChannelStreamUrlRole,
        ProgramsRole
    };
    Q_ENUM(Roles)

    explicit EpgGridModel(Core::EpgService *epg, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QVariantList timeSlots() const;
    QVariantList visibleTimeSlots() const;
    QString windowStartLabel() const;
    QString windowEndLabel() const;
    int windowSpanMinutes() const;
    int selectedChannelId() const;
    void setSelectedChannelId(int value);
    QString selectedProgramStart() const;
    void setSelectedProgramStart(const QString &value);
    QVariantMap selectedProgram() const;
    double currentTimeOffsetMinutes() const;
    int guidePastHours() const;
    int lookAheadHours() const;

    void rebuild(const QList<Core::Channel> &channels, int guidePastHours, int lookAheadHours);
    void rebuildAsync(const QList<Core::Channel> &channels, int guidePastHours, int lookAheadHours);
    Q_INVOKABLE int channelIdAt(int row) const;
    Q_INVOKABLE int rowIndexForChannelId(int channelId) const;
    Q_INVOKABLE int adjacentChannelId(int channelId, int delta) const;
    Q_INVOKABLE QVariantMap adjacentProgram(int channelId, const QString &currentStartIso, int delta) const;
    Q_INVOKABLE QVariantMap programForChannelAtTimestamp(int channelId, const QString &timestampIso) const;
    Q_INVOKABLE void setRenderViewport(double startMinutes, double durationMinutes);
    Q_INVOKABLE void setVisibleRowRange(int firstRow, int lastRow);

signals:
    void timeSlotsChanged();
    void visibleTimeSlotsChanged();
    void windowChanged();
    void selectedChannelIdChanged();
    void selectedProgramStartChanged();
    void selectedProgramChanged();

private:
    struct Row
    {
        Core::Channel channel;
    };

    QVariantList computeTimeSlots() const;
    QVariantList computeVisibleTimeSlots() const;
    QVariantList buildPrograms(int rowIndex, const Row &row) const;
    QVariantMap findSelectedProgram() const;
    QList<Core::EpgEntry> channelProgramsInRange(int channelId, const QDateTime &from, const QDateTime &to) const;
    QList<Core::EpgEntry> channelProgramsInWindow(int channelId) const;
    void emitProgramsChangedForRow(int row);
    void emitProgramsChangedForRange(int firstRow, int lastRow);
    void emitProgramsChangedForVisibleRows();
    void invalidateProgramTilesCache();
    void scheduleOffscreenRowWarmup();
    void warmProgramTilesForRows(int firstRow, int lastRow);
    void applyRows(
        const QList<Core::Channel> &channels,
        QList<Row> rows,
        int guidePastHours,
        int lookAheadHours,
        const QDateTime &windowStart,
        const QDateTime &windowEnd);
    QList<Row> buildRows(
        const QList<Core::Channel> &channels,
        int guidePastHours,
        int lookAheadHours,
        const QDateTime &windowStart,
        QDateTime *windowEnd) const;
    QDateTime defaultWindowStart(int guidePastHours) const;
    QDateTime windowEnd() const;

    Core::EpgService *m_epg;
    QList<Core::Channel> m_channels;
    QList<Row> m_rows;
    QDateTime m_windowStart;
    int m_guidePastHours { 6 };
    int m_lookAheadHours { 24 };
    int m_windowSpanMinutes { (6 + 24) * 60 };
    QVariantList m_timeSlots;
    QVariantList m_visibleTimeSlots;
    int m_selectedChannelId { -1 };
    QString m_selectedProgramStart;
    int m_renderViewportStartMinutes { 0 };
    int m_renderViewportDurationMinutes { 180 };
    int m_renderProgramsRangeStartMinutes { -1 };
    int m_renderProgramsRangeEndMinutes { -1 };
    int m_visibleRowStart { -1 };
    int m_visibleRowEnd { -1 };
    mutable QHash<int, QVariantList> m_programTilesCacheByRow;
    QHash<int, int> m_rowIndexByChannelId;
    QFutureSynchronizer<void> m_backgroundTasks;
    quint64 m_rebuildGeneration { 0 };
    quint64 m_rowWarmupGeneration { 0 };
};

} // namespace OKILTV::App
