#include "core/vod/vodmodels.h"
#include "player/playbackrequest.h"
#include <QtTest>

using namespace OKILTV::Vod;
class VodDomainTests : public QObject {
    Q_OBJECT
private slots:
    void episodeQueueAndContinuation() {
        ContentRef series{QUuid::createUuid(),QUuid::createUuid(),ContentKind::Series,QStringLiteral("s"),{}};
        VodDetails details;details.ref=series;
        details.seasons={{series,QStringLiteral("0"),0,0},{series,QStringLiteral("1"),1,1},{series,QStringLiteral("3"),3,2}};
        auto episode=[&](QString id,QString season,int number,int order) {
            return EpisodeSummary{{series.profileId,series.catalogNamespace,ContentKind::Episode,id,series.providerItemId},series,season,number,order,id,Availability::Available};
        };
        const auto special=episode(QStringLiteral("sp"),QStringLiteral("0"),1,0);
        const auto first=episode(QStringLiteral("1"),QStringLiteral("1"),1,1);
        const auto next=episode(QStringLiteral("4"),QStringLiteral("1"),4,2);
        const auto last=episode(QStringLiteral("9"),QStringLiteral("3"),2,3);
        details.episodes={last,special,next,first};
        QCOMPARE(orderedEpisodes(details).size(),3);
        QCOMPARE(adjacentEpisode(details,first.ref,1)->ref,next.ref);
        QCOMPARE(adjacentEpisode(details,next.ref,1)->ref,last.ref);
        QVERIFY(!adjacentEpisode(details,first.ref,-1)); QVERIFY(!adjacentEpisode(details,last.ref,1));
        QVERIFY(!adjacentEpisode(details,special.ref,1));
        SeriesProgress history;
        QCOMPARE(continueEpisode(details,history)->ref,first.ref);
        history.lastEpisode=first.ref;
        VodProgress saved;saved.positionMs=95000;saved.durationMs=100000;
        history.episodes.insert(first.ref.key(),saved);
        QCOMPARE(continueEpisode(details,history)->ref,first.ref);
        saved.positionMs=95001;history.episodes.insert(first.ref.key(),saved);
        QCOMPARE(continueEpisode(details,history)->ref,next.ref);
        saved.status=WatchStatus::Watched;history.episodes.insert(next.ref.key(),saved);
        QCOMPARE(continueEpisode(details,history)->ref,next.ref); // Do not skip watched successors.
        history.lastEpisode=last.ref;history.episodes.insert(last.ref.key(),saved);
        QVERIFY(!continueEpisode(details,history));
        history.lastEpisode=special.ref;
        saved.durationMs.reset();saved.status=WatchStatus::InProgress;history.episodes.insert(special.ref.key(),saved);
        QCOMPARE(continueEpisode(details,history)->ref,special.ref);
        saved.status=WatchStatus::Watched;history.episodes.insert(special.ref.key(),saved);
        QVERIFY(!continueEpisode(details,history));
    }
    void seriesWatchedExcludesSpecialsAndRequiresAllKnownOrdinaryEpisodes() {
        const ContentRef series{QUuid::createUuid(), QUuid::createUuid(), ContentKind::Series, QStringLiteral("s"), {}};
        VodDetails details; details.ref = series;
        details.seasons = {{series, QStringLiteral("0"), 0, 0}, {series, QStringLiteral("1"), 1, 1}};
        const auto make = [&](const QString &id, const QString &season) {
            return EpisodeSummary{{series.profileId, series.catalogNamespace, ContentKind::Episode, id, series.providerItemId}, series, season, 1, 0, id, Availability::Available};
        };
        const auto first = make(QStringLiteral("1"), QStringLiteral("1"));
        auto last = make(QStringLiteral("2"), QStringLiteral("1"));
        last.availability = Availability::Unavailable;
        const auto special = make(QStringLiteral("sp"), QStringLiteral("0"));
        details.episodes = {first, last, special};
        SeriesProgress history;
        QVERIFY(!seriesWatched(details, history));
        VodProgress watched; watched.status = WatchStatus::Watched;
        history.episodes.insert(first.ref.key(), watched);
        QVERIFY(!seriesWatched(details, history));
        history.episodes.insert(last.ref.key(), watched);
        QVERIFY(seriesWatched(details, history)); // An unwatched Special does not block series completion.
        watched.status = WatchStatus::InProgress; watched.positionMs = 95000; watched.durationMs = 100000;
        history.episodes.insert(last.ref.key(), watched); QVERIFY(!seriesWatched(details, history));
        watched.positionMs = 95001; history.episodes.insert(last.ref.key(), watched); QVERIFY(seriesWatched(details, history));
        history.continuationHidden = true; QVERIFY(!continueEpisode(details, history));
        details.episodes = {special}; QVERIFY(!seriesWatched(details, history));
        details.episodes.clear(); QVERIFY(!seriesWatched(details, history));
    }
    void identitySeparatesKindsSourcesAndEpisodeParents()
    {
        ContentRef movie{QUuid::createUuid(), QUuid::createUuid(), ContentKind::Movie, QStringLiteral("007"), {}};
        auto series = movie; series.kind = ContentKind::Series;
        auto otherSource = movie; otherSource.profileId = QUuid::createUuid();
        auto episode = movie; episode.kind = ContentKind::Episode; episode.parentNamespace = QStringLiteral("series/a");
        auto otherEpisode = episode; otherEpisode.parentNamespace = QStringLiteral("series/b");
        QVERIFY(movie.playable()); QVERIFY(!series.playable()); QVERIFY(episode.playable());
        QVERIFY(movie.key() != series.key()); QVERIFY(movie.key() != otherSource.key());
        QVERIFY(episode.key() != otherEpisode.key());
        auto paddedId = movie; paddedId.providerItemId = QStringLiteral("7");
        QVERIFY(movie.key() != paddedId.key());
        auto delimited = episode; delimited.providerItemId = QStringLiteral("a/b"); delimited.parentNamespace = QStringLiteral("c");
        auto collision = episode; collision.providerItemId = QStringLiteral("a"); collision.parentNamespace = QStringLiteral("b/c");
        QVERIFY(delimited.key() != collision.key());
    }
    void absentValuesStayAbsent()
    {
        QVERIFY(!MovieSummary{}.year); QVERIFY(!Season{}.number); QVERIFY(!EpisodeSummary{}.number);
        QVERIFY(!VodDetails{}.declaredDurationMs); QVERIFY(!VodProgress{}.durationMs);
        QVERIFY(!ContentRef{}.valid());
        QCOMPARE(ProviderCapabilities{}.movies, Capability::Unknown);
    }
    void cancellationAndDeadlineAreDistinct()
    {
        RequestContext request;
        QVERIFY(!request.interruption());
        request.cancelled->store(true);
        QCOMPARE(request.interruption()->code, ErrorCode::Cancelled);
        request.cancelled->store(false);
        request.deadline = QDeadlineTimer(0);
        QCOMPARE(request.interruption()->code, ErrorCode::Timeout);
    }
    void transportDoesNotInferFromExtension()
    {
        OKILTV::Player::PlaybackRequest request;
        request.mediaUri = QUrl(QStringLiteral("https://fixture.invalid/movie.ts"));
        QCOMPARE(request.transport, OKILTV::Player::TransportPolicy::NativeMedia);
        QCOMPARE(request.policy, OKILTV::Player::PlaybackPolicy::OnDemand);
    }
};
QTEST_GUILESS_MAIN(VodDomainTests)
#include "tst_vod_domain.moc"
