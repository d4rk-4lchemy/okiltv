#include "vodservices.h"
#include <QSet>
#include <algorithm>

namespace OKILTV::Vod {
bool VodDependencies::complete() const { return provider && sources && catalog && progress && migrations && lists; }
Result<quint64> VodCatalogService::refresh(const VodDependencies &deps, const SourceContext &source,
    const CatalogScope &scope, const RequestContext &request)
{
    auto begin = deps.catalog->beginRefresh(scope, request);
    if (const auto *error = std::get_if<Error>(&begin)) return *error;
    const auto token = std::get<ImportToken>(begin);
    // Abandon staging on every exit, including exceptions. Published generations
    // are immutable; abandon of a published token is an idempotent no-op.
    struct Cleanup {
        std::shared_ptr<IVodCatalogRepository> catalog;
        ImportToken token;
        ~Cleanup() { catalog->abandonRefresh(token); }
    } cleanup{deps.catalog, token};
    QList<CatalogScope> requests;
    const auto allowed = scope.kind == CatalogKind::Movies ? source.allowedMovieCategories : source.allowedSeriesCategories;
    if (!allowed) requests.append(scope);
    else if (!allowed->isEmpty()) {
        const auto categoryCount = scope.kind == CatalogKind::Movies ? source.movieCategoryCount : source.seriesCategoryCount;
        if (categoryCount > 0 && allowed->size() * 10 < static_cast<qsizetype>(categoryCount) * 3) {
            for (const auto &category : *allowed) { auto selected = scope; selected.categoryId = category; requests.append(selected); }
        } else { auto full = scope; full.categoryId.reset(); requests.append(full); }
    }
    CatalogBatch snapshot{scope, {}, true, {}};
    QHash<QByteArray, qsizetype> identities;
    for (const auto &fetchScope : requests) {
        std::optional<ProviderCursor> cursor;
        QSet<QString> visited;
        bool complete = false;
        for (int page = 0; page < 10000; ++page) {
            if (const auto error = request.interruption()) return *error;
            if (!deps.sources->isCurrent(source.revision)) return Error{ErrorCode::Cancelled, request.operationId};
            auto response = deps.provider->fetchCatalog(source, fetchScope, cursor, request);
            if (const auto *error = std::get_if<Error>(&response)) return *error;
            const auto batch = std::get<CatalogBatch>(std::move(response));
            if (batch.scope != fetchScope || (batch.complete && batch.next) || (!batch.complete && !batch.next))
                return Error{ErrorCode::InvalidResponse, request.operationId};
            for (const auto &item : batch.items) {
                const auto ref = std::visit([](const auto &summary) { return summary.ref; }, item);
                if (!ref.valid() || ref.profileId != scope.profileId || ref.catalogNamespace != scope.catalogNamespace
                    || (scope.kind == CatalogKind::Movies && (ref.kind != ContentKind::Movie || !std::holds_alternative<MovieSummary>(item)))
                    || (scope.kind == CatalogKind::Series && (ref.kind != ContentKind::Series || !std::holds_alternative<SeriesSummary>(item))))
                    return Error{ErrorCode::InvalidResponse, request.operationId};
                const auto categories = std::visit([](const auto &summary) { return summary.categoryIds; }, item);
                if (fetchScope.categoryId && !categories.contains(*fetchScope.categoryId)) continue;
                if (scope.categoryId && !categories.contains(*scope.categoryId)) continue;
                if (allowed && std::none_of(categories.cbegin(), categories.cend(), [&](const QString &id) { return allowed->contains(id); })) continue;
                const auto existing = identities.constFind(ref.key());
                if (existing != identities.cend()) {
                    std::visit([&](auto &summary) { for (const auto &id : categories) if (!summary.categoryIds.contains(id)) summary.categoryIds.append(id); }, snapshot.items[*existing]);
                } else {
                    identities.insert(ref.key(), snapshot.items.size());
                    snapshot.items.append(item);
                    if (snapshot.items.size() > 1000000) return Error{ErrorCode::ResponseTooLarge, request.operationId};
                }
            }
            if (batch.complete) { complete = true; break; }
            cursor = batch.next;
            if (!cursor || cursor->value.isEmpty() || visited.contains(cursor->value)) return Error{ErrorCode::InvalidResponse, request.operationId};
            visited.insert(cursor->value);
        }
        if (!complete) return Error{ErrorCode::ResponseTooLarge, request.operationId};
    }
    const auto staged = deps.catalog->stageBatch(token, snapshot);
    if (const auto *error = std::get_if<Error>(&staged)) return *error;
    if (const auto error = request.interruption()) return *error;
    return deps.catalog->publishIfCurrent(token, true);
}
Result<PlaybackDescriptor> VodPlaybackResolver::resolve(const VodDependencies &deps, const SourceContext &source,
    const ContentRef &ref, const PlaybackPreferences &preferences, const RequestContext &request)
{
    if (!ref.playable() || ref.profileId != source.revision.profileId || ref.catalogNamespace != source.revision.catalogNamespace)
        return Error{ErrorCode::ContentUnavailable, request.operationId};
    auto result = deps.provider->resolvePlayback(source, ref, preferences, request);
    if (const auto *descriptor = std::get_if<PlaybackDescriptor>(&result)) {
        const auto &url = descriptor->mediaUri;
        if (descriptor->ref != ref || descriptor->source != source.revision || !url.isValid() || url.host().isEmpty()
            || (url.scheme() != QStringLiteral("https") && url.scheme() != QStringLiteral("http"))
            || !descriptor->expiresAtUtc.isValid() || descriptor->expiresAtUtc <= QDateTime::currentDateTimeUtc())
            return Error{ErrorCode::InvalidResponse, request.operationId};
        for (auto it = descriptor->allowedHeaders.cbegin(); it != descriptor->allowedHeaders.cend(); ++it) {
            // RFC 9110: field names are tokens; values cannot contain control
            // bytes other than HTAB. Validate before crossing into the engine.
            const bool validName = !it.key().isEmpty() && std::all_of(it.key().cbegin(), it.key().cend(), [](char value) {
                return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
                    || (value >= '0' && value <= '9') || QByteArrayLiteral("!#$%&'*+-.^_`|~").contains(value);
            });
            const bool validValue = std::all_of(it.value().cbegin(), it.value().cend(), [](unsigned char value) {
                return value == '\t' || (value >= 32 && value != 127);
            });
            if (!validName || !validValue)
                return Error{ErrorCode::InvalidResponse, request.operationId};
        }
    }
    return result;
}
}
