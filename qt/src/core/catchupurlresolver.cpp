#include "catchupurlresolver.h"

#include "debuglogger.h"

#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace OKILTV::Core {

namespace {

QString trimmedBaseUrl(const QString &value)
{
    auto base = value.trimmed();
    while (base.endsWith(u'/')) {
        base.chop(1);
    }
    return base;
}

QString resolveM3uTemplate(QString templateValue, const EpgEntry &program, const qint64 durationSeconds)
{
    // Providers may ship placeholders as single- or double-encoded braces.
    // Normalize both so template substitution always sees raw `{...}` tokens.
    templateValue.replace(QRegularExpression(QStringLiteral("%257B"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("{"));
    templateValue.replace(QRegularExpression(QStringLiteral("%257D"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("}"));
    // Some providers percent-encode placeholder braces inside M3U metadata
    // (`%7Butc%7D`) even though the placeholders are meant to be substituted
    // before URL encoding. Normalize those braces so placeholder expansion
    // works for both raw and encoded template variants.
    templateValue.replace(QRegularExpression(QStringLiteral("%7B"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("{"));
    templateValue.replace(QRegularExpression(QStringLiteral("%7D"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("}"));

    const auto startUtc = program.start.toUTC();
    const auto utcEpoch = startUtc.toSecsSinceEpoch();
    const auto stopUtc = program.stop.toUTC();
    const auto lutcEpoch = stopUtc.toSecsSinceEpoch();
    const auto replacements = QList<QPair<QString, QString>> {
        { QStringLiteral("${start}"), QString::number(utcEpoch) },
        { QStringLiteral("${end}"), QString::number(lutcEpoch) },
        { QStringLiteral("{utcend}"), QString::number(lutcEpoch) },
        { QStringLiteral("{utc}"), QString::number(utcEpoch) },
        { QStringLiteral("{lutc}"), QString::number(lutcEpoch) },
        { QStringLiteral("{Y}"), startUtc.toString(QStringLiteral("yyyy")) },
        { QStringLiteral("{m}"), startUtc.toString(QStringLiteral("MM")) },
        { QStringLiteral("{d}"), startUtc.toString(QStringLiteral("dd")) },
        { QStringLiteral("{H}"), startUtc.toString(QStringLiteral("HH")) },
        { QStringLiteral("{M}"), startUtc.toString(QStringLiteral("mm")) },
        { QStringLiteral("{S}"), startUtc.toString(QStringLiteral("ss")) },
        { QStringLiteral("{duration}"), QString::number(durationSeconds) }
    };

    for (const auto &replacement : replacements) {
        templateValue.replace(replacement.first, replacement.second);
    }

    static const QRegularExpression durationPattern(QStringLiteral(R"(\{duration:(\d+)\})"));
    auto match = durationPattern.match(templateValue);
    while (match.hasMatch()) {
        const auto divisor = std::max(1LL, match.captured(1).toLongLong());
        templateValue.replace(match.capturedStart(0), match.capturedLength(0), QString::number(durationSeconds / divisor));
        match = durationPattern.match(templateValue);
    }

    return templateValue;
}

bool hasUnresolvedTemplateTokens(const QString &value)
{
    static const QRegularExpression rawTokenPattern(QStringLiteral(R"(\{[^{}]+\})"));
    if (rawTokenPattern.match(value).hasMatch()) {
        return true;
    }

    static const QRegularExpression encodedTokenPattern(
        QStringLiteral(R"(%(?:25)?7B[^&?#\s]*(?:%(?:25)?7D))"),
        QRegularExpression::CaseInsensitiveOption);
    return encodedTokenPattern.match(value).hasMatch();
}

bool isQueryShapedAppendTemplate(const QString &templateValue)
{
    const auto trimmed = templateValue.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    if (trimmed.startsWith(u'?') || trimmed.startsWith(u'&')) {
        return true;
    }

    const auto equalsIndex = trimmed.indexOf(u'=');
    if (equalsIndex <= 0) {
        return false;
    }

    const auto firstSpecialIndex = trimmed.indexOf(QRegularExpression(QStringLiteral(R"([/?#])")));
    return firstSpecialIndex < 0 || equalsIndex < firstSpecialIndex;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- Named URL and query fragment have distinct call-site roles.
QString appendQueryStringFallback(const QString &baseUrl, const QString &queryFragment)
{
    const auto fragmentIndex = baseUrl.indexOf(u'#');
    const auto withoutFragment = fragmentIndex >= 0 ? baseUrl.left(fragmentIndex) : baseUrl;
    const auto fragment = fragmentIndex >= 0 ? baseUrl.mid(fragmentIndex) : QString {};
    const auto separator = withoutFragment.contains(u'?') ? u'&' : u'?';
    return withoutFragment + separator + queryFragment + fragment;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- Named URL and template have distinct call-site roles.
QString mergeAppendQueryTemplate(const QString &baseUrl, const QString &queryTemplate)
{
    const auto trimmedTemplate = queryTemplate.trimmed();
    auto normalizedQuery = trimmedTemplate;
    while (!normalizedQuery.isEmpty()
           && (normalizedQuery.startsWith(u'?') || normalizedQuery.startsWith(u'&'))) {
        normalizedQuery.remove(0, 1);
    }

    if (normalizedQuery.isEmpty()) {
        return baseUrl;
    }

    QUrl parsedBase(baseUrl);
    if (!parsedBase.isValid()) {
        return appendQueryStringFallback(baseUrl, normalizedQuery);
    }

    QUrlQuery mergedQuery(parsedBase);
    const auto existingItems = mergedQuery.queryItems(QUrl::FullyDecoded);
    QUrlQuery appendedQuery(normalizedQuery);
    const auto appendedItems = appendedQuery.queryItems(QUrl::FullyDecoded);
    if (appendedItems.isEmpty()) {
        return appendQueryStringFallback(baseUrl, normalizedQuery);
    }

    mergedQuery.clear();
    for (const auto &item : existingItems) {
        mergedQuery.addQueryItem(item.first, item.second);
    }
    for (const auto &item : appendedItems) {
        mergedQuery.addQueryItem(item.first, item.second);
    }

    parsedBase.setQuery(mergedQuery);
    const auto resolved = parsedBase.toString();
    return resolved.isEmpty() ? appendQueryStringFallback(baseUrl, normalizedQuery) : resolved;
}

QString extensionFromStreamUrl(const QString &streamUrl)
{
    QUrl parsed(streamUrl.trimmed());
    const auto path = parsed.isValid() ? parsed.path() : streamUrl.trimmed();
    const auto lastSlash = path.lastIndexOf(u'/');
    const auto fileName = lastSlash >= 0 ? path.mid(lastSlash + 1) : path;
    const auto dot = fileName.lastIndexOf(u'.');
    if (dot <= 0 || dot == fileName.size() - 1) {
        return QStringLiteral("ts");
    }

    auto ext = fileName.mid(dot + 1).trimmed().toLower();
    static const QRegularExpression allowedPattern(QStringLiteral(R"(^[a-z0-9]{1,16}$)"));
    if (!allowedPattern.match(ext).hasMatch()) {
        return QStringLiteral("ts");
    }
    return ext;
}

QString sourceName(const ChannelSource source)
{
    switch (source) {
    case ChannelSource::Xtream:
        return QStringLiteral("Xtream");
    case ChannelSource::M3U:
        return QStringLiteral("M3U");
    }
    return QStringLiteral("Unknown");
}

QDateTime programStartInXtreamTimezone(const QDateTime &programStartUtc, const QString &timezoneId, QString *effectiveTimezoneId)
{
    const auto trimmed = timezoneId.trimmed();
    if (!trimmed.isEmpty()) {
        const auto zone = QTimeZone(trimmed.toUtf8());
        if (zone.isValid()) {
            if (effectiveTimezoneId != nullptr) {
                *effectiveTimezoneId = QString::fromUtf8(zone.id());
            }
            return programStartUtc.toTimeZone(zone);
        }
    }

    if (effectiveTimezoneId != nullptr) {
        *effectiveTimezoneId = QStringLiteral("UTC");
    }
    return programStartUtc.toUTC();
}

} // namespace

CatchupUrlResolver::CatchupUrlResolver(std::optional<ServerProfile> profile)
    : m_profile(std::move(profile))
{
}

QDateTime CatchupUrlResolver::availableEdge(const QDateTime &stop, const int safetySeconds, const QDateTime &now)
{
    return std::min(stop.toUTC(), now.toUTC().addSecs(-std::clamp(safetySeconds, 0, 1800)));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- Named range endpoints are checked before URL construction.
QString CatchupUrlResolver::xtreamWindowUrl(const QString &canonicalUrl, const qint64 offsetSeconds,
                                          const qint64 availableSeconds, const bool withSeconds)
{
    static const QRegularExpression pattern(QStringLiteral(
        R"(^(https?://[^?#]+/timeshift/[^/]+/[^/]+)/\d+/(\d{4}-\d{2}-\d{2}:\d{2}-\d{2})/([^?#]+)(\?[^#]*)?$)"));
    const auto match = pattern.match(canonicalUrl);
    if (!match.hasMatch()) {
        return {};
    }
    auto stamp = QDateTime::fromString(match.captured(2), QStringLiteral("yyyy-MM-dd:HH-mm"));
    stamp.setTimeZone(QTimeZone::UTC);
    if (!stamp.isValid()) {
        return {};
    }
    const auto offset = std::max<qint64>(0, withSeconds ? offsetSeconds : (offsetSeconds / 60) * 60);
    if (availableSeconds < offset) {
        return {};
    }
    const auto minutes = std::max<qint64>(1, (availableSeconds - offset) / 60);
    return QStringLiteral("%1/%2/%3/%4")
        .arg(match.captured(1)).arg(minutes)
        .arg(stamp.addSecs(offset).toString(withSeconds ? QStringLiteral("yyyy-MM-dd:HH-mm-ss")
                                                       : QStringLiteral("yyyy-MM-dd:HH-mm")))
        .arg(match.captured(3) + match.captured(4));
}

std::optional<CatchupPlaybackTarget> CatchupUrlResolver::resolve(
    const Channel &channel,
    const EpgEntry &program,
    QString *failureReason) const
{
    CatchupPlaybackTarget target;
    auto fail = [&](const QString &reason) -> std::optional<CatchupPlaybackTarget> {
        target.reasonIfUnavailable = reason;
        if (failureReason != nullptr) {
            *failureReason = reason;
        }
        return std::nullopt;
    };
    target.programStartUtc = program.start.toUTC();
    target.programStopUtc = program.stop.toUTC();
    target.safetySeconds = 60 * std::clamp(m_profile.has_value() ? m_profile->catchupSafetyMinutes : 3, 0, 30);

    if (!channel.catchupSupported) {
        return fail(QStringLiteral("Channel archive is unavailable."));
    }
    if (!target.programStartUtc.isValid() || !target.programStopUtc.isValid() || target.programStopUtc <= target.programStartUtc) {
        return fail(QStringLiteral("Programme timing is invalid."));
    }
    if (channel.catchupWindowHours <= 0) {
        return fail(QStringLiteral("Archive window is unavailable."));
    }

    target.durationSeconds = std::max<qint64>(1, target.programStartUtc.secsTo(target.programStopUtc));

    if (channel.source == ChannelSource::Xtream) {
        if (!m_profile.has_value()) {
            return fail(QStringLiteral("Active Xtream profile metadata is missing."));
        }
        const auto &profile = m_profile.value();
        if (profile.xtreamBaseUrl.trimmed().isEmpty()
            || profile.xtreamUsername.trimmed().isEmpty()
            || profile.xtreamPassword.trimmed().isEmpty()) {
            return fail(QStringLiteral("Xtream profile credentials are incomplete."));
        }

        const auto extension = extensionFromStreamUrl(channel.streamUrl);
        QString effectiveTimezoneId;
        const auto startInProviderTimezone = programStartInXtreamTimezone(
            target.programStartUtc,
            profile.xtreamServerTimezone,
            &effectiveTimezoneId);
        const auto nowUtc = QDateTime::currentDateTimeUtc();
        const auto safeEdge = availableEdge(target.programStopUtc, target.safetySeconds, nowUtc);
        const auto programmeStillLive = safeEdge < target.programStopUtc;
        qint64 durationMinutes = 0;
        if (programmeStillLive) {
            const auto adjustedNowMinuteEpoch = static_cast<qint64>(
                std::floor(static_cast<double>(safeEdge.toSecsSinceEpoch()) / 60.0));
            const auto programStartMinuteEpoch = static_cast<qint64>(
                std::floor(static_cast<double>(target.programStartUtc.toSecsSinceEpoch()) / 60.0));
            durationMinutes = std::max<qint64>(1, adjustedNowMinuteEpoch - programStartMinuteEpoch);
        } else {
            durationMinutes = static_cast<qint64>(((target.durationSeconds + 59) / 60) + 1);
        }

        target.url = QStringLiteral("%1/timeshift/%2/%3/%4/%5/%6.%7")
            .arg(trimmedBaseUrl(profile.xtreamBaseUrl))
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(profile.xtreamUsername)))
            .arg(QString::fromUtf8(QUrl::toPercentEncoding(profile.xtreamPassword)))
            .arg(durationMinutes)
            .arg(startInProviderTimezone.toString(QStringLiteral("yyyy-MM-dd:HH-mm")))
            .arg(channel.id)
            .arg(extension);
        DebugLogger::instance().log(
            QStringLiteral("catchup.resolve.xtream"),
            QStringLiteral("source=%1 streamUrl=%2 ext=%3 startUtc=%4 startProviderTz=%5 timezone=%6 durationSec=%7 durationMin=%8 liveProgram=%9 url=%10")
                .arg(sourceName(channel.source),
                     channel.streamUrl,
                     extension,
                     target.programStartUtc.toString(Qt::ISODateWithMs),
                     startInProviderTimezone.toString(Qt::ISODateWithMs),
                     effectiveTimezoneId)
                .arg(target.durationSeconds)
                .arg(durationMinutes)
                .arg(programmeStillLive ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(target.url));
        return target;
    }

    auto templateValue = channel.catchupSourceTemplate.trimmed();
    if (templateValue.isEmpty()) {
        return fail(QStringLiteral("Archive URL template is missing."));
    }

    const auto mode = channel.catchupMode.trimmed().toLower();
    const auto resolvedTemplateValue = resolveM3uTemplate(templateValue, program, target.durationSeconds).trimmed();
    DebugLogger::instance().log(
        QStringLiteral("catchup.resolve.template"),
        QStringLiteral(
            "mode=%1 rawTemplate=%2 resolvedTemplate=%3 startUtc=%4 stopUtc=%5 utcEpoch=%6 lutcEpoch=%7")
            .arg(mode.isEmpty() ? QStringLiteral("<empty>") : mode)
            .arg(templateValue)
            .arg(resolvedTemplateValue)
            .arg(target.programStartUtc.toString(Qt::ISODateWithMs))
            .arg(target.programStopUtc.toString(Qt::ISODateWithMs))
            .arg(target.programStartUtc.toSecsSinceEpoch())
            .arg(target.programStopUtc.toSecsSinceEpoch()));
    if (resolvedTemplateValue.isEmpty()) {
        return fail(QStringLiteral("Archive URL template resolved to an empty URL."));
    }

    if (hasUnresolvedTemplateTokens(resolvedTemplateValue)) {
        return fail(QStringLiteral("Archive URL template still contains unresolved placeholders."));
    }

    if (mode == QStringLiteral("append")) {
        if (isQueryShapedAppendTemplate(templateValue)) {
            target.url = mergeAppendQueryTemplate(channel.streamUrl, resolvedTemplateValue);
        } else {
            target.url = (channel.streamUrl + resolvedTemplateValue).trimmed();
        }
    } else if (mode == QStringLiteral("default")) {
        target.url = resolvedTemplateValue;
    } else {
        return fail(QStringLiteral("Archive mode is unsupported."));
    }

    if (target.url.isEmpty()) {
        return fail(QStringLiteral("Archive URL template resolved to an empty URL."));
    }
    if (hasUnresolvedTemplateTokens(target.url)) {
        return fail(QStringLiteral("Archive playback URL still contains unresolved placeholders."));
    }

    return target;
}

std::optional<CatchupDownloadTarget> CatchupUrlResolver::resolveDownload(
    const Channel &channel, const EpgEntry &program, QString *failureReason) const
{
    const auto now = QDateTime::currentDateTimeUtc();
    const int safety = 60 * std::clamp(m_profile ? m_profile->catchupSafetyMinutes : 3, 0, 30);
    if (!program.start.isValid() || !program.stop.isValid() || program.stop <= program.start
        || availableEdge(program.stop, safety, now) < program.stop
        || program.start < now.addSecs(-3600LL * channel.catchupWindowHours)) {
        if (failureReason)
            *failureReason = QStringLiteral("The complete programme is not available in the archive.");
        return std::nullopt;
    }
    const auto playback = resolve(channel, program, failureReason);
    if (!playback)
        return std::nullopt;
    CatchupDownloadTarget result;
    result.url = playback->url;
    result.durationSeconds = program.start.secsTo(program.stop);
    if (channel.source == ChannelSource::Xtream)
        result.trimStartSeconds = program.start.toUTC().time().second();
    return result;
}

std::optional<CatchupPlaybackTarget> CatchupUrlResolver::resolveWindow(
    const Channel &channel, const QDateTime &startUtc, const QDateTime &endUtc,
    QString *failureReason) const
{
    if (!startUtc.isValid() || !endUtc.isValid() || endUtc <= startUtc) {
        if (failureReason) {
            *failureReason = QStringLiteral("Archive range is invalid.");
        }
        return std::nullopt;
    }
    EpgEntry range;
    range.start = startUtc.toUTC();
    range.stop = endUtc.toUTC();
    if (channel.source == ChannelSource::Xtream && range.start.isValid()) {
        range.start = QDateTime::fromSecsSinceEpoch((range.start.toSecsSinceEpoch() / 60) * 60, QTimeZone::UTC);
    }
    auto target = resolve(channel, range, failureReason);
    if (!target) {
        return std::nullopt;
    }
    if (channel.source == ChannelSource::Xtream) {
        // resolve() preserves the legacy finite-programme padding. A window
        // instead ends at the published edge, rounded to the provider minute.
        target->url = xtreamWindowUrl(target->url, 0, target->durationSeconds);
        target->durationSeconds = std::max<qint64>(1, target->durationSeconds / 60) * 60;
        target->programStopUtc = target->programStartUtc.addSecs(target->durationSeconds);
    }
    return target;
}

} // namespace OKILTV::Core
