#include "datetimeformat.h"

#include <QLocale>

namespace OKILTV::Core {
namespace {
QString unquotedPattern(const QString &pattern)
{
    QString tokens;
    bool quoted = false;
    for (qsizetype i = 0; i < pattern.size(); ++i) {
        if (pattern.at(i) == QLatin1Char('\'')) {
            if (i + 1 < pattern.size() && pattern.at(i + 1) == QLatin1Char('\'')) {
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (!quoted) {
            tokens += pattern.at(i);
        }
    }
    return tokens;
}

QDateTime programTimestamp(const QVariant &value)
{
    const auto text = value.toString();
    auto result = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!result.isValid()) {
        result = QDateTime::fromString(text, Qt::ISODate);
    }
    return result;
}
} // namespace

QString DateTimeFormatOptions::timePattern() const
{
    return twelveHour ? QStringLiteral("h:mm AP") : QStringLiteral("HH:mm");
}
QString DateTimeFormatOptions::guideDatePattern() const
{
    return monthFirst ? QStringLiteral("dddd, MM.dd") : QStringLiteral("dddd, dd.MM");
}
QString DateTimeFormatOptions::timelineDatePattern() const
{
    return monthFirst ? QStringLiteral("dddd MM.dd") : QStringLiteral("dddd dd.MM");
}
QString DateTimeFormatOptions::clockPattern() const
{
    return (monthFirst ? QStringLiteral("ddd MMM dd  ") : QStringLiteral("ddd dd MMM  ")) + timePattern();
}
QString DateTimeFormatOptions::dateTimePattern() const
{
    return (monthFirst ? QStringLiteral("MM-dd-yyyy ") : QStringLiteral("dd-MM-yyyy ")) + timePattern();
}
QString normalizeDateOrder(const QString &value)
{
    return value == QLatin1String("dmy") || value == QLatin1String("mdy") ? value : QStringLiteral("system");
}
QString normalizeTimeFormat(const QString &value)
{
    return value == QLatin1String("24h") || value == QLatin1String("12h") ? value : QStringLiteral("system");
}
DateTimeFormatOptions detectDateTimeFormat(const QString &datePattern, const QString &timePattern)
{
    const auto dateTokens = unquotedPattern(datePattern);
    const auto timeTokens = unquotedPattern(timePattern);
    const auto month = dateTokens.indexOf(QLatin1Char('M'));
    qsizetype day = -1;
    for (qsizetype i = 0; i < dateTokens.size(); ++i) {
        if (dateTokens.at(i) != QLatin1Char('d')) {
            continue;
        }
        const auto start = i;
        while (i + 1 < dateTokens.size() && dateTokens.at(i + 1) == QLatin1Char('d')) {
            ++i;
        }
        if (i - start < 2) {
            day = start;
            break;
        }
    }
    return { month >= 0 && day >= 0 && month < day,
        timeTokens.contains(QStringLiteral("AP"), Qt::CaseInsensitive)
            || timeTokens.contains(QLatin1Char('a'), Qt::CaseInsensitive) };
}
DateTimeFormatOptions systemDateTimeFormat()
{
    // Resolve once per process, independently of the language used for display.
    static const auto options = detectDateTimeFormat(
        QLocale::system().dateFormat(QLocale::ShortFormat),
        QLocale::system().timeFormat(QLocale::ShortFormat));
    return options;
}
DateTimeFormatOptions resolveDateTimeFormat(const QString &dateOrder, const QString &timeFormat)
{
    auto options = systemDateTimeFormat();
    if (normalizeDateOrder(dateOrder) != QLatin1String("system")) {
        options.monthFirst = dateOrder == QLatin1String("mdy");
    }
    if (normalizeTimeFormat(timeFormat) != QLatin1String("system")) {
        options.twelveHour = timeFormat == QLatin1String("12h");
    }
    return options;
}
QString formatDisplayDateTime(const QDateTime &value, const QString &pattern)
{
    return value.isValid() ? QLocale(QLocale::English, QLocale::UnitedStates).toString(value.toLocalTime(), pattern) : QString {};
}
QString formatDisplayTime(const QDateTime &value, const DateTimeFormatOptions options)
{
    return formatDisplayDateTime(value, options.timePattern());
}
QString formatDisplayTimeRange(const QDateTime &start, const QDateTime &stop, const DateTimeFormatOptions options)
{
    if (!start.isValid() || !stop.isValid()) {
        return {};
    }
    return formatDisplayTime(start, options) + QStringLiteral(" - ") + formatDisplayTime(stop, options);
}
QVariantMap formatProgramTimes(QVariantMap program, const DateTimeFormatOptions options)
{
    if (!program.isEmpty() && program.contains(QStringLiteral("start"))) {
        const auto start = programTimestamp(program.value(QStringLiteral("start")));
        const auto stop = programTimestamp(program.value(QStringLiteral("stop")));
        program.insert(QStringLiteral("startTimeLabel"), formatDisplayTime(start, options));
        program.insert(QStringLiteral("timeRange"), formatDisplayTimeRange(start, stop, options));
    }
    return program;
}
QVariantList formatProgramTimes(QVariantList programs, const DateTimeFormatOptions options)
{
    for (auto &program : programs) {
        program = formatProgramTimes(program.toMap(), options);
    }
    return programs;
}
} // namespace OKILTV::Core
