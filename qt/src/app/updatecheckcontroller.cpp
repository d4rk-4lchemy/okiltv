#include "updatecheckcontroller.h"

#include "../core/debuglogger.h"
#include "../core/settingsmanager.h"

#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QVersionNumber>

#include <utility>

namespace OKILTV::App {
namespace {
QVersionNumber parseVersion(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9]+\\.[0-9]+\\.[0-9]+\\z"));
    if (!pattern.match(text).hasMatch())
        return {};
    qsizetype suffix = 0;
    const auto version = QVersionNumber::fromString(text, &suffix);
    return suffix == text.size() && version.segmentCount() == 3 ? version : QVersionNumber {};
}

void logCheck(const QString &message)
{
    Core::DebugLogger::instance().log(QStringLiteral("updates"), message);
}
} // namespace

UpdateCheckController::UpdateCheckController(Core::SettingsManager *settings, QObject *parent)
    : UpdateCheckController(settings, QStringLiteral(OKILTV_APP_VERSION),
        QUrl(QStringLiteral("https://api.github.com/repos/d4rk-4lchemy/okiltv/releases/latest")),
        10000, parent)
{
}

UpdateCheckController::UpdateCheckController(Core::SettingsManager *settings, QString currentVersion,
    QUrl endpoint, int timeoutMs, QObject *parent)
    : QObject(parent), m_settings(settings), m_currentVersion(std::move(currentVersion)),
      m_endpoint(std::move(endpoint)), m_timeoutMs(timeoutMs)
{
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this]() {
        if (m_reply) {
            logCheck(QStringLiteral("Update check timed out."));
            m_reply->abort();
        }
    });
}

UpdateCheckController::~UpdateCheckController()
{
    shutdown();
}

void UpdateCheckController::check()
{
    if (m_started || m_stopped)
        return;
    m_started = true;
    QNetworkRequest request(m_endpoint);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "OKILTV/" + m_currentVersion.toUtf8());
    m_reply = m_network.get(request);
    connect(m_reply, &QNetworkReply::finished, this, &UpdateCheckController::finishReply);
    connect(m_reply, &QIODevice::readyRead, this, [this]() {
        // A release response should be small; never retain an unbounded response.
        if (m_reply && m_reply->bytesAvailable() > 1024LL * 1024) {
            logCheck(QStringLiteral("Update response exceeded size limit."));
            m_reply->abort();
        }
    });
    m_deadline.start(m_timeoutMs);
}

void UpdateCheckController::finishReply()
{
    m_deadline.stop();
    auto *reply = m_reply.data();
    m_reply.clear();
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError
        || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        logCheck(QStringLiteral("Update check failed (HTTP %1, network error %2).")
            .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
            .arg(static_cast<int>(reply->error())));
        emit checkFinished();
        return;
    }
    const auto document = QJsonDocument::fromJson(reply->readAll());
    const auto release = document.object();
    const auto tag = release.value(QStringLiteral("tag_name")).toString();
    const auto remote = tag.startsWith(QLatin1Char('v')) ? parseVersion(tag.mid(1)) : QVersionNumber {};
    const auto local = parseVersion(m_currentVersion);
    if (!document.isObject() || release.value(QStringLiteral("draft")) != QJsonValue(false)
        || release.value(QStringLiteral("prerelease")) != QJsonValue(false)
        || remote.isNull() || local.isNull()) {
        logCheck(QStringLiteral("Ignoring invalid update release metadata."));
    } else if (QVersionNumber::compare(remote, local) > 0
               && !m_settings->current().skippedUpdateVersions.contains(remote.toString())) {
        m_latestVersion = remote.toString();
        m_releaseTag = tag;
        m_pending = true;
        emit changed();
    }
    emit checkFinished();
}

void UpdateCheckController::shutdown()
{
    m_stopped = true;
    m_deadline.stop();
    if (m_reply) {
        auto *reply = m_reply.data();
        m_reply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    dismiss();
}

void UpdateCheckController::openRelease()
{
    if (!m_pending)
        return;
    // Construct from the validated tag so API metadata cannot redirect outside this repository.
    const QUrl url(QStringLiteral("https://github.com/d4rk-4lchemy/okiltv/releases/tag/") + m_releaseTag);
    if (!QDesktopServices::openUrl(url)) {
        m_errorText = QStringLiteral("Could not open the browser. Try again or choose Not now.");
        emit changed();
        return;
    }
    dismiss();
}

void UpdateCheckController::skipVersion()
{
    if (!m_pending)
        return;
    auto &skipped = m_settings->current().skippedUpdateVersions;
    const auto previous = skipped;
    if (!skipped.contains(m_latestVersion))
        skipped.append(m_latestVersion);
    m_settings->save();
    if (!m_settings->lastSaveError().isEmpty()) {
        skipped = previous;
        m_errorText = QStringLiteral("Could not save this choice. Try again or choose Not now.");
        emit changed();
        return;
    }
    dismiss();
}

void UpdateCheckController::dismiss()
{
    if (!m_pending && m_errorText.isEmpty())
        return;
    m_pending = false;
    m_errorText.clear();
    emit changed();
}

} // namespace OKILTV::App
