#include <QElapsedTimer>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QtTest>

#include "core/appdatapaths.h"
#include "core/epgcache_service.h"
#include "core/epgservice.h"

using namespace OKILTV::Core;

class EpgSearchStoreTests : public QObject
{
    Q_OBJECT
  private:
    const QUuid profile{QStringLiteral("11111111-1111-1111-1111-111111111111")};
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-05T12:00:00Z"), Qt::ISODate);
    Channel channel(int id = 1, QString epg = QStringLiteral("epg")) const
    {
        Channel c;
        c.id = id;
        c.tvgId = std::move(epg);
        c.name = QStringLiteral("Channel %1").arg(id);
        c.profileId = profile;
        c.sortOrder = id;
        return c;
    }
    EpgEntry entry(QString title, int start = -60, int stop = 60, QString epg = QStringLiteral("epg"),
                   QString subtitle = {}) const
    {
        EpgEntry e;
        e.channelId = std::move(epg);
        e.title = std::move(title);
        e.subTitle = std::move(subtitle);
        e.description = QStringLiteral("Secret description keyword");
        e.start = now.addSecs(start);
        e.stop = now.addSecs(stop);
        return e;
    }
    EpgSearchRequest request(QString query = QStringLiteral("planet")) const
    {
        EpgSearchRequest r;
        r.profileId = profile.toString(QUuid::WithoutBraces);
        r.query = std::move(query);
        r.nowUtc = now;
        r.eligibleChannels = {channel()};
        return r;
    }
    std::shared_ptr<EpgStore> store(const QString &path, const QList<EpgEntry> &entries) const
    {
        return EpgStore::create(path, {profile, QStringLiteral("test-fingerprint"), now, 0},
                                [&](const EpgStore::Sink &sink)
                                {
                                    for (const auto &e : entries)
                                        sink(e);
                                });
    }
    void sameResult(const EpgSearchResult &a, const EpgSearchResult &b)
    {
        QCOMPARE(a.status, b.status);
        QCOMPARE(a.hasMore, b.hasMore);
        QCOMPARE(a.nextOffset, b.nextOffset);
        QCOMPARE(a.rows.size(), b.rows.size());
        for (int i = 0; i < a.rows.size(); ++i)
        {
            QCOMPARE(a.rows[i].resultKey, b.rows[i].resultKey);
            QCOMPARE(a.rows[i].sectionKey, b.rows[i].sectionKey);
            QCOMPARE(a.rows[i].program.title, b.rows[i].program.title);
            QCOMPARE(a.rows[i].titleHighlights, b.rows[i].titleHighlights);
            QCOMPARE(a.rows[i].subTitleHighlights, b.rows[i].subTitleHighlights);
        }
    }
  private slots:
    void actualQsqliteFts5()
    {
        QVERIFY2(EpgStore::supportsFts5(), "The delivered Qt QSQLITE plugin must support FTS5.");
    }
    void normalizationAndHighlights()
    {
        QCOMPARE(normalizeEpgSearchText(QStringLiteral("ŁÓDŹ Zażółć gęślą JAŹŃ")),
                 QStringLiteral("lodz zazolc gesla jazn"));
        const auto decomposed = QStringLiteral("Ło\u0301dz\u0301");
        QCOMPARE(normalizeEpgSearchText(decomposed), QStringLiteral("lodz"));
        const auto spans = epgSearchHighlights(decomposed, {QStringLiteral("lodz")});
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans.first().toMap().value(QStringLiteral("start")).toInt(), 0);
        QCOMPARE(spans.first().toMap().value(QStringLiteral("length")).toInt(), decomposed.size());
        QCOMPARE(normalizeEpgSearchText(QStringLiteral("Ελλάδα 東京 007")), QStringLiteral("ελλαδα 東京 007"));
    }
    void queryValidation()
    {
        for (const auto &q : {QString(), QStringLiteral(" ! * ( ) "), QStringLiteral("p"), QStringLiteral("a b")})
            QCOMPARE(epgSearchQueryError(q), QStringLiteral("query-too-short"));
        QVERIFY(epgSearchQueryError(QStringLiteral("a planet")).isEmpty());
        QCOMPARE(epgSearchQueryError(QString(257, u'a')), QStringLiteral("query-too-long"));
        QCOMPARE(epgSearchQueryError(QStringLiteral("ab ").repeated(17)), QStringLiteral("too-many-tokens"));
    }
    void punctuationPrefixes_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QString>("title");
        QTest::addColumn<int>("secondStart");
        QTest::newRow("hyphen") << QStringLiteral("Spider-M") << QStringLiteral("Spider-Man") << 7;
        QTest::newRow("hyphen-space") << QStringLiteral("Spider-M") << QStringLiteral("Spider Man") << 7;
        QTest::newRow("unicode-dash") << QStringLiteral("spider\u2013m") << QStringLiteral("Spider\u2014Man") << 7;
        QTest::newRow("quoted") << QStringLiteral("\"Spider-M\"") << QStringLiteral("Spider-Man") << 7;
        QTest::newRow("parentheses") << QStringLiteral("Spider (M)") << QStringLiteral("Spider-Man") << 7;
        QTest::newRow("colon") << QStringLiteral("Mission:I") << QStringLiteral("Mission:Impossible") << 8;
        QTest::newRow("slash") << QStringLiteral("Star/W") << QStringLiteral("Star/Wars") << 5;
        QTest::newRow("dot") << QStringLiteral("Doctor.W") << QStringLiteral("Doctor.Who") << 7;
        QTest::newRow("accent") << QStringLiteral("lodz-n") << QStringLiteral("Łódź-Nocą") << 5;
        QTest::newRow("digit") << QStringLiteral("Apollo-1") << QStringLiteral("Apollo-13") << 7;
    }
    void punctuationPrefixes()
    {
        QFETCH(QString, query);
        QFETCH(QString, title);
        QFETCH(int, secondStart);
        QTemporaryDir dir;
        const QList<EpgEntry> entries{entry(title),
            entry(QStringLiteral("Other programme"), 10, 70, QStringLiteral("epg"), title),
            entry(QStringLiteral("Spider-Zebra")), entry(QStringLiteral("NotSpider-Man"))};
        const auto sql = store(dir.filePath(QStringLiteral("epg.sqlite")), entries);
        const auto memory = std::make_shared<EpgService::Snapshot>(EpgService::buildSnapshot(entries));
        const auto r = request(query);
        const auto result = sql->search(r);
        QCOMPARE(result.status, EpgSearchStatus::Ready);
        QCOMPARE(result.rows.size(), 2);
        sameResult(result, EpgService::search(memory, r));
        QCOMPARE(result.rows[0].program.title, title);
        QCOMPARE(result.rows[1].program.subTitle, title);
        const QVariantList expected{
            QVariantMap{{QStringLiteral("start"), 0}, {QStringLiteral("length"), secondStart - 1}},
            QVariantMap{{QStringLiteral("start"), secondStart}, {QStringLiteral("length"), 1}}};
        QCOMPARE(result.rows[0].titleHighlights, expected);
        QCOMPARE(result.rows[1].subTitleHighlights, expected);
        QCOMPARE(sql->search(request(QStringLiteral("Spider-Z"))).rows.size(), 1);
        QCOMPARE(sql->search(request(QStringLiteral("Spider-X"))).rows.size(), 0);
        QCOMPARE(sql->search(request(QStringLiteral("ider-M"))).rows.size(), 0);
        QCOMPARE(sql->search(request(QStringLiteral("M"))).status, EpgSearchStatus::Ready);
        QCOMPARE(sql->search(request(QStringLiteral("M"))).rows.size(), 0);
    }
    void indexedAndMemorySemantics()
    {
        QTemporaryDir dir;
        QList<EpgEntry> entries{
            entry(QStringLiteral("Planeta Ziemia")),
            entry(QStringLiteral("Ziemia"), -30, 30, QStringLiteral("epg"), QStringLiteral("Planeta")),
            entry(QStringLiteral("Planeta Ziemia"), 10, 70),
            entry(QStringLiteral("ŁÓDŹ nocą"), -120, -60),
            entry(QStringLiteral("Ło\u0301dz\u0301 nocą"), 120, 180),
            entry(QStringLiteral("Zażółć gęślą jaźń")),
            entry(QStringLiteral("<a & \"Planet\">")),
            entry(QStringLiteral("OR Planeta")),
            entry(QStringLiteral("Ελλάδα 東京 007")),
            entry(QStringLiteral("Planeta Ziemia"), 100, 90),
            entry(QStringLiteral("Planeta Ziemia"), -20, 20, QStringLiteral("other"))};
        entries += entries.first();
        auto invalidStart = entry(QStringLiteral("Planeta Ziemia"));
        invalidStart.start = {};
        entries += invalidStart;
        auto invalidStop = entry(QStringLiteral("Planeta Ziemia"));
        invalidStop.stop = {};
        entries += invalidStop;
        const auto sql = store(dir.filePath(QStringLiteral("epg.sqlite")), entries);
        const auto memory = std::make_shared<EpgService::Snapshot>(EpgService::buildSnapshot(entries));
        for (const auto &q : {QStringLiteral("planet ziem"), QStringLiteral("lodz"), QStringLiteral("ŁÓDŹ"),
                              QStringLiteral("zażółć gęś"), QStringLiteral("iemia"), QStringLiteral("secret"),
                              QStringLiteral("planet missing"), QStringLiteral("a planet"), QStringLiteral("planet OR"),
                              QStringLiteral("p planet"), QStringLiteral("ελλ 東京"), QStringLiteral("007"),
                              QStringLiteral("\"planet\" * ( )"), QStringLiteral("planet'; DROP TABLE programmes; --")})
        {
            auto r = request(q);
            r.eligibleChannels += channel(2, QStringLiteral(" EPG "));
            for (const auto filter : {EpgSearchTimeFilter::All, EpgSearchTimeFilter::Now, EpgSearchTimeFilter::Upcoming,
                                      EpgSearchTimeFilter::Past})
            {
                r.timeFilter = filter;
                sameResult(sql->search(r), EpgService::search(memory, r));
            }
        }
        QCOMPARE(sql->search(request(QStringLiteral("iemia"))).rows.size(), 0);
        QCOMPARE(sql->search(request(QStringLiteral("secret"))).rows.size(), 0);
        QCOMPARE(sql->search(request(QStringLiteral("lodz"))).rows.size(), 2);
    }
    void filtersRankingAndSharedEpg()
    {
        QTemporaryDir dir;
        const auto sql =
            store(dir.filePath(QStringLiteral("epg.sqlite")),
                  {entry(QStringLiteral("Planeta")), entry(QStringLiteral("Planeta Ziemia")),
                   entry(QStringLiteral("Other"), -60, 60, QStringLiteral("epg"), QStringLiteral("Planeta")),
                   entry(QStringLiteral("Planeta"), 10, 50), entry(QStringLiteral("Planeta"), 20, 50),
                   entry(QStringLiteral("Planeta"), -100, -50), entry(QStringLiteral("Planeta"), -200, -150)});
        auto r = request(QStringLiteral("planeta"));
        r.eligibleChannels += channel(2);
        Channel foreign = channel(3);
        foreign.profileId = QUuid::createUuid();
        r.eligibleChannels += foreign;
        const auto result = sql->search(r);
        QCOMPARE(result.rows.size(), 14);
        QCOMPARE(result.rows[0].program.title, QStringLiteral("Planeta"));
        QCOMPARE(result.rows[0].channel.id, 1);
        QCOMPARE(result.rows[1].channel.id, 2);
        QCOMPARE(result.rows[2].program.title, QStringLiteral("Planeta Ziemia"));
        QCOMPARE(result.rows[4].program.title, QStringLiteral("Other"));
        QCOMPARE(result.rows[6].sectionKey, QStringLiteral("upcoming"));
        QCOMPARE(result.rows[10].program.start, now.addSecs(-100));
        r.timeFilter = EpgSearchTimeFilter::Past;
        QCOMPARE(sql->search(r).rows.size(), 4);
        r.profileId = QUuid::createUuid().toString();
        QCOMPARE(sql->search(r).errorCode, QStringLiteral("profile-mismatch"));
    }
    void mappingBeforePagesAndStableTies()
    {
        QTemporaryDir dir;
        QList<EpgEntry> entries;
        for (int i = 0; i < 2000; ++i)
            entries += entry(QStringLiteral("Planeta"), i + 10, i + 100, QStringLiteral("hidden"));
        for (int i = 0; i < 130; ++i)
            entries += entry(QStringLiteral("Planeta %1").arg(i), 10, 100);
        const auto sql = store(dir.filePath(QStringLiteral("epg.sqlite")), entries);
        const auto memory = std::make_shared<EpgService::Snapshot>(EpgService::buildSnapshot(entries));
        auto r = request();
        QSet<QString> keys;
        for (int i = 0; i < 3; ++i)
        {
            const auto result = sql->search(r);
            sameResult(result, EpgService::search(memory, r));
            QCOMPARE(result.rows.size(), i < 2 ? 50 : 30);
            QCOMPARE(result.hasMore, i < 2);
            for (const auto &row : result.rows)
            {
                QVERIFY(!keys.contains(row.resultKey));
                keys.insert(row.resultKey);
            }
            r.offset = result.nextOffset;
        }
        QCOMPARE(keys.size(), 130);
    }
    void cancellationAndFailureKeepGuide()
    {
        QTemporaryDir dir;
        QList<EpgEntry> entries;
        for (int i = 0; i < 3000; ++i)
            entries += entry(QStringLiteral("Planeta"), i, i + 100, QStringLiteral("hidden"));
        const auto sql = store(dir.filePath(QStringLiteral("epg.sqlite")), entries);
        int polls = 0;
        QCOMPARE(sql->search(request(), [&] { return ++polls > 50; }).status, EpgSearchStatus::Cancelled);
        QCOMPARE(sql->range(QStringLiteral("hidden"), now, now.addSecs(4000)).size(), 3000);
        const auto failedPath = dir.filePath(QStringLiteral("interrupted.sqlite"));
        bool threw = false;
        try
        {
            sql->withSearchIndex(failedPath, [] { return true; });
        }
        catch (...)
        {
            threw = true;
        }
        QVERIFY(threw);
        QVERIFY(!QFileInfo::exists(failedPath));
        QCOMPARE(sql->range(QStringLiteral("hidden"), now, now.addSecs(4000)).size(), 3000);
    }
    void datesPrecedeMatchQualityAcrossPages()
    {
        QTemporaryDir dir;
        const QList<EpgEntry> entries {
            entry(QStringLiteral("Planeta current"), -20, 100),
            entry(QStringLiteral("Planet"), -10, 50),
            entry(QStringLiteral("Other upcoming"), 10, 60, QStringLiteral("epg"), QStringLiteral("Planet")),
            entry(QStringLiteral("Planeta upcoming"), 20, 70),
            entry(QStringLiteral("Planet"), 30, 80),
            entry(QStringLiteral("Other past"), -80, -20, QStringLiteral("epg"), QStringLiteral("Planet")),
            entry(QStringLiteral("Planeta past"), -90, -30),
            entry(QStringLiteral("Planet"), -100, -40)
        };
        const auto sql = store(dir.filePath(QStringLiteral("dates.sqlite")), entries);
        const auto memory = std::make_shared<EpgService::Snapshot>(EpgService::buildSnapshot(entries));
        for (const auto filter : {EpgSearchTimeFilter::All, EpgSearchTimeFilter::Now,
                 EpgSearchTimeFilter::Upcoming, EpgSearchTimeFilter::Past}) {
            auto r = request();
            r.eligibleChannels += channel(2);
            r.pageSize = 3;
            r.timeFilter = filter;
            QList<qint64> actual;
            QSet<QString> identities;
            for (;;) {
                const auto result = sql->search(r);
                sameResult(result, EpgService::search(memory, r));
                for (const auto& row : result.rows) {
                    QVERIFY(!identities.contains(row.resultKey));
                    identities.insert(row.resultKey);
                    actual += now.secsTo(row.program.start);
                }
                if (!result.hasMore) break;
                QVERIFY(result.nextOffset > r.offset);
                r.offset = result.nextOffset;
            }
            QList<qint64> expected;
            if (filter == EpgSearchTimeFilter::All || filter == EpgSearchTimeFilter::Now)
                expected += { -20, -20, -10, -10 };
            if (filter == EpgSearchTimeFilter::All || filter == EpgSearchTimeFilter::Upcoming)
                expected += { 10, 10, 20, 20, 30, 30 };
            if (filter == EpgSearchTimeFilter::All || filter == EpgSearchTimeFilter::Past)
                expected += { -80, -80, -90, -90, -100, -100 };
            QCOMPARE(actual, expected);
        }
    }
    void chronologicalPaginationAcrossAllSections()
    {
        QTemporaryDir dir;
        QList<EpgEntry> entries;
        for (int i = 0; i < 240; ++i)
        {
            const auto title = i % 3 == 0   ? QStringLiteral("Planet")
                               : i % 3 == 1 ? QStringLiteral("Planeta %1").arg(i)
                                            : QStringLiteral("Other %1").arg(i);
            const auto subtitle = i % 3 == 2 ? QStringLiteral("Planet %1").arg(i) : QStringLiteral("Episode %1").arg(i);
            const int start = i < 80 ? -60 : i < 160 ? (i - 80) / 4 + 10 : -((i - 160) / 4 + 10) - 300;
            const int stop = i < 80 ? 60 : start + 120;
            entries += entry(title, start, stop, QStringLiteral("epg"), subtitle);
        }
        const auto sql = store(dir.filePath(QStringLiteral("epg.sqlite")), entries);
        const auto memory = std::make_shared<EpgService::Snapshot>(EpgService::buildSnapshot(entries));
        for (const auto filter : {EpgSearchTimeFilter::All, EpgSearchTimeFilter::Now, EpgSearchTimeFilter::Upcoming,
                                  EpgSearchTimeFilter::Past})
        {
            auto r = request();
            r.eligibleChannels += channel(2);
            r.pageSize = 37;
            r.timeFilter = filter;
            for (;;)
            {
                const auto result = sql->search(r);
                sameResult(result, EpgService::search(memory, r));
                if (!result.hasMore)
                    break;
                QVERIFY(result.nextOffset > r.offset);
                r.offset = result.nextOffset;
            }
        }
    }
    void chronologicalLargeSetsAgreeWithMemory()
    {
        QTemporaryDir dir;
        for (const bool largeExactSet : {false, true})
        {
            QList<EpgEntry> entries;
            for (int i = 0; i < (largeExactSet ? 4200 : 6000); ++i)
                entries +=
                    entry(largeExactSet ? QStringLiteral("Planeta Ziemia") : QStringLiteral("Planeta Ziemia %1").arg(i),
                          100 + (largeExactSet ? i % 13 : i), 9000, QStringLiteral("epg"),
                          QStringLiteral("Episode %1").arg(i));
            if (!largeExactSet)
            {
                entries += entry(QStringLiteral("Planeta Ziemia"), 50, 100, QStringLiteral("hidden"));
                entries += entry(QStringLiteral("PLANETA ZIEMIA"), 7000, 8000);
            }
            const auto sql = store(
                dir.filePath(largeExactSet ? QStringLiteral("common.sqlite") : QStringLiteral("rare.sqlite")), entries);
            const auto memory = std::make_shared<EpgService::Snapshot>(EpgService::buildSnapshot(entries));
            auto r = request(QStringLiteral("planeta ziemia"));
            r.timeFilter = EpgSearchTimeFilter::Upcoming;
            r.pageSize = 37;
            r.eligibleChannels += channel(2);
            for (int page = 0; page < 3; ++page)
            {
                const auto result = sql->search(r);
                sameResult(result, EpgService::search(memory, r));
                QCOMPARE(result.rows.size(), 37);
                QVERIFY(result.hasMore);
                if (!largeExactSet && page == 0)
                    QCOMPARE(result.rows.first().program.title, QStringLiteral("Planeta Ziemia 0"));
                r.offset = result.nextOffset;
            }
            int polls = 0;
            QCOMPARE(sql->search(r, [&] { return ++polls > 8; }).status, EpgSearchStatus::Cancelled);
        }
    }
    void cancellationDuringFtsPopulationKeepsPreviousGeneration()
    {
        QTemporaryDir dir;
        const auto previous = store(dir.filePath(QStringLiteral("previous.sqlite")), {entry(QStringLiteral("Planet"))});
        const auto pendingPath = dir.filePath(QStringLiteral("interrupted-index.sqlite"));
        bool producerFinished = false, interrupted = false;
        int populationPolls = 0;
        try
        {
            EpgStore::create(
                pendingPath, {profile, QStringLiteral("test-fingerprint"), now, 0},
                [&](const EpgStore::Sink &sink)
                {
                    for (int i = 0; i < 6000; ++i)
                        sink(entry(QStringLiteral("Planeta %1").arg(i), i, i + 100));
                    producerFinished = true;
                },
                false, [&] { return producerFinished && ++populationPolls > 64; });
        }
        catch (const std::exception &)
        {
            interrupted = true;
        }
        QVERIFY(producerFinished);
        QVERIFY(interrupted);
        QCOMPARE(populationPolls, 65);
        QVERIFY(!QFileInfo::exists(pendingPath));
        QCOMPARE(previous->search(request()).rows.size(), 1);
        QCOMPARE(previous->range(QStringLiteral("epg"), now.addSecs(-100), now.addSecs(100)).size(), 1);
    }
    void legacyCacheUpgradeIsImmutable()
    {
        QTemporaryDir dir;
        const auto path = dir.filePath(QStringLiteral("old.sqlite"));
        auto old = store(path, {entry(QStringLiteral("Planeta Ziemia"))});
        old->keep();
        old.reset();
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("DROP TABLE programme_search")));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE search_documents")));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE search_metadata")));
        }
        QSqlDatabase::removeDatabase(name);
        old = EpgStore::open(path);
        QCOMPARE(old->search(request()).status, EpgSearchStatus::Preparing);
        QVERIFY(!old->range(QStringLiteral("epg"), now.addSecs(-100), now.addSecs(100)).isEmpty());
        const auto size = QFileInfo(path).size();
        auto rebuilt = old->withSearchIndex(dir.filePath(QStringLiteral("indexed.sqlite")));
        old->useSearchIndex(rebuilt);
        QVERIFY(old->searchReady());
        QCOMPARE(old->search(request()).rows.size(), 1);
        QCOMPARE(QFileInfo(path).size(), size);
        // Old-reader metadata version remains compatible with rollback builds.
        const auto n = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), n);
            db.setDatabaseName(rebuilt->path());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("SELECT version FROM metadata")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1);
        }
        QSqlDatabase::removeDatabase(n);
    }
    void emptyAndCorruptIndex()
    {
        QTemporaryDir dir;
        const auto empty = store(dir.filePath(QStringLiteral("empty.sqlite")), {});
        QCOMPARE(empty->search(request()).status, EpgSearchStatus::NoEpg);
        const auto sql = store(dir.filePath(QStringLiteral("epg.sqlite")), {entry(QStringLiteral("Planet"))});
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(sql->path());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("DROP TABLE programme_search")));
        }
        QSqlDatabase::removeDatabase(name);
        QCOMPARE(sql->search(request()).status, EpgSearchStatus::Error);
        QCOMPARE(sql->range(QStringLiteral("epg"), now.addSecs(-100), now.addSecs(100)).size(), 1);
    }
    void backgroundLegacyUpgradeAndRetry()
    {
        QTemporaryDir dir;
        RuntimeContext runtime;
        runtime.dataRootOverride = dir.path();
        AppDataPaths::initializeRuntime(runtime);
        const auto cleanup = qScopeGuard(
            []
            {
                EpgService::importPool()->waitForDone();
                AppDataPaths::resetRuntimeForTests();
            });
        EpgCacheService cache;
        EpgCacheService::CacheData data;
        data.profileId = profile;
        data.sourceFingerprint = QStringLiteral("test-fingerprint");
        data.fetchedAt = now;
        const auto path = QDir(AppDataPaths::epgCacheDirectory())
                              .filePath(profile.toString(QUuid::WithoutBraces) + QStringLiteral("-legacy.sqlite"));
        data.snapshot.store = store(path, {entry(QStringLiteral("Planeta"))});
        cache.save(data);
        data = {};
        const auto connectionName = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("DROP TABLE programme_search")));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE search_documents")));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE search_metadata")));
        }
        QSqlDatabase::removeDatabase(connectionName);
        QSemaphore entered, release;
        EpgService::importPool()->start(
            [&]
            {
                entered.release();
                release.acquire();
            });
        const auto releaseWorker = qScopeGuard(
            [&]
            {
                release.release();
                EpgService::importPool()->waitForDone();
            });
        QVERIFY(entered.tryAcquire(1, 5000));
        auto token = EpgCacheService::beginImport(profile);
        auto loaded = cache.load(profile, token);
        QCOMPARE(loaded.status, EpgCacheService::LoadStatus::Loaded);
        QCOMPARE(loaded.data.snapshot.store->search(request()).status, EpgSearchStatus::Preparing);
        QCOMPARE(loaded.data.snapshot.store->range(QStringLiteral("epg"), now.addSecs(-100), now.addSecs(100)).size(),
                 1);
        EpgCacheService::cancel(token);
        release.release();
        QVERIFY(EpgService::importPool()->waitForDone(5000));
        QCOMPARE(loaded.data.snapshot.store->search(request()).errorCode,
                 QStringLiteral("search-index-preparation-failed"));
        QCOMPARE(loaded.data.snapshot.store->range(QStringLiteral("epg"), now.addSecs(-100), now.addSecs(100)).size(),
                 1);
        QFile manifest(EpgCacheService::manifestFile(profile));
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto previousManifest = manifest.readAll();
        manifest.close();
        QVERIFY(previousManifest.contains(QFileInfo(path).fileName().toUtf8()));
        EpgCacheService::prepareSearch(loaded.data.snapshot.store);
        QTRY_VERIFY_WITH_TIMEOUT(loaded.data.snapshot.store->searchReady(), 5000);
        QCOMPARE(loaded.data.snapshot.store->search(request()).rows.size(), 1);
        QVERIFY(manifest.open(QIODevice::ReadOnly));
        QVERIFY(manifest.readAll() != previousManifest);
        const auto latest = cache.load(profile);
        QCOMPARE(latest.status, EpgCacheService::LoadStatus::Loaded);
        QVERIFY(latest.data.snapshot.store->searchReady());
    }
    void benchmark()
    {
        const int count = qEnvironmentVariableIntValue("OKILTV_EPG_SEARCH_BENCHMARK");
        if (count <= 0)
            QSKIP("Set OKILTV_EPG_SEARCH_BENCHMARK=100000 or 1000000 for explicit measurements.");
        QTemporaryDir dir;
        QElapsedTimer timer;
        timer.start();
        const auto sql =
            EpgStore::create(dir.filePath(QStringLiteral("bench.sqlite")), {profile, QStringLiteral("bench"), now, 0},
                             [&](const EpgStore::Sink &sink)
                             {
                                 for (int i = 0; i < count; ++i)
                                 {
                                     auto e = entry(i == count - 1 ? QStringLiteral("Planeta Ziemia")
                                                    : i % 997 == 0 ? QStringLiteral("Rare unique planet %1").arg(i)
                                                                   : QStringLiteral("Planeta Ziemia %1").arg(i),
                                                    (i % 2000 - 1000) * 600, (i % 2000 - 1000) * 600 + 1800,
                                                    QStringLiteral("epg%1").arg(i % 1000));
                                     sink(e);
                                 }
                             });
        qInfo().noquote() << QStringLiteral("EPG benchmark entries=%1 buildMs=%2 dbBytes=%3 Qt=%4")
                                 .arg(count)
                                 .arg(timer.elapsed())
                                 .arg(QFileInfo(sql->path()).size())
                                 .arg(qVersion());
        const auto connectionName = QUuid::createUuid().toString();
        bool haveIndexSize = false;
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
            db.setDatabaseName(sql->path());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("SELECT sqlite_version()")));
            QVERIFY(q.next());
            qInfo().noquote() << QStringLiteral("SQLite=%1").arg(q.value(0).toString());
            if (q.exec(QStringLiteral("SELECT sum(pgsize) FROM dbstat WHERE name LIKE 'programme_search%' OR name LIKE "
                                      "'%search_documents%' OR name LIKE '%search_order%' OR name='search_exact_title' "
                                      "OR name='search_metadata'")) &&
                q.next())
            {
                haveIndexSize = true;
                qInfo().noquote() << QStringLiteral("searchIndexBytes=%1").arg(q.value(0).toLongLong());
            }
            else
                qInfo() << "searchIndexBytes unavailable: QSQLITE dbstat module absent";
        }
        QSqlDatabase::removeDatabase(connectionName);
        if (!haveIndexSize)
        {
            // Capability testing uses Qt's SQLite, whose SDK build omits dbstat.
            // Measure a compact fixture copy without search tables instead.
            const auto basePath = dir.filePath(QStringLiteral("baseline.sqlite"));
            QVERIFY(QFile::copy(sql->path(), basePath));
            const auto baseConnection = QUuid::createUuid().toString();
            {
                auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), baseConnection);
                db.setDatabaseName(basePath);
                QVERIFY(db.open());
                QSqlQuery q(db);
                for (const auto *statement : {"DROP TABLE programme_search", "DROP TABLE search_documents",
                                              "DROP TABLE search_order", "DROP TABLE search_metadata", "VACUUM"})
                    QVERIFY(q.exec(QString::fromLatin1(statement)));
            }
            QSqlDatabase::removeDatabase(baseConnection);
            qInfo().noquote() << QStringLiteral("searchIncrementalFileBytes=%1 compactBaselineBytes=%2")
                                     .arg(QFileInfo(sql->path()).size() - QFileInfo(basePath).size())
                                     .arg(QFileInfo(basePath).size());
            QFile::remove(basePath);
        }
        QFile processStatus(QStringLiteral("/proc/self/status"));
        if (processStatus.open(QIODevice::ReadOnly))
        {
            for (const auto &line : processStatus.readAll().split('\n'))
                if (line.startsWith("VmRSS:") || line.startsWith("VmHWM:"))
                    qInfo().noquote() << line;
        }
        auto r = request();
        r.eligibleChannels.clear();
        for (int i = 0; i < 1000; ++i)
            r.eligibleChannels += channel(i, QStringLiteral("epg%1").arg(i));
        for (const auto &query : {QStringLiteral("rare unique"), QStringLiteral("pl"), QStringLiteral("planet ziem"),
                                  QStringLiteral("unfindableword"), QStringLiteral("planeta ziemia")})
        {
            r.query = query;
            r.timeFilter =
                query == QStringLiteral("planeta ziemia") ? EpgSearchTimeFilter::Upcoming : EpgSearchTimeFilter::All;
            QList<qint64> samples;
            for (int run = 0; run < 6; ++run)
            {
                timer.restart();
                const auto result = sql->search(r);
                const auto ms = timer.elapsed();
                QCOMPARE(result.status, EpgSearchStatus::Ready);
                samples += ms;
                if (run == 0)
                    qInfo().noquote() << QStringLiteral("firstAfterBuild queryKind=%1 ms=%2 rows=%3 hasMore=%4")
                                             .arg(query == QStringLiteral("pl") ? QStringLiteral("popular-prefix")
                                                  : query == QStringLiteral("rare unique") ? QStringLiteral("rare")
                                                  : query == QStringLiteral("planet ziem") ? QStringLiteral("multiple")
                                                  : query == QStringLiteral("planeta ziemia")
                                                      ? QStringLiteral("rare-exact-common-words")
                                                      : QStringLiteral("absent"))
                                             .arg(ms)
                                             .arg(result.rows.size())
                                             .arg(result.hasMore);
            }
            samples.removeFirst();
            std::sort(samples.begin(), samples.end());
            qInfo().noquote() << QStringLiteral("warm p95Ms=%1").arg(samples.last());
        }
        int polls = 0;
        timer.restart();
        r.query = QStringLiteral("pl");
        r.timeFilter = EpgSearchTimeFilter::All;
        QCOMPARE(sql->search(r, [&] { return ++polls > 64; }).status, EpgSearchStatus::Cancelled);
        qInfo() << "cancelMs=" << timer.elapsed() << "polls=" << polls;
    }
    // Last in the suite: shutdown deliberately fences this process forever.
    void shutdownCancelsUnownedUpgradeAndPreventsLateScheduling()
    {
        QTemporaryDir dir;
        RuntimeContext runtime;
        runtime.dataRootOverride = dir.path();
        AppDataPaths::initializeRuntime(runtime);
        const auto cleanup = qScopeGuard(
            []
            {
                EpgService::importPool()->waitForDone();
                AppDataPaths::resetRuntimeForTests();
            });
        EpgCacheService cache;
        EpgCacheService::CacheData data;
        data.profileId = profile;
        data.sourceFingerprint = QStringLiteral("test-fingerprint");
        data.fetchedAt = now;
        const auto path = QDir(AppDataPaths::epgCacheDirectory())
                              .filePath(profile.toString(QUuid::WithoutBraces) + QStringLiteral("-shutdown.sqlite"));
        data.snapshot.store = store(path, {entry(QStringLiteral("Planeta"))});
        cache.save(data);
        data = {};
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("DROP TABLE programme_search")));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE search_documents")));
            QVERIFY(q.exec(QStringLiteral("DROP TABLE search_metadata")));
        }
        QSqlDatabase::removeDatabase(name);
        QSemaphore entered, release;
        EpgService::importPool()->start(
            [&]
            {
                entered.release();
                release.acquire();
            });
        const auto unblock = qScopeGuard(
            [&]
            {
                release.release();
                EpgService::importPool()->waitForDone();
            });
        QVERIFY(entered.tryAcquire(1, 5000));
        const auto loaded = cache.load(profile); // Upgrade has no AppController import token.
        QCOMPARE(loaded.status, EpgCacheService::LoadStatus::Loaded);
        EpgCacheService::prepareSearch(loaded.data.snapshot.store);
        const auto observedImport = EpgCacheService::beginImport(QUuid::createUuid());
        const auto releaseThread = std::unique_ptr<QThread>(QThread::create(
            [&]
            {
                while (!observedImport->load())
                    QThread::yieldCurrentThread();
                release.release();
            }));
        releaseThread->start();
        EpgCacheService::shutdownSearchPreparations();
        QVERIFY(releaseThread->wait(5000));
        QCOMPARE(EpgService::importPool()->activeThreadCount(), 0);
        QCOMPARE(loaded.data.snapshot.store->search(request()).errorCode,
                 QStringLiteral("search-index-preparation-failed"));
        QCOMPARE(loaded.data.snapshot.store->range(QStringLiteral("epg"), now.addSecs(-100), now.addSecs(100)).size(),
                 1);
        EpgCacheService::prepareSearch(loaded.data.snapshot.store);
        const auto reloaded = cache.load(profile);
        QCOMPARE(reloaded.status, EpgCacheService::LoadStatus::Loaded);
        QCOMPARE(EpgService::importPool()->activeThreadCount(), 0);
        QVERIFY(EpgCacheService::beginImport(profile)->load());
        QCOMPARE(QDir(AppDataPaths::epgCacheDirectory()).entryList({QStringLiteral("*.sqlite")}, QDir::Files).size(),
                 1);
    }
};

QTEST_GUILESS_MAIN(EpgSearchStoreTests)
#include "tst_epg_search_store.moc"
