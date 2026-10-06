#include "epgsearchtypes.h"

#include <QChar>
#include <algorithm>

namespace OKILTV::Core
{
namespace
{
struct Normalized
{
    QString text{QStringLiteral("")};
    QList<int> starts;
    QList<int> ends;
};
Normalized normalized(const QString &source)
{
    Normalized result;
    int position = 0;
    for (const char32_t scalar : source.toUcs4())
    {
        const int width = scalar > 0xffff ? 2 : 1;
        const auto folded = QString::fromUcs4(&scalar, 1).normalized(QString::NormalizationForm_KD).toCaseFolded();
        bool appended = false;
        for (char32_t c : folded.toUcs4())
        {
            const auto category = QChar::category(c);
            if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining ||
                category == QChar::Mark_Enclosing)
                continue;
            if (c == 0x142)
                c = 'l'; // Polish ł does not decompose in Unicode.
            const bool word = QChar::isLetterOrNumber(c);
            const auto fragment = word ? QString::fromUcs4(&c, 1) : QStringLiteral(" ");
            for (const auto unit : fragment)
            {
                result.text += unit;
                result.starts += position;
                result.ends += position + width;
            }
            appended = true;
        }
        // Include a decomposed combining mark in the original highlight span.
        if (!appended && !result.ends.isEmpty())
            result.ends.last() = position + width;
        position += width;
    }
    return result;
}
} // namespace

QString normalizeEpgSearchText(const QString &text) { return normalized(text).text.simplified(); }

QStringList epgSearchTokens(const QString &text)
{
    return normalizeEpgSearchText(text).split(u' ', Qt::SkipEmptyParts);
}

QString epgSearchQueryError(const QString &text)
{
    if (text.toUcs4().size() > 256)
        return QStringLiteral("query-too-long");
    const auto tokens = epgSearchTokens(text);
    if (tokens.size() > 16)
        return QStringLiteral("too-many-tokens");
    if (std::none_of(tokens.cbegin(), tokens.cend(), [](const QString &token) { return token.toUcs4().size() >= 2; }))
        return QStringLiteral("query-too-short");
    return {};
}

QVariantList epgSearchHighlights(const QString &text, const QStringList &tokens)
{
    const auto n = normalized(text);
    QVariantList spans;
    qsizetype start = 0;
    while (start < n.text.size())
    {
        if (n.text.at(start) == u' ')
        {
            ++start;
            continue;
        }
        qsizetype end = n.text.indexOf(u' ', start);
        if (end < 0)
            end = n.text.size();
        const auto word = n.text.mid(start, end - start);
        qsizetype matchLength = 0;
        for (const auto &token : tokens)
        {
            if (word.startsWith(token))
                matchLength = std::max(matchLength, token.size());
        }
        if (matchLength > 0)
        {
            const int originalStart = n.starts.at(start);
            const int originalEnd = n.ends.at(start + matchLength - 1);
            spans += QVariantMap{{QStringLiteral("start"), originalStart},
                                 {QStringLiteral("length"), originalEnd - originalStart}};
        }
        start = end;
    }
    return spans;
}
} // namespace OKILTV::Core
