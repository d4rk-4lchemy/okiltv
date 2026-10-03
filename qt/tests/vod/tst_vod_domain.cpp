#include "core/vod/vodmodels.h"
#include "player/playbackrequest.h"
#include <QtTest>

using namespace OKILTV::Vod;
class VodDomainTests : public QObject {
    Q_OBJECT
private slots:
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
