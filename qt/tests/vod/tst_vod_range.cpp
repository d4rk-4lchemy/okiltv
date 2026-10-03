#include "player/vodrangestream.h"
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtConcurrent>
#include <QtTest>
#include <optional>

using OKILTV::Player::VodRangeStream;
namespace {
class Server : public QTcpServer {
public:
    QString mode;
    int requests = 0;
    int active = 0;
    int maximumActive = 0;
    QByteArray data = QByteArray(20 * 1024 * 1024 + 123, 'v');
    Server()
    {
        data.replace(18 * 1024 * 1024, 256 * 1024, QByteArray(256 * 1024, 'a'));
        connect(this, &QTcpServer::newConnection, this, [this]() {
            auto *socket = nextPendingConnection();
            ++active;
            maximumActive = std::max(maximumActive, active);
            connect(socket, &QTcpSocket::disconnected, this, [this]() { --active; });
            auto request = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request]() {
                request->append(socket->readAll());
                if (!request->contains("\r\n\r\n")) return;
                ++requests;
                if (mode == QStringLiteral("stall")) return;
                if (mode == QStringLiteral("redirect") && request->startsWith("GET /movie.mp4 ")) {
                    socket->write("HTTP/1.1 301 Moved Permanently\r\nLocation: /media.mp4\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                    return;
                }
                const auto match = QRegularExpression(QStringLiteral("Range: bytes=([0-9]+)-([0-9]*)"),
                    QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(*request));
                const auto start = match.captured(1).toLongLong();
                const auto end = match.captured(2).isEmpty() ? qint64(data.size() - 1) : std::min(match.captured(2).toLongLong(), qint64(data.size() - 1));
                QByteArray body = data.mid(start, end - start + 1);
                QByteArray headers = mode == QStringLiteral("ignore") ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 206 Partial Content\r\n";
                headers += "Content-Range: bytes " + QByteArray::number(start + (mode == QStringLiteral("wrong-range") ? 1 : 0))
                    + '-' + QByteArray::number(end) + '/' + QByteArray::number(data.size()) + "\r\n";
                headers += mode == QStringLiteral("changed") ? "ETag: \"v2\"\r\n" : "ETag: \"v1\"\r\n";
                if (mode == QStringLiteral("oversized")) body.append('x');
                headers += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
                if (mode == QStringLiteral("truncated")) body.chop(1);
                socket->write(headers);
                auto remaining = std::make_shared<QByteArray>(std::move(body));
                auto position = std::make_shared<qint64>(0);
                auto *pump = new QTimer(socket);
                connect(pump, &QTimer::timeout, socket, [socket, remaining, position, pump]() {
                    if (socket->state() != QAbstractSocket::ConnectedState) { pump->stop(); return; }
                    if (socket->bytesToWrite() > 64 * 1024) return;
                    const auto count = std::min(qint64(64 * 1024), remaining->size() - *position);
                    socket->write(remaining->constData() + *position, count);
                    *position += count;
                    if (*position == remaining->size()) { pump->stop(); socket->disconnectFromHost(); }
                });
                pump->start(1);
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        listen(QHostAddress::LocalHost);
    }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/movie.mp4").arg(serverPort())); }
};
}
class VodRangeTests : public QObject {
    Q_OBJECT
private slots:
    void followsRedirectBeforeValidatingRange()
    {
        Server server; server.mode = QStringLiteral("redirect");
        auto stream = VodRangeStream::create(server.url(), "test");
        std::optional<bool> ready;
        stream->prepare([&](bool value) { ready = value; });
        QTRY_VERIFY(ready.has_value()); QVERIFY(*ready);
        QCOMPARE(stream->size(), server.data.size());
        QCOMPARE(server.requests, 2);
        auto read = QtConcurrent::run([stream]() {
            char byte = 0;
            stream->seek(1024 * 1024);
            return stream->read(&byte, 1);
        });
        QTRY_VERIFY(read.isFinished()); QCOMPARE(read.result(), 1);
        QCOMPARE(server.requests, 3); // Reuse the final media URL, without redirecting again.
    }
    void sequentialReadsKeepOneResponseAndIdleCancellationClosesIt()
    {
        Server server;
        auto stream = VodRangeStream::create(server.url(), "test");
        std::optional<bool> ready;
        stream->prepare([&](bool value) { ready = value; });
        QTRY_VERIFY(ready.has_value()); QVERIFY(*ready);
        auto read = QtConcurrent::run([stream]() {
            QByteArray bytes(64 * 1024, 0);
            qint64 total = 0;
            while (total < 17 * 1024 * 1024) {
                const auto count = stream->read(bytes.data(), bytes.size());
                if (count <= 0 || bytes.first(count) != QByteArray(count, 'v')) return false;
                total += count;
            }
            return true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(read.isFinished(), 15000); QVERIFY(read.result());
        QCOMPARE(server.requests, 1);
        QTest::qWait(200); // Paused consumer keeps the same upstream response.
        QCOMPARE(server.active, 1);
        QCOMPARE(server.maximumActive, 1);
        stream->cancel();
        QTRY_COMPARE_WITH_TIMEOUT(server.active, 0, 1000);
    }
    void truncatedResponseFailsWhenReaderReachesMissingBytes()
    {
        Server server; server.mode = QStringLiteral("truncated");
        server.data.resize(1024 * 1024);
        auto stream = VodRangeStream::create(server.url(), "test");
        std::optional<bool> ready;
        stream->prepare([&](bool value) { ready = value; });
        QTRY_VERIFY(ready.has_value()); QVERIFY(*ready);
        auto read = QtConcurrent::run([stream]() {
            QByteArray bytes(256 * 1024, 0);
            qint64 count = 0;
            do { count = stream->read(bytes.data(), bytes.size()); } while (count > 0);
            return count;
        });
        QTRY_VERIFY(read.isFinished()); QCOMPARE(read.result(), -1);
        QVERIFY(stream->failed());
        QCOMPARE(server.requests, 1);
    }
    void distantTracksReuseBytesAndCacheIsBounded()
    {
        Server server;
        auto stream = VodRangeStream::create(server.url(), "test");
        std::optional<bool> ready;
        stream->prepare([&](bool value) { ready = value; });
        QTRY_VERIFY(ready.has_value()); QVERIFY(*ready);
        QCOMPARE(stream->size(), server.data.size());
        auto reads = QtConcurrent::run([stream]() {
            QByteArray result;
            for (int i = 0; i < 100; ++i) {
                for (const auto base : {qint64(0), qint64(18 * 1024 * 1024)}) {
                    if (stream->seek(base + i) < 0) return QByteArray{};
                    char byte = 0;
                    if (stream->read(&byte, 1) != 1) return QByteArray{};
                    result.append(byte);
                }
            }
            return result;
        });
        QTRY_VERIFY(reads.isFinished());
        QCOMPARE(reads.result(), QByteArray("va").repeated(100));
        QCOMPARE(server.requests, 2); // 200 alternating reads, only two ranges.
        auto eviction = QtConcurrent::run([stream]() {
            char byte = 0;
            for (int i = 1; i <= 65; ++i) {
                if (stream->seek(qint64(i) * 256 * 1024) < 0 || stream->read(&byte, 1) != 1) return false;
            }
            return stream->seek(0) == 0 && stream->read(&byte, 1) == 1;
        });
        QTRY_VERIFY_WITH_TIMEOUT(eviction.isFinished(), 15000);
        QVERIFY(eviction.result());
        QCOMPARE(server.requests, 4); // Evicted beginning is fetched again.
        auto tail = QtConcurrent::run([stream]() {
            char bytes[10]{};
            stream->seek(stream->size() - 1);
            return stream->read(bytes, 10);
        });
        QTRY_VERIFY(tail.isFinished()); QCOMPARE(tail.result(), 1);
        QCOMPARE(stream->seek(stream->size()), stream->size());
        char byte = 0;
        QCOMPARE(stream->read(&byte, 1), 0);
        QCOMPARE(stream->seek(-1), -1);
        QCOMPARE(stream->seek(stream->size() + 1), -1);
        QCOMPARE(server.maximumActive, 1);
    }
    void unsupportedOrInvalidResponse_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode : {"ignore", "wrong-range", "truncated", "oversized"}) QTest::newRow(mode) << QString::fromLatin1(mode);
    }
    void unsupportedOrInvalidResponse()
    {
        QFETCH(QString, mode);
        Server server; server.mode = mode;
        server.data.resize(256 * 1024);
        auto stream = VodRangeStream::create(server.url(), "test");
        std::optional<bool> ready;
        stream->prepare([&](bool value) { ready = value; });
        QTRY_VERIFY(ready.has_value()); QVERIFY(!*ready);
    }
    void changedResourceCannotMixCachedBytes()
    {
        Server server;
        auto stream = VodRangeStream::create(server.url(), "test");
        std::optional<bool> ready;
        stream->prepare([&](bool value) { ready = value; });
        QTRY_VERIFY(ready.has_value()); QVERIFY(*ready);
        server.mode = QStringLiteral("changed");
        auto read = QtConcurrent::run([stream]() { char byte = 0; stream->seek(1024 * 1024); return stream->read(&byte, 1); });
        QTRY_VERIFY(read.isFinished()); QCOMPARE(read.result(), -1);
    }
    void cancellationInterruptsPreparationAndRead_data()
    {
        QTest::addColumn<bool>("duringRead");
        QTest::newRow("prepare") << false;
        QTest::newRow("read") << true;
    }
    void cancellationInterruptsPreparationAndRead()
    {
        QFETCH(bool, duringRead);
        Server server;
        if (!duringRead) server.mode = QStringLiteral("stall");
        auto stream = VodRangeStream::create(server.url(), "test");
        std::optional<bool> ready;
        stream->prepare([&](bool value) { ready = value; });
        QFuture<qint64> read;
        if (duringRead) {
            QTRY_VERIFY(ready.has_value()); QVERIFY(*ready);
            server.mode = QStringLiteral("stall");
            read = QtConcurrent::run([stream]() { char byte = 0; stream->seek(1024 * 1024); return stream->read(&byte, 1); });
        }
        QTRY_COMPARE(server.requests, duringRead ? 2 : 1);
        QElapsedTimer timer; timer.start();
        stream->cancel();
        if (duringRead) { QTRY_VERIFY(read.isFinished()); QCOMPARE(read.result(), -1); }
        else { QTRY_VERIFY(ready.has_value()); QVERIFY(!*ready); }
        stream.reset(); // Joins worker and closes its socket.
        QVERIFY(timer.elapsed() < 1000);
    }
};
QTEST_GUILESS_MAIN(VodRangeTests)
#include "tst_vod_range.moc"
