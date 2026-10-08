#include "core/vod/storage/sqlitevodstore.h"
#include "app/vod/vodsubtitlecache.h"
#include <QJsonArray>
#include <QCryptographicHash>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

using namespace OKILTV::Vod;
namespace {
struct Fixture {
    QTemporaryDir directory;
    QString path = directory.filePath(QStringLiteral("iptv.db"));
    SourceContext config{{QUuid::createUuid(), {}, 1}, QStringLiteral("xtream"), true,
        QUrl(QStringLiteral("https://fixture.invalid")), QStringLiteral("synthetic-user"), QStringLiteral("synthetic-password")};
    std::shared_ptr<SqliteVodStore> store;
    SourceContext source;
    Fixture() { reopen(); }
    void reopen()
    {
        store = std::make_shared<SqliteVodStore>(path, [this](const QUuid &) -> Result<SourceContext> { return config; });
        const auto snapshot = store->snapshot(config.revision.profileId);
        if (std::holds_alternative<SourceContext>(snapshot)) source = std::get<SourceContext>(snapshot);
    }
    RequestContext request() const { RequestContext value; value.source = source.revision; return value; }
    CatalogScope scope() const { return {source.revision.profileId, source.revision.catalogNamespace, CatalogKind::Movies, {}}; }
    MovieSummary movie(QString id = QStringLiteral("007")) const
    {
        MovieSummary value;
        value.ref = {source.revision.profileId, source.revision.catalogNamespace, ContentKind::Movie, std::move(id), {}};
        value.title = QStringLiteral("Same title"); value.categoryIds = {QStringLiteral("a"), QStringLiteral("b")};
        return value;
    }
    Result<quint64> publish(const CatalogScope &scope, const QList<CatalogItem> &items)
    {
        const auto begun = store->beginRefresh(scope, request());
        if (const auto *error = std::get_if<Error>(&begun)) return *error;
        const auto token = std::get<ImportToken>(begun);
        const auto staged = store->stageBatch(token, {scope, items, true, {}});
        if (const auto *error = std::get_if<Error>(&staged)) return *error;
        return store->publishIfCurrent(token, true);
    }
};
QVariant sqlValue(const QString &path, const QString &statement)
{
    const auto name = QUuid::createUuid().toString();
    QVariant value;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (db.open()) { QSqlQuery query(db); if (query.exec(statement) && query.next()) value = query.value(0); }
    }
    QSqlDatabase::removeDatabase(name);
    return value;
}
void sql(const QString &path, const QString &statement)
{
    const auto name = QUuid::createUuid().toString();
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db); QVERIFY(query.exec(statement));
    }
    QSqlDatabase::removeDatabase(name);
}
}
class VodStorageTests : public QObject {
    Q_OBJECT
private slots:
    void unchangedSourceSnapshotRemainsReadableDuringImport() {
        Fixture fixture;
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {fixture.movie()})));
        const auto name = QUuid::createUuid().toString();
        const auto remove = qScopeGuard([&]() { QSqlDatabase::removeDatabase(name); });
        auto writer = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        writer.setDatabaseName(fixture.path); QVERIFY(writer.open());
        QSqlQuery lock(writer); QVERIFY(lock.exec(QStringLiteral("BEGIN IMMEDIATE")));
        // Ordinary reads must not acquire a second write reservation, even
        // after an in-process invalidation discarded the cached revision.
        fixture.store->invalidate(fixture.source.revision.profileId);
        const auto snapshot = fixture.store->snapshot(fixture.source.revision.profileId);
        QVERIFY(std::holds_alternative<SourceContext>(snapshot));
        QCOMPARE(std::get<SourceContext>(snapshot).revision, fixture.source.revision);
        QVERIFY(fixture.store->isCurrent(fixture.source.revision));
        CatalogQuery query; query.scope = fixture.scope();
        const auto page = fixture.store->query(query, fixture.request());
        QVERIFY(std::holds_alternative<CatalogPage>(page));
        QCOMPARE(std::get<CatalogPage>(page).items.size(), 1);
        QVERIFY(lock.exec(QStringLiteral("ROLLBACK")));
    }
    void sqliteBusyIsDistinctFromStorageFailure() {
        Fixture fixture;
        const auto name = QUuid::createUuid().toString();
        const auto remove = qScopeGuard([&]() { QSqlDatabase::removeDatabase(name); });
        auto writer = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        writer.setDatabaseName(fixture.path); QVERIFY(writer.open());
        QSqlQuery lock(writer); QVERIFY(lock.exec(QStringLiteral("BEGIN IMMEDIATE")));
        auto request = fixture.request(); request.deadline = QDeadlineTimer(100);
        const auto blocked = fixture.store->beginRefresh(fixture.scope(), request);
        QVERIFY(std::holds_alternative<Error>(blocked));
        QCOMPARE(std::get<Error>(blocked).code, ErrorCode::StorageBusy);
        QVERIFY(std::get<Error>(blocked).retryable);
        QVERIFY(lock.exec(QStringLiteral("ROLLBACK")));
        QVERIFY(lock.exec(QStringLiteral("DROP TABLE vod_category_snapshots")));
        const auto broken = fixture.store->readCategories(fixture.scope(), fixture.request());
        QVERIFY(std::holds_alternative<Error>(broken));
        QCOMPARE(std::get<Error>(broken).code, ErrorCode::StorageUnavailable);
        QVERIFY(!std::get<Error>(broken).retryable);
    }
    void uploadedSubtitlesSurviveRestartAndDoNotCreateProgress() {
        Fixture fixture;
        const auto ref = fixture.movie().ref;
        const auto root = fixture.directory.filePath(QStringLiteral("subtitles"));
        const auto original = fixture.directory.filePath(QStringLiteral("Polskie napisy.srt"));
        QFile file(original); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("1\n00:00:00,000 --> 00:00:05,000\nTest\n"); file.close();
        VodSubtitleCache cache(root, fixture.store);
        const auto imported = cache.importFile(ref, QUrl::fromLocalFile(original), fixture.request());
        QVERIFY(std::holds_alternative<QJsonObject>(imported));
        auto state = std::get<QJsonObject>(imported);
        QCOMPARE(state.value(QStringLiteral("files")).toArray().size(), 1);
        const auto id = state.value(QStringLiteral("selection")).toObject().value(QStringLiteral("externalId")).toString();
        const auto copied = cache.playbackFiles(ref, state).first().toMap().value(QStringLiteral("path")).toString();
        QVERIFY(QFileInfo::exists(copied));
        auto duplicate = cache.importFile(ref, QUrl::fromLocalFile(original), fixture.request());
        QCOMPARE(std::get<QJsonObject>(duplicate).value(QStringLiteral("files")).toArray().size(), 1);
        auto progress = fixture.store->read(ref, fixture.request());
        QVERIFY(std::holds_alternative<std::optional<VodProgress>>(progress));
        QVERIFY(!std::get<std::optional<VodProgress>>(progress));
        QVERIFY(QFile::remove(original));
        fixture.reopen();
        VodSubtitleCache reopened(root, fixture.store);
        state = std::get<QJsonObject>(reopened.read(ref, fixture.request()));
        QCOMPARE(state.value(QStringLiteral("selection")).toObject().value(QStringLiteral("externalId")).toString(), id);
        QVERIFY(QFileInfo::exists(copied));
        QVERIFY(std::holds_alternative<Success>(fixture.store->evictCache(fixture.scope(), fixture.request())));
        QCOMPARE(std::get<QJsonObject>(reopened.read(ref, fixture.request())), state);
        const auto removed = reopened.remove(ref, id, fixture.request());
        QVERIFY(std::holds_alternative<QJsonObject>(removed));
        QCOMPARE(std::get<QJsonObject>(removed).value(QStringLiteral("selection")).toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("off"));
        QVERIFY(!QFileInfo::exists(copied));
    }
    void subtitleCacheMigratesLongLegacyPaths_data() {
        QTest::addColumn<bool>("hashedContent");
        QTest::addColumn<bool>("startup");
        QTest::newRow("expanded-read") << false << false;
        QTest::newRow("hashed-read") << true << false;
        QTest::newRow("expanded-startup") << false << true;
        QTest::newRow("hashed-startup") << true << true;
    }
    void subtitleCacheMigratesLongLegacyPaths() {
        QFETCH(bool, hashedContent); QFETCH(bool, startup);
        Fixture fixture;
        auto ref = fixture.movie().ref;
        ref.kind = ContentKind::Episode; ref.providerItemId = QStringLiteral("2296"); ref.parentNamespace = QStringLiteral("56111");
        const auto root = fixture.directory.filePath(QStringLiteral("vod-subtitles"));
        const auto original = fixture.directory.filePath(QStringLiteral("local.srt"));
        QFile file(original); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes("1\n00:00:00,000 --> 00:00:05,000\nLocal subtitles\n");
        file.write(bytes); file.close();
        VodSubtitleCache cache(root, fixture.store);
        auto imported = cache.importFile(ref, QUrl::fromLocalFile(original), fixture.request());
        QVERIFY(std::holds_alternative<QJsonObject>(imported));
        const auto firstState = std::get<QJsonObject>(imported);
        const auto id = firstState.value(QStringLiteral("files")).toArray().first().toObject().value(QStringLiteral("id")).toString();
        const auto copied = cache.playbackFiles(ref, firstState).first().toMap().value(QStringLiteral("path")).toString();
        const auto importDir = QFileInfo(copied).dir();
        QCOMPARE(importDir.dirName().size(), 64);
        const auto relative = QDir(root).relativeFilePath(copied);
        QCOMPARE(relative.split(u'/').size(), 2); // One hash directory, directly below vod-subtitles.
        QVERIFY((QStringLiteral("C:/Users/Jabuk/AppData/Roaming/OKILTV/vod-subtitles/") + relative).size() < 150);
        // Keep a second file in the new layout while migrating the first one.
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write(bytes + "\n"); file.close();
        imported = cache.importFile(ref, QUrl::fromLocalFile(original), fixture.request());
        QVERIFY(std::holds_alternative<QJsonObject>(imported));
        const auto state = std::get<QJsonObject>(imported);
        const auto secondCopy = cache.playbackFiles(ref, state).last().toMap().value(QStringLiteral("path")).toString();
        QVERIFY(secondCopy != copied);
        const auto content = QString::fromLatin1(hashedContent
            ? QCryptographicHash::hash(ref.key(), QCryptographicHash::Sha256).toHex() : ref.key().toHex());
        const auto legacy = root + u'/' + ref.profileId.toString(QUuid::WithoutBraces) + u'/' + content;
        QVERIFY(QDir().mkpath(legacy));
        QVERIFY(QDir().rename(importDir.absolutePath(), QDir(legacy).filePath(id)));
        QVERIFY(QFile::remove(original));
        fixture.reopen();
        VodSubtitleCache reopened(root, fixture.store);
        if (startup) reopened.reconcile({ref.profileId});
        QCOMPARE(std::get<QJsonObject>(reopened.read(ref, fixture.request())), state);
        QCOMPARE(reopened.playbackFiles(ref, state).first().toMap().value(QStringLiteral("path")).toString(), copied);
        QVERIFY(!QFileInfo::exists(legacy)); QVERIFY(QFileInfo::exists(secondCopy));
        QFile restored(copied); QVERIFY(restored.open(QIODevice::ReadOnly)); QCOMPARE(restored.readAll(), bytes); restored.close();
        reopened.reconcile({ref.profileId}); // Repeated reconciliation preserves migrated copies and selections.
        QVERIFY(QFileInfo::exists(copied)); QVERIFY(QFileInfo::exists(secondCopy));
        QCOMPARE(std::get<QJsonObject>(reopened.read(ref, fixture.request())), state);
    }
    void subtitleFlatCacheRemovalPreservesOtherSources() {
        Fixture fixture;
        const auto firstRef = fixture.movie().ref;
        const auto root = fixture.directory.filePath(QStringLiteral("vod-subtitles"));
        const auto original = fixture.directory.filePath(QStringLiteral("local.srt"));
        QFile file(original); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("1\n00:00:00,000 --> 00:00:05,000\nLocal subtitles\n"); file.close();
        VodSubtitleCache cache(root, fixture.store);
        const auto first = cache.importFile(firstRef, QUrl::fromLocalFile(original), fixture.request());
        QVERIFY(std::holds_alternative<QJsonObject>(first));
        const auto firstCopy = cache.playbackFiles(firstRef, std::get<QJsonObject>(first)).first().toMap().value(QStringLiteral("path")).toString();
        fixture.config.revision.profileId = QUuid::createUuid(); fixture.reopen();
        const auto secondRef = fixture.movie().ref;
        VodSubtitleCache reopened(root, fixture.store);
        const auto second = reopened.importFile(secondRef, QUrl::fromLocalFile(original), fixture.request());
        QVERIFY(std::holds_alternative<QJsonObject>(second));
        const auto secondState = std::get<QJsonObject>(second);
        const auto secondCopy = reopened.playbackFiles(secondRef, secondState).first().toMap().value(QStringLiteral("path")).toString();
        QVERIFY(firstCopy != secondCopy);
        QVERIFY(std::holds_alternative<Success>(fixture.store->prepareRemoval(firstRef.profileId)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->removeSourceState(firstRef.profileId)));
        reopened.removeSource(firstRef.profileId);
        QVERIFY(!QFileInfo::exists(firstCopy)); QVERIFY(QFileInfo::exists(secondCopy));
        QCOMPARE(std::get<QJsonObject>(reopened.read(secondRef, fixture.request())), secondState);
        QVERIFY(QFileInfo::exists(original));
    }
    void subtitlePairsCancellationAndSourceIsolation() {
        Fixture fixture; const auto ref = fixture.movie().ref;
        const auto root = fixture.directory.filePath(QStringLiteral("subtitles"));
        VodSubtitleCache cache(root, fixture.store);
        const auto idx = fixture.directory.filePath(QStringLiteral("DVD.IDX"));
        const auto sub = fixture.directory.filePath(QStringLiteral("DVD.SUB"));
        QFile index(idx); QVERIFY(index.open(QIODevice::WriteOnly)); index.write("# VobSub index file, v7\n"); index.close();
        QVERIFY(std::holds_alternative<Error>(cache.importFile(ref, QUrl::fromLocalFile(idx), fixture.request())));
        QFile data(sub); QVERIFY(data.open(QIODevice::WriteOnly)); data.write(QByteArray::fromHex("000001ba00000000")); data.close();
        auto cancelled = fixture.request(); cancelled.cancelled->store(true);
        QVERIFY(std::holds_alternative<Error>(cache.importFile(ref, QUrl::fromLocalFile(idx), cancelled)));
        auto imported = cache.importFile(ref, QUrl::fromLocalFile(sub), fixture.request());
        QVERIFY(std::holds_alternative<QJsonObject>(imported));
        const auto copied = cache.playbackFiles(ref, std::get<QJsonObject>(imported)).first().toMap().value(QStringLiteral("path")).toString();
        QVERIFY(copied.endsWith(QStringLiteral(".idx")));
        QVERIFY(QFileInfo::exists(QFileInfo(copied).dir().filePath(QStringLiteral("subtitle.sub"))));
        auto episode = ref; episode.kind = ContentKind::Episode; episode.parentNamespace = QStringLiteral("series");
        QVERIFY(std::get<QJsonObject>(cache.read(episode, fixture.request())).isEmpty());
        const auto orphan = root + u'/' + ref.profileId.toString(QUuid::WithoutBraces) + u'/' + QString::fromLatin1(ref.key().toHex()) + QStringLiteral("/orphan");
        QVERIFY(QDir().mkpath(orphan));
        cache.reconcile({ref.profileId}); QVERIFY(!QFileInfo::exists(orphan)); QVERIFY(QFileInfo::exists(copied));
        QVERIFY(std::holds_alternative<Success>(fixture.store->prepareRemoval(ref.profileId)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->removeSourceState(ref.profileId)));
        cache.reconcile({}); QVERIFY(!QFileInfo::exists(copied));
        QVERIFY(QFileInfo::exists(idx)); QVERIFY(QFileInfo::exists(sub));
    }
    void subtitleMigrationAndFailedPublication() {
        Fixture fixture;
        sql(fixture.path, QStringLiteral("DROP TABLE vod_subtitles"));
        sql(fixture.path, QStringLiteral("DELETE FROM vod_schema_migrations WHERE version=11"));
        sql(fixture.path, QStringLiteral("INSERT OR IGNORE INTO vod_schema_migrations(version) VALUES(10)"));
        fixture.reopen();
        QCOMPARE(sqlValue(fixture.path, QStringLiteral("SELECT MAX(version) FROM vod_schema_migrations")).toInt(), 11);
        const auto ref = fixture.movie().ref;
        QJsonObject state{{QStringLiteral("selection"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("off")}}}};
        QVERIFY(std::holds_alternative<Success>(fixture.store->writeSubtitles(ref, state, fixture.request())));
        sql(fixture.path, QStringLiteral("CREATE TRIGGER fail_subtitles BEFORE UPDATE ON vod_subtitles BEGIN SELECT RAISE(ABORT,'fixture'); END"));
        QVERIFY(std::holds_alternative<Error>(fixture.store->writeSubtitles(ref, {}, fixture.request())));
        QCOMPARE(std::get<QJsonObject>(fixture.store->readSubtitles(ref, fixture.request())), state);
    }
    void seasonMediaMetadataUsesNewestCacheAndEpisodeOrder() {
        Fixture fixture;
        const auto movie = fixture.movie();
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {movie})));
        ContentRef series{movie.ref.profileId, movie.ref.catalogNamespace, ContentKind::Series, QStringLiteral("s"), {}};
        VodDetails details; details.ref = series;
        details.seasons = {{series, QStringLiteral("1"), 1, 0}, {series, QStringLiteral("2"), 2, 1}};
        for (int i = 0; i < 4; ++i) details.episodes.append({
            {series.profileId, series.catalogNamespace, ContentKind::Episode, QString::number(i), series.providerItemId},
            series, i == 3 ? QStringLiteral("2") : QStringLiteral("1"), i + 1, i, QString::number(i), Availability::Available});
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeDetails(details, fixture.request())));
        QVERIFY(!std::get<std::optional<VodMediaProbe>>(fixture.store->readSeasonMediaMetadata(series, QStringLiteral("1"), fixture.request())));
        const auto now = QDateTime::currentDateTimeUtc();
        auto storeProbe = [&](int index, QDateTime time, QString language) {
            auto episode = std::get<std::optional<VodDetails>>(fixture.store->readDetails(details.episodes.at(index).ref, fixture.request()));
            QVERIFY(episode);
            VodMediaProbe metadata; metadata.observedAtUtc = time;
            metadata.audioTracks.append({0, 0, QStringLiteral("audio"), QStringLiteral("aac"), {}, language});
            episode->mediaProbe = metadata;
            QVERIFY(std::holds_alternative<Success>(fixture.store->storeDetails(*episode, fixture.request())));
        };
        storeProbe(0, now.addSecs(-60), QStringLiteral("pol"));
        storeProbe(1, now, QStringLiteral("eng"));
        storeProbe(2, now, QStringLiteral("fra"));
        storeProbe(3, now.addSecs(60), QStringLiteral("spa"));
        fixture.reopen();
        auto metadata = std::get<std::optional<VodMediaProbe>>(fixture.store->readSeasonMediaMetadata(series, QStringLiteral("1"), fixture.request()));
        QVERIFY(metadata); QCOMPARE(metadata->audioTracks.first().language, QStringLiteral("eng"));
        metadata = std::get<std::optional<VodMediaProbe>>(fixture.store->readSeasonMediaMetadata(series, QStringLiteral("2"), fixture.request()));
        QVERIFY(metadata); QCOMPARE(metadata->audioTracks.first().language, QStringLiteral("spa"));
        QVERIFY(!std::get<std::optional<VodMediaProbe>>(fixture.store->readSeasonMediaMetadata(series, QStringLiteral("missing"), fixture.request())));
        auto otherSeries = series; otherSeries.providerItemId = QStringLiteral("other");
        QVERIFY(!std::get<std::optional<VodMediaProbe>>(fixture.store->readSeasonMediaMetadata(otherSeries, QStringLiteral("1"), fixture.request())));
        sql(fixture.path, QStringLiteral("UPDATE vod_details SET fetched=0"));
        QVERIFY(!std::get<std::optional<VodMediaProbe>>(fixture.store->readSeasonMediaMetadata(series, QStringLiteral("1"), fixture.request())));
        auto cancelled = fixture.request(); cancelled.cancelled->store(true);
        QVERIFY(std::holds_alternative<Error>(fixture.store->readSeasonMediaMetadata(series, QStringLiteral("1"), cancelled)));
    }
    void seriesHistoryListsProjectionAndMigration() {
        Fixture fixture;
        const auto movie=fixture.movie();
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(),{movie})));
        ContentRef series{movie.ref.profileId,movie.ref.catalogNamespace,ContentKind::Series,QStringLiteral("s"),{}};
        CatalogScope scope{series.profileId,series.catalogNamespace,CatalogKind::Series,{}};
        SeriesSummary summary{series,QStringLiteral("Series"),2026,{{QStringLiteral("poster"),QStringLiteral("poster"),{}}},{QStringLiteral("a")},Availability::Available};
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(scope,{summary})));
        VodDetails details;details.ref=series;details.seasons={{series,QStringLiteral("0"),0,0},{series,QStringLiteral("1"),1,1},{series,QStringLiteral("2"),2,2}};
        auto episode=[&](QString id,QString season,int number,int order) { return EpisodeSummary{{series.profileId,series.catalogNamespace,ContentKind::Episode,id,series.providerItemId},series,season,number,order,id,Availability::Available}; };
        const auto special=episode(QStringLiteral("sp"),QStringLiteral("0"),1,0),first=episode(QStringLiteral("1"),QStringLiteral("1"),1,1),last=episode(QStringLiteral("4"),QStringLiteral("2"),4,2);
        details.episodes={special,first,last};
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeDetails(details,fixture.request())));
        auto checkpoint=[&](ContentRef ref,qint64 position,bool completed=false,bool played=true) {
            VodProgress progress;progress.positionMs=position;progress.durationMs=100000;progress.sessionToken=QUuid::createUuid();progress.sequence=1;progress.updatedAtUtc=QDateTime::currentDateTimeUtc();progress.status=watchedByPosition(position,progress.durationMs) ? WatchStatus::Watched : WatchStatus::InProgress;progress.playbackCheckpoint=played;
            QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(ref,progress.sessionToken,fixture.request())));
            QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref,progress,fixture.request(),completed)));
        };
        auto history=[&]() { return std::get<SeriesProgress>(fixture.store->readSeriesProgress(series,fixture.request())); };
        CatalogQuery query;query.scope=scope;query.continueWatchingOnly=true;
        auto page=[&]() { return std::get<CatalogPage>(fixture.store->query(query,fixture.request())); };
        checkpoint(first.ref,95000);
        QCOMPARE(history().lastEpisode,std::optional<ContentRef>(first.ref)); QCOMPARE(page().items.size(),1);
        checkpoint(last.ref,100000,true,false);
        QCOMPARE(history().lastEpisode,std::optional<ContentRef>(first.ref));
        QCOMPARE(continueEpisode(details,history())->ref,first.ref);
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(series,MovieList::ToWatch,true,fixture.request())));
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(series,MovieList::Favourites,true,fixture.request())));
        checkpoint(first.ref,95001,true);
        QCOMPARE(continueEpisode(details,history())->ref,last.ref); // Watched next episode still participates.
        QCOMPARE(page().items.size(),1);
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(series,fixture.request())),(MovieListState{false,true}));
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(series,MovieList::ToWatch,true,fixture.request())));
        checkpoint(first.ref,99000,false);
        QVERIFY(std::get<MovieListState>(fixture.store->readMovieLists(series,fixture.request())).toWatch);
        checkpoint(last.ref,100000,true);
        QVERIFY(page().items.isEmpty());
        checkpoint(special.ref,70000);
        QCOMPARE(continueEpisode(details,history())->ref,special.ref);QCOMPARE(page().items.size(),1);
        checkpoint(special.ref,100000,true);QVERIFY(page().items.isEmpty());
        // Refresh can reveal a new successor without changing durable playback history.
        const auto added=episode(QStringLiteral("5"),QStringLiteral("2"),5,3);details.episodes.append(added);
        checkpoint(last.ref,100000);
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeDetails(details,fixture.request())));
        QCOMPARE(page().items.size(),1);QCOMPARE(continueEpisode(details,history())->ref,added.ref);
        query.allowedCategories=QStringList{QStringLiteral("other")};QVERIFY(page().items.isEmpty());query.allowedCategories.reset();
        query.continueWatchingOnly=false;query.movieList=MovieList::Favourites;query.pageSize=1;QCOMPARE(page().items.size(),1);QVERIFY(!page().next);
        QCOMPARE(std::get<SeriesSummary>(page().items.first()).artwork.first().id,QStringLiteral("poster"));
        // Recreate a genuine schema-8 database, without the new view/history table.
        sql(fixture.path,QStringLiteral("UPDATE vod_progress SET updated=4102444800000 WHERE provider_id='4' AND kind=2"));
        sql(fixture.path,QStringLiteral("DROP VIEW vod_series_continue"));
        sql(fixture.path,QStringLiteral("DROP TABLE vod_series_history"));
        // Additive migration preserves both catalogues, posters and reconstructs episode history.
        sql(fixture.path,QStringLiteral("DELETE FROM vod_schema_migrations WHERE version>=9"));
        sql(fixture.path,QStringLiteral("INSERT OR IGNORE INTO vod_schema_migrations VALUES(8)"));
        fixture.reopen();
        QCOMPARE(history().lastEpisode,std::optional<ContentRef>(last.ref));
        QCOMPARE(std::get<SeriesSummary>(page().items.first()).artwork.first().id,QStringLiteral("poster"));
        QCOMPARE(sqlValue(fixture.path,QStringLiteral("SELECT COUNT(*) FROM vod_items WHERE kind=0")).toInt(),1);
        QVERIFY(std::holds_alternative<Success>(fixture.store->evictCache(scope,fixture.request())));
        QCOMPARE(history().lastEpisode,std::optional<ContentRef>(last.ref));
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(series,fixture.request())),(MovieListState{false,true}));
    }
    void seriesWatchedAtomicHistorySuppressionAndMigration() {
        Fixture fixture;
        const ContentRef series{fixture.source.revision.profileId, fixture.source.revision.catalogNamespace, ContentKind::Series, QStringLiteral("series"), {}};
        const CatalogScope scope{series.profileId, series.catalogNamespace, CatalogKind::Series, {}};
        const SeriesSummary summary{series, QStringLiteral("Series"), {}, {}, {QStringLiteral("a")}, Availability::Available};
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(scope, {summary})));
        VodDetails details; details.ref = series;
        details.seasons = {{series, QStringLiteral("0"), 0, 0}, {series, QStringLiteral("1"), 1, 1}};
        const auto episode = [&](const QString &id, const QString &season, int number) {
            return EpisodeSummary{{series.profileId, series.catalogNamespace, ContentKind::Episode, id, series.providerItemId}, series, season, number, number, id, Availability::Available};
        };
        const auto first = episode(QStringLiteral("1"), QStringLiteral("1"), 1);
        auto last = episode(QStringLiteral("2"), QStringLiteral("1"), 2); last.availability = Availability::Unavailable;
        const auto special = episode(QStringLiteral("sp"), QStringLiteral("0"), 1);
        details.episodes = {first, last, special};
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeDetails(details, fixture.request())));
        VodProgress active; active.sessionToken = QUuid::createUuid(); active.sequence = 1;
        active.positionMs = 70000; active.durationMs = 100000; active.updatedAtUtc = QDateTime::currentDateTimeUtc();
        active.trackPreferences = QJsonObject{{QStringLiteral("sub"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("off")}}}};
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(first.ref, active.sessionToken, fixture.request())));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(first.ref, active, fixture.request())));
        const auto historyTime = sqlValue(fixture.path, QStringLiteral("SELECT updated FROM vod_series_history"));
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(series, MovieList::ToWatch, true, fixture.request())));
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(series, MovieList::Favourites, true, fixture.request())));
        // Migrate an actual schema-9 database, retaining progress, tracks and playback history.
        sql(fixture.path, QStringLiteral("DROP TABLE vod_series_state"));
        sql(fixture.path, QStringLiteral("DELETE FROM vod_schema_migrations WHERE version>=10"));
        sql(fixture.path, QStringLiteral("INSERT OR IGNORE INTO vod_schema_migrations VALUES(9)"));
        sql(fixture.path, QStringLiteral("CREATE TRIGGER fail_v10 BEFORE INSERT ON vod_schema_migrations WHEN NEW.version>=10 BEGIN SELECT RAISE(ABORT,'synthetic'); END"));
        fixture.reopen();
        QVERIFY(std::holds_alternative<Error>(fixture.store->prepare({})));
        QCOMPARE(sqlValue(fixture.path, QStringLiteral("SELECT MAX(version) FROM vod_schema_migrations")).toInt(), 9);
        QCOMPARE(sqlValue(fixture.path, QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name='vod_series_state'")).toInt(), 0);
        sql(fixture.path, QStringLiteral("DROP TRIGGER fail_v10"));
        fixture.reopen();
        QCOMPARE(sqlValue(fixture.path, QStringLiteral("SELECT MAX(version) FROM vod_schema_migrations")).toInt(), schemaVersion);
        auto history = [&]() { return std::get<SeriesProgress>(fixture.store->readSeriesProgress(series, fixture.request())); };
        CatalogQuery query; query.scope = scope; query.continueWatchingOnly = true;
        auto page = [&]() { return std::get<CatalogPage>(fixture.store->query(query, fixture.request())); };
        QVERIFY(!history().continuationHidden); QCOMPARE(page().items.size(), 1);
        SeriesWatchedChange change; change.series = series; change.episodes = {first.ref, last.ref, special.ref}; change.watched = true;
        change.blockedSession = active.sessionToken; change.activeEpisode = first.ref;
        active.sequence = 2; change.activeProgress = active; change.activeSequence = active.sequence;
        // A write failure after the first row rolls back every row, memberships and suppression.
        sql(fixture.path, QStringLiteral("CREATE TRIGGER fail_series_mark BEFORE UPDATE OF position ON vod_progress WHEN NEW.provider_id='2' BEGIN SELECT RAISE(ABORT,'synthetic'); END"));
        QVERIFY(std::holds_alternative<Error>(fixture.store->setSeriesWatched(change, fixture.request())));
        QVERIFY(!seriesWatched(details, history())); QVERIFY(!history().continuationHidden);
        QCOMPARE(history().episodes.value(first.ref.key()).positionMs, qint64(70000));
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(series, fixture.request())), (MovieListState{true, true}));
        sql(fixture.path, QStringLiteral("DROP TRIGGER fail_series_mark"));
        QVERIFY(std::holds_alternative<Success>(fixture.store->setSeriesWatched(change, fixture.request())));
        QVERIFY(seriesWatched(details, history()));
        for (const auto &item : details.episodes) QCOMPARE(history().episodes.value(item.ref.key()).status, WatchStatus::Watched);
        QCOMPARE(history().episodes.value(first.ref.key()).trackPreferences, active.trackPreferences);
        QCOMPARE(history().lastEpisode, std::optional<ContentRef>(first.ref));
        QCOMPARE(sqlValue(fixture.path, QStringLiteral("SELECT updated FROM vod_series_history")), historyTime);
        QVERIFY(history().continuationHidden); QVERIFY(page().items.isEmpty());
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(series, fixture.request())), (MovieListState{false, true}));
        fixture.reopen(); QVERIFY(history().continuationHidden);
        active.sequence = 3; active.status = WatchStatus::Watched;
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(first.ref, active, fixture.request())));
        QVERIFY(history().continuationHidden); // The session already running at marking time cannot re-add it.
        change.watched = false; active.sequence = 4; change.activeProgress = active; change.activeSequence = active.sequence;
        QVERIFY(std::holds_alternative<Success>(fixture.store->setSeriesWatched(change, fixture.request())));
        for (const auto &item : details.episodes) {
            const auto progress = history().episodes.value(item.ref.key());
            QCOMPARE(progress.status, WatchStatus::InProgress); QCOMPARE(progress.positionMs, qint64(0));
        }
        QCOMPARE(history().episodes.value(first.ref.key()).trackPreferences, active.trackPreferences);
        QVERIFY(history().continuationHidden);
        // Cache eviction and a later catalogue refresh do not remove durable suppression.
        QVERIFY(std::holds_alternative<Success>(fixture.store->evictCache(scope, fixture.request())));
        QVERIFY(history().continuationHidden);
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(scope, {summary})));
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeDetails(details, fixture.request())));
        active.sessionToken = QUuid::createUuid(); active.sequence = 1; active.status = WatchStatus::InProgress; active.positionMs = 20000;
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(first.ref, active.sessionToken, fixture.request())));
        QVERIFY(history().continuationHidden); // Opening alone is not confirmed playback.
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(first.ref, active, fixture.request())));
        QVERIFY(!history().continuationHidden); QCOMPARE(page().items.size(), 1);
        QVERIFY(std::holds_alternative<Success>(fixture.store->prepareRemoval(series.profileId)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->removeSourceState(series.profileId)));
        QCOMPARE(sqlValue(fixture.path, QStringLiteral("SELECT COUNT(*) FROM vod_series_state")).toInt(), 0);
    }
    void allowedCategoriesBeforePaginationAndSpecialLists()
    {
        Fixture fixture;
        QList<CatalogItem> items;
        for (int i = 0; i < 20; ++i) {
            auto movie = fixture.movie(QString::number(i));
            movie.title = QStringLiteral("Movie %1").arg(i, 2, 10, QLatin1Char('0'));
            movie.categoryIds = {i < 18 ? QStringLiteral("excluded") : QStringLiteral("allowed")};
            items.append(movie);
        }
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), items)));
        for (const auto &item : items) {
            const auto ref = std::get<MovieSummary>(item).ref;
            const auto token = QUuid::createUuid();
            QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(ref, token, fixture.request())));
            VodProgress progress; progress.sessionToken = token; progress.sequence = 1; progress.positionMs = 10000; progress.durationMs = 100000; progress.updatedAtUtc = QDateTime::currentDateTimeUtc();
            QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, fixture.request())));
            QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(ref, MovieList::ToWatch, true, fixture.request())));
            QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(ref, MovieList::Favourites, true, fixture.request())));
        }
        CatalogQuery query; query.scope = fixture.scope(); query.allowedCategories = QStringList{QStringLiteral("allowed")}; query.pageSize = 1; query.titleContains = QStringLiteral("Movie");
        for (const auto list : {MovieList::None, MovieList::ToWatch, MovieList::Favourites}) {
            query.movieList = list; query.page.reset();
            const auto first = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
            QCOMPARE(first.items.size(), 1); QCOMPARE(std::get<MovieSummary>(first.items.first()).ref.providerItemId, QStringLiteral("18")); QVERIFY(first.next);
            query.page = first.next;
            const auto second = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
            QCOMPARE(second.items.size(), 1); QCOMPARE(std::get<MovieSummary>(second.items.first()).ref.providerItemId, QStringLiteral("19")); QVERIFY(!second.next);
        }
        query.page.reset(); query.continueWatchingOnly = true;
        auto first = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(first.items.size(), 1); QVERIFY(std::get<MovieSummary>(first.items.first()).categoryIds.contains(QStringLiteral("allowed")));
        query.allowedCategories = QStringList{}; query.page.reset();
        QVERIFY(std::get<CatalogPage>(fixture.store->query(query, fixture.request())).items.isEmpty());
    }
    void policyGenerationRejectsLatePublication()
    {
        Fixture fixture;
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {fixture.movie()})));
        auto request = fixture.request();
        const auto generation = std::make_shared<std::atomic_int>(1);
        request.policyMutex = std::make_shared<std::recursive_mutex>();
        request.policyCurrent = [generation]() { return generation->load() == 1; };
        const auto token = std::get<ImportToken>(fixture.store->beginRefresh(fixture.scope(), request));
        QVERIFY(std::holds_alternative<Success>(fixture.store->stageBatch(token, {fixture.scope(), {}, true, {}})));
        { std::lock_guard lock(*request.policyMutex); ++*generation; }
        const auto result = fixture.store->publishIfCurrent(token, true);
        QVERIFY(std::holds_alternative<Error>(result)); QCOMPARE(std::get<Error>(result).code, ErrorCode::Cancelled);
        CatalogQuery query; query.scope = fixture.scope();
        QCOMPARE(std::get<CatalogPage>(fixture.store->query(query, fixture.request())).items.size(), 1);
        QVERIFY(std::holds_alternative<Success>(fixture.store->abandonRefresh(token)));
        // Category snapshots obey the same policy fence and retain prior data.
        const CategorySnapshot categories{fixture.scope(), {{fixture.scope(), QStringLiteral("1"), QStringLiteral("One"), {}}}, QDateTime::currentDateTimeUtc()};
        auto initial = fixture.request();
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginCategoryRefresh(fixture.scope(), initial)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeCategories(categories, initial)));
        generation->store(1);
        request.operationId = QUuid::createUuid();
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginCategoryRefresh(fixture.scope(), request)));
        { std::lock_guard lock(*request.policyMutex); ++*generation; }
        QVERIFY(std::holds_alternative<Error>(fixture.store->storeCategories({fixture.scope(), {}, QDateTime::currentDateTimeUtc()}, request)));
        QCOMPARE(std::get<std::optional<CategorySnapshot>>(fixture.store->readCategories(fixture.scope(), fixture.request()))->categories.size(), 1);
    }
    void movieListsDurabilityAndMigration()
    {
        Fixture fixture;
        const auto ref = fixture.movie().ref;
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {fixture.movie()})));
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(ref, MovieList::ToWatch, true, fixture.request())));
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(ref, MovieList::Favourites, true, fixture.request())));
        fixture.reopen();
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(ref, fixture.request())), (MovieListState{true, true}));
        QVERIFY(std::holds_alternative<Success>(fixture.store->evictCache(fixture.scope(), fixture.request())));
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {})));
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(ref, fixture.request())), (MovieListState{true, true}));
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {fixture.movie()})));
        ++fixture.config.revision.credentialRevision;
        QVERIFY(std::holds_alternative<Success>(fixture.store->advanceCredentialRevision(ref.profileId, fixture.config.revision.credentialRevision)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->finishCredentialChange(ref.profileId)));
        fixture.reopen();
        QCOMPARE(fixture.source.revision.catalogNamespace, ref.catalogNamespace);
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(ref, fixture.request())), (MovieListState{true, true}));
        QVERIFY(std::holds_alternative<Success>(fixture.store->prepareRemoval(ref.profileId)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->removeSourceState(ref.profileId)));
        QCOMPARE(sqlValue(fixture.path, QStringLiteral("SELECT COUNT(*) FROM vod_movie_lists")).toInt(), 0);

        Fixture legacy;
        const auto legacyRef = legacy.movie().ref;
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 42000;
        QVERIFY(std::holds_alternative<Success>(legacy.store->beginSession(legacyRef, progress.sessionToken, legacy.request())));
        QVERIFY(std::holds_alternative<Success>(legacy.store->checkpoint(legacyRef, progress, legacy.request())));
        sql(legacy.path, QStringLiteral("DROP TABLE vod_movie_lists"));
        sql(legacy.path, QStringLiteral("DELETE FROM vod_schema_migrations WHERE version>=8"));
        sql(legacy.path, QStringLiteral("INSERT OR IGNORE INTO vod_schema_migrations VALUES(7)"));
        legacy.reopen();
        QCOMPARE(std::get<MovieListState>(legacy.store->readMovieLists(legacyRef, legacy.request())), MovieListState{});
        QCOMPARE(std::get<std::optional<VodProgress>>(legacy.store->read(legacyRef, legacy.request()))->positionMs, qint64(42000));
        QCOMPARE(sqlValue(legacy.path, QStringLiteral("SELECT MAX(version) FROM vod_schema_migrations")).toInt(), schemaVersion);
    }
    void movieListQueriesFilterBeforePagination()
    {
        Fixture fixture;
        QList<CatalogItem> items;
        for (int i = 0; i < 12; ++i) {
            auto movie = fixture.movie(QString::number(i)); movie.title = QStringLiteral("Film %1").arg(i, 2, 10, QChar(u'0'));
            items.append(movie);
            if (i % 2 == 0) QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(movie.ref, MovieList::ToWatch, true, fixture.request())));
            if (i % 3 == 0) QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(movie.ref, MovieList::Favourites, true, fixture.request())));
        }
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), items)));
        CatalogQuery query; query.scope = fixture.scope(); query.movieList = MovieList::ToWatch; query.pageSize = 2;
        QStringList ids;
        do {
            const auto result = fixture.store->query(query, fixture.request()); QVERIFY(std::holds_alternative<CatalogPage>(result));
            const auto page = std::get<CatalogPage>(result); QVERIFY(page.items.size() <= 2);
            for (const auto &item : page.items) { const auto ref = std::get<MovieSummary>(item).ref; ids.append(ref.providerItemId); QVERIFY(page.movieLists.value(ref.key()).toWatch); }
            query.page = page.next;
        } while (query.page);
        QCOMPARE(ids, (QStringList{QStringLiteral("0"), QStringLiteral("2"), QStringLiteral("4"), QStringLiteral("6"), QStringLiteral("8"), QStringLiteral("10")}));
        query.movieList = MovieList::Favourites; query.sort = CatalogSort::TitleDescending; query.titleContains = QStringLiteral("09");
        const auto page = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(page.items.size(), 1); QCOMPARE(std::get<MovieSummary>(page.items.first()).ref.providerItemId, QStringLiteral("9"));
        auto cancelled = fixture.request(); cancelled.cancelled->store(true);
        QVERIFY(std::holds_alternative<Error>(fixture.store->setMovieList(fixture.movie(QStringLiteral("0")).ref, MovieList::ToWatch, false, cancelled)));
        QVERIFY(std::get<MovieListState>(fixture.store->readMovieLists(fixture.movie(QStringLiteral("0")).ref, fixture.request())).toWatch);
        auto wrongSource = fixture.movie().ref; wrongSource.profileId = QUuid::createUuid();
        QVERIFY(std::holds_alternative<Error>(fixture.store->setMovieList(wrongSource, MovieList::ToWatch, true, fixture.request())));
    }
    void movieListsIsolateSourcesAndRejectOldRequests()
    {
        Fixture fixture;
        const auto firstRef = fixture.movie().ref;
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(firstRef, MovieList::ToWatch, true, fixture.request())));
        auto config = fixture.config; config.revision.profileId = QUuid::createUuid();
        SqliteVodStore other(fixture.path, [config](const QUuid &) -> Result<SourceContext> { return config; });
        const auto source = std::get<SourceContext>(other.snapshot(config.revision.profileId));
        auto otherRef = firstRef; otherRef.profileId = source.revision.profileId; otherRef.catalogNamespace = source.revision.catalogNamespace;
        RequestContext otherRequest; otherRequest.source = source.revision;
        QCOMPARE(std::get<MovieListState>(other.readMovieLists(otherRef, otherRequest)), MovieListState{});
        QVERIFY(std::holds_alternative<MovieListState>(other.setMovieList(otherRef, MovieList::Favourites, true, otherRequest)));
        QCOMPARE(std::get<MovieListState>(other.readMovieLists(otherRef, otherRequest)), (MovieListState{false, true}));
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(firstRef, fixture.request())), (MovieListState{true, false}));
        const auto oldRequest = fixture.request();
        ++fixture.config.revision.credentialRevision;
        QVERIFY(std::holds_alternative<Success>(fixture.store->advanceCredentialRevision(firstRef.profileId, fixture.config.revision.credentialRevision)));
        QVERIFY(std::holds_alternative<Error>(fixture.store->setMovieList(firstRef, MovieList::ToWatch, false, oldRequest)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->finishCredentialChange(firstRef.profileId)));
        fixture.reopen();
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(firstRef, fixture.request())), (MovieListState{true, false}));
    }
    void movieListCompletionIsAtomic()
    {
        Fixture fixture;
        const auto ref = fixture.movie().ref;
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(ref, MovieList::ToWatch, true, fixture.request())));
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(ref, MovieList::Favourites, true, fixture.request())));
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 95000; progress.durationMs = 100000;
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(ref, progress.sessionToken, fixture.request())));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, fixture.request(), true)));
        QVERIFY(std::get<MovieListState>(fixture.store->readMovieLists(ref, fixture.request())).toWatch);
        progress.positionMs = 95001; progress.status = WatchStatus::Watched;
        QVERIFY(std::holds_alternative<Error>(fixture.store->checkpoint(ref, progress, fixture.request(), true))); // Old sequence rolls back both writes.
        QVERIFY(std::get<MovieListState>(fixture.store->readMovieLists(ref, fixture.request())).toWatch);
        ++progress.sequence;
        sql(fixture.path, QStringLiteral("CREATE TRIGGER reject_list_update BEFORE UPDATE ON vod_movie_lists BEGIN SELECT RAISE(ABORT,'synthetic failure'); END"));
        QVERIFY(std::holds_alternative<Error>(fixture.store->checkpoint(ref, progress, fixture.request(), true)));
        QVERIFY(std::get<MovieListState>(fixture.store->readMovieLists(ref, fixture.request())).toWatch);
        QCOMPARE(std::get<std::optional<VodProgress>>(fixture.store->read(ref, fixture.request()))->positionMs, qint64(95000));
        sql(fixture.path, QStringLiteral("DROP TRIGGER reject_list_update"));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, fixture.request(), true)));
        QCOMPARE(std::get<MovieListState>(fixture.store->readMovieLists(ref, fixture.request())), (MovieListState{false, true}));
        QVERIFY(std::holds_alternative<MovieListState>(fixture.store->setMovieList(ref, MovieList::ToWatch, true, fixture.request())));
        ++progress.sequence;
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, fixture.request())));
        QVERIFY(std::get<MovieListState>(fixture.store->readMovieLists(ref, fixture.request())).toWatch);
    }
    void baseLetterTitleSearchAndPaging()
    {
        Fixture fixture;
        const QStringList titles{QStringLiteral("À demain"), QStringLiteral("Æon"), QStringLiteral("Ａlpha"),
            QStringLiteral("𝐀lpine"), QStringLiteral("Äpfel"), QStringLiteral("Čas"), QStringLiteral("Ðelta"),
            QStringLiteral("Đorđe"), QStringLiteral("Éclair"), QStringLiteral("E\u0301clair"), QStringLiteral("Ħero"),
            QStringLiteral("Işık"), QStringLiteral("Lato"), QStringLiteral("Łotr 1"), QStringLiteral("Lumina"),
            QStringLiteral("Œuvre"), QStringLiteral("Öland"), QStringLiteral("Øresund"), QStringLiteral("Straße"),
            QStringLiteral("Þor"), QStringLiteral("Tiếng Việt"), QStringLiteral("Zebra"),
            QStringLiteral("Ελλάδα"), QStringLiteral("Москва"), QStringLiteral("東京"), QStringLiteral("😀")};
        QList<CatalogItem> items;
        for (int i = 0; i < titles.size(); ++i) {
            auto movie = fixture.movie(QStringLiteral("%1").arg(i, 3, 10, QChar(u'0')));
            movie.title = titles[i]; items.append(movie);
        }
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), items)));
        fixture.reopen();
        for (const auto sort : {CatalogSort::TitleAscending, CatalogSort::TitleDescending}) {
            CatalogQuery query; query.scope = fixture.scope(); query.sort = sort; query.pageSize = 2;
            QStringList actual;
            do {
                const auto result = fixture.store->query(query, fixture.request());
                QVERIFY(std::holds_alternative<CatalogPage>(result));
                const auto page = std::get<CatalogPage>(result);
                for (const auto &item : page.items) actual.append(std::get<MovieSummary>(item).title);
                query.page = page.next;
            } while (query.page);
            auto expected = titles;
            if (sort == CatalogSort::TitleDescending) std::reverse(expected.begin(), expected.end());
            QCOMPARE(actual, expected);
        }
        const QList<QPair<QString, QString>> searches{{QStringLiteral("LOTR"), QStringLiteral("Łotr 1")},
            {QStringLiteral("łotr"), QStringLiteral("Łotr 1")}, {QStringLiteral("apfel"), QStringLiteral("Äpfel")},
            {QStringLiteral("AEON"), QStringLiteral("Æon")}, {QStringLiteral("CAS"), QStringLiteral("Čas")},
            {QStringLiteral("alpha"), QStringLiteral("Ａlpha")}, {QStringLiteral("ALPINE"), QStringLiteral("𝐀lpine")},
            {QStringLiteral("DELTA"), QStringLiteral("Ðelta")}, {QStringLiteral("hero"), QStringLiteral("Ħero")},
            {QStringLiteral("isik"), QStringLiteral("Işık")}, {QStringLiteral("oeuvre"), QStringLiteral("Œuvre")},
            {QStringLiteral("thor"), QStringLiteral("Þor")},
            {QStringLiteral("dorde"), QStringLiteral("Đorđe")}, {QStringLiteral("oland"), QStringLiteral("Öland")},
            {QStringLiteral("oresund"), QStringLiteral("Øresund")}, {QStringLiteral("STRASSE"), QStringLiteral("Straße")},
            {QStringLiteral("TIENG VIET"), QStringLiteral("Tiếng Việt")}, {QStringLiteral("Москва"), QStringLiteral("Москва")},
            {QStringLiteral("東京"), QStringLiteral("東京")}, {QStringLiteral("😀"), QStringLiteral("😀")}};
        for (const auto &[search, title] : searches) {
            for (const bool prefix : {false, true}) {
                CatalogQuery query; query.scope = fixture.scope();
                if (prefix) query.titlePrefix = search; else query.titleContains = search;
                const auto result = fixture.store->query(query, fixture.request());
                QVERIFY(std::holds_alternative<CatalogPage>(result));
                const auto page = std::get<CatalogPage>(result);
                QCOMPARE(page.items.size(), 1);
                QCOMPARE(std::get<MovieSummary>(page.items.first()).title, title);
            }
        }
        CatalogQuery query; query.scope = fixture.scope(); query.titleContains = QStringLiteral("éCLAIR");
        QCOMPARE(std::get<CatalogPage>(fixture.store->query(query, fixture.request())).items.size(), 2);
        query.titleContains = QStringLiteral("E\u0301clair");
        QCOMPARE(std::get<CatalogPage>(fixture.store->query(query, fixture.request())).items.size(), 2);
    }
    void titleKeyMigrationRollbackAndRetainedStaging()
    {
        Fixture fixture;
        QList<CatalogItem> items;
        for (int i = 0; i < 300; ++i) {
            auto movie = fixture.movie(QString::number(i)); movie.title = QStringLiteral("Łotr %1").arg(i);
            items.append(movie);
        }
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), items)));
        const auto ref = std::get<MovieSummary>(items.first()).ref;
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1;
        progress.positionMs = 60000; progress.durationMs = 120000;
        progress.trackPreferences = {{QStringLiteral("subtitle"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("off")}}}};
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(ref, progress.sessionToken, fixture.request())));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, fixture.request())));
        CatalogQuery query; query.scope = fixture.scope(); query.pageSize = 1;
        const auto before = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QVERIFY(before.next);
        const auto token = std::get<ImportToken>(fixture.store->beginRefresh(fixture.scope(), fixture.request()));
        auto staged = fixture.movie(QStringLiteral("staged")); staged.title = QStringLiteral("Öland");
        QVERIFY(std::holds_alternative<Success>(fixture.store->stageBatch(token, {fixture.scope(), {staged}, true, {}})));
        sql(fixture.path, QStringLiteral("DELETE FROM vod_schema_migrations WHERE version>=7"));
        sql(fixture.path, QStringLiteral("INSERT OR IGNORE INTO vod_schema_migrations VALUES(6)"));
        sql(fixture.path, QStringLiteral("UPDATE vod_items SET sort_key='legacy'"));
        sql(fixture.path, QStringLiteral("UPDATE vod_staging SET sort_key='legacy'"));
        sql(fixture.path, QStringLiteral("CREATE TRIGGER fail_title_migration BEFORE UPDATE OF sort_key ON vod_items "
            "WHEN OLD.provider_id='299' BEGIN SELECT RAISE(ABORT,'synthetic migration failure'); END"));
        fixture.store = std::make_shared<SqliteVodStore>(fixture.path, [&fixture](const QUuid &) -> Result<SourceContext> { return fixture.config; });
        const auto failed = fixture.store->prepare(fixture.request());
        QVERIFY(std::holds_alternative<Error>(failed));
        QCOMPARE(std::get<Error>(failed).code, ErrorCode::StorageUnavailable);
        const auto rolledBack = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(rolledBack.generation, before.generation);
        QVERIFY(rolledBack.next); QCOMPARE(rolledBack.next->lastSortKey, QStringLiteral("legacy"));
        QVERIFY(QFile::exists(fixture.store->backupPath()));
        sql(fixture.path, QStringLiteral("DROP TRIGGER fail_title_migration"));
        QVERIFY(std::holds_alternative<Success>(fixture.store->prepare(fixture.request())));
        query.titleContains = QStringLiteral("lotr"); query.pageSize = 1000;
        const auto migrated = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(migrated.items.size(), 300); QCOMPARE(migrated.generation, before.generation + 1);
        const auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(ref, fixture.request()));
        QVERIFY(saved); QCOMPARE(saved->positionMs, progress.positionMs); QCOMPARE(saved->trackPreferences, progress.trackPreferences);
        query.page = before.next;
        QCOMPARE(std::get<Error>(fixture.store->query(query, fixture.request())).code, ErrorCode::Cancelled);
        QVERIFY(std::holds_alternative<quint64>(fixture.store->publishIfCurrent(token, true)));
        query.page.reset(); query.titleContains = QStringLiteral("oland");
        const auto published = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(published.items.size(), 1); QCOMPARE(std::get<MovieSummary>(published.items.first()).ref, staged.ref);
        fixture.reopen();
        QCOMPARE(std::get<CatalogPage>(fixture.store->query(query, fixture.request())).generation, published.generation);
    }
    void continueWatchingThresholdAndPaging()
    {
        Fixture fixture;
        QList<CatalogItem> items;
        const QList<qint64> positions{0, 1, 95000, 95001, 100000, 60000, 5000};
        for (int i = 0; i < positions.size(); ++i) {
            const auto movie = fixture.movie(QString::number(i)); items.append(movie);
            VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1;
            progress.positionMs = positions[i]; progress.durationMs = 100000;
            progress.updatedAtUtc = QDateTime::fromMSecsSinceEpoch(1000 + i, QTimeZone::UTC);
            if (i == 5) progress.status = WatchStatus::Watched;
            if (i == 6) progress.durationMs.reset();
            QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(movie.ref, progress.sessionToken, fixture.request())));
            QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(movie.ref, progress, fixture.request())));
        }
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), items)));
        fixture.reopen();
        CatalogQuery query; query.scope = fixture.scope(); query.continueWatchingOnly = true; query.pageSize = 2;
        const auto first = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(first.items.size(), 2); QVERIFY(first.next);
        QCOMPARE(std::get<MovieSummary>(first.items[0]).ref.providerItemId, QStringLiteral("6"));
        QCOMPARE(std::get<MovieSummary>(first.items[1]).ref.providerItemId, QStringLiteral("2"));
        query.page = first.next;
        const auto second = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(second.items.size(), 1); QVERIFY(!second.next);
        QCOMPARE(std::get<MovieSummary>(second.items[0]).ref.providerItemId, QStringLiteral("1"));
        QVERIFY(!watchedByPosition(95000, 100000)); QVERIFY(watchedByPosition(95001, 100000));
        QVERIFY(!watchedByPosition(100000, std::nullopt));
    }
    void continueWatchingRecencyOverridesTitleSort()
    {
        Fixture fixture;
        QList<CatalogItem> items;
        const QStringList titles{QStringLiteral("Alpha"), QStringLiteral("Zulu"), QStringLiteral("Middle")};
        const QList<qint64> timestamps{100, 10000, 10000};
        QList<VodProgress> progress;
        for (int i = 0; i < titles.size(); ++i) {
            auto movie = fixture.movie(QString::number(i)); movie.title = titles[i]; items.append(movie);
            VodProgress saved; saved.sessionToken = QUuid::createUuid(); saved.sequence = 1;
            saved.positionMs = 60000; saved.durationMs = 120000;
            saved.updatedAtUtc = QDateTime::fromMSecsSinceEpoch(timestamps[i], QTimeZone::UTC);
            progress.append(saved);
            QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(movie.ref, saved.sessionToken, fixture.request())));
            QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(movie.ref, saved, fixture.request())));
        }
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), items)));
        fixture.reopen();
        for (const auto sort : {CatalogSort::TitleAscending, CatalogSort::TitleDescending}) {
            CatalogQuery query; query.scope = fixture.scope(); query.continueWatchingOnly = true;
            query.sort = sort; query.pageSize = 1;
            // Equal timestamps use descending content identity, even across page boundaries.
            for (const auto &id : {QStringLiteral("2"), QStringLiteral("1"), QStringLiteral("0")}) {
                const auto result = fixture.store->query(query, fixture.request());
                QVERIFY(std::holds_alternative<CatalogPage>(result));
                const auto page = std::get<CatalogPage>(result);
                QCOMPARE(page.items.size(), 1);
                QCOMPARE(std::get<MovieSummary>(page.items.first()).ref.providerItemId, id);
                QCOMPARE(page.next.has_value(), id != QStringLiteral("0"));
                query.page = page.next;
            }
        }
        // Watching an older movie again moves it to the front after a durable checkpoint.
        progress[0].sequence = 2;
        progress[0].updatedAtUtc = QDateTime::fromMSecsSinceEpoch(10001, QTimeZone::UTC);
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(fixture.movie(QStringLiteral("0")).ref, progress[0], fixture.request())));
        CatalogQuery query; query.scope = fixture.scope(); query.continueWatchingOnly = true;
        const auto page = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(std::get<MovieSummary>(page.items.first()).ref.providerItemId, QStringLiteral("0"));
    }
    void trackPreferencesPersistAndMigrate()
    {
        Fixture fixture;
        const auto ref = fixture.movie().ref;
        VodProgress progress;
        progress.sessionToken = QUuid::createUuid(); progress.sequence = 1;
        progress.positionMs = 60000; progress.durationMs = 120000;
        progress.trackPreferences = {{QStringLiteral("audio"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("track")},
            {QStringLiteral("id"), 2}, {QStringLiteral("lang"), QStringLiteral("pol")}}},
            {QStringLiteral("sub"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("off")}}}};
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(ref, progress.sessionToken, fixture.request())));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, fixture.request())));
        fixture.reopen();
        auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(ref, fixture.request()));
        QVERIFY(saved); QCOMPARE(saved->trackPreferences, progress.trackPreferences);
        QCOMPARE(saved->positionMs, progress.positionMs);
        QVERIFY(!std::get<std::optional<VodProgress>>(fixture.store->read(fixture.movie(QStringLiteral("other")).ref, fixture.request())));
        // Simulate an existing schema 5 database: migration must preserve time.
        sql(fixture.path, QStringLiteral("DELETE FROM vod_schema_migrations WHERE version>5"));
        sql(fixture.path, QStringLiteral("INSERT OR IGNORE INTO vod_schema_migrations VALUES(5)"));
        sql(fixture.path, QStringLiteral("ALTER TABLE vod_progress DROP COLUMN track_preferences"));
        fixture.reopen();
        saved = std::get<std::optional<VodProgress>>(fixture.store->read(ref, fixture.request()));
        QVERIFY(saved); QCOMPARE(saved->positionMs, progress.positionMs);
        QVERIFY(saved->trackPreferences.isEmpty());
        QVERIFY(!fixture.store->backupPath().isEmpty());
    }
    void artworkMigrationAndSubstringPaging()
    {
        Fixture fixture;
        auto first = fixture.movie(QStringLiteral("1")); first.title = QStringLiteral("The First Movie");
        first.artwork = {{QStringLiteral("opaque-poster-id"), QStringLiteral("poster"), {}}};
        auto second = fixture.movie(QStringLiteral("2")); second.title = QStringLiteral("The Second Movie");
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {first, second})));
        fixture.reopen();
        CatalogQuery query; query.scope = fixture.scope(); query.titleContains = QStringLiteral("MOVIE"); query.pageSize = 1;
        const auto page = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(page.items.size(), 1); QVERIFY(page.next);
        QCOMPARE(std::get<MovieSummary>(page.items.first()).artwork.first().id, QStringLiteral("opaque-poster-id"));
        query.page = page.next;
        QCOMPARE(std::get<MovieSummary>(std::get<CatalogPage>(fixture.store->query(query, fixture.request())).items.first()).ref, second.ref);
        sql(fixture.path, QStringLiteral("DELETE FROM vod_schema_migrations WHERE version>4"));
        sql(fixture.path, QStringLiteral("INSERT OR IGNORE INTO vod_schema_migrations VALUES(4)"));
        sql(fixture.path, QStringLiteral("ALTER TABLE vod_items DROP COLUMN artwork"));
        sql(fixture.path, QStringLiteral("ALTER TABLE vod_staging DROP COLUMN artwork"));
        fixture.reopen(); query.page.reset();
        const auto migrated = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(migrated.items.size(), 1); QVERIFY(std::get<MovieSummary>(migrated.items.first()).artwork.isEmpty());
        QVERIFY(!fixture.store->backupPath().isEmpty());
    }
    void categorySnapshotsPersistValidateAndSeparateScopes()
    {
        Fixture fixture;
        const auto scope = fixture.scope();
        const auto request = fixture.request();
        QVERIFY(!std::get<std::optional<CategorySnapshot>>(fixture.store->readCategories(scope, request)));
        CategorySnapshot movies{scope, {{scope, QStringLiteral("007"), QStringLiteral("Movies"), QStringLiteral("parent")}}, QDateTime::currentDateTimeUtc().addDays(-2)};
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginCategoryRefresh(scope, request)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeCategories(movies, request)));
        auto series = movies; series.scope.kind = CatalogKind::Series;
        series.categories[0].scope = series.scope; series.categories[0].name = QStringLiteral("Series");
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginCategoryRefresh(series.scope, request)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeCategories(series, request)));
        fixture.reopen();
        auto cached = std::get<std::optional<CategorySnapshot>>(fixture.store->readCategories(scope, fixture.request()));
        QVERIFY(cached); QCOMPARE(cached->categories.first().id, QStringLiteral("007"));
        QCOMPARE(cached->categories.first().parentId, std::optional<QString>(QStringLiteral("parent")));
        QCOMPARE(cached->refreshedAtUtc, movies.refreshedAtUtc);
        const auto replacement = fixture.request();
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginCategoryRefresh(scope, replacement)));
        auto invalid = movies; invalid.categories.append(invalid.categories.first());
        QCOMPARE(std::get<Error>(fixture.store->storeCategories(invalid, replacement)).code, ErrorCode::InvalidResponse);
        invalid = movies; invalid.categories[0].scope = series.scope;
        QCOMPARE(std::get<Error>(fixture.store->storeCategories(invalid, replacement)).code, ErrorCode::InvalidResponse);
        cached = std::get<std::optional<CategorySnapshot>>(fixture.store->readCategories(scope, fixture.request()));
        QCOMPARE(cached->categories.size(), 1); // validation rollback retains the old snapshot
        auto cancelled = fixture.request(); cancelled.cancelled->store(true);
        QCOMPARE(std::get<Error>(fixture.store->storeCategories(movies, cancelled)).code, ErrorCode::Cancelled);
        QVERIFY(std::holds_alternative<Success>(fixture.store->advanceCredentialRevision(scope.profileId, 2)));
        QCOMPARE(std::get<Error>(fixture.store->storeCategories(movies, request)).code, ErrorCode::Cancelled);
        QVERIFY(std::holds_alternative<Success>(fixture.store->finishCredentialChange(scope.profileId)));
        fixture.reopen();
        movies.categories.clear();
        const auto emptyRequest = fixture.request();
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginCategoryRefresh(scope, emptyRequest)));
        const auto newerRequest = fixture.request();
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginCategoryRefresh(scope, newerRequest)));
        QCOMPARE(std::get<Error>(fixture.store->storeCategories(movies, emptyRequest)).code, ErrorCode::Cancelled);
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeCategories(movies, newerRequest)));
        fixture.reopen();
        cached = std::get<std::optional<CategorySnapshot>>(fixture.store->readCategories(scope, fixture.request()));
        QVERIFY(cached); QVERIFY(cached->categories.isEmpty());
        auto filtered = scope; filtered.categoryId = QStringLiteral("007");
        QVERIFY(std::holds_alternative<Success>(fixture.store->evictCache(filtered, fixture.request())));
        QVERIFY(std::get<std::optional<CategorySnapshot>>(fixture.store->readCategories(scope, fixture.request())));
        QVERIFY(std::holds_alternative<Success>(fixture.store->evictCache(scope, fixture.request())));
        QVERIFY(!std::get<std::optional<CategorySnapshot>>(fixture.store->readCategories(scope, fixture.request())));
        cached = std::get<std::optional<CategorySnapshot>>(fixture.store->readCategories(series.scope, fixture.request()));
        QVERIFY(cached); QCOMPARE(cached->categories.first().name, QStringLiteral("Series"));
        QVERIFY(std::holds_alternative<Success>(fixture.store->prepareRemoval(scope.profileId)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->removeSourceState(scope.profileId)));
        sql(fixture.path, QStringLiteral("CREATE TABLE assert_empty(value INTEGER CHECK(value=0))"));
        sql(fixture.path, QStringLiteral("INSERT INTO assert_empty SELECT count(*) FROM vod_categories"));
        sql(fixture.path, QStringLiteral("INSERT INTO assert_empty SELECT count(*) FROM vod_category_snapshots"));
    }
    void schemaThreeUpgradePreservesCatalogAndHistory()
    {
        Fixture fixture;
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {fixture.movie()})));
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 45000;
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(fixture.movie().ref, progress.sessionToken, fixture.request())));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(fixture.movie().ref, progress, fixture.request())));
        sql(fixture.path, QStringLiteral("DROP TABLE vod_categories"));
        sql(fixture.path, QStringLiteral("DROP TABLE vod_category_snapshots"));
        sql(fixture.path, QStringLiteral("DELETE FROM vod_schema_migrations WHERE version>3"));
        sql(fixture.path, QStringLiteral("INSERT OR IGNORE INTO vod_schema_migrations VALUES(3)"));
        fixture.reopen();
        QVERIFY(QFile::exists(fixture.store->backupPath()));
        CatalogQuery query; query.scope = fixture.scope();
        QCOMPARE(std::get<CatalogPage>(fixture.store->query(query, fixture.request())).items.size(), 1);
        QCOMPARE(std::get<std::optional<VodProgress>>(fixture.store->read(fixture.movie().ref, fixture.request()))->positionMs, 45000);
        QVERIFY(!std::get<std::optional<CategorySnapshot>>(fixture.store->readCategories(fixture.scope(), fixture.request())));
    }
    void lazyEpisodeDetailsAndHistorySurviveCacheReplacement()
    {
        Fixture fixture;
        VodDetails details;
        details.ref = {fixture.source.revision.profileId, fixture.source.revision.catalogNamespace, ContentKind::Series, QStringLiteral("show"), {}};
        details.seasons = {{details.ref, QStringLiteral("0"), 0, 0}};
        details.declaredVideoWidth = 1920; details.declaredVideoHeight = 1080;
        VodMediaProbe probe; probe.videoWidth = 1920; probe.videoHeight = 1080;
        probe.observedAtUtc = QDateTime::currentDateTimeUtc();
        probe.audioTracks = {{1, 0, QStringLiteral("audio"), QStringLiteral("aac"), QStringLiteral("Main"), QStringLiteral("pol"), true, false}};
        probe.subtitleTracks = {{2, 0, QStringLiteral("sub"), QStringLiteral("subrip"), {}, QStringLiteral("eng"), false, true}};
        details.mediaProbe = probe;
        EpisodeSummary episode;
        episode.ref = {details.ref.profileId, details.ref.catalogNamespace, ContentKind::Episode, QStringLiteral("007"), QStringLiteral("show")};
        episode.series = details.ref; episode.seasonId = QStringLiteral("0"); episode.title = QStringLiteral("Special");
        details.episodes = {episode};
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeDetails(details, fixture.request())));
        const auto cached = std::get<std::optional<VodDetails>>(fixture.store->readDetails(details.ref, fixture.request()));
        QVERIFY(cached); QCOMPARE(cached->episodes.first().ref, episode.ref);
        QCOMPARE(cached->declaredVideoWidth, std::optional<int>(1920));
        QVERIFY(cached->mediaProbe); QCOMPARE(cached->mediaProbe->audioTracks.size(), 1);
        QCOMPARE(cached->mediaProbe->subtitleTracks.first().language, QStringLiteral("eng"));
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 45000;
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(episode.ref, progress.sessionToken, fixture.request())));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(episode.ref, progress, fixture.request())));
        QVERIFY(!std::get<std::optional<VodProgress>>(fixture.store->read(fixture.movie().ref, fixture.request())));
        details.episodes.clear(); // a later complete series detail no longer lists it
        QVERIFY(std::holds_alternative<Success>(fixture.store->storeDetails(details, fixture.request())));
        auto scope = fixture.scope(); scope.kind = CatalogKind::Series;
        QVERIFY(std::holds_alternative<Success>(fixture.store->evictCache(scope, fixture.request())));
        fixture.reopen();
        auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(episode.ref, fixture.request()));
        QVERIFY(saved); QCOMPARE(saved->positionMs, 45000);
        details.episodes = {episode};
        details.episodes.first().ref.parentNamespace = QStringLiteral("wrong-series");
        QCOMPARE(std::get<Error>(fixture.store->storeDetails(details, fixture.request())).code, ErrorCode::InvalidResponse);
    }
    void schemaOneUpgradeReclaimsAbandonedStagingAndRetainsPublishedCatalog()
    {
        Fixture fixture;
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {fixture.movie()})));
        auto token = std::get<ImportToken>(fixture.store->beginRefresh(fixture.scope(), fixture.request()));
        fixture.store->stageBatch(token, {fixture.scope(), {}, true, {}});
        sql(fixture.path, QStringLiteral("ALTER TABLE vod_sync_runs DROP COLUMN owner"));
        sql(fixture.path, QStringLiteral("DELETE FROM vod_schema_migrations"));
        sql(fixture.path, QStringLiteral("INSERT INTO vod_schema_migrations VALUES(1)"));
        fixture.reopen();
        QVERIFY(QFile::exists(fixture.store->backupPath()));
        QVERIFY(std::holds_alternative<Error>(fixture.store->publishIfCurrent(token, true)));
        CatalogQuery query; query.scope = fixture.scope();
        QCOMPARE(std::get<CatalogPage>(fixture.store->query(query, fixture.request())).items.size(), 1);
    }
    void completedRemovalIsReconciledBeforeSourceFileDeletion()
    {
        Fixture fixture;
        const auto id = fixture.source.revision.profileId;
        QVERIFY(std::holds_alternative<Success>(fixture.store->prepareRemoval(id)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->removeSourceState(id)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->finishRemoval(id)));
        // Simulate a crash after SQLite cleanup but before SourceStore deletion.
        const auto removed = fixture.store->reconcileRemovedSources({id});
        QVERIFY(std::holds_alternative<QList<QUuid>>(removed));
        QVERIFY(std::get<QList<QUuid>>(removed).contains(id));
        QVERIFY(std::holds_alternative<Error>(fixture.store->snapshot(id)));
    }
    void durableCredentialBarrierRejectsOldConfigurationAndKeepsHistory()
    {
        Fixture fixture;
        const auto ref = fixture.movie().ref;
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 73000;
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(ref, progress.sessionToken, fixture.request())));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, fixture.request())));
        auto token = std::get<ImportToken>(fixture.store->beginRefresh(fixture.scope(), fixture.request()));
        fixture.store->stageBatch(token, {fixture.scope(), {}, true, {}});
        QVERIFY(std::holds_alternative<Success>(fixture.store->advanceCredentialRevision(ref.profileId, 2)));
        QVERIFY(!fixture.store->isCurrent(fixture.source.revision));
        QVERIFY(std::holds_alternative<Error>(fixture.store->snapshot(ref.profileId)));
        QVERIFY(std::holds_alternative<Error>(fixture.store->publishIfCurrent(token, true)));
        fixture.config.revision.credentialRevision = 2;
        fixture.config.password = QStringLiteral("changed");
        QVERIFY(std::holds_alternative<Success>(fixture.store->finishCredentialChange(ref.profileId)));
        fixture.reopen();
        QCOMPARE(fixture.source.revision.catalogNamespace, ref.catalogNamespace);
        auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(ref, fixture.request()));
        QVERIFY(saved); QCOMPARE(saved->positionMs, 73000);
    }
    void failedCredentialWriteRecoversWithoutRevivingOldRequests_data()
    {
        QTest::addColumn<bool>("restart");
        QTest::newRow("same-process") << false;
        QTest::newRow("crash-before-finalization") << true;
    }
    void failedCredentialWriteRecoversWithoutRevivingOldRequests()
    {
        QFETCH(bool, restart);
        Fixture fixture;
        const auto ref = fixture.movie().ref;
        const auto originalRequest = fixture.request();
        const auto originalSource = fixture.source;
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 73000;
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(ref, progress.sessionToken, originalRequest)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, originalRequest)));
        const auto token = std::get<ImportToken>(fixture.store->beginRefresh(fixture.scope(), originalRequest));
        QVERIFY(std::holds_alternative<Success>(fixture.store->stageBatch(token, {fixture.scope(), {}, true, {}})));
        QVERIFY(std::holds_alternative<Success>(fixture.store->advanceCredentialRevision(ref.profileId, 2)));
        QVERIFY(std::holds_alternative<Error>(fixture.store->snapshot(ref.profileId)));
        // The protected configuration write failed: leave its revision/password unchanged.
        if (restart) {
            fixture.reopen();
            QVERIFY(std::holds_alternative<QList<QUuid>>(fixture.store->reconcileRemovedSources({ref.profileId})));
        } else QVERIFY(std::holds_alternative<Success>(fixture.store->finishCredentialChange(ref.profileId)));
        fixture.reopen();
        QCOMPARE(fixture.source.password, originalSource.password);
        QCOMPARE(fixture.source.revision.catalogNamespace, ref.catalogNamespace);
        QVERIFY(fixture.source.revision.credentialRevision > originalRequest.source.credentialRevision);
        QVERIFY(!fixture.store->isCurrent(originalRequest.source));
        const auto saved = std::get<std::optional<VodProgress>>(fixture.store->read(ref, fixture.request()));
        QVERIFY(saved); QCOMPARE(saved->positionMs, 73000);
        QVERIFY(std::holds_alternative<Error>(fixture.store->publishIfCurrent(token, true)));
        ++progress.sequence;
        QVERIFY(std::holds_alternative<Error>(fixture.store->checkpoint(ref, progress, originalRequest)));
        const auto recoveredRequest = fixture.request();
        // Retrying the edit uses configuration revision 2 again, but must issue
        // a strictly newer request revision than the compensated generation.
        QVERIFY(std::holds_alternative<Success>(fixture.store->advanceCredentialRevision(ref.profileId, 2)));
        fixture.config.revision.credentialRevision = 2;
        fixture.config.password = QStringLiteral("new-synthetic-password");
        QVERIFY(std::holds_alternative<Success>(fixture.store->finishCredentialChange(ref.profileId)));
        fixture.reopen();
        QVERIFY(fixture.source.revision.credentialRevision > recoveredRequest.source.credentialRevision);
        QCOMPARE(fixture.source.password, fixture.config.password);
        QCOMPARE(fixture.source.revision.catalogNamespace, ref.catalogNamespace);
        QVERIFY(std::holds_alternative<Error>(fixture.store->checkpoint(ref, progress, recoveredRequest)));
    }
    void schemaTwoUpgradePreservesRevisionAndProgress()
    {
        Fixture fixture;
        const auto original = fixture.source.revision;
        sql(fixture.path, QStringLiteral("ALTER TABLE vod_source_state DROP COLUMN configuration_revision"));
        sql(fixture.path, QStringLiteral("ALTER TABLE vod_source_state DROP COLUMN mutation_pending"));
        sql(fixture.path, QStringLiteral("DELETE FROM vod_schema_migrations WHERE version>2"));
        sql(fixture.path, QStringLiteral("INSERT OR IGNORE INTO vod_schema_migrations VALUES(2)"));
        fixture.reopen();
        QCOMPARE(fixture.source.revision, original);
        QVERIFY(QFile::exists(fixture.store->backupPath()));
        QVERIFY(std::holds_alternative<Success>(fixture.store->prepare({})));
    }
    void restartPreservesNamespaceAndBackwardProgress()
    {
        Fixture fixture;
        QVERIFY(!fixture.source.revision.catalogNamespace.isNull());
        const auto ref = fixture.movie().ref;
        VodProgress progress; progress.sessionToken = QUuid::createUuid(); progress.sequence = 1; progress.positionMs = 90000;
        progress.updatedAtUtc = QDateTime::currentDateTimeUtc();
        QVERIFY(std::holds_alternative<Success>(fixture.store->beginSession(ref, progress.sessionToken, fixture.request())));
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, fixture.request())));
        const auto late = progress;
        progress.sequence = 2; progress.positionMs = 10000;
        QVERIFY(std::holds_alternative<Success>(fixture.store->checkpoint(ref, progress, fixture.request())));
        QVERIFY(std::holds_alternative<Error>(fixture.store->checkpoint(ref, late, fixture.request())));
        const auto space = fixture.source.revision.catalogNamespace;
        fixture.reopen();
        QCOMPARE(fixture.source.revision.catalogNamespace, space);
        auto stored = fixture.store->read(ref, fixture.request());
        QVERIFY(std::holds_alternative<std::optional<VodProgress>>(stored));
        QVERIFY(std::get<std::optional<VodProgress>>(stored));
        QCOMPARE(std::get<std::optional<VodProgress>>(stored)->positionMs, 10000);
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {fixture.movie()})));
        QVERIFY(std::holds_alternative<Success>(fixture.store->evictCache(fixture.scope(), fixture.request())));
        stored = fixture.store->read(ref, fixture.request());
        QCOMPARE(std::get<std::optional<VodProgress>>(stored)->positionMs, 10000);
        fixture.config.password = QStringLiteral("replacement-secret"); ++fixture.config.revision.credentialRevision;
        fixture.reopen();
        QCOMPARE(fixture.source.revision.catalogNamespace, space);
        QCOMPARE(std::get<std::optional<VodProgress>>(fixture.store->read(ref, fixture.request()))->positionMs, 10000);
    }
    void cancellationAndCredentialEditBlockPublication()
    {
        Fixture fixture;
        const auto request = fixture.request();
        auto token = std::get<ImportToken>(fixture.store->beginRefresh(fixture.scope(), request));
        QVERIFY(std::holds_alternative<Success>(fixture.store->stageBatch(token, {fixture.scope(), {fixture.movie()}, true, {}})));
        request.cancelled->store(true);
        QCOMPARE(std::get<Error>(fixture.store->publishIfCurrent(token, true)).code, ErrorCode::Cancelled);
        token = std::get<ImportToken>(fixture.store->beginRefresh(fixture.scope(), fixture.request()));
        fixture.store->stageBatch(token, {fixture.scope(), {}, true, {}});
        ++fixture.config.revision.credentialRevision;
        QVERIFY(std::holds_alternative<SourceContext>(fixture.store->snapshot(fixture.config.revision.profileId)));
        QCOMPARE(std::get<Error>(fixture.store->publishIfCurrent(token, true)).code, ErrorCode::Cancelled);
    }
    void scopedReplacementPreservesSharedMembershipAndRejectsOldPages()
    {
        Fixture fixture;
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {fixture.movie(), fixture.movie(QStringLiteral("8"))})));
        CatalogQuery query; query.scope = fixture.scope(); query.pageSize = 1;
        auto first = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QVERIFY(first.next);
        query.page = first.next;
        auto second = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(second.items.size(), 1);
        QVERIFY(std::get<MovieSummary>(first.items.first()).ref != std::get<MovieSummary>(second.items.first()).ref);
        auto category = fixture.scope(); category.categoryId = QStringLiteral("a");
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(category, {})));
        QVERIFY(std::holds_alternative<Error>(fixture.store->query(query, fixture.request())));
        query.page.reset(); query.pageSize = 100; query.scope.categoryId = QStringLiteral("b");
        QCOMPARE(std::get<CatalogPage>(fixture.store->query(query, fixture.request())).items.size(), 2);
        query.scope.categoryId = QStringLiteral("a");
        QCOMPARE(std::get<CatalogPage>(fixture.store->query(query, fixture.request())).items.size(), 0);
    }
    void interruptedImportDoesNotReplacePublishedGeneration()
    {
        Fixture fixture;
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {fixture.movie()})));
        const auto token = std::get<ImportToken>(fixture.store->beginRefresh(fixture.scope(), fixture.request()));
        fixture.store->stageBatch(token, {fixture.scope(), {fixture.movie(QStringLiteral("new"))}, false, ProviderCursor{QStringLiteral("next")}});
        QVERIFY(std::holds_alternative<Error>(fixture.store->publishIfCurrent(token, true)));
        fixture.reopen();
        CatalogQuery query; query.scope = fixture.scope();
        const auto page = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(page.items.size(), 1);
        QCOMPARE(std::get<MovieSummary>(page.items.first()).ref.providerItemId, QStringLiteral("007"));
        const auto replacement = std::get<ImportToken>(fixture.store->beginRefresh(fixture.scope(), fixture.request()));
        QVERIFY(std::holds_alternative<Error>(fixture.store->stageBatch(token, {fixture.scope(), {}, true, {}})));
        QVERIFY(std::holds_alternative<Success>(fixture.store->abandonRefresh(replacement)));
    }
    void removalIntentSurvivesRestartAndPreventsResurrection()
    {
        Fixture fixture;
        const auto id = fixture.source.revision.profileId;
        const auto token = std::get<ImportToken>(fixture.store->beginRefresh(fixture.scope(), fixture.request()));
        fixture.store->stageBatch(token, {fixture.scope(), {fixture.movie()}, true, {}});
        QVERIFY(std::holds_alternative<Success>(fixture.store->prepareRemoval(id)));
        fixture.reopen();
        QCOMPARE(std::get<QList<QUuid>>(fixture.store->pendingRemovals()), QList<QUuid>{id});
        QVERIFY(std::holds_alternative<Error>(fixture.store->publishIfCurrent(token, true)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->removeSourceState(id)));
        QVERIFY(std::holds_alternative<Success>(fixture.store->finishRemoval(id)));
        QVERIFY(std::get<QList<QUuid>>(fixture.store->pendingRemovals()).isEmpty());
        QVERIFY(std::holds_alternative<Error>(fixture.store->snapshot(id)));
    }
    void migrationBackupAndNewerSchemaAreNonDestructive()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("iptv.db"));
        sql(path, QStringLiteral("CREATE TABLE live_sentinel(value TEXT)"));
        sql(path, QStringLiteral("INSERT INTO live_sentinel VALUES('preserve')"));
        SqliteVodStore store(path, {});
        QVERIFY(std::holds_alternative<Success>(store.prepare({})));
        QVERIFY(QFile::exists(store.backupPath()));
        const auto restored = directory.filePath(QStringLiteral("restored.db"));
        QVERIFY(QFile::copy(store.backupPath(), restored));
        sql(restored, QStringLiteral("INSERT INTO live_sentinel SELECT value FROM live_sentinel"));
        sql(path, QStringLiteral("INSERT INTO vod_schema_migrations VALUES(999)"));
        SqliteVodStore future(path, {});
        QCOMPARE(std::get<Error>(future.prepare({})).code, ErrorCode::UnsupportedCapability);
        sql(path, QStringLiteral("INSERT INTO live_sentinel SELECT value FROM live_sentinel"));
    }
    void blockedWriterDoesNotResetDatabase()
    {
        Fixture fixture;
        const auto name = QUuid::createUuid().toString();
        {
            auto blocker = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            blocker.setDatabaseName(fixture.path); QVERIFY(blocker.open());
            QSqlQuery lock(blocker); QVERIFY(lock.exec(QStringLiteral("BEGIN IMMEDIATE")));
            auto request = fixture.request(); request.deadline = QDeadlineTimer(30);
            QVERIFY(std::holds_alternative<Error>(fixture.store->beginRefresh(fixture.scope(), request)));
            QVERIFY(lock.exec(QStringLiteral("ROLLBACK")));
        }
        QSqlDatabase::removeDatabase(name);
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), {fixture.movie()})));
    }
    void failedMigrationRollsBackAndCanBeRetried()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("iptv.db"));
        sql(path, QStringLiteral("CREATE TABLE live_sentinel(value TEXT)"));
        // An incompatible leftover table makes creation of the required index
        // fail after earlier DDL. The entire migration must roll back.
        sql(path, QStringLiteral("CREATE TABLE vod_items(incompatible TEXT)"));
        SqliteVodStore store(path, {});
        QVERIFY(std::holds_alternative<Error>(store.prepare({})));
        sql(path, QStringLiteral("INSERT INTO live_sentinel VALUES('preserved')"));
        sql(path, QStringLiteral("DROP TABLE vod_items"));
        QVERIFY(std::holds_alternative<Success>(store.prepare({})));
    }
    void fiftyThousandMoviesUseBoundedPages()
    {
        Fixture fixture;
        QList<CatalogItem> items;
        items.reserve(50000);
        for (int index = 0; index < 50000; ++index) items.append(fixture.movie(QString::number(index)));
        QVERIFY(std::holds_alternative<quint64>(fixture.publish(fixture.scope(), items)));
        CatalogQuery query; query.scope = fixture.scope(); query.pageSize = 100;
        const auto page = std::get<CatalogPage>(fixture.store->query(query, fixture.request()));
        QCOMPARE(page.items.size(), 100); QVERIFY(page.next);
    }
};
QTEST_GUILESS_MAIN(VodStorageTests)
#include "tst_vod_storage.moc"
