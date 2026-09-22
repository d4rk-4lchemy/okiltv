#include "../src/app/catchupdownloadcontroller.h"
#include "../src/app/catchuparchivetransfer.h"
#include "../src/app/catchupdownloadtransfer.h"
#include "../src/core/catchupurlresolver.h"
#include "../src/core/secretprotection.h"
#include "../src/core/settingsmanager.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtTest>

using namespace OKILTV::App;
using namespace OKILTV::Core;

class CatchupDownloadTests final : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir m_media;
    QByteArray m_previousAppData;
    QByteArray m_video;
    QByteArray m_audio;

    struct Fixture {
        QTemporaryDir directory;
        SettingsManager settings { directory.filePath(QStringLiteral("settings.json")) };
        ServerProfile profile;
        Channel channel;
        EpgEntry program;
        QTcpServer server;
        QByteArray payload;
        QHash<QByteArray, QByteArray> resources;
        QByteArray receivedHeaders;
        int requests { 0 };
        bool stall { false };
        bool stallAfterPayload { false };
        int httpStatus { 200 };
        int delayMs { 0 };
        bool slow { false };
        int chunkIntervalMs { 10 };
        qsizetype chunkBytes { 16384 };
        bool interruptResponse { false };
        bool ranges { false };
        bool unknownLength { false };
        bool badRange { false };
        QByteArray etag;
        QByteArray contentType { "video/mp2t" };
        QList<QByteArray> requestsSeen;
        QMap<qint64, qsizetype> timeOffsets;
        QList<qint64> requestedTimes;
        qint64 sentBytes { 0 };
        Fixture()
        {
            profile.id = QUuid::createUuid();
            profile.type = ProfileType::M3UUrl;
            profile.name = QStringLiteral("Test source");
            profile.catchupSafetyMinutes = 0;
            settings.addProfile(profile);
            channel.id = 7;
            channel.profileId = profile.id;
            channel.name = QStringLiteral("Test channel");
            channel.source = ChannelSource::M3U;
            channel.catchupSupported = true;
            channel.catchupWindowHours = 24;
            channel.catchupMode = QStringLiteral("default");
            program.title = QStringLiteral("Programme Żółć");
            program.start = QDateTime::currentDateTimeUtc().addSecs(-3600);
            program.stop = program.start.addSecs(12);
            server.listen(QHostAddress::LocalHost);
            channel.catchupSourceTemplate = QStringLiteral("http://127.0.0.1:%1/archive?utc={utc}&duration={duration}").arg(server.serverPort());
            QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
                while (auto *socket = server.nextPendingConnection()) {
                    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                        auto request = socket->property("request").toByteArray() + socket->readAll();
                        socket->setProperty("request", request);
                        if (!request.contains("\r\n\r\n") || socket->property("responded").toBool())
                            return;
                        socket->setProperty("responded", true);
                        receivedHeaders = request;
                        requestsSeen.append(request);
                        ++requests;
                        if (stall)
                            return;
                        const auto requestPath = request.split(' ').value(1).split('?').first();
                        auto responsePayload = resources.value(requestPath, payload);
                        if (!timeOffsets.isEmpty()) {
                            const QUrlQuery query(QUrl::fromEncoded(request.split(' ').value(1)));
                            const auto offset = query.queryItemValue(QStringLiteral("offset")).toLongLong();
                            requestedTimes.append(offset);
                            responsePayload = payload.mid(timeOffsets.value(offset));
                        }
                        const QPointer<QTcpSocket> guarded(socket);
                        QTimer::singleShot(delayMs, socket, [this, guarded, responsePayload] {
                            if (!guarded)
                                return;
                            const auto range = QRegularExpression(QStringLiteral("Range: bytes=(\\d+)-"),
                                QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(receivedHeaders));
                            const bool partial = ranges && range.hasMatch() && httpStatus == 200;
                            const auto offset = partial ? range.captured(1).toLongLong() : 0;
                            const auto body = responsePayload.mid(offset);
                            QByteArray headers = "HTTP/1.1 " + QByteArray::number(partial ? 206 : httpStatus)
                                + " Response\r\nContent-Type: " + contentType + "\r\nConnection: close\r\n";
                            if (!unknownLength)
                                headers += "Content-Length: " + QByteArray::number(body.size() + (stallAfterPayload ? 1024 : 0)) + "\r\n";
                            if (!etag.isEmpty())
                                headers += "ETag: " + etag + "\r\n";
                            if (partial)
                                headers += "Content-Range: bytes " + QByteArray::number(offset + (badRange ? 1 : 0))
                                    + '-' + QByteArray::number(responsePayload.size() - 1) + '/' + QByteArray::number(responsePayload.size()) + "\r\n";
                            guarded->write(headers + "\r\n");
                            if (slow) {
                                auto *timer = new QTimer(guarded);
                                timer->setInterval(chunkIntervalMs);
                                QObject::connect(timer, &QTimer::timeout, guarded, [this, guarded, timer, body, cursor = qsizetype(0)]() mutable {
                                    if (!guarded || guarded->state() != QAbstractSocket::ConnectedState) {
                                        timer->stop();
                                        return;
                                    }
                                    const auto chunk = body.mid(cursor, chunkBytes);
                                    guarded->write(chunk);
                                    sentBytes += chunk.size();
                                    cursor += chunk.size();
                                    if (cursor >= body.size()) {
                                        timer->stop();
                                        if (!stallAfterPayload)
                                            guarded->disconnectFromHost();
                                    }
                                });
                                timer->start();
                            } else {
                                guarded->write(interruptResponse ? body.first(std::min<qsizetype>(32768, body.size())) : body);
                                if (!stallAfterPayload)
                                    guarded->disconnectFromHost();
                            }
                        });
                    });
                }
            });
        }
        QUrl output(const QString &name = QStringLiteral("programme.mkv")) const
        {
            return QUrl::fromLocalFile(directory.filePath(name));
        }
        QStringList temporaryFiles() const
        {
            return QDir(directory.path()).entryList({QStringLiteral(".okiltv-download-*")}, QDir::Files | QDir::Hidden);
        }
    };
    static QString state(const CatchupDownloadController &controller, int row = 0)
    {
        return controller.data(controller.index(row), CatchupDownloadController::StateRole).toString();
    }
    static QString id(const CatchupDownloadController &controller, int row = 0)
    {
        return controller.data(controller.index(row), CatchupDownloadController::JobIdRole).toString();
    }

private slots:
    void initTestCase()
    {
        m_previousAppData = qgetenv("APPDATA");
        qputenv("APPDATA", m_media.path().toUtf8());
        useIsolatedSecretKeyForTests();
        if (!ffmpegToolsAvailable())
            QSKIP("ffmpeg and ffprobe are needed for archive integration tests");
        QVERIFY(m_media.isValid());
        for (const bool audioOnly : {false, true}) {
            const auto path = m_media.filePath(audioOnly ? QStringLiteral("audio.ts") : QStringLiteral("video.ts"));
            QStringList args {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-y")};
            if (!audioOnly)
                args << QStringLiteral("-f") << QStringLiteral("lavfi") << QStringLiteral("-i") << QStringLiteral("testsrc2=size=160x90:rate=25");
            args << QStringLiteral("-f") << QStringLiteral("lavfi") << QStringLiteral("-i") << QStringLiteral("sine=frequency=440:sample_rate=48000")
                 << QStringLiteral("-t") << QStringLiteral("12");
            if (!audioOnly)
                args << QStringLiteral("-map") << QStringLiteral("0:v") << QStringLiteral("-map") << QStringLiteral("1:a")
                     << QStringLiteral("-map") << QStringLiteral("1:a") << QStringLiteral("-c:v") << QStringLiteral("mpeg2video")
                     << QStringLiteral("-g") << QStringLiteral("25");
            args << QStringLiteral("-c:a") << QStringLiteral("mp2") << QStringLiteral("-f") << QStringLiteral("mpegts") << path;
            QProcess generator;
            generator.start(resolveProcessBinary(QStringLiteral("ffmpeg")), args);
            QVERIFY(generator.waitForFinished(30000));
            QCOMPARE(generator.exitCode(), 0);
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            (audioOnly ? m_audio : m_video) = file.readAll();
        }
    }

    void cleanupTestCase()
    {
        if (m_previousAppData.isNull())
            qunsetenv("APPDATA");
        else
            qputenv("APPDATA", m_previousAppData);
    }

    void saveDialogSuggestsNonexistentName()
    {
        Fixture fixture;
        CatchupDownloadController controller(&fixture.settings);
        QSignalSpy chosen(&controller, &CatchupDownloadController::destinationChosen);
        QSignalSpy cancelled(&controller, &CatchupDownloadController::destinationCancelled);
        const auto output = fixture.output(QStringLiteral("Programme Żółć.mkv"));
        controller.chooseDestination(nullptr, output);
        QFileDialog *dialog = nullptr;
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (auto *candidate = qobject_cast<QFileDialog *>(widget))
                dialog = candidate;
        }
        QVERIFY(dialog);
        QCOMPARE(dialog->selectedUrls(), QList<QUrl>{output});
        QVERIFY(!QFileInfo::exists(output.toLocalFile()));
        controller.cancelDestination();
        QCOMPARE(cancelled.count(), 1);
        QCOMPARE(chosen.count(), 0);
        QVERIFY(!QFileInfo::exists(output.toLocalFile()));
    }

    void transferResume_data()
    {
        QTest::addColumn<bool>("ranges");
        QTest::addColumn<bool>("validator");
        QTest::addColumn<bool>("unknownLength");
        QTest::newRow("range-with-validator") << true << true << false;
        QTest::newRow("no-validator") << true << false << false;
        QTest::newRow("unknown-length") << true << false << true;
    }

    void transferResume()
    {
        QFETCH(bool, ranges);
        QFETCH(bool, validator);
        QFETCH(bool, unknownLength);
        Fixture fixture;
        fixture.payload = m_video;
        fixture.slow = true;
        fixture.ranges = ranges;
        fixture.unknownLength = unknownLength;
        fixture.etag = validator ? QByteArray("\"archive-v1\"") : QByteArray();
        const auto target = CatchupUrlResolver(fixture.profile).resolveDownload(fixture.channel, fixture.program);
        QVERIFY(target);
        CatchupDownloadTransfer transfer;
        QSignalSpy progress(&transfer, &CatchupDownloadTransfer::progress);
        QSignalSpy paused(&transfer, &CatchupDownloadTransfer::paused);
        QSignalSpy completed(&transfer, &CatchupDownloadTransfer::completed);
        QSignalSpy failed(&transfer, &CatchupDownloadTransfer::failed);
        const auto path = fixture.directory.filePath(QStringLiteral("source.part"));
        quint64 token = 1;
        transfer.start(token, QUrl(target->url), path, QStringLiteral("resume-test"));
        qint64 saved = 0;
        for (int attempt = 0; attempt < 3; ++attempt) {
            QTRY_VERIFY(!progress.isEmpty() && progress.last()[1].toLongLong() > saved + 32768);
            transfer.pause(++token);
            QCOMPARE(paused.count(), attempt + 1);
            saved = progress.last()[1].toLongLong();
            QFile partial(path);
            QVERIFY(partial.open(QIODevice::ReadOnly));
            QCOMPARE(partial.readAll(), fixture.payload.first(saved));
            partial.close();
            const auto requests = fixture.requests;
            QTest::qWait(30);
            QCOMPARE(fixture.requests, requests);
            QCOMPARE(QFileInfo(path).size(), saved);
            transfer.start(++token, QUrl(target->url), path, QStringLiteral("resume-test"));
        }
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QCOMPARE(failed.count(), 0);
        QFile result(path);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(result.readAll(), fixture.payload);
        QVERIFY(fixture.requestsSeen.first().contains("User-Agent: resume-test"));
        QVERIFY(fixture.requestsSeen[1].contains("Range: bytes="));
        QCOMPARE(fixture.requestsSeen[1].contains("If-Range:"), validator);
        QVERIFY(progress.last()[2].toLongLong() > 0); // Content-Range supplies the length on resume.
    }

    void ignoredRangesDoNotReplayLargePrefix()
    {
        Fixture fixture;
        fixture.payload = m_video;
        fixture.slow = true;
        CatchupDownloadTransfer transfer;
        QSignalSpy progress(&transfer, &CatchupDownloadTransfer::progress);
        QSignalSpy failed(&transfer, &CatchupDownloadTransfer::failed);
        const QUrl url(fixture.channel.catchupSourceTemplate);
        const auto path = fixture.directory.filePath(QStringLiteral("source.part"));
        transfer.start(1, url, path, {});
        QTRY_VERIFY(!progress.isEmpty() && progress.last()[1].toLongLong() > 128 * 1024);
        transfer.pause(2);
        const auto saved = QFileInfo(path).size();
        const auto sent = fixture.sentBytes;
        transfer.start(3, url, path, {});
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 45000);
        QCOMPARE(QFileInfo(path).size(), saved);
        QVERIFY(fixture.sentBytes - sent < 64 * 1024);
        QVERIFY(failed.first()[1].toString().contains(QStringLiteral("cannot resume")));
    }

    void timedResumeAndMediaProgress_data()
    {
        QTest::addColumn<bool>("audioOnly");
        QTest::addColumn<bool>("changed");
        QTest::addColumn<bool>("ignoresTime");
        QTest::addColumn<bool>("resetClock");
        QTest::newRow("video") << false << false << false << false;
        QTest::newRow("radio") << true << false << false << false;
        QTest::newRow("changed-overlap") << false << true << false << false;
        QTest::newRow("ignored-time-window") << false << false << true << false;
        QTest::newRow("clock-reset-video") << false << false << false << true;
        QTest::newRow("clock-reset-radio") << true << false << false << true;
    }

    void timedResumeAndMediaProgress()
    {
        QFETCH(bool, audioOnly);
        QFETCH(bool, changed);
        QFETCH(bool, ignoresTime);
        QFETCH(bool, resetClock);
        Fixture fixture;
        const auto media = fixture.directory.filePath(QStringLiteral("long.ts"));
        QProcess generator;
        generator.start(resolveProcessBinary(QStringLiteral("ffmpeg")), {
            QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"),
            QStringLiteral("-stream_loop"), QStringLiteral("14"), QStringLiteral("-i"),
            m_media.filePath(audioOnly ? QStringLiteral("audio.ts") : QStringLiteral("video.ts")),
            QStringLiteral("-map"), QStringLiteral("0"), QStringLiteral("-c"), QStringLiteral("copy"),
            QStringLiteral("-f"), QStringLiteral("mpegts"), media});
        QVERIFY(generator.waitForFinished(30000));
        QCOMPARE(generator.exitCode(), 0);
        QFile file(media);
        QVERIFY(file.open(QIODevice::ReadOnly));
        fixture.payload = file.readAll();
        if (resetClock) {
            // Three concatenated 60-second broadcasts with reset PTS/DTS.
            OKILTV::Player::CatchupTsJoiner first;
            qsizetype end = 0;
            while (end < fixture.payload.size() && first.durationSeconds() < 60) {
                first.push(fixture.payload.mid(end, 188));
                end += 188;
            }
            fixture.payload = fixture.payload.first(end).repeated(3);
        }
        fixture.slow = true;
        fixture.chunkIntervalMs = 2;
        fixture.chunkBytes = 128 * 1024;
        fixture.unknownLength = true; // The real provider has neither size, ETag nor ranges.
        fixture.timeOffsets.insert(0, 0);
        CatchupDownloadClock clock;
        qint64 nextMinute = 60;
        for (qsizetype offset = 0; offset < fixture.payload.size(); offset += 188 * 32) {
            clock.push(fixture.payload.mid(offset, 188 * 32));
            if (clock.durationSeconds() >= static_cast<double>(nextMinute)) {
                fixture.timeOffsets.insert(nextMinute, offset);
                nextMinute += 60;
            }
        }
        QVERIFY(clock.known());
        QVERIFY(clock.durationSeconds() > 170 && clock.durationSeconds() < 185);
        const auto url = QUrl(QStringLiteral("http://127.0.0.1:%1/archive").arg(fixture.server.serverPort()));
        DownloadTimeline timeline {180, 0, [url](qint64 offset) {
            auto result = url;
            result.setQuery(QStringLiteral("offset=%1").arg(offset));
            return result;
        }};
        CatchupDownloadTransfer transfer;
        QSignalSpy progress(&transfer, &CatchupDownloadTransfer::progress);
        QSignalSpy completed(&transfer, &CatchupDownloadTransfer::completed);
        QSignalSpy failed(&transfer, &CatchupDownloadTransfer::failed);
        const auto path = fixture.directory.filePath(QStringLiteral("source.part"));
        transfer.start(1, url, path, {}, false, timeline);
        QTRY_VERIFY_WITH_TIMEOUT(transfer.mediaFraction() > (ignoresTime ? 0.8 : 0.5), 15000);
        QVERIFY(transfer.mediaFraction() < 0.95);
        transfer.pause(2);
        const auto saved = QFileInfo(path).size();
        const auto fraction = transfer.mediaFraction();
        const auto sent = fixture.sentBytes;
        if (changed)
            fixture.payload.replace(saved - 3000, 3000, QByteArray(3000, 'x'));
        if (ignoresTime)
            for (auto &offset : fixture.timeOffsets) offset = 0;
        transfer.start(3, url, path, {}, false, timeline);
        if (changed || ignoresTime) {
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 45000);
            QCOMPARE(QFileInfo(path).size(), saved);
            QCOMPARE(completed.count(), 0);
            QVERIFY(failed.first()[1].toString().contains(QStringLiteral("overlap")));
            return;
        }
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(path).size() > saved + 32768, 15000);
        QVERIFY(transfer.mediaFraction() >= fraction);
        QVERIFY(fixture.sentBytes - sent < saved); // No full-prefix replay.
        QVERIFY(fixture.requestedTimes.last() >= 60);
        transfer.pause(4);
        const auto savedAgain = QFileInfo(path).size();
        transfer.start(5, url, path, {}, false, timeline);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QCOMPARE(failed.count(), 0);
        QVERIFY(savedAgain > saved);
        QFile result(path);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(result.readAll(), fixture.payload); // Exact source continuity, no gap or duplicate.
        QVERIFY(transfer.mediaFraction() > 0.98);
        QCOMPARE(fixture.requests, 3);
    }

    void resumeRejectsChangedArchive_data()
    {
        QTest::addColumn<int>("failure");
        QTest::newRow("changed-prefix") << 0;
        QTest::newRow("invalid-range") << 1;
        QTest::newRow("changed-validator") << 2;
        QTest::newRow("range-not-satisfiable") << 3;
        QTest::newRow("changed-range-overlap-without-etag") << 4;
    }

    void resumeRejectsChangedArchive()
    {
        QFETCH(int, failure);
        Fixture fixture;
        fixture.payload = m_video;
        fixture.slow = true;
        fixture.ranges = failure != 0;
        fixture.etag = failure != 0 && failure != 4 ? QByteArray("\"archive-v1\"") : QByteArray();
        const auto target = CatchupUrlResolver(fixture.profile).resolveDownload(fixture.channel, fixture.program);
        QVERIFY(target);
        CatchupDownloadTransfer transfer;
        QSignalSpy progress(&transfer, &CatchupDownloadTransfer::progress);
        QSignalSpy failed(&transfer, &CatchupDownloadTransfer::failed);
        const auto path = fixture.directory.filePath(QStringLiteral("source.part"));
        transfer.start(1, QUrl(target->url), path, {});
        QTRY_VERIFY(!progress.isEmpty() && progress.last()[1].toLongLong() > 32768);
        transfer.pause(2);
        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto saved = original.readAll();
        original.close();
        if (failure == 1)
            fixture.badRange = true;
        else if (failure == 2)
            fixture.etag = "\"archive-v2\"";
        else if (failure == 3)
            fixture.httpStatus = 416;
        else if (failure == 4)
            fixture.payload[saved.size() - 1] = static_cast<char>(fixture.payload[saved.size() - 1] ^ 1);
        else
            fixture.payload[0] = static_cast<char>(fixture.payload[0] ^ 1);
        transfer.start(3, QUrl(target->url), path, {});
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 45000);
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), saved);
    }

    void resumeRetriesAfterCooldowns_data()
    {
        QTest::addColumn<QString>("outcome");
        for (const auto &outcome : {"recover", "recover-second", "fail", "pause", "stop"})
            QTest::newRow(outcome) << QString::fromLatin1(outcome);
    }

    void resumeRetriesAfterCooldowns()
    {
        QFETCH(QString, outcome);
        Fixture fixture;
        fixture.payload = m_video;
        fixture.slow = true;
        const QUrl url(fixture.channel.catchupSourceTemplate);
        const DownloadTimeline timeline {12, 0, [url](qint64) { return url; }};
        CatchupDownloadTransfer transfer;
        QSignalSpy progress(&transfer, &CatchupDownloadTransfer::progress);
        QSignalSpy paused(&transfer, &CatchupDownloadTransfer::paused);
        QSignalSpy failed(&transfer, &CatchupDownloadTransfer::failed);
        QSignalSpy completed(&transfer, &CatchupDownloadTransfer::completed);
        const auto path = fixture.directory.filePath(QStringLiteral("source.part"));
        transfer.start(1, url, path, {}, false, timeline);
        QTRY_VERIFY(transfer.mediaFraction() > 0.1);
        transfer.pause(2);
        const auto saved = QFileInfo(path).size();
        const auto fraction = transfer.mediaFraction();
        fixture.httpStatus = 503;
        transfer.start(3, url, path, {}, false, timeline);
        QTRY_COMPARE(fixture.requests, 2);
        QTest::qWait(100); // Let the rejected response schedule its retry.
        QCOMPARE(failed.count(), 0);
        QCOMPARE(paused.count(), 1);
        QCOMPARE(QFileInfo(path).size(), saved);
        QCOMPARE(transfer.mediaFraction(), fraction);
        QVERIFY(progress.last()[3].toBool());
        QVERIFY(!fixture.requestsSeen.last().contains("Range:"));
        if (outcome == QStringLiteral("pause"))
            transfer.pause(4);
        else if (outcome == QStringLiteral("stop"))
            transfer.stop(4);
        if (outcome != QStringLiteral("fail") && outcome != QStringLiteral("recover-second"))
            fixture.httpStatus = 200;
        QTest::qWait(9000);
        QCOMPARE(fixture.requests, 2); // No immediate retry or full-prefix restart.
        QCOMPARE(QFileInfo(path).size(), saved);
        if (outcome == QStringLiteral("recover")) {
            QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
            QCOMPARE(failed.count(), 0);
            QCOMPARE(fixture.requests, 3);
            QVERIFY(!fixture.requestsSeen.last().contains("Range:"));
            QFile result(path);
            QVERIFY(result.open(QIODevice::ReadOnly));
            QCOMPARE(result.readAll(), fixture.payload);
        } else if (outcome == QStringLiteral("fail") || outcome == QStringLiteral("recover-second")) {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.requests, 3, 5000);
            QTest::qWait(100);
            QCOMPARE(failed.count(), 0);
            QCOMPARE(paused.count(), 1);
            if (outcome == QStringLiteral("recover-second"))
                fixture.httpStatus = 200;
            QTest::qWait(29000);
            QCOMPARE(fixture.requests, 3);
            QCOMPARE(QFileInfo(path).size(), saved);
            if (outcome == QStringLiteral("recover-second")) {
                QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
                QCOMPARE(failed.count(), 0);
                QFile result(path);
                QVERIFY(result.open(QIODevice::ReadOnly));
                QCOMPARE(result.readAll(), fixture.payload);
                QCOMPARE(fixture.requests, 4);
                return;
            }
            QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
            QCOMPARE(paused.count(), 1); // Only the third failure is terminal, even HTTP 503.
            QCOMPARE(fixture.requests, 4);
            QCOMPARE(QFileInfo(path).size(), saved);
            QTest::qWait(200);
            QCOMPARE(failed.count(), 1);
        } else {
            QTest::qWait(1500);
            QCOMPARE(fixture.requests, 2); // Pause/stop cancels the queued retry.
            QCOMPARE(failed.count(), 0);
            QCOMPARE(completed.count(), 0);
            QCOMPARE(QFileInfo(path).size(), saved);
        }
    }

    void mediaClockSurvivesConfigurationChange()
    {
        CatchupDownloadClock clock;
        for (const auto &media : {m_video, m_audio, m_video}) {
            const auto before = clock.durationSeconds();
            for (qsizetype at = 0; at < media.size(); at += 1024)
                clock.push(media.mid(at, 1024));
            QVERIFY(clock.known());
            QVERIFY(clock.durationSeconds() >= before + 11);
            QVERIFY(clock.durationSeconds() <= before + 13);
        }
    }

    void pauseQueueAndFinalize()
    {
        Fixture fixture;
        fixture.payload = m_video;
        fixture.slow = true;
        fixture.ranges = true;
        CatchupDownloadController controller(&fixture.settings);
        bool sourceMatches = false;
        connect(&controller, &QAbstractItemModel::dataChanged, this, [&] {
            if (state(controller) != QStringLiteral("finalizing"))
                return;
            const auto files = QDir(fixture.directory.path()).entryList({QStringLiteral("*.source")}, QDir::Files | QDir::Hidden);
            if (files.size() != 1)
                return;
            QFile source(fixture.directory.filePath(files.first()));
            if (source.open(QIODevice::ReadOnly))
                sourceMatches = source.readAll() == fixture.payload;
            controller.pauseAll(); // Local processing must finish during pause.
        });
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QTRY_VERIFY(controller.data(controller.index(0), CatchupDownloadController::BytesRole).toLongLong() > 32768);
        const auto firstId = id(controller);
        controller.pauseAll();
        controller.pauseAll();
        QVERIFY(controller.paused());
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output(QStringLiteral("second.mkv"))), QString());
        QTest::qWait(100);
        QCOMPARE(fixture.requests, 1);
        QCOMPARE(state(controller), QStringLiteral("paused"));
        const auto savedBytes = controller.data(controller.index(0), CatchupDownloadController::BytesRole).toLongLong();
        QTest::qWait(50);
        QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::BytesRole).toLongLong(), savedBytes);
        controller.resumeAll();
        controller.resumeAll();
        QTRY_COMPARE_WITH_TIMEOUT(state(controller), QStringLiteral("completed"), 15000);
        QVERIFY(sourceMatches);
        QVERIFY(controller.paused());
        QCOMPARE(state(controller, 1), QStringLiteral("queued"));
        QCOMPARE(fixture.requests, 2);
        QCOMPARE(id(controller), firstId);
        controller.cancel(id(controller, 1));
        QVERIFY(!controller.hasPending());
        QVERIFY(!controller.paused());
        QVERIFY(fixture.temporaryFiles().isEmpty());
    }

    void disconnectedTransferCanResume()
    {
        Fixture fixture;
        fixture.payload = m_video;
        fixture.interruptResponse = true;
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QTRY_VERIFY(controller.paused());
        QCOMPARE(state(controller), QStringLiteral("paused"));
        QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::BytesRole).toLongLong(), qint64(32768));
        fixture.interruptResponse = false;
        controller.resumeAll();
        QTRY_COMPARE_WITH_TIMEOUT(state(controller), QStringLiteral("completed"), 15000);
        QVERIFY(fixture.temporaryFiles().isEmpty());
    }

    void expiredArchivePreservesPausedSource()
    {
        Fixture fixture;
        fixture.payload = m_video;
        fixture.slow = true;
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QTRY_VERIFY(controller.data(controller.index(0), CatchupDownloadController::BytesRole).toLongLong() > 32768);
        controller.pauseAll();
        fixture.settings.current().catchupEnabled = false;
        controller.resumeAll();
        QTRY_COMPARE(state(controller), QStringLiteral("failed"));
        const auto partial = controller.data(controller.index(0), CatchupDownloadController::PathRole).toString();
        QVERIFY(partial.endsWith(QStringLiteral(".partial.download")));
        QVERIFY(QFileInfo(partial).size() > 0);
        QCOMPARE(fixture.requests, 1);
        QVERIFY(fixture.temporaryFiles().isEmpty());
        controller.dismiss(id(controller));
        QVERIFY(QFileInfo::exists(partial));
    }

    void pauseImmediatelyAndCancel()
    {
        Fixture fixture;
        fixture.payload = m_video;
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        controller.pauseAll();
        QTest::qWait(50);
        QCOMPARE(fixture.requests, 0);
        QCOMPARE(state(controller), QStringLiteral("queued"));
        controller.resumeAll();
        controller.pauseAll();
        QSignalSpy stopped(&controller, &CatchupDownloadController::shutdownFinished);
        controller.shutdown();
        QTRY_COMPARE(stopped.count(), 1);
        QVERIFY(!controller.hasPending());
        QVERIFY(fixture.temporaryFiles().isEmpty());
    }

    void completedTransferPauseRace()
    {
        Fixture fixture;
        fixture.payload = m_audio;
        const auto target = CatchupUrlResolver(fixture.profile).resolveDownload(fixture.channel, fixture.program);
        QVERIFY(target);
        CatchupDownloadTransfer transfer;
        QSignalSpy completed(&transfer, &CatchupDownloadTransfer::completed);
        const auto path = fixture.directory.filePath(QStringLiteral("source.part"));
        transfer.start(1, QUrl(target->url), path, {});
        QTRY_COMPARE(completed.count(), 1);
        transfer.pause(2);
        QCOMPARE(completed.count(), 2);
        QCOMPARE(completed.last()[0].toULongLong(), quint64(2));
        transfer.start(3, QUrl(target->url), path, {});
        QCOMPARE(completed.count(), 3);
        QCOMPARE(fixture.requests, 1);
    }

    void archiveProgressOutlivesProgrammeEstimate_data()
    {
        QTest::addColumn<bool>("unknownLength");
        QTest::newRow("known-size") << false;
        QTest::newRow("unknown-size") << true;
    }

    void archiveProgressOutlivesProgrammeEstimate()
    {
        QFETCH(bool, unknownLength);
        Fixture fixture;
        fixture.payload = m_video;
        fixture.unknownLength = unknownLength;
        fixture.slow = true;
        const auto target = CatchupUrlResolver(fixture.profile).resolveDownload(fixture.channel, fixture.program);
        QVERIFY(target);
        CatchupArchiveTransfer transfer;
        QSignalSpy completed(&transfer, &CatchupArchiveTransfer::completed);
        QSignalSpy failed(&transfer, &CatchupArchiveTransfer::failed);
        QSignalSpy progress(&transfer, &CatchupArchiveTransfer::progress);
        // Deliberately underestimate the twelve-second source, as providers can
        // return padding or discontinuous clocks beyond the requested programme.
        transfer.start(1, QUrl(target->url), fixture.directory.filePath(QStringLiteral("source.part")), {},
                       DownloadTimeline {1, 0, {}});
        QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty() || !failed.isEmpty(), 15000);
        QVERIFY(failed.isEmpty());
        QCOMPARE(completed.count(), 1);
        int laterUpdates = 0;
        for (const auto &update : progress) {
            const auto bytes = update[1].toLongLong();
            if (bytes <= m_video.size() / 2 || bytes >= m_video.size())
                continue;
            ++laterUpdates;
            if (unknownLength)
                QCOMPARE(update[4].toDouble(), 1.0);
            else
                QCOMPARE(update[4].toDouble(), static_cast<double>(bytes) / static_cast<double>(m_video.size()));
        }
        QVERIFY(laterUpdates >= 2);
        transfer.close();
    }

    void boundedTransferCompletesWithoutServerEof_data()
    {
        QTest::addColumn<bool>("unknownLength");
        QTest::addColumn<bool>("pauseAtLimit");
        QTest::newRow("known-size") << false << false;
        QTest::newRow("unknown-size") << true << false;
        QTest::newRow("pause-known-size") << false << true;
        QTest::newRow("pause-unknown-size") << true << true;
    }

    void boundedTransferCompletesWithoutServerEof()
    {
        QFETCH(bool, unknownLength);
        QFETCH(bool, pauseAtLimit);
        Fixture fixture;
        fixture.payload = m_video.repeated(3);
        fixture.unknownLength = unknownLength;
        fixture.slow = true;
        fixture.stallAfterPayload = true;
        const auto target = CatchupUrlResolver(fixture.profile).resolveDownload(fixture.channel, fixture.program);
        QVERIFY(target);
        CatchupDownloadTransfer transfer;
        QSignalSpy completed(&transfer, &CatchupDownloadTransfer::completed);
        QSignalSpy failed(&transfer, &CatchupDownloadTransfer::failed);
        bool pausedAtLimit = false;
        connect(&transfer, &CatchupDownloadTransfer::progress, this,
            [&](quint64 token, qint64, qint64, bool) {
                if (pauseAtLimit && token == 1 && transfer.mediaFraction() >= 1) {
                    pausedAtLimit = true;
                    transfer.pause(2);
                }
            });
        const DownloadTimeline timeline {14, 0, {}, true};
        const auto path = fixture.directory.filePath(QStringLiteral("bounded.source"));
        transfer.start(1, QUrl(target->url), path, {}, false, timeline);
        QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 15000);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(pausedAtLimit, pauseAtLimit);
        QCOMPARE(completed.first()[0].toULongLong(), pauseAtLimit ? quint64(2) : quint64(1));
        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto bytes = source.readAll();
        QVERIFY(bytes.size() > m_video.size());
        QVERIFY(bytes.size() < fixture.payload.size() * 2 / 3);
        QCOMPARE(bytes, fixture.payload.first(bytes.size()));
        transfer.start(3, QUrl(target->url), path, {}, false, timeline);
        QCOMPARE(completed.count(), 2);
        QCOMPARE(fixture.requests, 1);
    }

    void xtreamStopsAfterRequiredMedia()
    {
        Fixture fixture;
        fixture.profile.type = ProfileType::Xtream;
        fixture.profile.xtreamBaseUrl = QStringLiteral("http://127.0.0.1:%1").arg(fixture.server.serverPort());
        fixture.profile.xtreamUsername = QStringLiteral("user");
        fixture.profile.xtreamPassword = QStringLiteral("pass");
        QVERIFY(fixture.settings.replaceProfile(fixture.profile.id, fixture.profile));
        fixture.channel.source = ChannelSource::Xtream;
        fixture.channel.streamUrl = fixture.profile.xtreamBaseUrl + QStringLiteral("/live/user/pass/7.ts");
        fixture.program.start = fixture.program.start.addSecs(-fixture.program.start.time().second());
        fixture.program.stop = fixture.program.start.addSecs(12);
        // A twelve-second programme requests two full minutes from Xtream.
        fixture.payload = m_video.repeated(10);
        fixture.unknownLength = true;
        fixture.slow = true;
        fixture.chunkBytes = 64 * 1024;
        CatchupDownloadController controller(&fixture.settings);
        qint64 transferredBytes = 0;
        double progressAtFinalizing = 0;
        bool knownAtFinalizing = false;
        connect(&controller, &QAbstractItemModel::dataChanged, this, [&] {
            if (state(controller) != QStringLiteral("finalizing"))
                return;
            const auto row = controller.index(0);
            transferredBytes = controller.data(row, CatchupDownloadController::BytesRole).toLongLong();
            progressAtFinalizing = controller.data(row, CatchupDownloadController::ProgressRole).toDouble();
            knownAtFinalizing = controller.data(row, CatchupDownloadController::ProgressKnownRole).toBool();
        });
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasPending(), 15000);
        QVERIFY(fixture.receivedHeaders.startsWith("GET /timeshift/user/pass/2/"));
        QVERIFY(knownAtFinalizing);
        QVERIFY(transferredBytes > m_video.size());
        QVERIFY(transferredBytes < fixture.payload.size() / 4);
        QVERIFY(progressAtFinalizing >= 99 && progressAtFinalizing < 100);
        QCOMPARE(state(controller), QStringLiteral("completed"));
        QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::ProgressRole).toDouble(), 100.0);
    }

    void pauseAtLastByteFinalizesLocally()
    {
        Fixture fixture;
        fixture.payload = m_audio;
        const auto target = CatchupUrlResolver(fixture.profile).resolveDownload(fixture.channel, fixture.program);
        QVERIFY(target);
        CatchupDownloadTransfer transfer;
        QSignalSpy completed(&transfer, &CatchupDownloadTransfer::completed);
        bool pausedAtEnd = false;
        connect(&transfer, &CatchupDownloadTransfer::progress, this,
            [&](quint64 token, qint64 bytes, qint64 total, bool) {
                if (token == 1 && bytes > 0 && bytes == total) {
                    pausedAtEnd = true;
                    transfer.pause(2);
                }
            });
        const auto path = fixture.directory.filePath(QStringLiteral("source.part"));
        transfer.start(1, QUrl(target->url), path, {});
        QTRY_VERIFY(!completed.isEmpty());
        QVERIFY(pausedAtEnd);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.first()[0].toULongLong(), quint64(2));
        QCOMPARE(fixture.requests, 1);
    }

    void finiteHlsArchive_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto &mode : {"ts", "fmp4", "byterange", "aes", "master-audio", "pause", "cancel-restart"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void finiteHlsArchive()
    {
        QFETCH(QString, mode);
        Fixture fixture;
        const auto directory = fixture.directory.filePath(QStringLiteral("hls"));
        QVERIFY(QDir().mkpath(directory));
        const auto playlist = QDir(directory).filePath(QStringLiteral("index.m3u8"));
        QStringList args {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-y"), QStringLiteral("-i"), m_media.filePath(QStringLiteral("video.ts")),
            QStringLiteral("-map"), QStringLiteral("0:v:0"), QStringLiteral("-map"), QStringLiteral("0:a:0"),
            QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), QStringLiteral("ultrafast"),
            QStringLiteral("-g"), QStringLiteral("50"), QStringLiteral("-c:a"), QStringLiteral("aac"),
            QStringLiteral("-hls_time"), QStringLiteral("2"), QStringLiteral("-hls_playlist_type"), QStringLiteral("vod")};
        if (mode == QStringLiteral("fmp4"))
            args << QStringLiteral("-hls_segment_type") << QStringLiteral("fmp4");
        if (mode == QStringLiteral("byterange"))
            args << QStringLiteral("-hls_flags") << QStringLiteral("single_file");
        if (mode == QStringLiteral("aes")) {
            QFile key(QDir(directory).filePath(QStringLiteral("key.bin")));
            QVERIFY(key.open(QIODevice::WriteOnly));
            key.write("0123456789abcdef");
            key.close();
            QFile info(QDir(directory).filePath(QStringLiteral("key.info")));
            QVERIFY(info.open(QIODevice::WriteOnly));
            info.write("key.bin\n" + key.fileName().toUtf8() + '\n');
            info.close();
            args << QStringLiteral("-hls_key_info_file") << info.fileName();
        }
        if (mode == QStringLiteral("master-audio"))
            args << QStringLiteral("-an");
        args << playlist;
        QProcess generator;
        generator.start(resolveProcessBinary(QStringLiteral("ffmpeg")), args);
        QVERIFY(generator.waitForFinished(30000));
        QVERIFY2(generator.exitCode() == 0, generator.readAllStandardError().constData());
        if (mode == QStringLiteral("master-audio")) {
            generator.start(resolveProcessBinary(QStringLiteral("ffmpeg")), {
                QStringLiteral("-y"), QStringLiteral("-i"), m_media.filePath(QStringLiteral("audio.ts")),
                QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-hls_time"), QStringLiteral("2"),
                QStringLiteral("-hls_playlist_type"), QStringLiteral("vod"), QDir(directory).filePath(QStringLiteral("audio.m3u8"))});
            QVERIFY(generator.waitForFinished(30000));
            QVERIFY2(generator.exitCode() == 0, generator.readAllStandardError().constData());
        }
        for (const auto &name : QDir(directory).entryList(QDir::Files)) {
            QFile file(QDir(directory).filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            fixture.resources.insert('/' + name.toUtf8(), file.readAll());
        }
        if (mode == QStringLiteral("master-audio")) {
            fixture.resources.insert("/master.m3u8", QByteArray(
                "#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"Main\",DEFAULT=YES,URI=\"audio.m3u8\"\n"
                "#EXT-X-STREAM-INF:BANDWIDTH=1000\nunused.m3u8\n"
                "#EXT-X-STREAM-INF:BANDWIDTH=1000000,AUDIO=\"audio\"\nindex.m3u8\n"));
        }
        fixture.channel.catchupSourceTemplate = QStringLiteral("http://127.0.0.1:%1/%2").arg(fixture.server.serverPort())
            .arg(mode == QStringLiteral("master-audio") ? QStringLiteral("master.m3u8") : QStringLiteral("index.m3u8"));
        fixture.slow = mode == QStringLiteral("pause") || mode == QStringLiteral("cancel-restart");
        fixture.etag = "\"stable\"";
        fixture.ranges = true;
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        if (fixture.slow) {
            QTRY_VERIFY(fixture.requests >= 3);
            controller.pauseAll();
            QTRY_COMPARE(state(controller), QStringLiteral("paused"));
            const auto requests = fixture.requests;
            QTest::qWait(150);
            QCOMPARE(fixture.requests, requests);
            if (mode == QStringLiteral("cancel-restart")) {
                const auto jobId = id(controller);
                controller.cancel(jobId);
                QTRY_VERIFY(!controller.hasPending());
                QCOMPARE(state(controller), QStringLiteral("cancelled"));
                QVERIFY(fixture.temporaryFiles().isEmpty());
                QVERIFY(QDir(fixture.directory.path()).entryList({QStringLiteral("*.hls")}, QDir::Dirs | QDir::Hidden).isEmpty());
                fixture.slow = false;
                controller.restart(jobId);
                QVERIFY(controller.hasPending());
            } else {
                controller.resumeAll();
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasPending(), 15000);
        QVERIFY2(state(controller) == QStringLiteral("completed"),
            qPrintable(controller.data(controller.index(0), CatchupDownloadController::ErrorRole).toString()));
        QVERIFY(QFileInfo(fixture.output().toLocalFile()).size() > 0);
        QVERIFY(fixture.temporaryFiles().isEmpty());
        QVERIFY(QDir(fixture.directory.path()).entryList({QStringLiteral("*.hls")}, QDir::Dirs | QDir::Hidden).isEmpty());
        QProcess probe;
        probe.start(resolveProcessBinary(QStringLiteral("ffprobe")), {QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-show_entries"), QStringLiteral("stream=codec_type"), QStringLiteral("-of"), QStringLiteral("json"),
            fixture.output().toLocalFile()});
        QVERIFY(probe.waitForFinished());
        const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value(QStringLiteral("streams")).toArray();
        QCOMPARE(streams.size(), 2);
        for (const auto &request : fixture.requestsSeen)
            QVERIFY(!request.contains("unused.m3u8"));
    }

    void unsupportedManifest_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<QByteArray>("mime");
        QTest::newRow("hls-live") << QByteArray("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nsegment.ts\n") << QByteArray("application/octet-stream");
        QTest::newRow("hls-file-reference") << QByteArray("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nfile:///etc/passwd\n#EXT-X-ENDLIST\n") << QByteArray("application/octet-stream");
        QTest::newRow("hls-cycle") << QByteArray("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1000\n/archive\n") << QByteArray("application/octet-stream");
        QTest::newRow("hls-body") << QByteArray("#EXTM3U\n#EXT-X-ENDLIST\n") << QByteArray("application/octet-stream");
        QTest::newRow("dash-body") << QByteArray("<?xml version=\"1.0\"?><MPD></MPD>") << QByteArray("application/octet-stream");
        QTest::newRow("hls-mime") << QByteArray("anything") << QByteArray("application/vnd.apple.mpegurl");
    }

    void unsupportedManifest()
    {
        QFETCH(QByteArray, payload);
        QFETCH(QByteArray, mime);
        Fixture fixture;
        fixture.payload = payload;
        fixture.contentType = mime;
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QTRY_COMPARE(state(controller), QStringLiteral("failed"));
        QVERIFY(controller.data(controller.index(0), CatchupDownloadController::ErrorRole).toString().contains(QStringLiteral("HLS")));
        QVERIFY(!QFileInfo::exists(fixture.output().toLocalFile()));
    }

    void finiteResolver()
    {
        Fixture fixture;
        auto profile = fixture.profile;
        profile.xtreamBaseUrl = QStringLiteral("http://example.invalid");
        profile.xtreamUsername = QStringLiteral("user");
        profile.xtreamPassword = QStringLiteral("secret");
        profile.xtreamServerTimezone = QStringLiteral("Europe/Warsaw");
        auto channel = fixture.channel;
        channel.source = ChannelSource::Xtream;
        channel.streamUrl = QStringLiteral("http://example.invalid/live/user/secret/7.ts");
        auto program = fixture.program;
        program.start = QDateTime::fromSecsSinceEpoch((program.start.toSecsSinceEpoch() / 60) * 60 + 37, QTimeZone::UTC);
        program.stop = program.start.addSecs(125);
        const auto result = CatchupUrlResolver(profile).resolveDownload(channel, program);
        QVERIFY(result);
        QCOMPARE(result->trimStartSeconds, 37);
        QCOMPARE(result->durationSeconds, 125);
        QVERIFY(result->url.contains(QStringLiteral("/4/")));
        const auto zone = QTimeZone("Europe/Warsaw");
        QVERIFY(result->url.contains(program.start.toTimeZone(zone).toString(QStringLiteral("yyyy-MM-dd:HH-mm"))));
        program.stop = QDateTime::currentDateTimeUtc().addSecs(60);
        QVERIFY(!CatchupUrlResolver(profile).resolveDownload(channel, program));
        program.stop = QDateTime::currentDateTimeUtc().addSecs(-30);
        profile.catchupSafetyMinutes = 3;
        QVERIFY(!CatchupUrlResolver(profile).resolveDownload(channel, program));
        program.start = QDateTime::currentDateTimeUtc().addDays(-2);
        program.stop = program.start.addSecs(125);
        QVERIFY(!CatchupUrlResolver(profile).resolveDownload(channel, program));
    }

    void download_data()
    {
        QTest::addColumn<bool>("audioOnly");
        QTest::addColumn<int>("expectedSeconds");
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<bool>("success");
        QTest::newRow("video-multiple-audio") << false << 12 << 200 << true;
        QTest::newRow("radio") << true << 12 << 200 << true;
        QTest::newRow("short-response-clean-exit") << false << 40 << 200 << false;
        QTest::newRow("http-error") << false << 12 << 403 << false;
    }

    void download()
    {
        QFETCH(bool, audioOnly);
        QFETCH(int, expectedSeconds);
        QFETCH(int, httpStatus);
        QFETCH(bool, success);
        Fixture fixture;
        fixture.payload = audioOnly ? m_audio : m_video;
        fixture.httpStatus = httpStatus;
        fixture.program.stop = fixture.program.start.addSecs(expectedSeconds);
        fixture.settings.current().playerUserAgent = QStringLiteral("OKILTV-download-test");
        CatchupDownloadController controller(&fixture.settings);
        QSignalSpy notifications(&controller, &CatchupDownloadController::notification);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasPending(), 20000);
        QCOMPARE(state(controller), success ? QStringLiteral("completed") : QStringLiteral("failed"));
        QCOMPARE(QFileInfo::exists(fixture.output().toLocalFile()), success);
        QVERIFY(fixture.temporaryFiles().isEmpty());
        QCOMPARE(notifications.count(), 1);
        QVERIFY(controller.unread());
        controller.markRead();
        QVERIFY(!controller.unread());
        QVERIFY(fixture.receivedHeaders.contains("User-Agent: OKILTV-download-test"));
        QCOMPARE(fixture.requests, 1);
        if (success) {
            QCOMPARE(controller.activeProgress(), 0.0);
            QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::ProgressRole).toDouble(), 100.0);
            QProcess probe;
            probe.start(resolveProcessBinary(QStringLiteral("ffprobe")),
                {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                 QStringLiteral("stream=codec_type"), QStringLiteral("-of"), QStringLiteral("json"), fixture.output().toLocalFile()});
            QVERIFY(probe.waitForFinished(10000));
            const auto streams = QJsonDocument::fromJson(probe.readAllStandardOutput()).object().value(QStringLiteral("streams")).toArray();
            QCOMPARE(streams.size(), audioOnly ? 1 : 3);
        }
        const auto actualPath = controller.data(controller.index(0), CatchupDownloadController::PathRole).toString();
        if (!success) {
            QVERIFY(actualPath.endsWith(httpStatus == 200 ? QStringLiteral(".partial.mkv") : QStringLiteral(".partial.download")));
            QVERIFY(QFileInfo::exists(actualPath));
            if (httpStatus == 200) {
                QVERIFY(QFileInfo(actualPath).size() > 0);
                QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::BytesRole).toLongLong(), QFileInfo(actualPath).size());
            }
        }
        controller.dismiss(id(controller));
        QCOMPARE(controller.rowCount(), 0);
        QVERIFY(QFileInfo::exists(actualPath));
    }

    void queueAndSourceSwitch()
    {
        Fixture fixture;
        fixture.payload = m_video;
        fixture.delayMs = 150;
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output(QStringLiteral("second.mkv"))), QString());
        QCOMPARE(controller.queuedCount(), 2);
        QTRY_COMPARE(fixture.requests, 1);
        QCOMPARE(state(controller, 1), QStringLiteral("queued"));
        auto secondProfile = fixture.profile;
        secondProfile.id = QUuid::createUuid();
        QVERIFY(fixture.settings.addProfile(secondProfile));
        fixture.settings.setActiveProfileId(secondProfile.id);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasPending(), 20000);
        QCOMPARE(state(controller, 0), QStringLiteral("completed"));
        QCOMPARE(state(controller, 1), QStringLiteral("completed"));
        QCOMPARE(fixture.requests, 2);
    }

    void cancellationAndShutdown()
    {
        Fixture fixture;
        fixture.stall = true;
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output(QStringLiteral("queued.mkv"))), QString());
        const auto firstId = id(controller);
        QSignalSpy removed(&controller, &QAbstractItemModel::rowsRemoved);
        controller.cancel(id(controller, 1));
        QCOMPARE(controller.rowCount(), 2);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(state(controller, 1), QStringLiteral("cancelled"));
        QCOMPARE(controller.queuedCount(), 1);
        QCOMPARE(id(controller), firstId);
        QTRY_COMPARE(fixture.requests, 1);
        QVERIFY(!fixture.temporaryFiles().isEmpty());
        QSignalSpy stopped(&controller, &CatchupDownloadController::shutdownFinished);
        controller.shutdown();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 10000);
        QVERIFY(!controller.hasPending());
        QCOMPARE(state(controller), QStringLiteral("cancelled"));
        QVERIFY(fixture.temporaryFiles().isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.output().toLocalFile()));
        QCOMPARE(fixture.requests, 1);
        QVERIFY(!controller.enqueue(fixture.channel, fixture.program, fixture.output()).isEmpty());
    }

    void cancellationDeletesAlreadyReceivedData()
    {
        Fixture fixture;
        fixture.payload = m_video;
        fixture.stallAfterPayload = true;
        fixture.program.stop = fixture.program.start.addSecs(40);
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QTRY_VERIFY_WITH_TIMEOUT(controller.data(controller.index(0), CatchupDownloadController::BytesRole).toLongLong() > 0, 10000);
        controller.cancel(id(controller));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasPending(), 10000);
        QCOMPARE(state(controller), QStringLiteral("cancelled"));
        QVERIFY(fixture.temporaryFiles().isEmpty());
        QVERIFY(!QFileInfo::exists(fixture.output().toLocalFile()));
        QVERIFY(QDir(fixture.directory.path()).entryList({QStringLiteral("*.mkv")}, QDir::Files).isEmpty());
    }

    void stalledSourceTimesOutWithoutBlocking()
    {
        Fixture fixture;
        fixture.payload = m_video;
        fixture.stallAfterPayload = true;
        fixture.program.stop = fixture.program.start.addSecs(40);
        CatchupDownloadController controller(&fixture.settings);
        int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&ticks] { ++ticks; });
        heartbeat.start(100);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QTRY_COMPARE(fixture.requests, 1);
        QTRY_VERIFY_WITH_TIMEOUT(controller.paused(), 70000);
        QCOMPARE(state(controller), QStringLiteral("paused"));
        QVERIFY(ticks > 20);
        QVERIFY(controller.hasPending());
        QVERIFY(!fixture.temporaryFiles().isEmpty());
        QVERIFY(controller.data(controller.index(0), CatchupDownloadController::BytesRole).toLongLong() > 0);
        controller.cancel(id(controller));
        QTRY_VERIFY(!controller.hasPending());
        QVERIFY(fixture.temporaryFiles().isEmpty());
    }

    void failedDownloadsSurviveShutdownAndDoNotOverwrite()
    {
        Fixture fixture;
        fixture.payload = m_video;
        fixture.program.stop = fixture.program.start.addSecs(40);
        const auto firstPartial = fixture.directory.filePath(QStringLiteral("programme.partial.mkv"));
        QFile existing(firstPartial);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("previous partial file");
        existing.close();
        QString retainedPath;
        qint64 retainedBytes = 0;
        {
            CatchupDownloadController controller(&fixture.settings);
            QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
            QTRY_VERIFY_WITH_TIMEOUT(!controller.hasPending(), 15000);
            QCOMPARE(state(controller), QStringLiteral("failed"));
            retainedPath = controller.data(controller.index(0), CatchupDownloadController::PathRole).toString();
            QCOMPARE(retainedPath, fixture.directory.filePath(QStringLiteral("programme.partial (2).mkv")));
            retainedBytes = QFileInfo(retainedPath).size();
            QVERIFY(retainedBytes > 0);
            controller.shutdown();
        }
        QCOMPARE(QFileInfo(retainedPath).size(), retainedBytes);
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("previous partial file"));
        QVERIFY(fixture.temporaryFiles().isEmpty());
    }

    void restartFailedDownload_data()
    {
        QTest::addColumn<int>("httpStatus");
        QTest::newRow("http-error") << 403;
        QTest::newRow("invalid-media") << 200;
    }

    void restartFailedDownload()
    {
        QFETCH(int, httpStatus);
        Fixture fixture;
        fixture.httpStatus = httpStatus;
        fixture.payload = QByteArray("invalid media");
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasPending(), 15000);
        QCOMPARE(state(controller), QStringLiteral("failed"));
        const auto failedId = id(controller);
        const auto retainedPath = controller.data(controller.index(0), CatchupDownloadController::PathRole).toString();
        QVERIFY(retainedPath != fixture.output().toLocalFile());
        QFile retained(retainedPath);
        QVERIFY(retained.open(QIODevice::ReadOnly));
        const auto retainedData = retained.readAll();
        retained.close();

        QFile collision(fixture.output().toLocalFile());
        QVERIFY(collision.open(QIODevice::WriteOnly));
        collision.write("existing file");
        collision.close();
        QSignalSpy notices(&controller, &CatchupDownloadController::notification);
        controller.restart(failedId);
        QCOMPARE(notices.count(), 1);
        QCOMPARE(id(controller), failedId);
        QCOMPARE(state(controller), QStringLiteral("failed"));
        QVERIFY(collision.open(QIODevice::ReadOnly));
        QCOMPARE(collision.readAll(), QByteArray("existing file"));
        collision.close();
        QVERIFY(collision.remove());

        fixture.httpStatus = 200;
        fixture.payload = m_video;
        controller.restart(failedId);
        QCOMPARE(controller.rowCount(), 1);
        QCOMPARE(state(controller), QStringLiteral("queued"));
        QVERIFY(id(controller) != failedId);
        QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::ProgressRole).toDouble(), 0.0);
        QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::BytesRole).toLongLong(), 0);
        controller.restart(failedId);
        QCOMPARE(controller.rowCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasPending(), 15000);
        QCOMPARE(state(controller), QStringLiteral("completed"));
        QCOMPARE(fixture.requests, 2);
        QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::PathRole).toString(), fixture.output().toLocalFile());
        QVERIFY(QFileInfo(fixture.output().toLocalFile()).size() > 0);
        QVERIFY(retained.open(QIODevice::ReadOnly));
        QCOMPARE(retained.readAll(), retainedData);
    }

    void restartCancelledQueuedDownload()
    {
        Fixture fixture;
        fixture.payload = m_video;
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        const auto queuedId = id(controller);
        controller.cancel(queuedId);
        QCOMPARE(controller.rowCount(), 1);
        QCOMPARE(state(controller), QStringLiteral("cancelled"));
        QVERIFY(!controller.hasPending());
        QCOMPARE(fixture.requests, 0);
        controller.restart(queuedId);
        QCOMPARE(controller.rowCount(), 1);
        QCOMPARE(state(controller), QStringLiteral("queued"));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasPending(), 15000);
        QCOMPARE(state(controller), QStringLiteral("completed"));
        QCOMPARE(fixture.requests, 1);
        QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::PathRole).toString(), fixture.output().toLocalFile());
        QVERIFY(QFileInfo(fixture.output().toLocalFile()).size() > 0);
    }

    void restartCancelledDownload()
    {
        Fixture fixture;
        fixture.stall = true;
        CatchupDownloadController controller(&fixture.settings);
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        const auto cancelledId = id(controller);
        controller.restart(cancelledId); // Pending jobs cannot be duplicated.
        QCOMPARE(controller.rowCount(), 1);
        QTRY_COMPARE(fixture.requests, 1);
        controller.cancel(cancelledId);
        QTRY_COMPARE(state(controller), QStringLiteral("cancelled"));
        QVERIFY(fixture.temporaryFiles().isEmpty());

        QFile collision(fixture.output().toLocalFile());
        QVERIFY(collision.open(QIODevice::WriteOnly));
        collision.write("existing file");
        collision.close();
        QSignalSpy notices(&controller, &CatchupDownloadController::notification);
        controller.restart(cancelledId);
        QCOMPARE(notices.count(), 1);
        QCOMPARE(state(controller), QStringLiteral("cancelled"));
        QCOMPARE(id(controller), cancelledId);
        QVERIFY(collision.open(QIODevice::ReadOnly));
        QCOMPARE(collision.readAll(), QByteArray("existing file"));
        collision.close();
        QVERIFY(collision.remove());

        const auto originalRequest = fixture.receivedHeaders.split('\n').first();
        fixture.stall = false;
        fixture.payload = m_video;
        controller.restart(cancelledId);
        QCOMPARE(controller.rowCount(), 1);
        QCOMPARE(state(controller), QStringLiteral("queued"));
        QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::ProgressRole).toDouble(), 0.0);
        QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::BytesRole).toLongLong(), 0);
        controller.restart(cancelledId); // A stale/double click is harmless.
        QCOMPARE(controller.rowCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.hasPending(), 15000);
        QCOMPARE(state(controller), QStringLiteral("completed"));
        QCOMPARE(fixture.requests, 2);
        QCOMPARE(fixture.receivedHeaders.split('\n').first(), originalRequest);
        QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::PathRole).toString(), fixture.output().toLocalFile());
        QVERIFY(QFileInfo(fixture.output().toLocalFile()).size() > 0);
        QVERIFY(fixture.temporaryFiles().isEmpty());
    }

    void destinationFormatAndPersistence()
    {
        Fixture fixture;
        fixture.settings.current().dateOrder = QStringLiteral("dmy");
        fixture.settings.current().timeFormat = QStringLiteral("24h");
        const QDateTime start(QDate(2026, 9, 22), QTime(13, 45));
        {
            CatchupDownloadController controller(&fixture.settings);
            QCOMPARE(QFileInfo(controller.suggestedDestination(QStringLiteral("News"), QStringLiteral("TV"), start).toLocalFile()).fileName(),
                     QStringLiteral("TV - 22-09-2026 13_45 - News.mkv"));
            fixture.settings.current().dateOrder = QStringLiteral("mdy");
            fixture.settings.current().timeFormat = QStringLiteral("12h");
            QCOMPARE(QFileInfo(controller.suggestedDestination(QStringLiteral("News"), QStringLiteral("TV"), start).toLocalFile()).fileName(),
                     QStringLiteral("TV - 09-22-2026 1_45 PM - News.mkv"));
            QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
            QCOMPARE(controller.data(controller.index(0), CatchupDownloadController::ProgramStartRole).toDateTime(), fixture.program.start);
            controller.cancel(id(controller));
        }
        SettingsManager restored(fixture.settings.settingsFilePath());
        restored.load();
        QCOMPARE(restored.current().catchupDownloadDirectory, fixture.directory.path());
        CatchupDownloadController controller(&restored);
        QCOMPARE(QFileInfo(controller.suggestedDestination(QStringLiteral("Other")).toLocalFile()).absolutePath(), fixture.directory.path());
    }

    void collisionsAndRemovedSource()
    {
        Fixture fixture;
        CatchupDownloadController controller(&fixture.settings);
        fixture.settings.current().recordingsDirectory = fixture.directory.path();
        QCOMPARE(controller.enqueue(fixture.channel, fixture.program, fixture.output()), QString());
        QVERIFY(!controller.enqueue(fixture.channel, fixture.program, fixture.output()).isEmpty());
        QVERIFY(controller.suggestedDestination(QStringLiteral("programme")).toLocalFile().endsWith(QStringLiteral("programme (2).mkv")));
        QVERIFY(!controller.enqueue(fixture.channel, fixture.program, fixture.output(QStringLiteral("wrong.ts"))).isEmpty());
        QVERIFY(!controller.enqueue(fixture.channel, fixture.program, QUrl(QStringLiteral("https://example.invalid/file.mkv"))).isEmpty());
        QVERIFY(fixture.settings.removeProfile(fixture.profile.id));
        QTRY_VERIFY(!controller.hasPending());
        QCOMPARE(state(controller), QStringLiteral("failed"));
        QCOMPARE(fixture.requests, 0);
        QFile existing(fixture.output().toLocalFile());
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("existing file");
        existing.close();
        QVERIFY(!controller.enqueue(fixture.channel, fixture.program, fixture.output()).isEmpty());
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("existing file"));
        QCOMPARE(CatchupDownloadController::safeFileName(QStringLiteral("CON")), QStringLiteral("_CON"));
        QCOMPARE(CatchupDownloadController::safeFileName(QStringLiteral(" A/B: Żółć. ")), QStringLiteral("A_B_ Żółć"));
    }
};

QTEST_MAIN(CatchupDownloadTests)
#include "tst_catchup_downloads.moc"
