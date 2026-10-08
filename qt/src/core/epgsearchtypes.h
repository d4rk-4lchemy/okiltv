#pragma once

#include "models.h"
#include <functional>
#include <QVariantList>

namespace OKILTV::Core {
enum class EpgSearchTimeFilter { All, Now, Upcoming, Past };
enum class EpgSearchStatus { Ready, NoEpg, Preparing, Unsupported, Error, Cancelled };

struct EpgSearchRequest {
    quint64 requestId = 0;
    QString profileId;
    quint64 epgGeneration = 0;
    quint64 channelRevision = 0;
    QString query;
    EpgSearchTimeFilter timeFilter = EpgSearchTimeFilter::All;
    QDateTime nowUtc;
    QList<Channel> eligibleChannels;
    int pageSize = 50;
    int offset = 0;
};
struct EpgSearchRow {
    QString resultKey;
    Channel channel;
    EpgEntry program;
    QString sectionKey;
    QVariantList titleHighlights;
    QVariantList subTitleHighlights;
};
struct EpgSearchResult {
    EpgSearchRequest request;
    QList<EpgSearchRow> rows;
    int nextOffset = 0;
    bool hasMore = false;
    EpgSearchStatus status = EpgSearchStatus::Ready;
    QString errorCode;
    QString errorText;
};
// Identical normalization is used by the index, fixtures, ranking and highlights.
QString normalizeEpgSearchText(const QString &text);
QStringList epgSearchTokens(const QString &text);
QString epgSearchQueryError(const QString &text);
QVariantList epgSearchHighlights(const QString &text, const QStringList &tokens);
} // namespace OKILTV::Core
