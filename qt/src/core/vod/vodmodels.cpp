#include "vodmodels.h"
#include <QDataStream>
#include <QIODevice>

namespace OKILTV::Vod {
QString Error::message() const
{
    switch (code) {
    case ErrorCode::Cancelled: return QStringLiteral("Operation cancelled.");
    case ErrorCode::Timeout: return QStringLiteral("Operation timed out.");
    case ErrorCode::Unauthorized: return QStringLiteral("Source authentication required.");
    case ErrorCode::Forbidden: return QStringLiteral("Source access denied.");
    case ErrorCode::RateLimited: return QStringLiteral("Source request limit reached.");
    case ErrorCode::ProviderUnavailable: return QStringLiteral("Source unavailable.");
    case ErrorCode::UnsupportedCapability: return QStringLiteral("VOD capability unavailable.");
    case ErrorCode::InvalidResponse: return QStringLiteral("Invalid source response.");
    case ErrorCode::ResponseTooLarge: return QStringLiteral("Source response exceeds the limit.");
    case ErrorCode::ContentUnavailable: return QStringLiteral("Content unavailable.");
    case ErrorCode::StorageUnavailable: return QStringLiteral("VOD storage unavailable.");
    case ErrorCode::SecretUnavailable: return QStringLiteral("Source secrets unavailable.");
    case ErrorCode::PlaybackConflict: return QStringLiteral("Playback resources are in use.");
    case ErrorCode::PlaybackFailed: return QStringLiteral("VOD playback failed.");
    }
    return QStringLiteral("VOD operation failed.");
}
bool ContentRef::valid() const
{
    return (kind == ContentKind::Movie || kind == ContentKind::Series || kind == ContentKind::Episode)
        && !profileId.isNull() && !catalogNamespace.isNull() && !providerItemId.isEmpty()
        && (!parentNamespace || !parentNamespace->isEmpty());
}
bool ContentRef::playable() const { return valid() && kind != ContentKind::Series; }
QByteArray ContentRef::key() const
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << quint8(1) << profileId << catalogNamespace << qint32(kind)
           << providerItemId << parentNamespace.has_value();
    if (const auto parent = parentNamespace) stream << *parent;
    return bytes;
}
std::optional<Error> RequestContext::interruption() const
{
    if (cancelled->load()) return Error{ErrorCode::Cancelled, operationId};
    if (deadline.hasExpired()) return Error{ErrorCode::Timeout, operationId};
    return std::nullopt;
}
}
