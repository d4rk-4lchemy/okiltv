#include "core/catchupurlresolver.h"
#include "player/catchupstreamsession.h"
#include "player/catchuptsjoiner.h"

#include <QFile>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimeZone>
#include <QtConcurrent>
#include <thread>

using namespace OKILTV;

namespace {
QByteArray transport(const int count, const qint64 clockStart = 0)
{
    QByteArray result;
    for (int frame = 0; frame < count; ++frame) {
        QByteArray packet(188, static_cast<char>(frame % 251));
        packet[0] = 0x47;
        packet[1] = 0x41;
        packet[2] = 0;
        packet[3] = static_cast<char>(0x10 | (frame % 16));
        packet[4] = 0; packet[5] = 0; packet[6] = 1; packet[7] = static_cast<char>(0xe0);
        packet[8] = 0; packet[9] = 0; packet[10] = static_cast<char>(0x80);
        packet[11] = static_cast<char>(0x80); packet[12] = 5;
        const auto pts = static_cast<quint64>(clockStart + static_cast<qint64>(frame) * 1800) & ((1ULL << 33) - 1);
        packet[13] = static_cast<char>(0x21U | (((pts >> 30) & 7U) << 1));
        packet[14] = static_cast<char>(pts >> 22);
        packet[15] = static_cast<char>(1U | (((pts >> 15) & 127U) << 1));
        packet[16] = static_cast<char>(pts >> 7);
        packet[17] = static_cast<char>(1U | ((pts & 127U) << 1));
        result.append(packet);
    }
    return result;
}

QByteArray psiPacket(QByteArray section, const int pid)
{
    quint32 crc = 0xffffffffU;
    for (const auto byte : section) {
        crc ^= static_cast<quint32>(static_cast<unsigned char>(byte)) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = crc & 0x80000000U ? (crc << 1) ^ 0x04c11db7U : crc << 1;
        }
    }
    for (int shift = 24; shift >= 0; shift -= 8) { section += static_cast<char>(crc >> shift); }
    QByteArray packet(188, char(0xff));
    packet[0] = 0x47; packet[1] = static_cast<char>(0x40 | (pid >> 8));
    packet[2] = static_cast<char>(pid); packet[3] = 0x10; packet[4] = 0;
    packet.replace(5, section.size(), section);
    return packet;
}

QByteArray periodTransport(const bool replacement)
{
    auto pat = QByteArray::fromHex(replacement ? "00b00d0001c100003dcd efff" : "00b00d0001c100000001f000");
    auto pmt = QByteArray::fromHex(replacement ? "02b0173dcdc10000e065f0001be065f00003e066f000"
                                             : "02b0170001c10000e100f0001be100f0000fe101f000");
    auto media = transport(200, replacement ? 4307067442LL : 285108030LL);
    if (replacement) {
        for (qsizetype i = 0; i < media.size(); i += 188) {
            media[i + 1] = 0x40; media[i + 2] = 101;
        }
    }
    return psiPacket(pat, 0) + psiPacket(pmt, replacement ? 4095 : 4096) + media;
}

QByteArray clockFrame(const quint64 clock)
{
    constexpr quint64 mask = (1ULL << 33) - 1;
    QByteArray packet(188, char(0x55));
    packet[0] = 0x47; packet[1] = 0x41; packet[2] = 0; packet[3] = 0x30;
    packet[4] = 7; packet[5] = 0x10;
    const auto pcr = (clock - 32040) & mask;
    packet[6] = static_cast<char>(pcr >> 25); packet[7] = static_cast<char>(pcr >> 17);
    packet[8] = static_cast<char>(pcr >> 9); packet[9] = static_cast<char>(pcr >> 1);
    packet[10] = static_cast<char>(((pcr & 1) << 7) | 0x7e); packet[11] = 17;
    packet.replace(12, 9, QByteArray::fromHex("000001e0000080c00a"));
    for (const int at : {21, 26}) {
        packet[at] = static_cast<char>((at == 21 ? 0x31U : 0x11U) | (((clock >> 30) & 7) << 1));
        packet[at + 1] = static_cast<char>(clock >> 22);
        packet[at + 2] = static_cast<char>(1U | (((clock >> 15) & 127) << 1));
        packet[at + 3] = static_cast<char>(clock >> 7);
        packet[at + 4] = static_cast<char>(1U | ((clock & 127) << 1));
    }
    return packet;
}

class ArchiveServer : public QTcpServer
{
public:
    QList<QByteArray> replies;
    QList<QByteArray> requests;
    ArchiveServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("sent").toBool()) {
                        return;
                    }
                    socket->setProperty("sent", true);
                    const auto body = replies.value(requests.size());
                    requests.append(request.split('\n').first());
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: "
                                  + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QString url() const
    {
        return QStringLiteral("http://127.0.0.1:%1/timeshift/test/test/1/2026-01-01:12-00/1.ts").arg(serverPort());
    }
};

struct ReadResult { QByteArray bytes; qint64 terminal { 0 }; };
ReadResult readAll(const Player::CatchupStreamSession::Ptr &session)
{
    ReadResult result;
    char buffer[4096];
    for (;;) {
        const auto count = session->read(buffer, sizeof(buffer));
        if (count <= 0) {
            result.terminal = count;
            return result;
        }
        result.bytes.append(buffer, static_cast<qsizetype>(count));
    }
}
} // namespace

class CatchupStreamTests : public QObject
{
    Q_OBJECT
private slots:
    void finalCallbackReferenceIsDestroyedOnQtThread_data();
    void finalCallbackReferenceIsDestroyedOnQtThread();
    void joinAcrossArbitraryNetworkBoundaries();
    void synchronizeInitialPartialPacket();
    void rejectUnsynchronizedAndCorruptedPackets();
    void rejectChangedMuxAndClock();
    void preserveTimestampWrap();
    void splitConfigurationPeriods();
    void audioOnlyClockAndPeriods();
    void repairIsolatedVideoClock();
    void retryForwardArchiveGap();
    void retriedGapRetainsHttpAndAdvancesTimeline_data();
    void retriedGapRetainsHttpAndAdvancesTimeline();
    void configurationPeriodRetainsHttpAndIsolatesReaders();
    void continueAfterEofUnderBackpressure_data();
    void continueAfterEofUnderBackpressure();
    void cancelWhileWaitingAtSafeEdge();
    void repeatedMinuteSnapshot_data();
    void repeatedMinuteSnapshot();
    void minuteContinuationRetriesPartialResponses();
    void minuteContinuationWaitsAfterIdenticalSnapshot();
    void continuationStartsWhenOverlapAnchorIsSafe();
    void failUnmatchedContinuation();
    void safeWindowUsesProviderTimestampAndMargin();
    void completedArchiveDoesNotContinue();
    void endlessContinuesBeyondProgrammeEnd_data();
    void endlessContinuesBeyondProgrammeEnd();
    void startupBufferUsesConfiguredTarget_data();
    void startupBufferUsesConfiguredTarget();
};

void CatchupStreamTests::finalCallbackReferenceIsDestroyedOnQtThread_data()
{
    QTest::addColumn<bool>("stopBeforeRelease");
    QTest::newRow("stopped-provider") << true;
    QTest::newRow("active-provider") << false;
}

void CatchupStreamTests::finalCallbackReferenceIsDestroyedOnQtThread()
{
    QFETCH(bool, stopBeforeRelease);
    QTest::failOnWarning("QObject::killTimer: Timers cannot be stopped from another thread");
    QTest::failOnWarning("QObject::~QObject: Timers cannot be stopped from another thread");
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    Player::CatchupStreamSession::BufferingPolicy policy;
    policy.transferTimeoutMs = 60000;
    auto session = Player::CatchupStreamSession::create(
        QStringLiteral("http://127.0.0.1:%1/test.ts").arg(server.serverPort()), {}, policy);
    QVERIFY(session->start());
    QTRY_VERIFY(server.hasPendingConnections());
    auto *socket = server.nextPendingConnection();
    QTRY_VERIFY(socket->bytesAvailable() > 0);
    socket->readAll();
    // Leave the response open so teardown includes a live reply and idle timer.
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: 1000000\r\n\r\n"
                  + transport(32));
    QTRY_VERIFY(session->bufferedBytes() > 0);
    QPointer<QObject> guard(session.get());
    auto *ownerThread = QThread::currentThread();
    QThread *destructionThread = nullptr;
    QObject::connect(session.get(), &QObject::destroyed, session.get(), [&destructionThread]() {
        destructionThread = QThread::currentThread();
    }, Qt::DirectConnection);
    if (stopBeforeRelease) {
        session->closeProviderConnection(QStringLiteral("test-stop"));
    }
    std::thread callback([session = std::move(session)]() mutable {
        session->cancelRead();
        session.reset();
    });
    callback.join();
    // Releasing the callback must defer destruction until the owner processes it.
    QVERIFY(guard);
    QVERIFY(destructionThread == nullptr);
    QTRY_VERIFY(guard.isNull());
    QCOMPARE(destructionThread, ownerThread);
}

void CatchupStreamTests::repairIsolatedVideoClock()
{
    constexpr quint64 mask = (1ULL << 33) - 1;
    const auto tables = periodTransport(false).left(376);
    QByteArray healthy = tables, damaged = tables;
    for (int i = 0; i < 100; ++i) {
        const auto clock = quint64(4453979299LL + i * 1800);
        healthy += clockFrame(clock);
        damaged += clockFrame(i == 60 ? (clock - 3600000000ULL) & mask : clock);
    }
    for (const qsizetype chunk : {qsizetype(1), qsizetype(197), damaged.size()}) {
        Player::CatchupTsJoiner joiner;
        QByteArray output;
        for (qsizetype at = 0; at < damaged.size(); at += chunk) { output += joiner.push(damaged.mid(at, chunk)); }
        output += joiner.finish();
        QVERIFY2(joiner.valid(), qPrintable(joiner.errorString()));
        QCOMPARE(joiner.repairedClockCount(), quint64(1));
        QCOMPARE(output, healthy); // Encoded frame bytes, PCR extension and all unrelated bytes unchanged.
    }
    const qsizetype nextFrame = 376 + 61 * 188;
    Player::CatchupTsJoiner repeatedTables;
    const auto withTables = damaged.left(nextFrame) + tables + damaged.mid(nextFrame);
    QCOMPARE(repeatedTables.push(withTables), healthy.left(nextFrame) + tables + healthy.mid(nextFrame));
    QVERIFY(repeatedTables.valid());
    QCOMPARE(repeatedTables.repairedClockCount(), quint64(1));

    Player::CatchupTsJoiner overlap;
    const auto firstEnd = 376 + 80 * 188;
    auto joined = overlap.push(damaged.left(firstEnd));
    QVERIFY(overlap.beginContinuation());
    joined += overlap.push(damaged.mid(376 + 40 * 188));
    QVERIFY(overlap.valid());
    QVERIFY(!overlap.matching());
    QCOMPARE(joined, healthy); // Signature must match raw, not repaired timestamps.

    auto sustained = damaged;
    sustained.replace(376 + 61 * 188, 188, clockFrame((4453979299ULL + 61 * 1800 - 3600000000ULL) & mask));
    Player::CatchupTsJoiner reject;
    reject.push(sustained);
    QVERIFY(!reject.valid());
    QCOMPARE(reject.repairedClockCount(), quint64(0));

    Player::CatchupTsJoiner truncated;
    truncated.push(damaged.left(376 + 61 * 188));
    QVERIFY(truncated.valid()); // Quarantined until there is evidence or final EOF.
    truncated.finish();
    QVERIFY(!truncated.valid());
}

void CatchupStreamTests::retryForwardArchiveGap()
{
    const auto tables = periodTransport(false).left(376);
    QByteArray before = tables, after;
    for (quint64 i = 0; i < 100; ++i) {
        before += clockFrame(900000 + i * 1800);
        after += clockFrame(900000 + (100 + i) * 1800 + 61128000);
    }
    const auto source = before + after;
    Player::CatchupTsJoiner first;
    first.push(source);
    QVERIFY(!first.valid());
    QVERIFY(first.failedForwardGap());
    QVERIFY(!first.periodPending());

    for (const qsizetype chunk : {qsizetype(1), qsizetype(197), source.size()}) {
        Player::CatchupTsJoiner retry;
        retry.allowRetriedForwardGap(first.failedForwardGap());
        QByteArray output, pending;
        for (qsizetype at = 0; at < source.size(); at += chunk) {
            output += retry.push(source.mid(at, chunk));
            QVERIFY2(retry.valid(), qPrintable(retry.errorString()));
            if (retry.periodPending()) {
                pending = retry.takeNextPeriod() + source.mid(std::min(source.size(), at + chunk));
                break;
            }
        }
        QCOMPARE(output, before);
        QCOMPARE(pending, tables + after);
        QCOMPARE(retry.periodDurationSeconds(), 681.2); // 2s media + 679.2s missing.
        Player::CatchupTsJoiner next;
        QCOMPARE(next.push(pending) + next.finish(), pending);
        QVERIFY(next.valid());
        QCOMPARE(next.durationSeconds(), 1.98);
    }
    auto different = *first.failedForwardGap();
    ++different.nextDts;
    Player::CatchupTsJoiner wrongGap;
    wrongGap.allowRetriedForwardGap(different);
    wrongGap.push(source);
    QVERIFY(!wrongGap.valid());
    QVERIFY(!wrongGap.periodPending());

    Player::CatchupTsJoiner backwards;
    backwards.allowRetriedForwardGap(first.failedForwardGap());
    backwards.push(tables + after + before.mid(tables.size()));
    QVERIFY(!backwards.valid());
    QVERIFY(!backwards.failedForwardGap());
}

void CatchupStreamTests::retriedGapRetainsHttpAndAdvancesTimeline_data()
{
    QTest::addColumn<bool>("secondGap");
    QTest::newRow("one-gap") << false;
    QTest::newRow("successive-gaps") << true;
}

void CatchupStreamTests::retriedGapRetainsHttpAndAdvancesTimeline()
{
    QFETCH(bool, secondGap);
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto tables = periodTransport(false).left(376);
    QByteArray before = tables, after;
    for (quint64 i = 0; i < 100; ++i) {
        before += clockFrame(900000 + i * 1800);
        after += clockFrame(900000 + (100 + i) * 1800 + 61128000); // 679.2s archive gap.
    }
    QByteArray following;
    if (secondGap) {
        for (quint64 i = 0; i < 100; ++i) {
            auto packet = clockFrame(900000 + (200 + i) * 1800 + 61128000 + 36900000);
            if (i == 1) { packet[5] = 0; } // Next PES has DTS but no PCR.
            following += packet;
        }
    }
    server.replies = {before + after + following, before + after + following, tables + after + following};
    auto first = Player::CatchupStreamSession::create(server.url(), {}, {4096, 2048, 4096, QStringLiteral("test"), false, 1000});
    first->configureMediaPeriods(3180);
    QVERIFY(first->start());
    auto future = QtConcurrent::run([first]() { return readAll(first); });
    auto cleanup = qScopeGuard([&]() { first->cancelRead(); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QVERIFY(first->hasNetworkError());
    QVERIFY(first->failedForwardGap());

    auto retry = Player::CatchupStreamSession::create(server.url(), {}, {4096, 2048, 4096, QStringLiteral("test"), false, 1000});
    retry->configureMediaPeriods(3180);
    retry->allowRetriedForwardGap(first->failedForwardGap());
    QVERIFY(retry->start());
    future = QtConcurrent::run([retry]() { return readAll(retry); });
    const auto retryCleanup = qScopeGuard([&]() { retry->cancelRead(); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QCOMPARE(future.result().bytes, before);
    QCOMPARE(future.result().terminal, 0);
    QVERIFY(!retry->hasNetworkError());
    QCOMPARE(retry->nextPeriodBaseSeconds(), std::optional<double>(3861.2));
    const auto oldGeneration = retry->readGeneration();
    retry->cancelRead(oldGeneration);
    QVERIFY(retry->advancePeriod());
    retry->cancelRead(oldGeneration);
    future = QtConcurrent::run([retry]() { return readAll(retry); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    if (!secondGap) {
        QCOMPARE(future.result().bytes, tables + after);
        QCOMPARE(future.result().terminal, 0);
        QVERIFY(!retry->hasNetworkError());
        QVERIFY(!retry->nextPeriodBaseSeconds());
        QCOMPARE(server.requests.size(), 2);
        return;
    }
    // A different gap in the next period must first fail, then be recoverable
    // using its own validated signature rather than the previously skipped gap.
    QVERIFY(retry->failedMediaTransport());
    const auto secondSignature = retry->failedForwardGap();
    QVERIFY(secondSignature);
    QVERIFY(secondSignature != first->failedForwardGap());
    auto secondRetry = Player::CatchupStreamSession::create(server.url(), {}, {4096, 2048, 4096, QStringLiteral("test"), false, 1000});
    secondRetry->configureMediaPeriods(3861.2);
    secondRetry->allowRetriedForwardGap(secondSignature);
    QVERIFY(secondRetry->start());
    future = QtConcurrent::run([secondRetry]() { return readAll(secondRetry); });
    const auto secondCleanup = qScopeGuard([&]() { secondRetry->cancelRead(); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QVERIFY(!secondRetry->hasNetworkError());
    QCOMPARE(secondRetry->nextPeriodBaseSeconds(), std::optional<double>(4273.2));
    QVERIFY(secondRetry->advancePeriod());
    future = QtConcurrent::run([secondRetry]() { return readAll(secondRetry); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QCOMPARE(future.result().bytes, tables + following);
    QCOMPARE(future.result().terminal, 0);
    QVERIFY(!secondRetry->failedMediaTransport());
    QCOMPARE(server.requests.size(), 3);
}

void CatchupStreamTests::audioOnlyClockAndPeriods()
{
    const auto tables = psiPacket(QByteArray::fromHex("00b00d0001c100000001f000"), 0)
        + psiPacket(QByteArray::fromHex("02b0120001c10000e100f0000fe100f000"), 4096);
    auto audio = transport(200, (1LL << 33) - 180000);
    for (qsizetype i = 0; i < audio.size(); i += 188) { audio[i + 7] = char(0xc0); }
    const auto first = tables + audio;
    const auto second = periodTransport(true);
    for (const qsizetype chunk : {1, 187, 4096}) {
        Player::CatchupTsJoiner joiner;
        QByteArray output;
        for (qsizetype i = 0; i < first.size(); i += chunk) {
            output += joiner.push(first.mid(i, chunk));
        }
        QCOMPARE(output, first);
        QVERIFY(joiner.valid());
        QCOMPARE(joiner.periodDurationSeconds(), 4.0);
        QVERIFY(joiner.beginContinuation());
        // Byte-identical overlap remains valid when the clock source is audio.
        QCOMPARE(joiner.push(first), QByteArray());
        QVERIFY(!joiner.matching());
        QCOMPARE(joiner.push(second), QByteArray());
        QVERIFY(joiner.valid());
        QVERIFY(joiner.periodPending());
        QCOMPARE(joiner.takeNextPeriod(), second);
        Player::CatchupTsJoiner backToRadio;
        QCOMPARE(backToRadio.push(second + first), second);
        QVERIFY(backToRadio.periodPending());
        QCOMPARE(backToRadio.takeNextPeriod(), first);
    }
    Player::CatchupTsJoiner discontinuity;
    QCOMPARE(discontinuity.push(first), first);
    auto jumped = transport(10, 100 * 90000LL);
    for (qsizetype i = 0; i < jumped.size(); i += 188) { jumped[i + 7] = char(0xc0); }
    discontinuity.push(jumped);
    QVERIFY(!discontinuity.valid());
}

void CatchupStreamTests::splitConfigurationPeriods()
{
    const auto first = periodTransport(false);
    const auto second = periodTransport(true);
    for (const qsizetype chunk : {qsizetype(1), qsizetype(113), qsizetype(4096), first.size() + second.size()}) {
        Player::CatchupTsJoiner joiner;
        QByteArray output, pending;
        const auto input = first + second;
        qsizetype offset = 0;
        for (; offset < input.size(); offset += chunk) {
            output += joiner.push(input.mid(offset, chunk));
            QVERIFY2(joiner.valid(), qPrintable(joiner.errorString()));
            if (joiner.periodPending()) {
                pending = joiner.takeNextPeriod() + input.mid(std::min(input.size(), offset + chunk));
                break;
            }
        }
        QCOMPARE(output, first);
        QCOMPARE(pending, second);
        QCOMPARE(joiner.periodDurationSeconds(), 4.0);
        QVERIFY(joiner.periodDescription().contains(QStringLiteral("program=1->15821")));
        Player::CatchupTsJoiner next;
        QCOMPARE(next.push(pending), second);
        QVERIFY(next.valid());
    }
    // A PSI section can itself span TS packets, independently of HTTP chunks.
    auto section = second.mid(5, 16);
    QByteArray patStart(188, char(0));
    patStart[0] = 0x47; patStart[1] = 0x40; patStart[3] = 0x30;
    patStart[4] = char(172); // Only pointer + ten section bytes fit after adaptation.
    patStart.replace(178, 10, section.left(10));
    QByteArray patEnd(188, char(0xff));
    patEnd[0] = 0x47; patEnd[1] = 0; patEnd[2] = 0; patEnd[3] = 0x11;
    patEnd.replace(4, 6, section.mid(10));
    const auto splitSecond = patStart + patEnd + second.mid(188);
    Player::CatchupTsJoiner splitPsi;
    QCOMPARE(splitPsi.push(first + splitSecond), first);
    QVERIFY(splitPsi.valid());
    QVERIFY(splitPsi.periodPending());
    QCOMPARE(splitPsi.takeNextPeriod(), splitSecond);

    // Corrupt PSI must not authorize an arbitrary clock change.
    auto corrupt = second;
    corrupt[12] = static_cast<char>(corrupt[12] ^ 1);
    Player::CatchupTsJoiner invalid;
    invalid.push(first + corrupt);
    QVERIFY(!invalid.periodPending());
    QVERIFY(!invalid.valid());
    // Repeated identical PAT/PMT does not split a programme.
    Player::CatchupTsJoiner repeated;
    const auto tables = first.left(2 * 188);
    const auto repeatedInput = first + tables + transport(100, 285468030LL);
    QCOMPARE(repeated.push(repeatedInput), repeatedInput);
    QVERIFY(repeated.valid());
    QVERIFY(!repeated.periodPending());
}

void CatchupStreamTests::configurationPeriodRetainsHttpAndIsolatesReaders()
{
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto first = periodTransport(false);
    const auto second = periodTransport(true);
    server.replies = {first + second + first}; // Two consecutive configuration changes.
    auto session = Player::CatchupStreamSession::create(server.url(), {}, {4096, 2048, 4096, QStringLiteral("test"), false, 100});
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-600);
    session->configureContinuous({server.url(), start, start.addSecs(150), 240, 180, 30.0, false});
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session]() { return readAll(session); });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QCOMPARE(future.result().bytes, first);
    QCOMPARE(future.result().terminal, 0);
    QCOMPARE(session->nextPeriodBaseSeconds(), std::optional<double>(244.0));
    QTest::qWait(200); // Intentional period backpressure must not trip idle timeout.
    QVERIFY(!session->hasNetworkError());
    auto oldGeneration = session->readGeneration();
    session->cancelRead(oldGeneration); // mpv closes the old item before loading the next.
    QVERIFY(session->advancePeriod());
    session->cancelRead(oldGeneration); // A late cancel must not cancel the new reader.
    char buffer[188];
    QCOMPARE(session->read(oldGeneration, buffer, sizeof(buffer)), 0);
    future = QtConcurrent::run([session]() { return readAll(session); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QCOMPARE(future.result().bytes, second);
    QCOMPARE(session->nextPeriodBaseSeconds(), std::optional<double>(248.0));
    QVERIFY(session->advancePeriod());
    future = QtConcurrent::run([session]() { return readAll(session); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QCOMPARE(future.result().bytes, first);
    QCOMPARE(future.result().terminal, 0);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(!session->hasNetworkError());
    QVERIFY(!session->nextPeriodBaseSeconds());
}

void CatchupStreamTests::endlessContinuesBeyondProgrammeEnd_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("xtream") << QString {};
    QTest::newRow("m3u-default") << QStringLiteral("default");
    QTest::newRow("m3u-append") << QStringLiteral("append");
}

void CatchupStreamTests::endlessContinuesBeyondProgrammeEnd()
{
    QFETCH(QString, mode);
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto source = transport(5000);
    server.replies = {source.left(188 * 2500), source.mid(188 * 2000)};
    auto session = Player::CatchupStreamSession::create(server.url());
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-600);
    Player::CatchupStreamSession::ContinuousPolicy policy {server.url(), start, start.addSecs(20), 0, 180, 3.0, true, true};
    if (!mode.isEmpty()) {
        Core::Channel channel;
        channel.source = Core::ChannelSource::M3U;
        channel.catchupSupported = true;
        channel.catchupWindowHours = 24;
        channel.catchupMode = mode;
        channel.streamUrl = server.url();
        channel.catchupSourceTemplate = (mode == QStringLiteral("default") ? server.url() + u'?' : QString {})
            + QStringLiteral("utc={utc}&lutc={lutc}&duration={duration}");
        policy.templateChannel = channel;
    }
    session->configureContinuous(policy);
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session, expected = source.size()]() {
        QByteArray output;
        char buffer[4096];
        while (output.size() < expected) {
            const auto count = session->read(buffer, sizeof(buffer));
            if (count <= 0) { break; }
            output.append(buffer, static_cast<qsizetype>(count));
        }
        return output;
    });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QCOMPARE(future.result(), source);
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(session->continuous());
    if (!mode.isEmpty()) {
        QVERIFY(server.requests.last().contains("utc="));
        QVERIFY(server.requests.last().contains("duration="));
        QVERIFY(!server.requests.last().contains("{utc}"));
    }
}

void CatchupStreamTests::completedArchiveDoesNotContinue()
{
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.replies = {transport(2000)};
    auto session = Player::CatchupStreamSession::create(server.url());
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-600);
    session->configureContinuous({server.url(), start, start.addSecs(150), 0, 180, 3.0, false});
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session]() { return readAll(session); });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QCOMPARE(future.result().bytes, server.replies.first());
    QCOMPARE(future.result().terminal, 0);
    QTest::qWait(1000);
    QCOMPARE(server.requests.size(), 1);
}

void CatchupStreamTests::startupBufferUsesConfiguredTarget_data()
{
    QTest::addColumn<double>("target");
    QTest::newRow("archived-player-setting") << 3.0;
    QTest::newRow("live-or-unpublished-tail") << 30.0;
}

void CatchupStreamTests::startupBufferUsesConfiguredTarget()
{
    QFETCH(double, target);
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto body = transport(2000);
    QPointer<QTcpSocket> socket;
    connect(&server, &QTcpServer::newConnection, &server, [&]() {
        socket = server.nextPendingConnection();
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\n\r\n" + body.left(188 * 500));
    });
    auto session = Player::CatchupStreamSession::create(QStringLiteral("http://127.0.0.1:%1/archive.ts").arg(server.serverPort()));
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-600);
    session->configureContinuous({session->sourceUrl(), start, start.addSecs(40), 0, 180, target, false});
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session]() { char byte; return session->read(&byte, 1); });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(session->bufferedBytes() > 0, 3000);
    qsizetype sentPackets = 500;
    if (target < 10.0) {
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
    } else {
        QTest::qWait(200);
        QVERIFY(!future.isFinished());
        QVERIFY(socket);
        socket->write(body.mid(188 * 500, 188 * 750)); // 25 seconds is below the 30-second target.
        sentPackets = 1250;
        QTRY_VERIFY_WITH_TIMEOUT(session->bufferedBytes() >= 188 * sentPackets, 1000);
        QVERIFY(!future.isFinished());
    }
    QVERIFY(socket);
    socket->write(body.mid(188 * sentPackets));
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QCOMPARE(future.result(), 1);
}

void CatchupStreamTests::joinAcrossArbitraryNetworkBoundaries()
{
    const auto source = transport(3000);
    Player::CatchupTsJoiner joiner;
    QByteArray output;
    const auto first = source.left(188 * 1800 + 71); // HTTP may end inside a TS packet.
    for (qsizetype i = 0; i < first.size(); i += 113) {
        output += joiner.push(first.mid(i, 113));
    }
    QVERIFY(joiner.beginContinuation());
    const auto second = source.mid(188 * 1500);
    for (qsizetype i = 0; i < second.size(); i += 97) {
        output += joiner.push(second.mid(i, 97));
    }
    QVERIFY(!joiner.matching());
    QVERIFY(joiner.valid());
    QCOMPARE(output, source);
    QCOMPARE(joiner.durationSeconds(), 59.98);
}

void CatchupStreamTests::synchronizeInitialPartialPacket()
{
    const auto source = transport(3000);
    for (int prefix = 0; prefix < 188; ++prefix) {
        Player::CatchupTsJoiner joiner;
        // A stray sync byte in the incomplete packet must not establish lock.
        const auto first = QByteArray(prefix, char(0x47)) + source.left(188 * 1800 + 71);
        QByteArray output;
        for (qsizetype i = 0; i < first.size(); i += 113) {
            output += joiner.push(first.mid(i, 113));
        }
        QVERIFY(joiner.valid());
        QVERIFY(joiner.beginContinuation());
        const auto second = source.mid(188 * 1500 + 44);
        for (qsizetype i = 0; i < second.size(); i += 97) {
            output += joiner.push(second.mid(i, 97));
        }
        QVERIFY(joiner.valid());
        QVERIFY(!joiner.matching());
        QCOMPARE(output, source);
        QCOMPARE(joiner.durationSeconds(), 59.98);
    }
    Player::CatchupTsJoiner bytewise;
    QByteArray output;
    const auto input = QByteArray(44, char(0x47)) + source;
    for (const auto byte : input) {
        output += bytewise.push(QByteArray(1, byte));
    }
    QCOMPARE(output, source);
}

void CatchupStreamTests::rejectUnsynchronizedAndCorruptedPackets()
{
    Player::CatchupTsJoiner invalid;
    QCOMPARE(invalid.push(QByteArray(188 * 6, 'x')), QByteArray {});
    QVERIFY(!invalid.valid());
    QCOMPARE(invalid.push(transport(100)), QByteArray {});

    Player::CatchupTsJoiner corrupted;
    corrupted.push(transport(100));
    QVERIFY(corrupted.valid());
    corrupted.push(QByteArray(1, 'x') + transport(100, 180000));
    QVERIFY(!corrupted.valid()); // Only the initial partial packet can be discarded.
}

void CatchupStreamTests::rejectChangedMuxAndClock()
{
    Player::CatchupTsJoiner joiner;
    joiner.push(transport(2000));
    QVERIFY(joiner.beginContinuation());
    QCOMPARE(joiner.push(transport(1000, 9000000)), QByteArray {});
    QVERIFY(joiner.matching());
    Player::CatchupTsJoiner changedClock;
    changedClock.push(transport(2000));
    changedClock.push(transport(1000));
    QVERIFY(!changedClock.valid());
    const auto diagnostic = changedClock.errorString();
    QVERIFY(diagnostic.contains(QStringLiteral("pid=256 previousPid=256")));
    QVERIFY(diagnostic.contains(QStringLiteral("rawPts=0 previousRawPts=3598200")));
    QVERIFY(diagnostic.contains(QStringLiteral("packet=2001 cc=0 discontinuity=0")));
    QVERIFY(diagnostic.contains(QStringLiteral("verifiedDuration=39.980s")));

    Player::CatchupTsJoiner flaggedClock;
    flaggedClock.push(transport(2000));
    auto packet = transport(1, 9000000);
    packet.replace(4, 0, QByteArray::fromHex("02c000"));
    packet.resize(188);
    packet[1] = static_cast<char>(0xc1); // Transport error, payload start, PID 257.
    packet[2] = 1;
    packet[3] = 0x35; // Adaptation + payload, continuity counter 5.
    flaggedClock.push(packet);
    QVERIFY(!flaggedClock.valid());
    QVERIFY(flaggedClock.errorString().contains(QStringLiteral("pid=257 previousPid=256")));
    QVERIFY(flaggedClock.errorString().contains(QStringLiteral("cc=5 discontinuity=1 randomAccess=1 transportError=1 adaptationLength=2")));
}

void CatchupStreamTests::preserveTimestampWrap()
{
    Player::CatchupTsJoiner joiner;
    joiner.push(transport(3000, (1LL << 33) - 30 * 90000LL));
    QVERIFY(joiner.valid());
    QCOMPARE(joiner.durationSeconds(), 59.98);
}

void CatchupStreamTests::continueAfterEofUnderBackpressure_data()
{
    QTest::addColumn<int>("prefix");
    QTest::addColumn<bool>("normalize");
    QTest::newRow("aligned-dispatcharr") << 0 << false;
    QTest::newRow("partial-gamehub") << 44 << false;
    QTest::newRow("partial-max") << 187 << false;
    QTest::newRow("aligned-normalized") << 0 << true;
    QTest::newRow("partial-normalized") << 44 << true;
    QTest::newRow("partial-max-normalized") << 187 << true;
}

void CatchupStreamTests::continueAfterEofUnderBackpressure()
{
    QFETCH(int, prefix);
    QFETCH(bool, normalize);
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto source = transport(7500);
    server.replies = {QByteArray(prefix, char(0x47)) + source.left(188 * 2500), source.mid(188 * 2000 + prefix, 188 * 3000 - prefix), source.mid(188 * 4500 + prefix)};
    auto session = Player::CatchupStreamSession::create(server.url(), {}, {
        .queueHighWaterBytes = 8192, .queueLowWaterBytes = 4096, .replyReadBufferBytes = 16384,
        .roleLabel = QStringLiteral("test"), .normalizeMpegTsTimestamps = normalize,
    });
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-600);
    session->configureContinuous({server.url(), start, start.addSecs(150)});
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session]() { return readAll(session); });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 10000);
    QCOMPARE(future.result().terminal, 0);
    Player::MpegTsTimestampNormalizer normalizer;
    auto expected = source;
    if (normalize) {
        expected = normalizer.push(source);
        expected += normalizer.finish();
    }
    QCOMPARE(future.result().bytes, expected);
    QCOMPARE(server.requests.size(), 3);
    QVERIFY(server.requests[1].contains("12-00-41/1.ts")); // floor(49.98) - 8 seconds.
    QVERIFY(server.requests[1] != server.requests[0]);
    QVERIFY(server.requests[2] != server.requests[1]);
    // Normalization can release the partial packet retained from the previous read.
    QVERIFY(session->peakBufferedBytes() <= 8192 + (normalize ? 187 : 0));
    QVERIFY(!session->hasNetworkError());
}

void CatchupStreamTests::cancelWhileWaitingAtSafeEdge()
{
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.replies = {transport(5000)};
    auto session = Player::CatchupStreamSession::create(server.url());
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-200);
    session->configureContinuous({server.url(), start, start.addSecs(3600)});
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session]() { return readAll(session); });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(session->providerConnectionClosed(), 3000);
    QTest::qWait(1000);
    QCOMPARE(server.requests.size(), 1);
    QVERIFY(!future.isFinished());
    session->cancelRead();
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 1000);
    QCOMPARE(future.result().terminal, -1);
}

void CatchupStreamTests::repeatedMinuteSnapshot_data()
{
    QTest::addColumn<bool>("published");
    QTest::addColumn<int>("duration");
    QTest::newRow("next-minute-published") << true << 180;
    QTest::newRow("prefetch-growing-minute") << false << 180;
    QTest::newRow("published-finite-partial-minute") << true << 80;
}

void CatchupStreamTests::repeatedMinuteSnapshot()
{
    QFETCH(bool, published);
    QFETCH(int, duration);
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto source = transport(duration * 50);
    const auto first = source.left(188 * 3021); // 60.4 seconds, as in the provider log.
    server.replies = {first, first, published ? source : source.left(188 * 6000)};
    auto session = Player::CatchupStreamSession::create(server.url());
    const auto start = QDateTime::currentDateTimeUtc().addSecs(published ? -600 : -260);
    session->configureContinuous({server.url(), start, start.addSecs(duration)});
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session]() { return readAll(session); });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() >= 2, 3000);
    QVERIFY(server.requests[1].contains("12-00-52/1.ts"));
    if (published) {
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        QCOMPARE(future.result().terminal, 0);
        QCOMPARE(future.result().bytes, source); // No duplicate playback at the join.
        QCOMPARE(server.requests.size(), 3);
        QVERIFY(server.requests[2].contains("12-00/1.ts"));
        if (duration == 80) {
            QVERIFY(server.requests[2].contains("/2/2026-01-01:12-00/"));
        }
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(server.requests.size() == 3, 3000);
        QVERIFY(server.requests[2].contains("/2/2026-01-01:12-00/"));
        QTest::qWait(6000);
        QCOMPARE(server.requests.size(), 3);
        QVERIFY(!future.isFinished());
        QVERIFY(session->continuous());
    }
    QVERIFY(!session->hasNetworkError());
}

void CatchupStreamTests::minuteContinuationRetriesPartialResponses()
{
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto source = transport(9000);
    const auto first = source.left(188 * 3021);
    // The provider ends a successful three-minute request after only 118.8s.
    // Requested HTTP coverage must not be treated as delivered media coverage.
    server.replies = {first, first, source.left(188 * 5941), source.mid(188 * 3000)};
    auto session = Player::CatchupStreamSession::create(server.url());
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-320);
    session->configureContinuous({server.url(), start, start.addSecs(180)});
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session]() { return readAll(session); });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 6000);
    QCOMPARE(server.requests.size(), 4);
    QVERIFY(server.requests[2].contains("/3/2026-01-01:12-00/"));
    QVERIFY(server.requests[3].contains("/2/2026-01-01:12-01/"));
    QCOMPARE(future.result().terminal, 0);
    QCOMPARE(future.result().bytes, source);
    QVERIFY(!session->hasNetworkError());
}

void CatchupStreamTests::continuationStartsWhenOverlapAnchorIsSafe()
{
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto source = transport(9000);
    const auto first = source.left(188 * 3001); // 60s delivered, safe edge only 54s.
    server.replies = {first, source.mid(188 * 2600)};
    auto session = Player::CatchupStreamSession::create(server.url());
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-234);
    session->configureContinuous({server.url(), start, start.addSecs(180)});
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session]() { return readAll(session); });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 3000);
    QCOMPARE(server.requests.size(), 2);
    QVERIFY(server.requests[1].contains("12-00-52/1.ts"));
    QCOMPARE(future.result().terminal, 0);
    QCOMPARE(future.result().bytes, source);
}

void CatchupStreamTests::failUnmatchedContinuation()
{
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto first = transport(2500);
    server.replies = {first, transport(2500, 90 * 90000)};
    auto session = Player::CatchupStreamSession::create(server.url());
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-600);
    session->configureContinuous({server.url(), start, start.addSecs(150)});
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session]() { return readAll(session); });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
    QCOMPARE(future.result().bytes, first);
    QCOMPARE(future.result().terminal, -1);
    QVERIFY(!session->continuous());
}

void CatchupStreamTests::minuteContinuationWaitsAfterIdenticalSnapshot()
{
    ArchiveServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const auto first = transport(3000);
    const auto partial = transport(4000);
    const auto complete = transport(9000);
    server.replies = {first, first, partial, partial.mid(3000 * 188), complete.mid(3000 * 188)};
    auto session = Player::CatchupStreamSession::create(server.url());
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-600);
    session->configureContinuous({server.url(), start, start.addSecs(180)});
    QVERIFY(session->start());
    auto future = QtConcurrent::run([session]() { return readAll(session); });
    const auto cleanup = qScopeGuard([&]() { session->cancelRead(); session->closeProviderConnection(QStringLiteral("test")); future.waitForFinished(); });
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 4, 6000);
    QTest::qWait(6000);
    QCOMPARE(server.requests.size(), 4);
    QVERIFY(session->continuous());
    QVERIFY(!session->hasNetworkError());
    // A finite URL may gain its missing tail without ever changing. The slower
    // retry must eventually fetch it rather than leave the reader waiting forever.
    QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 35000);
    QCOMPARE(server.requests.size(), 5);
    QCOMPARE(future.result().bytes, complete);
    QCOMPARE(future.result().terminal, 0);
    QVERIFY(!session->hasNetworkError());
}

void CatchupStreamTests::safeWindowUsesProviderTimestampAndMargin()
{
    const auto now = QDateTime::fromString(QStringLiteral("2026-09-12T19:20:00Z"), Qt::ISODate);
    QCOMPARE(Core::CatchupUrlResolver::availableEdge(now.addSecs(2400), 180, now), now.addSecs(-180));
    QCOMPARE(Core::CatchupUrlResolver::availableEdge(now.addSecs(2400), 0, now), now);
    QCOMPARE(Core::CatchupUrlResolver::availableEdge(now.addSecs(2400), -60, now), now);
    QCOMPARE(Core::CatchupUrlResolver::availableEdge(now.addSecs(2400), 60, now), now.addSecs(-60));
    QCOMPARE(Core::CatchupUrlResolver::availableEdge(now.addSecs(2400), 120, now), now.addSecs(-120));
    QCOMPARE(Core::CatchupUrlResolver::availableEdge(now.addSecs(-60), 180, now), now.addSecs(-180));
    QCOMPARE(Core::CatchupUrlResolver::availableEdge(now.addSecs(-600), 180, now), now.addSecs(-600));
    const auto canonical = QStringLiteral("https://provider/timeshift/user/password/17/2026-09-12:21-00/1.ts");
    QCOMPARE(Core::CatchupUrlResolver::xtreamWindowUrl(canonical, 892, 1800, true),
             QStringLiteral("https://provider/timeshift/user/password/15/2026-09-12:21-14-52/1.ts"));
}

QTEST_GUILESS_MAIN(CatchupStreamTests)
#include "tst_catchup_stream.moc"
