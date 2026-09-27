#pragma once
#include "app/vod/vodmodule.h"
#include <QDataStream>
#include <QIODevice>
#include <QRecursiveMutex>
#include <QMutexLocker>
#include <QThread>
#include <algorithm>

namespace OKILTV::Vod::Test {
inline SourceContext makeSource()
{
    SourceContext source;
    source.revision = {QUuid::createUuid(), QUuid::createUuid(), 1};
    source.enabled = true;
    source.provider = QStringLiteral("fake");
    source.endpoint = QUrl(QStringLiteral("https://fixture.invalid"));
    source.username = QStringLiteral("synthetic-user");
    source.password = QStringLiteral("synthetic-secret");
    return source;
}
inline CatalogScope scopeFor(const SourceContext &source)
{ return {source.revision.profileId, source.revision.catalogNamespace, CatalogKind::Movies, {}}; }
inline ContentRef refFor(const SourceContext &source, QString id = QStringLiteral("1"))
{ return {source.revision.profileId, source.revision.catalogNamespace, ContentKind::Movie, std::move(id), {}}; }
inline PlaybackDescriptor descriptorFor(const SourceContext &source, const ContentRef &ref)
{
    return {ref, source.revision, QUrl(QStringLiteral("https://fixture.invalid/movie/synthetic-user/synthetic-secret/1.ts")),
        {}, QDateTime::currentDateTimeUtc().addSecs(60), {}};
}
inline RequestContext requestFor(const SourceContext &source)
{ RequestContext request; request.source = source.revision; return request; }
inline QByteArray scopeKey(const CatalogScope &scope)
{
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << scope.profileId << scope.catalogNamespace << qint32(scope.kind) << scope.categoryId.has_value();
    if (const auto category = scope.categoryId) stream << *category;
    return key;
}
// Executable reference contract, not a production/persistent repository. A single
// lock models the SQL transaction/barrier required of B2 implementations.
class MemoryStore final : public IVodSourceAccess, public IVodCatalogRepository,
    public IVodProgressRepository, public IVodMigrations {
public:
    explicit MemoryStore(SourceContext source) : m_source(std::move(source)) {}
    std::atomic_int migrations{0};
    std::atomic_int publications{0};
    std::atomic_int checkpoints{0};
    void editCredentials()
    {
        QMutexLocker lock(&m_mutex);
        ++m_source.revision.credentialRevision;
        m_source.endpoint = QUrl(QStringLiteral("https://moved.invalid"));
        m_source.username = QStringLiteral("changed-login");
        m_source.password = QStringLiteral("changed-secret");
    }
    Result<SourceContext> snapshot(const QUuid &id) override
    {
        QMutexLocker lock(&m_mutex);
        if (m_removing || id != m_source.revision.profileId) return Error{ErrorCode::ContentUnavailable, {}};
        return m_source;
    }
    bool isCurrent(const SourceRevision &source) override
    { QMutexLocker lock(&m_mutex); return !m_removing && source == m_source.revision; }
    Outcome prepareRemoval(const QUuid &id) override
    {
        QMutexLocker lock(&m_mutex);
        if (id != m_source.revision.profileId) return Error{ErrorCode::ContentUnavailable, {}};
        m_removing = true;
        return Success{};
    }
    Outcome finishRemoval(const QUuid &) override { return Success{}; }
    Outcome prepare(const RequestContext &context) override
    { ++migrations; if (const auto error = context.interruption()) return *error; return Success{}; }
    Result<ImportToken> beginRefresh(const CatalogScope &scope, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        ImportToken token{QUuid::createUuid(), scope, context.source, context};
        m_latest[scopeKey(scope)] = token.id;
        m_staging[token.id] = {token, {}, false};
        return token;
    }
    Outcome stageBatch(const ImportToken &token, const CatalogBatch &batch) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(token.request)) return *error;
        if (!m_staging.contains(token.id) || batch.scope != token.scope || m_latest.value(scopeKey(token.scope)) != token.id)
            return Error{ErrorCode::Cancelled, token.request.operationId};
        auto &stage = m_staging[token.id];
        stage.items.append(batch.items);
        stage.complete = batch.complete;
        return Success{};
    }
    Result<quint64> publishIfCurrent(const ImportToken &token, bool complete) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(token.request)) return *error;
        if (!m_staging.contains(token.id) || m_latest.value(scopeKey(token.scope)) != token.id)
            return Error{ErrorCode::Cancelled, token.request.operationId};
        const auto stage = m_staging.value(token.id);
        if (!complete || !stage.complete) return Error{ErrorCode::InvalidResponse, token.request.operationId};
        const auto generation = ++m_generation;
        m_pages[scopeKey(token.scope)] = {token.scope, generation, stage.items, {}, QDateTime::currentDateTimeUtc()};
        m_staging.remove(token.id);
        ++publications;
        return generation;
    }
    Outcome abandonRefresh(const ImportToken &token) noexcept override
    { QMutexLocker lock(&m_mutex); m_staging.remove(token.id); return Success{}; }
    Result<CatalogPage> query(const CatalogQuery &query, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        auto page = m_pages.value(scopeKey(query.scope), CatalogPage{query.scope});
        if (query.page && query.page->generation != page.generation) return Error{ErrorCode::Cancelled, context.operationId};
        auto title = [](const CatalogItem &item) { return std::visit([](const auto &value) { return value.title; }, item); };
        auto identity = [](const CatalogItem &item) { return std::visit([](const auto &value) { return value.ref.key(); }, item); };
        std::sort(page.items.begin(), page.items.end(), [&](const auto &a, const auto &b) {
            const auto left = std::make_pair(title(a), identity(a));
            const auto right = std::make_pair(title(b), identity(b));
            return query.sort == CatalogSort::TitleAscending ? left < right : left > right;
        });
        QList<CatalogItem> filtered;
        bool pastCursor = !query.page;
        for (const auto &item : page.items) {
            if (!pastCursor) {
                if (identity(item) == query.page->lastIdentity) pastCursor = true;
                continue;
            }
            if (query.continueWatchingOnly) {
                const auto progress = m_progress.value(identity(item));
                if (progress.positionMs <= 0 || progress.status == WatchStatus::Watched
                    || watchedByPosition(progress.positionMs, progress.durationMs)) continue;
            }
            if (title(item).startsWith(query.titlePrefix)) filtered.append(item);
        }
        page.items = filtered.mid(0, query.pageSize);
        if (filtered.size() > query.pageSize && !page.items.isEmpty()) {
            const auto &last = page.items.constLast();
            page.next = LocalPageToken{page.generation, title(last), identity(last)};
        } else page.next.reset();
        return page;
    }
    Result<std::optional<CategorySnapshot>> readCategories(const CatalogScope &scope, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        if (!m_categories.contains(scopeKey(scope))) return std::optional<CategorySnapshot>{};
        return std::optional<CategorySnapshot>{m_categories.value(scopeKey(scope))};
    }
    Outcome beginCategoryRefresh(const CatalogScope &scope, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        m_categoryRequests[scopeKey(scope)] = context.operationId;
        return Success{};
    }
    Outcome storeCategories(const CategorySnapshot &snapshot, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        if (m_categoryRequests.value(scopeKey(snapshot.scope)) != context.operationId)
            return Error{ErrorCode::Cancelled, context.operationId};
        m_categoryRequests.remove(scopeKey(snapshot.scope));
        m_categories[scopeKey(snapshot.scope)] = snapshot;
        return Success{};
    }
    Result<std::optional<VodDetails>> readDetails(const ContentRef &ref, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        if (!m_details.contains(ref.key())) return std::optional<VodDetails>{};
        return std::optional<VodDetails>{m_details.value(ref.key())};
    }
    Outcome storeDetails(const VodDetails &details, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        m_details[details.ref.key()] = details;
        return Success{};
    }
    Outcome evictCache(const CatalogScope &scope, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        m_pages.remove(scopeKey(scope)); m_categories.remove(scopeKey(scope)); m_categoryRequests.remove(scopeKey(scope)); m_details.clear(); return Success{};
    }
    Result<std::optional<VodProgress>> read(const ContentRef &ref, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        if (!m_progress.contains(ref.key())) return std::optional<VodProgress>{};
        return std::optional<VodProgress>{m_progress.value(ref.key())};
    }
    Outcome beginSession(const ContentRef &ref, const QUuid &session, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        m_sessions[ref.key()] = session;
        m_sequences[ref.key()] = 0;
        return Success{};
    }
    Outcome checkpoint(const ContentRef &ref, const VodProgress &progress, const RequestContext &context) override
    {
        QMutexLocker lock(&m_mutex);
        if (const auto error = check(context)) return *error;
        if (m_sessions.value(ref.key()) != progress.sessionToken || progress.sequence <= m_sequences.value(ref.key())
            || progress.positionMs < 0) return Error{ErrorCode::Cancelled, context.operationId};
        m_sequences[ref.key()] = progress.sequence;
        m_progress[ref.key()] = progress;
        ++checkpoints;
        return Success{};
    }
    Outcome removeSourceState(const QUuid &id) override
    {
        QMutexLocker lock(&m_mutex);
        if (!m_removing || id != m_source.revision.profileId) return Error{ErrorCode::StorageUnavailable, {}};
        m_pages.clear(); m_categories.clear(); m_categoryRequests.clear(); m_details.clear(); m_progress.clear(); m_sessions.clear(); m_sequences.clear(); m_staging.clear();
        return Success{};
    }
private:
    std::optional<Error> check(const RequestContext &context)
    {
        if (const auto error = context.interruption()) return error;
        if (!isCurrent(context.source)) return Error{ErrorCode::Cancelled, context.operationId};
        return {};
    }
    struct Stage { ImportToken token; QList<CatalogItem> items; bool complete; };
    QRecursiveMutex m_mutex;
    SourceContext m_source;
    bool m_removing = false;
    quint64 m_generation = 0;
    QHash<QByteArray, QUuid> m_latest;
    QHash<QUuid, Stage> m_staging;
    QHash<QByteArray, CatalogPage> m_pages;
    QHash<QByteArray, VodDetails> m_details;
    QHash<QByteArray, CategorySnapshot> m_categories;
    QHash<QByteArray, QUuid> m_categoryRequests;
    QHash<QByteArray, VodProgress> m_progress;
    QHash<QByteArray, QUuid> m_sessions;
    QHash<QByteArray, quint64> m_sequences;
};
class Provider final : public IVodProvider {
public:
    std::atomic_int calls{0};
    std::atomic_bool calledOnGui{false};
    std::function<Result<QList<VodCategory>>(const CatalogScope &)> categories;
    std::function<Result<CatalogBatch>(const CatalogScope &, const RequestContext &)> fetch;
    std::function<Result<PlaybackDescriptor>(const SourceContext &, const ContentRef &)> resolve;
    void called() { ++calls; if (QThread::isMainThread()) calledOnGui = true; }
    Result<ProviderCapabilities> capabilities(const SourceContext &, const RequestContext &) override
    { called(); ProviderCapabilities result; result.movies = Capability::Supported; return result; }
    Result<QList<VodCategory>> listCategories(const SourceContext &, const CatalogScope &scope, const RequestContext &) override
    { called(); return categories ? categories(scope) : Result<QList<VodCategory>>{QList<VodCategory>{}}; }
    Result<CatalogBatch> fetchCatalog(const SourceContext &, const CatalogScope &scope,
        const std::optional<ProviderCursor> &, const RequestContext &context) override
    { called(); return fetch ? fetch(scope, context) : Result<CatalogBatch>{CatalogBatch{scope, {}, true, {}}}; }
    Result<VodDetails> fetchDetails(const SourceContext &, const ContentRef &ref, const RequestContext &) override
    { called(); VodDetails details; details.ref = ref; return details; }
    Result<PlaybackDescriptor> resolvePlayback(const SourceContext &source, const ContentRef &ref,
        const PlaybackPreferences &, const RequestContext &) override
    { called(); return resolve ? resolve(source, ref) : Result<PlaybackDescriptor>{descriptorFor(source, ref)}; }
};
class Engine final : public Player::IPlaybackEngine {
public:
    Listener listener;
    int loads = 0;
    int stops = 0;
    int seeks = 0;
    int pauses = 0;
    int resumes = 0;
    bool acknowledgeStop = true;
    QUuid token;
    Player::PlaybackRequest request;
    qint64 seekPosition = -1;
    void setListener(Listener callback) override { listener = std::move(callback); }
    void load(const Player::PlaybackRequest &value, const QUuid &load) override { ++loads; request = value; token = load; }
    void pause() override { ++pauses; }
    void resume() override { ++resumes; }
    void seek(qint64 position) override { ++seeks; seekPosition = position; }
    void stop(Player::EndReason reason) override { ++stops; if (acknowledgeStop) emitEnd(token, reason); }
    void setVolume(double) override {}
    void *renderHandle() const override { return nullptr; }
    void emitEnd(const QUuid &load, Player::EndReason reason)
    { Player::PlaybackEvent event; event.loadToken = load; event.end = reason; if (listener) listener(event); }
    void emitState(Player::EngineState state, qint64 position = 0, std::optional<qint64> duration = 120000, bool seekable = true)
    { if (listener) listener({token, state, position, duration, seekable, false, {}}); }
    void emitSeekCompleted(qint64 position)
    { if (listener) listener({token, Player::EngineState::Playing, position, 120000, true, true, {}}); }
};
struct Fixture {
    SourceContext source = makeSource();
    std::shared_ptr<MemoryStore> store = std::make_shared<MemoryStore>(source);
    std::shared_ptr<Provider> provider = std::make_shared<Provider>();
    Engine *engine = nullptr;
    int releases = 0;
    int liveActivations = 0;
    LegacyResources resources;
    bool deferRelease = false;
    LegacyPlaybackAdapter::Acknowledgement releaseAck;
    VodDependencies dependencies() const { return {provider, store, store, store, store}; }
    VodComposition composition()
    {
        auto legacy = std::make_shared<LegacyPlaybackAdapter>(LegacyPlaybackAdapter::Hooks{
            [this]() { return resources; },
            [this](const QUuid &, LegacyPlaybackAdapter::Acknowledgement ack) {
                ++releases;
                if (deferRelease) releaseAck = std::move(ack); else ack(Success{});
            }, [this]() { ++liveActivations; }});
        return {dependencies(), legacy, [this]() { auto value = std::make_unique<Engine>(); engine = value.get(); return value; }};
    }
};
}
