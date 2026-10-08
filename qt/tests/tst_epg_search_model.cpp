#include "app/epgsearchcontroller.h"
#include <QAbstractItemModelTester>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
using namespace OKILTV;
namespace {
Core::EpgSearchRow row(const QString& key)
{
    Core::EpgSearchRow r;
    r.resultKey = key;
    r.channel.id = 1;
    r.channel.profileId = QUuid::createUuid();
    r.channel.name = QStringLiteral("Channel");
    r.channel.tvgId = QStringLiteral("epg");
    r.program.channelId = r.channel.tvgId;
    r.program.title = key;
    r.program.start = QDateTime::currentDateTimeUtc().addSecs(-10);
    r.program.stop = r.program.start.addSecs(3600);
    r.sectionKey = QStringLiteral("now");
    return r;
}
App::EpgSearchController::Context context()
{
    App::EpgSearchController::Context c;
    c.request.profileId = QStringLiteral("source");
    c.request.channelRevision = 1;
    c.request.epgGeneration = 1;
    c.request.eligibleChannels = { row(QStringLiteral("scope")).channel };
    c.snapshot = std::make_shared<Core::EpgService::Snapshot>();
    return c;
}
App::EpgSearchController::Actions actions(int* activated)
{
    App::EpgSearchController::Actions a;
    a.state = [](const auto&, const auto&) {
        return QVariantMap { { QStringLiteral("primaryEnabled"), true },
            { QStringLiteral("actionKind"), QStringLiteral("live") } };
    };
    a.play = [activated](const auto&, const auto&, bool) {
        ++*activated;
        return true;
    };
    return a;
}
} // namespace
class EpgSearchModelTests : public QObject {
    Q_OBJECT
private slots:
    void broadcastStateUsesRefreshedClockWithoutRegrouping()
    {
        App::EpgSearchModel model;
        const auto start = QDateTime::fromString(QStringLiteral("2026-10-07T12:00:00Z"), Qt::ISODate);
        auto result = row(QStringLiteral("airing"));
        result.program.start = start;
        result.program.stop = start.addSecs(3600);
        result.sectionKey = QStringLiteral("upcoming");
        model.replace({ result });
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QCOMPARE(model.roleNames().value(App::EpgSearchModel::BroadcastStateRole), QByteArray("broadcastState"));
        const QList<QDateTime> times { start.addMSecs(-1), start, result.program.stop.addMSecs(-1), result.program.stop };
        const QStringList states { QStringLiteral("upcoming"), QStringLiteral("now"), QStringLiteral("now"), QStringLiteral("past") };
        for (int i = 0; i < times.size(); ++i) {
            model.refreshLabels(times.at(i));
            const auto index = model.index(0);
            QCOMPARE(model.data(index, App::EpgSearchModel::BroadcastStateRole).toString(), states.at(i));
            const auto label = model.data(index, App::EpgSearchModel::StatusLabelRole).toString();
            if (states.at(i) == QStringLiteral("upcoming"))
                QCOMPARE(label, QStringLiteral("Upcoming"));
            else if (states.at(i) == QStringLiteral("past"))
                QCOMPARE(label, QStringLiteral("Past broadcast"));
            else
                QVERIFY(label.startsWith(QStringLiteral("On now")));
            QCOMPARE(model.data(index, App::EpgSearchModel::SectionKeyRole).toString(), result.sectionKey);
            QCOMPARE(model.indexOfKey(result.resultKey), 0);
            const auto roles = qvariant_cast<QList<int>>(changed.last().at(2));
            QVERIFY(roles.contains(App::EpgSearchModel::BroadcastStateRole));
            QVERIFY(roles.contains(App::EpgSearchModel::StatusLabelRole));
        }
        QCOMPARE(changed.count(), static_cast<int>(times.size()));
        QCOMPARE(reset.count(), 0);
    }
    void channelLogosUseLocalFileUrls()
    {
        App::EpgSearchModel model;
        auto result = row(QStringLiteral("logo"));
        result.channel.iconUrl = QStringLiteral("https://example.invalid/station.png");
        model.replace({ result });
        QCOMPARE(model.data(model.index(0), App::EpgSearchModel::ChannelLogoRole).toString(),
            result.channel.iconUrl);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        result.channel.cachedIconPath = directory.filePath(QStringLiteral("Station #1 logo.png"));
        model.replace({ result });
        const QUrl source(model.data(model.index(0), App::EpgSearchModel::ChannelLogoRole).toString());
        QVERIFY(source.isLocalFile());
        QCOMPARE(source.toLocalFile(), result.channel.cachedIconPath);
        result.channel.cachedIconPath.clear();
        result.channel.iconUrl.clear();
        model.replace({ result });
        QVERIFY(model.data(model.index(0), App::EpgSearchModel::ChannelLogoRole).toString().isEmpty());
    }
    void modelInsertionsAndSelection()
    {
        App::EpgSearchModel model;
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        model.replace({ row(QStringLiteral("a")) });
        model.append({ row(QStringLiteral("a")), row(QStringLiteral("b")) });
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(model.indexOfKey(QStringLiteral("b")), 1);
        QVERIFY(!model.row(-1));
    }
    void debounceInvalidatesActionsImmediately()
    {
        auto c = context();
        int activated = 0;
        App::EpgSearchController ctl([&] { return c; },
            [](const auto&, const auto& request, const auto&) {
                Core::EpgSearchResult r;
                r.request = request;
                r.rows = { row(request.query) };
                return r;
            },
            actions(&activated));
        ctl.openSession();
        QCOMPARE(ctl.status(), QStringLiteral("idle"));
        QVERIFY(ctl.errorText().isEmpty());
        ctl.setQuery(QStringLiteral("a b"));
        QCOMPARE(ctl.status(), QStringLiteral("idle"));
        QVERIFY(!ctl.busy());
        ctl.setQuery(QStringLiteral("planet"));
        QTRY_VERIFY(ctl.resultsCurrent());
        QCOMPARE(ctl.selectedKey(), QStringLiteral("planet"));
        ctl.setQuery(QStringLiteral("earth"));
        QVERIFY(!ctl.resultsCurrent());
        ctl.activateSelected();
        QCOMPARE(activated, 0);
        QTRY_VERIFY(ctl.resultsCurrent());
        ctl.activateSelected();
        QCOMPARE(activated, 1);
        ctl.setQuery({ });
        QVERIFY(ctl.expanded());
        QVERIFY(!ctl.resultsCurrent());
        QCOMPARE(ctl.model()->rowCount(), 0);
    }
    void latestPendingAndCloseFence()
    {
        auto c = context();
        QSemaphore started, release;
        std::atomic<int> count = 0;
        int activated = 0;
        App::EpgSearchController ctl([&] { return c; },
            [&](const auto&, const auto& request, const auto&) {
                ++count;
                if (request.query == QStringLiteral("first")) {
                    started.release();
                    release.acquire();
                }
                Core::EpgSearchResult r;
                r.request = request;
                r.rows = { row(request.query) };
                return r;
            },
            actions(&activated));
        ctl.openSession();
        ctl.setQuery(QStringLiteral("first"));
        QTRY_VERIFY(started.available() > 0);
        started.acquire();
        ctl.setQuery(QStringLiteral("second"));
        ctl.setQuery(QStringLiteral("latest"));
        QTRY_VERIFY(ctl.busy());
        release.release();
        QTRY_COMPARE(ctl.selectedKey(), QStringLiteral("latest"));
        QCOMPARE(count.load(), 2);
        ctl.closeSession();
        QVERIFY(!ctl.resultsCurrent());
        QCOMPARE(ctl.model()->rowCount(), 0);
        ctl.activateSelected();
        QCOMPARE(activated, 0);
    }
    void closeDuringRequestRejectsLatePublication()
    {
        auto c = context();
        QSemaphore started, release;
        std::atomic_bool completed = false;
        App::EpgSearchController ctl([&] { return c; },
            [&](const auto&, const auto& request, const auto&) {
                started.release();
                release.acquire();
                Core::EpgSearchResult result;
                result.request = request;
                result.rows = { row(QStringLiteral("late")) };
                completed.store(true);
                return result;
            });
        ctl.openSession();
        ctl.setQuery(QStringLiteral("query"));
        QTRY_VERIFY(started.available() > 0);
        started.acquire();
        ctl.closeSession();
        QSignalSpy changed(&ctl, &App::EpgSearchController::stateChanged);
        release.release();
        QTRY_VERIFY(completed.load());
        ctl.shutdown();
        QCoreApplication::processEvents();
        QVERIFY(!ctl.active());
        QVERIFY(!ctl.resultsCurrent());
        QCOMPARE(ctl.model()->rowCount(), 0);
        // shutdown emits its own state changes; the completed worker cannot publish rows.
        QVERIFY(changed.count() >= 1);
    }
    void destructionJoinsWorkerAndCannotPublish()
    {
        auto c = context();
        QSemaphore started, release;
        std::atomic_bool completed = false;
        int publications = 0;
        App::EpgSearchController::Actions a;
        a.state = [&](const auto&, const auto&) {
            ++publications;
            return QVariantMap { };
        };
        auto ctl = std::make_unique<App::EpgSearchController>([&] { return c; },
            [&](const auto&, const auto& request, const auto&) {
                started.release();
                release.acquire();
                Core::EpgSearchResult result;
                result.request = request;
                result.rows = { row(QStringLiteral("late")) };
                completed.store(true);
                return result;
            },
            a);
        ctl->openSession();
        ctl->setQuery(QStringLiteral("query"));
        QTRY_VERIFY(started.available() > 0);
        started.acquire();
        release.release();
        ctl.reset();
        QVERIFY(completed.load());
        QCoreApplication::processEvents();
        QCOMPARE(publications, 0);
    }
    void pagesRetainSelectionAndRetry()
    {
        auto c = context();
        std::atomic_bool fail = true;
        App::EpgSearchController ctl([&] { return c; },
            [&](const auto&, const auto& request, const auto&) {
                Core::EpgSearchResult r;
                r.request = request;
                if (request.offset == 0) {
                    r.rows = { row(QStringLiteral("a")), row(QStringLiteral("b")) };
                    r.hasMore = true;
                    r.nextOffset = 2;
                } else if (fail.exchange(false)) {
                    r.status = Core::EpgSearchStatus::Error;
                    r.errorText = QStringLiteral("temporary");
                } else
                    r.rows = { row(QStringLiteral("c")) };
                return r;
            });
        ctl.openSession();
        ctl.setQuery(QStringLiteral("programme"));
        QTRY_VERIFY(ctl.resultsCurrent());
        ctl.selectIndex(1);
        ctl.fetchNextPage();
        QTRY_VERIFY(!ctl.errorText().isEmpty());
        QVERIFY(ctl.resultsCurrent());
        QCOMPARE(ctl.selectedKey(), QStringLiteral("b"));
        QCOMPARE(ctl.model()->rowCount(), 2);
        ctl.retry();
        QTRY_COMPARE(ctl.model()->rowCount(), 3);
        QCOMPARE(ctl.selectedKey(), QStringLiteral("b"));
        QVERIFY(!ctl.hasMore());
        c.request.epgGeneration++;
        c.snapshot = std::make_shared<Core::EpgService::Snapshot>();
        ctl.refreshContext();
        QVERIFY(!ctl.resultsCurrent());
        QTRY_VERIFY(ctl.resultsCurrent());
        QCOMPARE(ctl.selectedKey(), QStringLiteral("b"));
        c.request.profileId = QStringLiteral("other");
        ctl.refreshContext();
        QCOMPARE(ctl.query(), QString());
        QCOMPARE(ctl.timeFilter(), QStringLiteral("all"));
        QVERIFY(!ctl.expanded());
        QCOMPARE(ctl.model()->rowCount(), 0);
        QVERIFY(!ctl.resultsCurrent());
    }
    void nativeDownloadTargetRevalidatesContextAndCapability()
    {
        auto c = context();
        bool enabled = true;
        App::EpgSearchController::Actions a;
        a.state = [&](const auto&, const auto&) {
            return QVariantMap { { QStringLiteral("downloadEnabled"), enabled } };
        };
        App::EpgSearchController ctl([&] { return c; },
            [](const auto&, const auto& request, const auto&) {
                Core::EpgSearchResult result;
                result.request = request;
                result.rows = { row(QStringLiteral("stable-result")) };
                return result;
            }, a);
        ctl.openSession();
        ctl.setQuery(QStringLiteral("first"));
        QTRY_VERIFY(ctl.resultsCurrent());
        const auto token = ctl.downloadActionToken();
        const auto key = ctl.selectedKey();
        QVERIFY(ctl.validateDownloadTarget(token, key));
        enabled = false;
        QVERIFY(!ctl.validateDownloadTarget(token, key));
        enabled = true;
        ++c.request.channelRevision;
        QVERIFY(!ctl.validateDownloadTarget(token, key));
        QTRY_VERIFY(ctl.resultsCurrent());
        QVERIFY(!ctl.validateDownloadTarget(token, key));
        auto currentToken = ctl.downloadActionToken();
        QVERIFY(ctl.validateDownloadTarget(currentToken, key));
        ctl.setQuery(QStringLiteral("replacement"));
        QVERIFY(!ctl.validateDownloadTarget(currentToken, key));
        QTRY_VERIFY(ctl.resultsCurrent());
        QCOMPARE(ctl.selectedKey(), key);
        QVERIFY(!ctl.validateDownloadTarget(currentToken, key));
        currentToken = ctl.downloadActionToken();
        ctl.closeSession();
        QVERIFY(!ctl.validateDownloadTarget(currentToken, key));
    }
    void restartOrDefaultUsesFreshAvailability_data()
    {
        QTest::addColumn<bool>("restart");
        QTest::newRow("restart-became-available") << true;
        QTest::newRow("resume-expired") << false;
    }
    void restartOrDefaultUsesFreshAvailability()
    {
        QFETCH(bool, restart);
        auto c = context();
        bool available = !restart;
        int calls = 0;
        bool fromBeginning = !restart;
        App::EpgSearchController::Actions a;
        a.state = [&](const auto&, const auto&) {
            return QVariantMap { { QStringLiteral("primaryEnabled"), true },
                { QStringLiteral("fromBeginningEnabled"), available },
                { QStringLiteral("actionKind"), QStringLiteral("catchup") } };
        };
        a.play = [&](const auto&, const auto&, bool beginning) { ++calls; fromBeginning = beginning; return true; };
        App::EpgSearchController ctl([&] { return c; },
            [](const auto&, const auto& request, const auto&) {
                Core::EpgSearchResult result;
                result.request = request;
                result.rows = { row(QStringLiteral("archive")) };
                return result;
            }, a);
        ctl.openSession();
        ctl.setQuery(QStringLiteral("archive"));
        QTRY_VERIFY(ctl.resultsCurrent());
        QCOMPARE(ctl.selectedDetails().value(QStringLiteral("fromBeginningEnabled")).toBool(), !restart);
        available = restart;
        ctl.activateSelectedFromBeginningOrDefault();
        QCOMPARE(calls, 1);
        QCOMPARE(fromBeginning, restart);
    }
    void obsoleteDetailsDoNotReplaceSelection()
    {
        auto c = context();
        c.snapshot = std::make_shared<Core::EpgService::Snapshot>();
        auto snapshot = std::make_shared<Core::EpgService::Snapshot>();
        QTemporaryDir directory;
        snapshot->store = Core::EpgStore::create(
            directory.filePath(QStringLiteral("epg.sqlite")), { }, [](const auto&) { });
        c.snapshot = snapshot;
        quint64 request = 0;
        App::EpgSearchController::Actions a;
        a.details = [&](const auto&, const auto&) { return ++request; };
        App::EpgSearchController ctl([&] { return c; },
            [](const auto&, const auto& request, const auto&) {
                Core::EpgSearchResult r;
                r.request = request;
                r.rows = { row(QStringLiteral("a")), row(QStringLiteral("b")) };
                return r;
            },
            a);
        ctl.openSession();
        ctl.setQuery(QStringLiteral("query"));
        QTRY_VERIFY(ctl.resultsCurrent());
        QCOMPARE(request, quint64(1));
        ctl.selectIndex(1);
        QCOMPARE(request, quint64(2));
        ctl.completeDetails(1, { { QStringLiteral("title"), QStringLiteral("obsolete") } }, { });
        QCOMPARE(ctl.selectedDetails().value(QStringLiteral("title")).toString(), QStringLiteral("b"));
        ctl.completeDetails(2, { { QStringLiteral("title"), QStringLiteral("hydrated") } }, { });
        QCOMPARE(ctl.selectedDetails().value(QStringLiteral("title")).toString(), QStringLiteral("hydrated"));
        QVERIFY(!ctl.detailsBusy());
    }
};
QTEST_GUILESS_MAIN(EpgSearchModelTests)
#include "tst_epg_search_model.moc"
