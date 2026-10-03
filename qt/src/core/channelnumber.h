#pragma once

#include "models.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <algorithm>

namespace OKILTV::Core {

// Text is intentional: channel identities must not acquire floating-point rounding.
inline QString normalizeChannelNumber(QString text)
{
    text = text.trimmed();
    if (text.isEmpty()) return {};
    bool point = false;
    for (const auto ch : text) {
        if (ch == u'.' && !point) point = true;
        else if (ch < u'0' || ch > u'9') return {};
    }
    if (text.front() == u'.' || text.back() == u'.') return {};
    while (text.size() > 1 && text.front() == u'0' && text.at(1) != u'.') text.remove(0, 1);
    if (point) {
        while (text.endsWith(u'0')) text.chop(1);
        if (text.endsWith(u'.')) text.chop(1);
    }
    return text == QStringLiteral("0") ? QString {} : text;
}

// Inputs are canonical positive decimals (or empty, which sorts first).
inline int compareChannelNumbers(const QString &left, const QString &right)
{
    if (left.isEmpty() || right.isEmpty()) return QString::compare(left, right);
    const auto li = left.section(u'.', 0, 0);
    const auto ri = right.section(u'.', 0, 0);
    if (li.size() != ri.size()) return li.size() < ri.size() ? -1 : 1;
    if (const auto result = QString::compare(li, ri); result != 0) return result;
    auto lf = left.section(u'.', 1, 1);
    auto rf = right.section(u'.', 1, 1);
    const auto width = std::max(lf.size(), rf.size());
    return QString::compare(lf.leftJustified(width, u'0'), rf.leftJustified(width, u'0'));
}

inline QString channelNumberFromJson(const QJsonValue &value)
{
    if (value.isString()) return normalizeChannelNumber(value.toString());
    if (!value.isDouble() || value.toDouble() <= 0) return {};
    // Qt's JSON writer produces the shortest round-trippable decimal representation.
    const auto json = QJsonDocument(QJsonArray {value}).toJson(QJsonDocument::Compact);
    auto text = QString::fromLatin1(json.mid(1, json.size() - 2));
    const auto exponentAt = text.indexOf(u'e', 0, Qt::CaseInsensitive);
    if (exponentAt >= 0) {
        const auto exponent = text.mid(exponentAt + 1).toInt();
        text.truncate(exponentAt);
        auto point = text.indexOf(u'.');
        if (point < 0) point = text.size();
        text.remove(u'.');
        point += exponent;
        if (point <= 0) text = QStringLiteral("0.") + QString(-point, u'0') + text;
        else if (point >= text.size()) text += QString(point - text.size(), u'0');
        else text.insert(point, u'.');
    }
    return normalizeChannelNumber(text);
}

inline QString effectiveChannelNumber(const Channel &channel, int sortedIndex = 0)
{
    const auto explicitNumber = normalizeChannelNumber(channel.channelNumber);
    if (!explicitNumber.isEmpty()) return explicitNumber;
    if (channel.source == ChannelSource::M3U) return QString::number(std::max(1, channel.sortOrder));
    return QString::number(channel.sortOrder > 0 ? channel.sortOrder : sortedIndex + 1);
}

} // namespace OKILTV::Core
