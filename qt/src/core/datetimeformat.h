#pragma once

#include <QDateTime>
#include <QString>
#include <QVariantMap>

namespace OKILTV::Core {

struct DateTimeFormatOptions
{
    bool monthFirst { false };
    bool twelveHour { false };
    bool operator==(const DateTimeFormatOptions &) const = default;

    QString timePattern() const;
    QString guideDatePattern() const;
    QString timelineDatePattern() const;
    QString clockPattern() const;
    QString dateTimePattern() const;
};

QString normalizeDateOrder(const QString &value);
QString normalizeTimeFormat(const QString &value);
DateTimeFormatOptions detectDateTimeFormat(const QString &datePattern, const QString &timePattern);
DateTimeFormatOptions systemDateTimeFormat();
DateTimeFormatOptions resolveDateTimeFormat(const QString &dateOrder, const QString &timeFormat);
QString formatDisplayDateTime(const QDateTime &value, const QString &pattern);
QString formatDisplayTime(const QDateTime &value, DateTimeFormatOptions options);
QString formatDisplayTimeRange(const QDateTime &start, const QDateTime &stop, DateTimeFormatOptions options);
QVariantMap formatProgramTimes(QVariantMap program, DateTimeFormatOptions options);
QVariantList formatProgramTimes(QVariantList programs, DateTimeFormatOptions options);

} // namespace OKILTV::Core
