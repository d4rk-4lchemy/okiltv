#include "vodmediaprobe.h"
#include "core/processutils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <algorithm>

namespace OKILTV::Vod {
namespace {
constexpr qsizetype kMaxProbeOutput = 2 * 1024 * 1024;

QString boundedText(const QJsonValue &value, qsizetype limit)
{
    auto result = value.toString().trimmed();
    if (result.size() > limit) result.truncate(limit);
    result.remove(QChar::Null);
    return result;
}

std::optional<int> dimension(const QJsonValue &value)
{
    const auto result = value.toInt(-1);
    return result > 0 && result <= 32768 ? std::optional<int>(result) : std::nullopt;
}
}

Result<VodMediaProbe> parseVodMediaProbe(const QByteArray &payload, const RequestContext &context)
{
    if (const auto interrupted = context.interruption()) return *interrupted;
    if (payload.isEmpty() || payload.size() > kMaxProbeOutput)
        return Error{payload.size() > kMaxProbeOutput ? ErrorCode::ResponseTooLarge : ErrorCode::InvalidResponse, context.operationId};
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return Error{ErrorCode::InvalidResponse, context.operationId};
    const auto streams = document.object().value(QStringLiteral("streams"));
    if (!streams.isArray() || streams.toArray().size() > 128)
        return Error{ErrorCode::InvalidResponse, context.operationId};

    VodMediaProbe result;
    int audioOrdinal = 0;
    int subtitleOrdinal = 0;
    for (const auto &entry : streams.toArray()) {
        if (const auto interrupted = context.interruption()) return *interrupted;
        if (!entry.isObject()) return Error{ErrorCode::InvalidResponse, context.operationId};
        const auto stream = entry.toObject();
        const auto type = stream.value(QStringLiteral("codec_type")).toString();
        if (type == QLatin1String("video") && !result.videoWidth) {
            const auto width = dimension(stream.value(QStringLiteral("width")));
            const auto height = dimension(stream.value(QStringLiteral("height")));
            if (width && height) { result.videoWidth = width; result.videoHeight = height; }
            continue;
        }
        if (type != QLatin1String("audio") && type != QLatin1String("subtitle")) continue;
        VodMediaTrack track;
        track.streamIndex = stream.value(QStringLiteral("index")).toInt(-1);
        if (track.streamIndex < 0) return Error{ErrorCode::InvalidResponse, context.operationId};
        track.type = type == QLatin1String("audio") ? QStringLiteral("audio") : QStringLiteral("sub");
        track.ordinal = type == QLatin1String("audio") ? audioOrdinal++ : subtitleOrdinal++;
        track.codec = boundedText(stream.value(QStringLiteral("codec_name")), 128);
        const auto tags = stream.value(QStringLiteral("tags")).toObject();
        track.language = boundedText(tags.value(QStringLiteral("language")), 128);
        track.title = boundedText(tags.value(QStringLiteral("title")), 1024);
        const auto disposition = stream.value(QStringLiteral("disposition")).toObject();
        track.isDefault = disposition.value(QStringLiteral("default")).toInt() == 1;
        track.forced = disposition.value(QStringLiteral("forced")).toInt() == 1;
        if (track.type == QLatin1String("audio")) result.audioTracks.append(track);
        else result.subtitleTracks.append(track);
    }
    result.observedAtUtc = QDateTime::currentDateTimeUtc();
    return result;
}

Result<VodMediaProbe> probeVodMedia(const PlaybackDescriptor &descriptor, const RequestContext &context)
{
    if (const auto interrupted = context.interruption()) return *interrupted;
    if (!descriptor.mediaUri.isValid()
        || (descriptor.mediaUri.scheme() != QLatin1String("http") && descriptor.mediaUri.scheme() != QLatin1String("https"))) {
        return Error{ErrorCode::ContentUnavailable, context.operationId};
    }
    if (!Core::processBinaryAvailable(QStringLiteral("ffprobe")))
        return Error{ErrorCode::UnsupportedCapability, context.operationId};

    QStringList arguments{QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-probesize"), QStringLiteral("16M"),
        QStringLiteral("-analyzeduration"), QStringLiteral("10M")};
    if (!descriptor.allowedHeaders.isEmpty()) {
        QByteArray headers;
        for (auto it = descriptor.allowedHeaders.cbegin(); it != descriptor.allowedHeaders.cend(); ++it)
            headers += it.key() + ": " + it.value() + "\r\n";
        arguments << QStringLiteral("-headers") << QString::fromLatin1(headers);
    }
    arguments << QStringLiteral("-show_entries")
        << QStringLiteral("stream=index,codec_type,codec_name,width,height:stream_tags=language,title:stream_disposition=default,forced")
        << QStringLiteral("-of") << QStringLiteral("json") << descriptor.mediaUri.toString(QUrl::FullyEncoded);

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(Core::resolveProcessBinary(QStringLiteral("ffprobe")), arguments);
    if (!process.waitForStarted(static_cast<int>(std::clamp<qint64>(context.deadline.remainingTime(), 0, 3000)))) {
        if (const auto interrupted = context.interruption()) return *interrupted;
        return Error{ErrorCode::UnsupportedCapability, context.operationId};
    }
    qint64 diagnosticBytes = 0;
    while (process.state() != QProcess::NotRunning) {
        if (const auto interrupted = context.interruption()) {
            process.kill(); process.waitForFinished(1000); return *interrupted;
        }
        process.waitForFinished(static_cast<int>(std::clamp<qint64>(context.deadline.remainingTime(), 1, 50)));
        diagnosticBytes += process.readAllStandardError().size();
        if (process.bytesAvailable() > kMaxProbeOutput || diagnosticBytes > kMaxProbeOutput) {
            process.kill(); process.waitForFinished(1000);
            return Error{ErrorCode::ResponseTooLarge, context.operationId};
        }
    }
    const auto output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return Error{ErrorCode::ProviderUnavailable, context.operationId, true};
    return parseVodMediaProbe(output, context);
}

}
