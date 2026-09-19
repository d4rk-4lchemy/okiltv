#include "datetimeformatter.h"

namespace OKILTV::App {
void DateTimeFormatter::apply(const QString &dateOrder, const QString &timeFormat)
{
    const auto options = Core::resolveDateTimeFormat(dateOrder, timeFormat);
    if (options == m_options) {
        return;
    }
    m_options = options;
    emit formatChanged();
}
QString DateTimeFormatter::systemDateOrderLabel() const
{
    return Core::systemDateTimeFormat().monthFirst ? QStringLiteral("MM/DD") : QStringLiteral("DD/MM");
}
QString DateTimeFormatter::systemTimeFormatLabel() const
{
    return Core::systemDateTimeFormat().twelveHour ? QStringLiteral("12-hour") : QStringLiteral("24-hour");
}
QString DateTimeFormatter::preview(const QString &dateOrder, const QString &timeFormat) const
{
    const auto options = Core::resolveDateTimeFormat(dateOrder, timeFormat);
    const QDateTime example(QDate(2026, 9, 18), QTime(18, 5));
    return Core::formatDisplayDateTime(example, options.guideDatePattern())
        + QStringLiteral("  ") + Core::formatDisplayTime(example, options);
}
} // namespace OKILTV::App
