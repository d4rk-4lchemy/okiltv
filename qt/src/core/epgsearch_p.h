#pragma once

#include "epgsearchtypes.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSet>
#include <algorithm>

namespace OKILTV::Core::EpgSearchDetail
{
inline QString queryErrorText(const QString &code)
{
    return code == QStringLiteral("query-too-long")
               ? QCoreApplication::translate("EpgSearch", "Enter at most 256 characters.")
               : QCoreApplication::translate("EpgSearch", "Enter at most 16 search words.");
}
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- Corpus words and query tokens are distinct algorithm roles.
inline bool matches(const QStringList &words, const QStringList &tokens)
{
    return std::all_of(tokens.cbegin(), tokens.cend(),
                       [&](const QString &token)
                       {
                           return std::any_of(
                               words.cbegin(), words.cend(), [&](const QString &word)
                               { return word.startsWith(token); });
                       });
}
inline QString identity(const EpgEntry &e)
{
    // Length prefixes keep separators in provider text from colliding.
    QByteArray bytes;
    for (const auto &field : {e.channelId.trimmed().toLower(), e.title, e.subTitle, e.episodeNum})
    {
        const auto utf8 = field.toUtf8();
        bytes += QByteArray::number(utf8.size()) + ':' + utf8;
    }
    bytes +=
        ':' + QByteArray::number(e.start.toMSecsSinceEpoch()) + ':' + QByteArray::number(e.stop.toMSecsSinceEpoch());
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
inline int section(const EpgEntry &e, const QDateTime &now)
{
    if (!e.start.isValid() || !e.stop.isValid() || e.stop <= e.start)
        return -1;
    return e.start > now ? 1 : e.stop <= now ? 2 : 0;
}
inline bool accepts(int value, EpgSearchTimeFilter filter)
{
    return value >= 0 && (filter == EpgSearchTimeFilter::All || value == static_cast<int>(filter) - 1);
}
struct Candidate
{
    EpgSearchRow row;
    int section = 0;
    int rank = 0;
    int channelOrder = 0;
    qint64 time = 0;
};
inline bool better(const Candidate &a, const Candidate &b)
{
    if (a.section != b.section)
        return a.section < b.section;
    if (a.time != b.time)
        return a.time < b.time;
    if (a.rank != b.rank)
        return a.rank < b.rank;
    if (a.channelOrder != b.channelOrder)
        return a.channelOrder < b.channelOrder;
    return a.row.resultKey < b.row.resultKey;
}
class Page
{
  public:
    explicit Page(const EpgSearchRequest &request)
        : request(request), tokens(epgSearchTokens(request.query)),
          normalizedQuery(normalizeEpgSearchText(request.query)),
          maximum(std::max(0, request.offset) + std::clamp(request.pageSize, 1, 200) + 1)
    {
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- Hash identity and normalized title have distinct roles.
    void append(const EpgEntry &e, const Channel &channel, int channelOrder, const QString &entryIdentity,
                const QString &normalizedTitle = {})
    {
        const int group = section(e, request.nowUtc);
        if (!accepts(group, request.timeFilter))
            return;
        Candidate c;
        c.section = group;
        const auto title = normalizedTitle.isNull() ? normalizeEpgSearchText(e.title) : normalizedTitle;
        c.rank = titleRank(title);
        c.channelOrder = channel.sortOrder;
        Q_UNUSED(channelOrder);
        c.time = group == 2 ? -e.start.toMSecsSinceEpoch() : e.start.toMSecsSinceEpoch();
        c.row.resultKey = channel.profileId.toString(QUuid::WithoutBraces) + u':' + QString::number(channel.id) + u':' +
                          entryIdentity;
        if (rows.size() == maximum && !better(c, rows.front()))
            return;
        c.row.channel = channel;
        c.row.program = e;
        c.row.program.description.clear();
        c.row.sectionKey = group == 0   ? QStringLiteral("now")
                           : group == 1 ? QStringLiteral("upcoming")
                                        : QStringLiteral("past");
        if (rows.size() == maximum)
        {
            std::pop_heap(rows.begin(), rows.end(), better);
            rows.pop_back();
        }
        rows.push_back(std::move(c));
        std::push_heap(rows.begin(), rows.end(), better);
    }
    EpgSearchResult result()
    {
        EpgSearchResult result;
        result.request = request;
        std::sort(rows.begin(), rows.end(), better);
        const int offset = std::max(0, request.offset);
        const int count = std::clamp(request.pageSize, 1, 200);
        result.hasMore = rows.size() > offset + count;
        for (int i = offset; i < std::min(static_cast<int>(rows.size()), offset + count); ++i)
        {
            auto row = std::move(rows[i].row);
            row.titleHighlights = epgSearchHighlights(row.program.title, tokens);
            row.subTitleHighlights = epgSearchHighlights(row.program.subTitle, tokens);
            result.rows.push_back(std::move(row));
        }
        result.nextOffset = offset + static_cast<int>(result.rows.size());
        return result;
    }
    bool full() const { return rows.size() == maximum; }
    qint64 worstTime() const { return rows.front().time; }
    int titleRank(const QString &title) const
    {
        return title == normalizedQuery ? 0 : matches(title.split(u' ', Qt::SkipEmptyParts), tokens) ? 1 : 2;
    }

  private:
    EpgSearchRequest request;
    QStringList tokens;
    QString normalizedQuery;
    int maximum;
    QList<Candidate> rows;
};
inline QList<Channel> channels(const EpgSearchRequest &request)
{
    QList<Channel> result;
    QSet<int> seen;
    const QUuid profile(request.profileId);
    for (const auto &channel : request.eligibleChannels)
    {
        if (channel.profileId != profile || channel.tvgId.trimmed().isEmpty() || seen.contains(channel.id))
            continue;
        seen.insert(channel.id);
        result += channel;
    }
    return result;
}
} // namespace OKILTV::Core::EpgSearchDetail
