#include "uitestbridge.h"

#include "appcontroller.h"
#include "channellistmodel.h"
#include "guidestatemodel.h"
#include "nownextmodel.h"
#include "playercontroller.h"
#include "settingscontroller.h"
#include "shellcontroller.h"
#include "timeshiftcontroller.h"
#include "uitestcapturecontroller.h"

#include "../core/redaction.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTcpSocket>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <QUrlQuery>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace OKILTV::App {

namespace {

constexpr int kMaxBufferedEvents = 6000;
constexpr int kMaxNetworkMapEntries = 400;
constexpr int kStatePollIntervalMs = 300;

UiTestBridge::RegionRecord emptyRegion(const QString &name)
{
    UiTestBridge::RegionRecord region;
    region.name = name;
    return region;
}

QString jsonStatusText(const int statusCode)
{
    switch (statusCode) {
    case 200:
        return QStringLiteral("OK");
    case 202:
        return QStringLiteral("Accepted");
    case 400:
        return QStringLiteral("Bad Request");
    case 401:
        return QStringLiteral("Unauthorized");
    case 404:
        return QStringLiteral("Not Found");
    case 405:
        return QStringLiteral("Method Not Allowed");
    case 500:
        return QStringLiteral("Internal Server Error");
    case 503:
        return QStringLiteral("Service Unavailable");
    default:
        return QStringLiteral("Error");
    }
}

QJsonObject jsonBoundsForRegion(const UiTestBridge::RegionRecord &region)
{
    return {
        { QStringLiteral("x"), region.x },
        { QStringLiteral("y"), region.y },
        { QStringLiteral("width"), region.width },
        { QStringLiteral("height"), region.height }
    };
}

QJsonObject makeElement(
    const QString &id,
    const QString &role,
    const QString &region,
    const bool visible,
    const bool enabled,
    const bool focused,
    const bool checked,
    const QString &text,
    const QJsonValue &value,
    const QJsonObject &bounds)
{
    return {
        { QStringLiteral("id"), id },
        { QStringLiteral("role"), role },
        { QStringLiteral("region"), region },
        { QStringLiteral("visible"), visible },
        { QStringLiteral("enabled"), enabled },
        { QStringLiteral("focused"), focused },
        { QStringLiteral("checked"), checked },
        { QStringLiteral("text"), text },
        { QStringLiteral("value"), value },
        { QStringLiteral("bounds"), bounds }
    };
}

} // namespace

UiTestBridge::UiTestBridge(QObject *parent)
    : QObject(parent)
{
    m_enabled = qEnvironmentVariable("OKILTV_UI_TEST").trimmed() == QStringLiteral("1");
    m_token = qEnvironmentVariable("OKILTV_UI_TEST_TOKEN").trimmed();
    m_requestedPort = static_cast<quint16>(
        std::clamp(qEnvironmentVariableIntValue("OKILTV_UI_TEST_PORT"), 0, 65535));

    if (m_enabled && m_token.isEmpty()) {
        Core::DebugLogger::instance().log(
            QStringLiteral("ui-test"),
            QStringLiteral("UI test bridge disabled: missing OKILTV_UI_TEST_TOKEN."));
        m_enabled = false;
    }

    if (m_enabled) {
        m_runDirectory = qEnvironmentVariable("OKILTV_UI_TEST_RUN_DIR").trimmed();
        if (m_runDirectory.isEmpty()) {
            m_runDirectory = QDir::temp().filePath(
                QStringLiteral("okiltv-ui-manual-%1")
                    .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"))));
        }
        ensureRunDirectoryLayout();
        startServerIfEnabled();
        installLogSubscription();
        installNetworkSubscription();
        setupStatePolling();
        qApp->installEventFilter(this);
    }
}

UiTestBridge::~UiTestBridge()
{
    stopSubscriptions();
    for (const auto &client : m_sseClients) {
        if (client != nullptr) {
            client->disconnectFromHost();
        }
    }
    m_sseClients.clear();
}

bool UiTestBridge::enabled() const
{
    return m_enabled;
}

quint16 UiTestBridge::port() const
{
    return m_boundPort;
}

QString UiTestBridge::runDirectory() const
{
    return m_runDirectory;
}

void UiTestBridge::setWindow(QObject *windowObject)
{
    m_rootObject = windowObject;
    if (auto *quickWindow = qobject_cast<QQuickWindow *>(windowObject)) {
        m_window = quickWindow;
    } else if (auto *quickItem = qobject_cast<QQuickItem *>(windowObject)) {
        m_window = quickItem->window();
    }

    if (m_enabled) {
        appendEvent(
            QStringLiteral("state.changed"),
            QJsonObject { { QStringLiteral("snapshot"), buildStateSnapshot() } });
    }
}

void UiTestBridge::attachControllers(
    AppController *appController,
    ShellController *shellController,
    ChannelListModel *channelListModel,
    GuideStateModel *guideStateModel,
    NowNextModel *playbackNowNextModel,
    PlayerController *playerController,
    TimeshiftController *timeshiftController,
    SettingsController *settingsController,
    UiTestCaptureController *captureController)
{
    m_appController = appController;
    m_shellController = shellController;
    m_channelListModel = channelListModel;
    m_guideStateModel = guideStateModel;
    m_playbackNowNextModel = playbackNowNextModel;
    m_playerController = playerController;
    m_timeshiftController = timeshiftController;
    m_settingsController = settingsController;
    m_captureController = captureController;

    if (!m_enabled) {
        return;
    }

    if (m_playerController != nullptr) {
        connect(m_playerController, &PlayerController::currentPlaybackUrlChanged, this, &UiTestBridge::observePlaybackUrl);
    }
    if (m_timeshiftController != nullptr) {
        connect(
            m_timeshiftController,
            &TimeshiftController::uiTestPlaybackUrlObserved,
            this,
            &UiTestBridge::observeTimeshiftPlaybackUrl);
        connect(
            m_timeshiftController,
            &TimeshiftController::uiTestLocalRequestObserved,
            this,
            &UiTestBridge::observeTimeshiftLocalRequest);
    }
    if (m_captureController != nullptr) {
        connect(m_captureController, &UiTestCaptureController::captureFinished, this, &UiTestBridge::handleCaptureCompletion);
    }
}

bool UiTestBridge::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_enabled || event == nullptr) {
        return QObject::eventFilter(watched, event);
    }

    const auto objectName = watched != nullptr ? watched->objectName() : QString {};
    const auto className = watched != nullptr ? QString::fromUtf8(watched->metaObject()->className()) : QString {};
    const auto basePayload = QJsonObject {
        { QStringLiteral("objectName"), objectName },
        { QStringLiteral("className"), className }
    };

    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        auto payload = basePayload;
        payload.insert(QStringLiteral("key"), keyEvent->key());
        payload.insert(QStringLiteral("text"), keyEvent->text());
        payload.insert(QStringLiteral("modifiers"), static_cast<int>(keyEvent->modifiers()));
        payload.insert(QStringLiteral("autoRepeat"), keyEvent->isAutoRepeat());
        payload.insert(QStringLiteral("pressed"), event->type() == QEvent::KeyPress);
        appendEvent(QStringLiteral("input.key"), payload);
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove: {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        auto payload = basePayload;
        payload.insert(QStringLiteral("eventType"), static_cast<int>(event->type()));
        payload.insert(QStringLiteral("button"), static_cast<int>(mouseEvent->button()));
        payload.insert(QStringLiteral("buttons"), static_cast<int>(mouseEvent->buttons()));
        payload.insert(QStringLiteral("x"), mouseEvent->position().x());
        payload.insert(QStringLiteral("y"), mouseEvent->position().y());
        payload.insert(QStringLiteral("globalX"), mouseEvent->globalPosition().x());
        payload.insert(QStringLiteral("globalY"), mouseEvent->globalPosition().y());
        appendEvent(QStringLiteral("input.mouse"), payload);
        break;
    }
    case QEvent::Wheel: {
        const auto *wheelEvent = static_cast<QWheelEvent *>(event);
        auto payload = basePayload;
        payload.insert(QStringLiteral("x"), wheelEvent->position().x());
        payload.insert(QStringLiteral("y"), wheelEvent->position().y());
        payload.insert(QStringLiteral("deltaX"), wheelEvent->angleDelta().x());
        payload.insert(QStringLiteral("deltaY"), wheelEvent->angleDelta().y());
        appendEvent(QStringLiteral("input.mouse"), payload);
        break;
    }
    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}

void UiTestBridge::startServerIfEnabled()
{
    if (!m_enabled) {
        return;
    }

    connect(&m_server, &QTcpServer::newConnection, this, &UiTestBridge::handleServerConnection);
    if (!m_server.listen(QHostAddress::LocalHost, m_requestedPort)) {
        Core::DebugLogger::instance().log(
            QStringLiteral("ui-test"),
            QStringLiteral("UI test bridge failed to bind localhost: %1").arg(m_server.errorString()));
        m_enabled = false;
        return;
    }

    m_boundPort = m_server.serverPort();
    Core::DebugLogger::instance().log(
        QStringLiteral("ui-test"),
        QStringLiteral("UI test bridge listening on 127.0.0.1:%1 (run_dir=%2)")
            .arg(m_boundPort)
            .arg(m_runDirectory));
}

void UiTestBridge::ensureRunDirectoryLayout()
{
    if (!m_enabled) {
        return;
    }

    QDir().mkpath(m_runDirectory);
    m_eventsFilePath = QDir(m_runDirectory).filePath(QStringLiteral("events.ndjson"));
    m_networkFilePath = QDir(m_runDirectory).filePath(QStringLiteral("network.ndjson"));
    m_stateSnapshotDirectory = QDir(m_runDirectory).filePath(QStringLiteral("state-snapshots"));
    m_screenshotDirectory = QDir(m_runDirectory).filePath(QStringLiteral("screenshots"));
    QDir().mkpath(m_stateSnapshotDirectory);
    QDir().mkpath(m_screenshotDirectory);
}

void UiTestBridge::installLogSubscription()
{
    if (!m_enabled) {
        return;
    }

    m_logSubscriptionId = Core::DebugLogger::instance().subscribe([this](const Core::DebugLogger::Entry &entry) {
        auto payload = QJsonObject {
            { QStringLiteral("cursor"), static_cast<qint64>(entry.cursor) },
            { QStringLiteral("timestamp"), entry.timestampUtc },
            { QStringLiteral("category"), entry.category },
            { QStringLiteral("message"), Core::redactSensitiveText(entry.message) },
            { QStringLiteral("line"), Core::redactSensitiveText(entry.line) }
        };
        appendEventThreadSafe(QStringLiteral("log"), payload);
    });
}

void UiTestBridge::installNetworkSubscription()
{
    if (!m_enabled) {
        return;
    }

    m_networkObserverId = Core::addNetworkObserver([this](const Core::NetworkObservation &observation) {
        auto payload = QJsonObject {
            { QStringLiteral("sequence"), static_cast<qint64>(observation.sequence) },
            { QStringLiteral("phase"), observation.phase },
            { QStringLiteral("method"), observation.method },
            { QStringLiteral("category"), observation.category },
            { QStringLiteral("url"), observation.redactedUrl },
            { QStringLiteral("statusCode"), observation.statusCode },
            { QStringLiteral("durationMs"), static_cast<qint64>(observation.durationMs) },
            { QStringLiteral("payloadBytes"), static_cast<qint64>(observation.payloadBytes) },
            { QStringLiteral("timedOut"), observation.timedOut },
            { QStringLiteral("errorText"), Core::redactSensitiveText(observation.errorText) }
        };
        appendEventThreadSafe(
            observation.phase == QStringLiteral("request")
                ? QStringLiteral("network.request")
                : QStringLiteral("network.reply"),
            payload);
    });
}

void UiTestBridge::stopSubscriptions()
{
    if (m_logSubscriptionId != 0) {
        Core::DebugLogger::instance().unsubscribe(m_logSubscriptionId);
        m_logSubscriptionId = 0;
    }
    if (m_networkObserverId != 0) {
        Core::removeNetworkObserver(m_networkObserverId);
        m_networkObserverId = 0;
    }
}

void UiTestBridge::setupStatePolling()
{
    if (!m_enabled) {
        return;
    }

    m_statePollTimer.setInterval(kStatePollIntervalMs);
    connect(&m_statePollTimer, &QTimer::timeout, this, &UiTestBridge::handleStatePoll);
    m_statePollTimer.start();
}

void UiTestBridge::appendEvent(const QString &type, const QJsonObject &payload)
{
    if (!m_enabled) {
        return;
    }

    EventRecord record;
    record.id = m_nextEventId++;
    record.type = type;
    QJsonObject redactedPayload = redactedJsonValue(payload).toObject();
    if (type == QStringLiteral("state.changed")) {
        const auto snapshot = redactedPayload.value(QStringLiteral("snapshot")).toObject();
        if (!snapshot.isEmpty()) {
            writeStateSnapshotArtifact(snapshot, record.id);
            redactedPayload.remove(QStringLiteral("snapshot"));
            redactedPayload.insert(QStringLiteral("snapshotDir"), QStringLiteral("state-snapshots"));
            redactedPayload.insert(QStringLiteral("snapshotFile"), QStringLiteral("state-%1.json").arg(record.id));
            if (!redactedPayload.contains(QStringLiteral("digest"))) {
                const auto snapshotJson = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
                const auto digest = QCryptographicHash::hash(snapshotJson, QCryptographicHash::Sha256).toHex();
                redactedPayload.insert(QStringLiteral("digest"), QString::fromLatin1(digest));
            }
        }
    }
    record.payload = redactedPayload;
    record.timestampUtc = timestampUtc();
    m_events.push_back(record);
    while (m_events.size() > kMaxBufferedEvents) {
        m_events.pop_front();
    }

    writeEventToArtifacts(record);
    if (type.startsWith(QStringLiteral("network."))) {
        auto networkEntry = record.payload;
        networkEntry.insert(QStringLiteral("type"), record.type);
        networkEntry.insert(QStringLiteral("timestamp"), record.timestampUtc);
        m_networkMap.push_back(networkEntry);
        while (m_networkMap.size() > kMaxNetworkMapEntries) {
            m_networkMap.removeFirst();
        }
    }

    broadcastSseEvent(record);
}

void UiTestBridge::appendEventThreadSafe(const QString &type, const QJsonObject &payload)
{
    if (thread() == QThread::currentThread()) {
        appendEvent(type, payload);
        return;
    }

    QMetaObject::invokeMethod(
        this,
        [this, type, payload]() {
            appendEvent(type, payload);
        },
        Qt::QueuedConnection);
}

void UiTestBridge::appendNetworkEvent(const QString &type, const QJsonObject &payload)
{
    appendEvent(type, payload);
}

void UiTestBridge::writeEventToArtifacts(const EventRecord &record)
{
    if (m_eventsFilePath.isEmpty()) {
        return;
    }

    QFile file(m_eventsFilePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    const QJsonObject eventPayload {
        { QStringLiteral("id"), static_cast<qint64>(record.id) },
        { QStringLiteral("type"), record.type },
        { QStringLiteral("timestamp"), record.timestampUtc },
        { QStringLiteral("payload"), record.payload }
    };
    file.write(QJsonDocument(eventPayload).toJson(QJsonDocument::Compact));
    file.write("\n");
    file.close();

    if (!record.type.startsWith(QStringLiteral("network.")) || m_networkFilePath.isEmpty()) {
        return;
    }

    QFile networkFile(m_networkFilePath);
    if (!networkFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    networkFile.write(QJsonDocument(eventPayload).toJson(QJsonDocument::Compact));
    networkFile.write("\n");
    networkFile.close();
}

void UiTestBridge::writeStateSnapshotArtifact(const QJsonObject &snapshot, const quint64 eventId) const
{
    if (m_stateSnapshotDirectory.isEmpty()) {
        return;
    }

    QFile file(QDir(m_stateSnapshotDirectory).filePath(QStringLiteral("state-%1.json").arg(eventId)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }

    file.write(QJsonDocument(snapshot).toJson(QJsonDocument::Indented));
    file.close();
}

void UiTestBridge::broadcastSseEvent(const EventRecord &record)
{
    if (m_sseClients.isEmpty()) {
        return;
    }

    const auto payload = QJsonDocument(record.payload).toJson(QJsonDocument::Compact);
    QByteArray packet;
    packet += "id: ";
    packet += QByteArray::number(record.id);
    packet += "\n";
    packet += "event: ";
    packet += record.type.toUtf8();
    packet += "\n";
    packet += "data: ";
    packet += payload;
    packet += "\n\n";

    QList<QPointer<QTcpSocket>> survivors;
    for (const auto &socket : m_sseClients) {
        if (socket == nullptr || socket->state() != QAbstractSocket::ConnectedState) {
            continue;
        }
        socket->write(packet);
        socket->flush();
        survivors.push_back(socket);
    }
    m_sseClients = survivors;
}

void UiTestBridge::handleServerConnection()
{
    while (m_server.hasPendingConnections()) {
        auto *socket = m_server.nextPendingConnection();
        if (socket == nullptr) {
            continue;
        }
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            handleSocketReadyRead(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_sseClients.removeAll(socket);
            socket->deleteLater();
        });
    }
}

void UiTestBridge::handleSocketReadyRead(QTcpSocket *socket)
{
    if (socket == nullptr || socket->property("uiTestSseOpen").toBool()) {
        return;
    }

    auto buffer = socket->property("uiTestBuffer").toByteArray();
    buffer += socket->readAll();

    int separatorLength = 4;
    auto headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        separatorLength = 2;
        headerEnd = buffer.indexOf("\n\n");
    }
    if (headerEnd < 0) {
        socket->setProperty("uiTestBuffer", buffer);
        return;
    }

    const auto headerBlock = buffer.left(headerEnd);
    const auto lines = headerBlock.split('\n');
    if (lines.isEmpty()) {
        writeErrorResponse(socket, 400, QStringLiteral("bad-request"), QStringLiteral("Missing request line."));
        return;
    }

    const auto requestLine = lines.first().trimmed();
    const auto requestParts = requestLine.split(' ');
    if (requestParts.size() < 2) {
        writeErrorResponse(socket, 400, QStringLiteral("bad-request"), QStringLiteral("Malformed request line."));
        return;
    }

    QMap<QByteArray, QByteArray> headers;
    int contentLength = 0;
    for (int index = 1; index < lines.size(); ++index) {
        const auto line = lines.at(index).trimmed();
        const auto separator = line.indexOf(':');
        if (separator <= 0) {
            continue;
        }
        const auto key = line.left(separator).trimmed().toLower();
        const auto value = line.mid(separator + 1).trimmed();
        headers.insert(key, value);
        if (key == QByteArrayLiteral("content-length")) {
            contentLength = std::max(0, value.toInt());
        }
    }

    const auto totalSize = headerEnd + separatorLength + contentLength;
    if (buffer.size() < totalSize) {
        socket->setProperty("uiTestBuffer", buffer);
        return;
    }
    const auto body = buffer.mid(headerEnd + separatorLength, contentLength);
    socket->setProperty("uiTestBuffer", buffer.mid(totalSize));

    const auto method = requestParts.at(0).toUpper();
    const auto target = QString::fromLatin1(requestParts.at(1));
    const auto requestUrl = QUrl(QStringLiteral("http://127.0.0.1%1").arg(target));
    const auto path = requestUrl.path();
    const QUrlQuery query(requestUrl);

    if (!isAuthorized(headers, query)) {
        writeErrorResponse(socket, 401, QStringLiteral("unauthorized"), QStringLiteral("Missing or invalid token."));
        return;
    }

    if (method == QByteArrayLiteral("GET") && path == QStringLiteral("/health")) {
        writeJsonResponse(socket, 200, buildHealthPayload());
        return;
    }
    if (method == QByteArrayLiteral("GET") && path == QStringLiteral("/state")) {
        writeJsonResponse(socket, 200, buildStateSnapshot());
        return;
    }
    if (method == QByteArrayLiteral("GET") && path == QStringLiteral("/regions")) {
        writeJsonResponse(socket, 200, QJsonObject { { QStringLiteral("regions"), buildRegionsArray() } });
        return;
    }
    if (method == QByteArrayLiteral("GET") && path == QStringLiteral("/elements")) {
        writeJsonResponse(socket, 200, buildElementsPayload());
        return;
    }
    if (method == QByteArrayLiteral("GET") && path == QStringLiteral("/logs")) {
        const auto cursor = static_cast<quint64>(std::max<qint64>(0, query.queryItemValue(QStringLiteral("cursor")).toLongLong()));
        const auto entries = buildLogsPayload(cursor);
        writeJsonResponse(
            socket,
            200,
            QJsonObject {
                { QStringLiteral("cursor"), static_cast<qint64>(cursor) },
                { QStringLiteral("nextCursor"), static_cast<qint64>(Core::DebugLogger::instance().latestCursor()) },
                { QStringLiteral("entries"), entries }
            });
        return;
    }
    if (method == QByteArrayLiteral("GET") && path == QStringLiteral("/events")) {
        const auto cursor = static_cast<quint64>(std::max<qint64>(0, query.queryItemValue(QStringLiteral("cursor")).toLongLong()));
        handleEventsStream(socket, cursor);
        return;
    }
    if (method == QByteArrayLiteral("POST") && path == QStringLiteral("/capture")) {
        QJsonParseError parseError;
        const auto requestDocument = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !requestDocument.isObject()) {
            writeErrorResponse(socket, 400, QStringLiteral("bad-request"), QStringLiteral("Capture body must be a JSON object."));
            return;
        }

        QString label;
        const auto outputPath = captureOutputPathForRequest(requestDocument.object(), &label);
        if (outputPath.isEmpty()) {
            writeErrorResponse(socket, 400, QStringLiteral("bad-request"), QStringLiteral("Capture path could not be resolved."));
            return;
        }

        const auto requestId = m_nextCaptureRequestId++;
        m_captureRequestIdByPath.insert(outputPath, requestId);
        appendEvent(
            QStringLiteral("capture.requested"),
            QJsonObject {
                { QStringLiteral("requestId"), static_cast<qint64>(requestId) },
                { QStringLiteral("label"), label },
                { QStringLiteral("outputPath"), outputPath }
            });

        if (m_captureController != nullptr) {
            m_captureController->requestCapture(outputPath);
        } else if (m_window != nullptr) {
            const auto image = m_window->grabWindow();
            const auto ok = !image.isNull() && image.save(outputPath);
            handleCaptureCompletion(outputPath, ok);
        } else {
            handleCaptureCompletion(outputPath, false);
        }

        writeJsonResponse(
            socket,
            202,
            QJsonObject {
                { QStringLiteral("requestId"), static_cast<qint64>(requestId) },
                { QStringLiteral("outputPath"), outputPath },
                { QStringLiteral("status"), QStringLiteral("queued") }
            });
        return;
    }

    if (method != QByteArrayLiteral("GET") && method != QByteArrayLiteral("POST")) {
        writeErrorResponse(socket, 405, QStringLiteral("method-not-allowed"), QStringLiteral("Only GET and POST are supported."));
        return;
    }

    writeErrorResponse(socket, 404, QStringLiteral("not-found"), QStringLiteral("Unknown endpoint."));
}

void UiTestBridge::handleEventsStream(QTcpSocket *socket, const quint64 cursor)
{
    if (socket == nullptr) {
        return;
    }

    writeSseHeaders(socket);
    socket->setProperty("uiTestSseOpen", true);
    m_sseClients.push_back(socket);

    for (const auto &event : m_events) {
        if (event.id <= cursor) {
            continue;
        }
        const auto payload = QJsonDocument(event.payload).toJson(QJsonDocument::Compact);
        QByteArray packet;
        packet += "id: ";
        packet += QByteArray::number(event.id);
        packet += "\n";
        packet += "event: ";
        packet += event.type.toUtf8();
        packet += "\n";
        packet += "data: ";
        packet += payload;
        packet += "\n\n";
        socket->write(packet);
    }
    socket->flush();
}

bool UiTestBridge::isAuthorized(const QMap<QByteArray, QByteArray> &headers, const QUrlQuery &query) const
{
    if (!m_enabled) {
        return false;
    }

    const auto tokenFromQuery = query.queryItemValue(QStringLiteral("token"));
    if (tokenFromQuery == m_token) {
        return true;
    }

    const auto headerValue = headers.value(QByteArrayLiteral("authorization"));
    if (headerValue.startsWith("Bearer ")) {
        return QString::fromUtf8(headerValue.mid(7).trimmed()) == m_token;
    }
    return false;
}

void UiTestBridge::writeJsonResponse(QTcpSocket *socket, const int statusCode, const QJsonObject &payload)
{
    if (socket == nullptr) {
        return;
    }

    const auto body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QByteArray response;
    response += "HTTP/1.1 ";
    response += QByteArray::number(statusCode);
    response += ' ';
    response += jsonStatusText(statusCode).toUtf8();
    response += "\r\n";
    response += "Content-Type: application/json\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "Connection: close\r\n";
    response += "Content-Length: ";
    response += QByteArray::number(body.size());
    response += "\r\n\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void UiTestBridge::writeErrorResponse(
    QTcpSocket *socket,
    const int statusCode,
    const QString &code,
    const QString &message)
{
    writeJsonResponse(
        socket,
        statusCode,
        QJsonObject {
            { QStringLiteral("ok"), false },
            { QStringLiteral("error"), code },
            { QStringLiteral("message"), message }
        });
}

void UiTestBridge::writeSseHeaders(QTcpSocket *socket)
{
    if (socket == nullptr) {
        return;
    }

    QByteArray headers;
    headers += "HTTP/1.1 200 OK\r\n";
    headers += "Content-Type: text/event-stream\r\n";
    headers += "Cache-Control: no-cache\r\n";
    headers += "Connection: keep-alive\r\n";
    headers += "X-Accel-Buffering: no\r\n\r\n";
    socket->write(headers);
}

QString UiTestBridge::timestampUtc()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QString UiTestBridge::sanitizePathSegment(const QString &value, const QString &fallback)
{
    auto normalized = value.trimmed();
    normalized.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
    normalized = normalized.simplified();
    normalized.replace(u' ', u'-');
    while (normalized.startsWith(u'.') || normalized.startsWith(u'-')) {
        normalized.remove(0, 1);
    }
    while (normalized.endsWith(u'.') || normalized.endsWith(u'-')) {
        normalized.chop(1);
    }
    if (normalized.isEmpty()) {
        normalized = fallback;
    }
    return normalized.left(64);
}

QJsonObject UiTestBridge::buildHealthPayload() const
{
    return {
        { QStringLiteral("ok"), true },
        { QStringLiteral("enabled"), m_enabled },
        { QStringLiteral("pid"), QCoreApplication::applicationPid() },
        { QStringLiteral("port"), static_cast<int>(m_boundPort) },
        { QStringLiteral("runDirectory"), m_runDirectory },
        { QStringLiteral("windowTitle"), m_window != nullptr ? m_window->title() : QString {} },
        { QStringLiteral("build"), QCoreApplication::applicationVersion() },
        { QStringLiteral("qtVersion"), QString::fromUtf8(qVersion()) }
    };
}

QJsonObject UiTestBridge::buildStateSnapshot() const
{
    const auto regions = buildRegionsArray();
    const auto regionsByName = buildRegionMap();
    return {
        { QStringLiteral("timestamp"), timestampUtc() },
        { QStringLiteral("window"), currentWindowState() },
        { QStringLiteral("regions"), regions },
        { QStringLiteral("elements"), buildSemanticElements(regionsByName) },
        { QStringLiteral("playback"), currentPlaybackState() },
        { QStringLiteral("inventory"), buildVisibleTextInventory() },
        { QStringLiteral("networkMap"), currentNetworkMap() }
    };
}

QJsonArray UiTestBridge::buildRegionsArray() const
{
    const auto regionsByName = buildRegionMap();
    const QStringList regionOrder {
        QStringLiteral("main_window"),
        QStringLiteral("video_canvas"),
        QStringLiteral("left_pane"),
        QStringLiteral("right_pane"),
        QStringLiteral("bottom_controls"),
        QStringLiteral("timeshift_timeline"),
        QStringLiteral("guide_overlay"),
        QStringLiteral("guide_grid"),
        QStringLiteral("settings_overlay"),
        QStringLiteral("settings_rail"),
        QStringLiteral("settings_content"),
        QStringLiteral("debug_bubble")
    };

    QJsonArray regions;
    for (const auto &name : regionOrder) {
        regions.push_back(regionToJson(regionsByName.value(name, emptyRegion(name))));
    }
    return regions;
}

QJsonObject UiTestBridge::buildElementsPayload() const
{
    const auto regionsByName = buildRegionMap();
    return {
        { QStringLiteral("elements"), buildSemanticElements(regionsByName) },
        { QStringLiteral("inventory"), buildVisibleTextInventory() }
    };
}

QJsonArray UiTestBridge::buildSemanticElements(const QHash<QString, UiTestBridge::RegionRecord> &regionsByName) const
{
    auto regionBounds = [&regionsByName](const QString &regionName) {
        return jsonBoundsForRegion(regionsByName.value(regionName, emptyRegion(regionName)));
    };

    QJsonArray elements;
    const auto selectedChannel = m_channelListModel != nullptr ? m_channelListModel->currentChannel() : QVariantMap {};
    const auto nowProgram = m_playbackNowNextModel != nullptr ? m_playbackNowNextModel->currentProgram() : QVariantMap {};
    const auto nextProgram = m_playbackNowNextModel != nullptr ? m_playbackNowNextModel->nextProgram() : QVariantMap {};
    const auto guideChannel = m_guideStateModel != nullptr ? m_guideStateModel->selectedChannel() : QVariantMap {};
    const auto guideProgram = m_guideStateModel != nullptr ? m_guideStateModel->selectedProgram() : QVariantMap {};

    elements.push_back(makeElement(
        QStringLiteral("left.search"),
        QStringLiteral("text"),
        QStringLiteral("left_pane"),
        regionsByName.value(QStringLiteral("left_pane")).visible,
        true,
        m_shellController != nullptr && m_shellController->focusedZone() == QStringLiteral("search"),
        false,
        m_channelListModel != nullptr ? m_channelListModel->searchText() : QString {},
        QJsonValue::fromVariant(m_channelListModel != nullptr ? m_channelListModel->searchText() : QString {}),
        regionBounds(QStringLiteral("left_pane"))));

    elements.push_back(makeElement(
        QStringLiteral("left.selection"),
        QStringLiteral("channel"),
        QStringLiteral("left_pane"),
        regionsByName.value(QStringLiteral("left_pane")).visible,
        true,
        m_shellController != nullptr && m_shellController->focusedZone() == QStringLiteral("leftpane"),
        false,
        selectedChannel.value(QStringLiteral("name")).toString(),
        redactedJsonValue(QJsonValue::fromVariant(selectedChannel)),
        regionBounds(QStringLiteral("left_pane"))));

    elements.push_back(makeElement(
        QStringLiteral("right.now"),
        QStringLiteral("program"),
        QStringLiteral("right_pane"),
        regionsByName.value(QStringLiteral("right_pane")).visible,
        true,
        m_shellController != nullptr && m_shellController->focusedZone() == QStringLiteral("rightpane"),
        false,
        nowProgram.value(QStringLiteral("title")).toString(),
        redactedJsonValue(QJsonValue::fromVariant(nowProgram)),
        regionBounds(QStringLiteral("right_pane"))));

    elements.push_back(makeElement(
        QStringLiteral("right.next"),
        QStringLiteral("program"),
        QStringLiteral("right_pane"),
        regionsByName.value(QStringLiteral("right_pane")).visible,
        true,
        false,
        false,
        nextProgram.value(QStringLiteral("title")).toString(),
        redactedJsonValue(QJsonValue::fromVariant(nextProgram)),
        regionBounds(QStringLiteral("right_pane"))));

    elements.push_back(makeElement(
        QStringLiteral("bottom.transport"),
        QStringLiteral("controls"),
        QStringLiteral("bottom_controls"),
        regionsByName.value(QStringLiteral("bottom_controls")).visible,
        true,
        false,
        false,
        m_appController != nullptr ? m_appController->statusText() : QString {},
        QJsonObject {
            { QStringLiteral("isPlaying"), m_playerController != nullptr && m_playerController->isPlaying() },
            { QStringLiteral("isLoading"), m_playerController != nullptr && m_playerController->isLoading() },
            { QStringLiteral("isBuffering"), m_playerController != nullptr && m_playerController->isBuffering() }
        },
        regionBounds(QStringLiteral("bottom_controls"))));

    elements.push_back(makeElement(
        QStringLiteral("timeshift.timeline"),
        QStringLiteral("timeline"),
        QStringLiteral("timeshift_timeline"),
        regionsByName.value(QStringLiteral("timeshift_timeline")).visible,
        m_playerController != nullptr && m_playerController->timeshiftActive(),
        false,
        m_playerController != nullptr && m_playerController->timeshiftAtLiveEdge(),
        m_playerController != nullptr ? m_playerController->timeshiftNoticeText() : QString {},
        QJsonObject {
            { QStringLiteral("active"), m_playerController != nullptr && m_playerController->timeshiftActive() },
            { QStringLiteral("behindLiveSeconds"), m_playerController != nullptr ? m_playerController->timeshiftBehindLiveSeconds() : 0.0 },
            { QStringLiteral("windowSeconds"), m_playerController != nullptr ? m_playerController->timeshiftWindowSeconds() : 0 },
            { QStringLiteral("availableSeconds"), m_playerController != nullptr ? m_playerController->timeshiftAvailableSeconds() : 0.0 },
            { QStringLiteral("positionSeconds"), m_playerController != nullptr ? m_playerController->timeshiftPositionSeconds() : 0.0 }
        },
        regionBounds(QStringLiteral("timeshift_timeline"))));

    elements.push_back(makeElement(
        QStringLiteral("guide.selection"),
        QStringLiteral("guide"),
        QStringLiteral("guide_grid"),
        regionsByName.value(QStringLiteral("guide_grid")).visible,
        true,
        m_shellController != nullptr && m_shellController->activeOverlay() == QStringLiteral("guide"),
        false,
        guideProgram.value(QStringLiteral("title")).toString(),
        QJsonObject {
            { QStringLiteral("selectedChannel"), redactedJsonValue(QJsonValue::fromVariant(guideChannel)) },
            { QStringLiteral("selectedProgram"), redactedJsonValue(QJsonValue::fromVariant(guideProgram)) }
        },
        regionBounds(QStringLiteral("guide_grid"))));

    elements.push_back(makeElement(
        QStringLiteral("settings.section"),
        QStringLiteral("settings"),
        QStringLiteral("settings_content"),
        regionsByName.value(QStringLiteral("settings_content")).visible,
        true,
        m_shellController != nullptr && m_shellController->activeOverlay() == QStringLiteral("settings"),
        false,
        m_shellController != nullptr ? m_shellController->overlaySection() : QString {},
        QJsonObject {
            { QStringLiteral("activeOverlay"), m_shellController != nullptr ? m_shellController->activeOverlay() : QString {} },
            { QStringLiteral("activeSection"), m_shellController != nullptr ? m_shellController->overlaySection() : QString {} },
            { QStringLiteral("dirty"), m_settingsController != nullptr && m_settingsController->dirty() }
        },
        regionBounds(QStringLiteral("settings_content"))));

    return elements;
}

QJsonArray UiTestBridge::buildVisibleTextInventory() const
{
    QJsonArray inventory;
    if (m_rootObject == nullptr) {
        return inventory;
    }

    QSet<QString> seen;
    QList<QObject *> queue { m_rootObject.data() };
    while (!queue.isEmpty()) {
        QObject *object = queue.takeFirst();
        if (object == nullptr) {
            continue;
        }
        queue.append(object->children());

        auto visible = true;
        if (auto *item = qobject_cast<QQuickItem *>(object)) {
            visible = item->isVisible() && item->width() > 0.0 && item->height() > 0.0;
        } else if (object->metaObject()->indexOfProperty("visible") >= 0) {
            visible = object->property("visible").toBool();
        }
        if (!visible) {
            continue;
        }

        const QStringList propertyNames {
            QStringLiteral("text"),
            QStringLiteral("placeholderText"),
            QStringLiteral("title"),
            QStringLiteral("caption")
        };

        for (const auto &propertyName : propertyNames) {
            if (object->metaObject()->indexOfProperty(propertyName.toUtf8().constData()) < 0) {
                continue;
            }
            const auto text = Core::redactSensitiveText(object->property(propertyName.toUtf8().constData()).toString().trimmed());
            if (text.isEmpty() || seen.contains(text)) {
                continue;
            }
            seen.insert(text);

            QJsonObject record {
                { QStringLiteral("text"), text },
                { QStringLiteral("property"), propertyName },
                { QStringLiteral("className"), QString::fromUtf8(object->metaObject()->className()) },
                { QStringLiteral("objectName"), object->objectName() }
            };
            if (auto *item = qobject_cast<QQuickItem *>(object)) {
                const auto scenePosition = item->mapToScene(QPointF(0.0, 0.0));
                record.insert(
                    QStringLiteral("bounds"),
                    QJsonObject {
                        { QStringLiteral("x"), scenePosition.x() },
                        { QStringLiteral("y"), scenePosition.y() },
                        { QStringLiteral("width"), item->width() },
                        { QStringLiteral("height"), item->height() }
                    });
            }
            inventory.push_back(record);
        }
    }

    return inventory;
}

QJsonArray UiTestBridge::buildLogsPayload(const quint64 cursor) const
{
    QJsonArray logs;
    const auto entries = Core::DebugLogger::instance().entriesSince(cursor);
    for (const auto &entry : entries) {
        logs.push_back(QJsonObject {
            { QStringLiteral("cursor"), static_cast<qint64>(entry.cursor) },
            { QStringLiteral("timestamp"), entry.timestampUtc },
            { QStringLiteral("category"), entry.category },
            { QStringLiteral("message"), Core::redactSensitiveText(entry.message) },
            { QStringLiteral("line"), Core::redactSensitiveText(entry.line) }
        });
    }
    return logs;
}

QJsonObject UiTestBridge::regionToJson(const RegionRecord &region) const
{
    return {
        { QStringLiteral("name"), region.name },
        { QStringLiteral("visible"), region.visible },
        { QStringLiteral("x"), region.x },
        { QStringLiteral("y"), region.y },
        { QStringLiteral("width"), region.width },
        { QStringLiteral("height"), region.height },
        { QStringLiteral("cx"), region.cx },
        { QStringLiteral("cy"), region.cy },
        { QStringLiteral("nx"), region.nx },
        { QStringLiteral("ny"), region.ny },
        { QStringLiteral("nwidth"), region.nwidth },
        { QStringLiteral("nheight"), region.nheight }
    };
}

UiTestBridge::RegionRecord UiTestBridge::buildRegion(
    const QString &name,
    QQuickItem *item,
    const qreal windowWidth, // NOLINT(bugprone-easily-swappable-parameters)
    const qreal windowHeight) const
{
    auto region = emptyRegion(name);
    if (item == nullptr) {
        return region;
    }

    region.visible = item->isVisible() && item->width() > 0.0 && item->height() > 0.0;
    const auto scenePosition = item->mapToScene(QPointF(0.0, 0.0));
    region.x = scenePosition.x();
    region.y = scenePosition.y();
    region.width = item->width();
    region.height = item->height();
    region.cx = region.x + (region.width * 0.5);
    region.cy = region.y + (region.height * 0.5);
    if (windowWidth > 0.0) {
        region.nx = region.x / windowWidth;
        region.nwidth = region.width / windowWidth;
    }
    if (windowHeight > 0.0) {
        region.ny = region.y / windowHeight;
        region.nheight = region.height / windowHeight;
    }
    return region;
}

UiTestBridge::RegionRecord UiTestBridge::buildWindowRegion(
    const QString &name,
    const qreal windowWidth, // NOLINT(bugprone-easily-swappable-parameters)
    const qreal windowHeight) const
{
    auto region = emptyRegion(name);
    region.visible = m_window != nullptr && m_window->isVisible();
    region.x = 0.0;
    region.y = 0.0;
    region.width = windowWidth;
    region.height = windowHeight;
    region.cx = region.width * 0.5;
    region.cy = region.height * 0.5;
    region.nx = 0.0;
    region.ny = 0.0;
    region.nwidth = 1.0;
    region.nheight = 1.0;
    return region;
}

QQuickItem *UiTestBridge::findItemByObjectName(const QString &objectName) const
{
    if (m_rootObject == nullptr || objectName.trimmed().isEmpty()) {
        return nullptr;
    }
    return m_rootObject->findChild<QQuickItem *>(objectName, Qt::FindChildrenRecursively);
}

QHash<QString, UiTestBridge::RegionRecord> UiTestBridge::buildRegionMap() const
{
    const auto windowWidth = m_window != nullptr ? m_window->width() : 0.0;
    const auto windowHeight = m_window != nullptr ? m_window->height() : 0.0;

    QHash<QString, UiTestBridge::RegionRecord> regions;
    regions.insert(
        QStringLiteral("main_window"),
        buildWindowRegion(QStringLiteral("main_window"), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("video_canvas"),
        buildRegion(QStringLiteral("video_canvas"), findItemByObjectName(QStringLiteral("ui.region.video_canvas")), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("left_pane"),
        buildRegion(QStringLiteral("left_pane"), findItemByObjectName(QStringLiteral("ui.region.left_pane")), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("right_pane"),
        buildRegion(QStringLiteral("right_pane"), findItemByObjectName(QStringLiteral("ui.region.right_pane")), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("bottom_controls"),
        buildRegion(QStringLiteral("bottom_controls"), findItemByObjectName(QStringLiteral("ui.region.bottom_controls")), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("timeshift_timeline"),
        buildRegion(QStringLiteral("timeshift_timeline"), findItemByObjectName(QStringLiteral("ui.region.timeshift_timeline")), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("guide_overlay"),
        buildRegion(QStringLiteral("guide_overlay"), findItemByObjectName(QStringLiteral("ui.region.guide_overlay")), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("guide_grid"),
        buildRegion(QStringLiteral("guide_grid"), findItemByObjectName(QStringLiteral("ui.region.guide_grid")), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("settings_overlay"),
        buildRegion(QStringLiteral("settings_overlay"), findItemByObjectName(QStringLiteral("ui.region.settings_overlay")), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("settings_rail"),
        buildRegion(QStringLiteral("settings_rail"), findItemByObjectName(QStringLiteral("ui.region.settings_rail")), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("settings_content"),
        buildRegion(QStringLiteral("settings_content"), findItemByObjectName(QStringLiteral("ui.region.settings_content")), windowWidth, windowHeight));
    regions.insert(
        QStringLiteral("debug_bubble"),
        buildRegion(QStringLiteral("debug_bubble"), findItemByObjectName(QStringLiteral("ui.region.debug_bubble")), windowWidth, windowHeight));
    return regions;
}

QJsonObject UiTestBridge::currentWindowState() const
{
    return {
        { QStringLiteral("width"), m_window != nullptr ? m_window->width() : 0.0 },
        { QStringLiteral("height"), m_window != nullptr ? m_window->height() : 0.0 },
        { QStringLiteral("x11WindowId"), m_window != nullptr ? static_cast<qint64>(m_window->winId()) : 0 },
        { QStringLiteral("visibleOverlay"), m_shellController != nullptr ? m_shellController->activeOverlay() : QStringLiteral("none") },
        { QStringLiteral("layoutBand"), m_shellController != nullptr ? m_shellController->layoutBand() : QString {} },
        { QStringLiteral("focusedZone"), m_shellController != nullptr ? m_shellController->focusedZone() : QString {} }
    };
}

QJsonObject UiTestBridge::currentPlaybackState() const
{
    if (m_playerController == nullptr) {
        return {};
    }

    auto playback = QJsonObject {
        { QStringLiteral("currentChannel"), redactedJsonValue(QJsonValue::fromVariant(m_playerController->currentChannel())) },
        { QStringLiteral("playbackUrl"), Core::redactSensitiveUrl(m_playerController->currentPlaybackUrl()) },
        { QStringLiteral("debugOverlay"), redactedJsonValue(QJsonValue::fromVariant(m_playerController->debugOverlaySnapshot())) },
        { QStringLiteral("timeshift"),
            QJsonObject {
                { QStringLiteral("active"), m_playerController->timeshiftActive() },
                { QStringLiteral("atLiveEdge"), m_playerController->timeshiftAtLiveEdge() },
                { QStringLiteral("behindLiveSeconds"), m_playerController->timeshiftBehindLiveSeconds() },
                { QStringLiteral("windowSeconds"), m_playerController->timeshiftWindowSeconds() },
                { QStringLiteral("availableSeconds"), m_playerController->timeshiftAvailableSeconds() },
                { QStringLiteral("positionSeconds"), m_playerController->timeshiftPositionSeconds() },
                { QStringLiteral("windowStartEpochMs"), static_cast<qint64>(m_playerController->timeshiftWindowStartEpochMs()) },
                { QStringLiteral("liveEdgeEpochMs"), static_cast<qint64>(m_playerController->timeshiftLiveEdgeEpochMs()) },
                { QStringLiteral("attachedWindowStartEpochMs"), static_cast<qint64>(m_playerController->timeshiftAttachedWindowStartEpochMs()) },
                { QStringLiteral("attachedWindowEndEpochMs"), static_cast<qint64>(m_playerController->timeshiftAttachedWindowEndEpochMs()) },
                { QStringLiteral("noticeText"), m_playerController->timeshiftNoticeText() }
            } }
    };
    return playback;
}

QJsonArray UiTestBridge::currentNetworkMap() const
{
    return m_networkMap;
}

QJsonValue UiTestBridge::redactedJsonValue(const QJsonValue &value) const
{
    if (value.isString()) {
        return QJsonValue(Core::redactSensitiveText(value.toString()));
    }
    if (value.isArray()) {
        QJsonArray redactedArray;
        for (const auto &entry : value.toArray()) {
            redactedArray.push_back(redactedJsonValue(entry));
        }
        return redactedArray;
    }
    if (value.isObject()) {
        QJsonObject redactedObject;
        const auto object = value.toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            redactedObject.insert(it.key(), redactedJsonValue(it.value()));
        }
        return redactedObject;
    }
    return value;
}

QVariantMap UiTestBridge::redactedVariantMap(const QVariantMap &value) const
{
    QVariantMap redacted = value;
    for (auto it = redacted.begin(); it != redacted.end(); ++it) {
        if (it->typeId() == QMetaType::QString) {
            it.value() = Core::redactSensitiveText(it->toString());
            continue;
        }
        if (it->typeId() == QMetaType::QVariantMap) {
            it.value() = redactedVariantMap(it->toMap());
        }
    }
    return redacted;
}

QString UiTestBridge::captureOutputPathForRequest(const QJsonObject &requestBody, QString *labelOut) const
{
    const auto rawLabel = requestBody.value(QStringLiteral("label")).toString();
    const auto rawSubdir = requestBody.value(QStringLiteral("subdir")).toString();
    const auto label = sanitizePathSegment(rawLabel, QStringLiteral("capture"));
    const auto subdir = sanitizePathSegment(rawSubdir, QStringLiteral(""));
    if (labelOut != nullptr) {
        *labelOut = label;
    }

    auto screenshotsDir = m_screenshotDirectory;
    if (!subdir.isEmpty()) {
        screenshotsDir = QDir(screenshotsDir).filePath(subdir);
        QDir().mkpath(screenshotsDir);
    }

    const auto fileName = QStringLiteral("%1-%2.png")
                              .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ")))
                              .arg(label);
    return QDir(screenshotsDir).filePath(fileName);
}

void UiTestBridge::handleCaptureCompletion(const QString &outputPath, const bool ok)
{
    const auto requestId = m_captureRequestIdByPath.take(outputPath);
    appendEvent(
        ok ? QStringLiteral("capture.saved") : QStringLiteral("capture.failed"),
        QJsonObject {
            { QStringLiteral("requestId"), static_cast<qint64>(requestId) },
            { QStringLiteral("outputPath"), outputPath },
            { QStringLiteral("ok"), ok }
        });
}

void UiTestBridge::observePlaybackUrl()
{
    if (m_playerController == nullptr) {
        return;
    }
    const auto playbackUrl = Core::redactSensitiveUrl(m_playerController->currentPlaybackUrl());
    if (playbackUrl.trimmed().isEmpty()) {
        return;
    }

    appendNetworkEvent(
        QStringLiteral("network.reply"),
        QJsonObject {
            { QStringLiteral("category"), QStringLiteral("playback.url") },
            { QStringLiteral("url"), playbackUrl },
            { QStringLiteral("statusCode"), 200 },
            { QStringLiteral("durationMs"), 0 },
            { QStringLiteral("payloadBytes"), 0 },
            { QStringLiteral("source"), QStringLiteral("player") }
        });
}

void UiTestBridge::observeTimeshiftPlaybackUrl(const QString &layer, const QString &url)
{
    appendNetworkEvent(
        QStringLiteral("network.request"),
        QJsonObject {
            { QStringLiteral("category"), layer },
            { QStringLiteral("url"), Core::redactSensitiveUrl(url) },
            { QStringLiteral("statusCode"), 0 },
            { QStringLiteral("durationMs"), 0 },
            { QStringLiteral("payloadBytes"), 0 },
            { QStringLiteral("source"), QStringLiteral("timeshift") }
        });
}

void UiTestBridge::observeTimeshiftLocalRequest(
    const QString &method,
    const QString &target,
    const int statusCode,
    const qint64 payloadBytes)
{
    appendNetworkEvent(
        QStringLiteral("network.reply"),
        QJsonObject {
            { QStringLiteral("category"), QStringLiteral("timeshift.local-http") },
            { QStringLiteral("method"), method },
            { QStringLiteral("target"), target },
            { QStringLiteral("statusCode"), statusCode },
            { QStringLiteral("durationMs"), 0 },
            { QStringLiteral("payloadBytes"), static_cast<qint64>(payloadBytes) },
            { QStringLiteral("source"), QStringLiteral("timeshift") }
        });
}

void UiTestBridge::handleStatePoll()
{
    if (!m_enabled) {
        return;
    }

    const auto snapshot = buildStateSnapshot();
    const auto snapshotJson = QJsonDocument(snapshot).toJson(QJsonDocument::Compact);
    const auto digest = QCryptographicHash::hash(snapshotJson, QCryptographicHash::Sha256);
    if (digest == m_lastStateDigest) {
        return;
    }

    m_lastStateDigest = digest;
    appendEvent(
        QStringLiteral("state.changed"),
        QJsonObject {
            { QStringLiteral("snapshot"), snapshot },
            { QStringLiteral("digest"), QString::fromLatin1(digest.toHex()) }
        });
}

} // namespace OKILTV::App
