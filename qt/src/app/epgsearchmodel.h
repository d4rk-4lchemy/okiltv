#pragma once
#include "../core/epgsearchtypes.h"
#include <QAbstractListModel>
namespace OKILTV::App {
class EpgSearchModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        ResultKeyRole = Qt::UserRole + 1,
        TitleRole,
        SubTitleRole,
        EpisodeNumRole,
        ChannelNameRole,
        ChannelLogoRole,
        SectionKeyRole,
        TimeLabelRole,
        StatusLabelRole,
        TitleHighlightsRole,
        SubTitleHighlightsRole,
        ProfileIdRole,
        PlaybackChannelIdRole,
        EpgChannelKeyRole,
        StartUtcMsRole,
        StopUtcMsRole,
        ChannelRole,
        ProgramRole,
        BroadcastStateRole
    };
    explicit EpgSearchModel(QObject* parent = nullptr)
        : QAbstractListModel(parent)
    {
    }
    int rowCount(const QModelIndex& parent = { }) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void replace(QList<Core::EpgSearchRow> rows);
    void append(const QList<Core::EpgSearchRow>& rows);
    const Core::EpgSearchRow* row(int index) const;
    int indexOfKey(const QString& key) const;
    void setDateTimeFormat(Core::DateTimeFormatOptions options);
    void refreshLabels(QDateTime nowUtc = QDateTime::currentDateTimeUtc());
    void setSummaries(bool summaries);
    QString timeLabel(const Core::EpgSearchRow& row) const;

private:
    QList<Core::EpgSearchRow> m_rows;
    QDateTime m_nowUtc = QDateTime::currentDateTimeUtc();
    bool m_summaries = false;
    Core::DateTimeFormatOptions m_format = Core::systemDateTimeFormat();
};
} // namespace OKILTV::App
