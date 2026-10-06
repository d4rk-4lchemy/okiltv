#include "epgsearchcontroller.h"
#include <QCoreApplication>
#include <QtConcurrentRun>
#include <algorithm>
namespace OKILTV::App {
using namespace Core;
EpgSearchController::EpgSearchController(
    ContextProvider provider, SearchBackend backend, Actions actions, QObject* parent)
    : QObject(parent)
    , m_model(this)
    , m_provider(std::move(provider))
    , m_backend(std::move(backend))
    , m_actions(std::move(actions))
{
    m_pool.setMaxThreadCount(1);
    m_debounce.setInterval(150);
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, [this] { launch(); });
    connect(&m_watcher, &QFutureWatcher<EpgSearchResult>::finished, this, &EpgSearchController::finished);
    m_preparingRetry.setSingleShot(true);
    m_preparingRetry.setInterval(500);
    connect(&m_preparingRetry, &QTimer::timeout, this, [this] {
        if (m_active && m_status == QStringLiteral("preparing"))
            launch();
    });
    m_clock.setInterval(30000);
    connect(&m_clock, &QTimer::timeout, this, [this] {
        m_model.refreshLabels();
        refreshActions();
    });
}
EpgSearchController::~EpgSearchController() { shutdown(); }
void EpgSearchController::shutdown()
{
    m_stopping = true;
    closeSession();
    m_pool.waitForDone();
}
QString EpgSearchController::selectedKey() const
{
    const auto* r = m_model.row(m_selected);
    return r ? r->resultKey : QString();
}
QString EpgSearchController::dataAgeText() const
{
    return m_context.fetchedAt.isValid()
        ? tr("EPG updated %1")
              .arg(Core::formatDisplayDateTime(m_context.fetchedAt, m_context.format.dateTimePattern()))
        : QString();
}
void EpgSearchController::openSession()
{
    if (m_active || m_stopping)
        return;
    m_active = true;
    m_expanded = false;
    m_query.clear();
    m_filter = QStringLiteral("all");
    m_context = m_provider();
    m_model.setDateTimeFormat(m_context.format);
    m_model.setSummaries(m_context.snapshot && m_context.snapshot->store);
    m_clock.start();
    invalidate(true);
    schedule();
}
void EpgSearchController::closeSession()
{
    m_active = false;
    m_debounce.stop();
    m_clock.stop();
    invalidate(true);
    m_busy = false;
    m_status = QStringLiteral("idle");
    emit stateChanged();
}
void EpgSearchController::invalidate(bool clear, bool preserve)
{
    ++m_generation;
    if (m_cancel)
        m_cancel->store(true);
    m_pending = false;
    m_debounce.stop();
    m_preparingRetry.stop();
    m_current = false;
    m_hasMore = false;
    m_more = false;
    m_detailsBusy = false;
    m_detailId = 0;
    m_details.clear();
    m_program.clear();
    m_error.clear();
    m_preserveKey = preserve ? selectedKey() : QString();
    if (clear) {
        m_model.replace({ });
        m_selected = -1;
    }
    emit stateChanged();
}
void EpgSearchController::setQuery(const QString& value)
{
    if (m_query == value)
        return;
    m_query = value;
    invalidate(false);
    schedule();
}
void EpgSearchController::setTimeFilter(const QString& value)
{
    if (value != QStringLiteral("all") && value != QStringLiteral("now")
        && value != QStringLiteral("upcoming") && value != QStringLiteral("past"))
        return;
    if (m_filter == value)
        return;
    m_filter = value;
    invalidate(false);
    schedule();
}
void EpgSearchController::schedule()
{
    if (!m_active || m_stopping)
        return;
    m_busy = false;
    const auto tokens = epgSearchTokens(m_query);
    const auto error = epgSearchQueryError(m_query);
    if (m_context.request.profileId.isEmpty()) {
        m_status = QStringLiteral("no-source");
    } else if (!error.isEmpty() && error != QStringLiteral("query-too-short")) {
        m_status = QStringLiteral("error");
        m_error = error == QStringLiteral("query-too-long") ? tr("Enter no more than 256 characters.")
            : error == QStringLiteral("too-many-tokens")    ? tr("Enter no more than 16 words.")
                                                            : tr("The search query is invalid.");
    } else if (std::none_of(tokens.cbegin(), tokens.cend(),
                   [](const QString& token) { return token.toUcs4().size() >= 2; })) {
        m_status = QStringLiteral("idle");
        m_model.replace({ });
        m_selected = -1;
    } else {
        m_expanded = true;
        if (m_context.request.profileId.isEmpty())
            m_status = QStringLiteral("no-source");
        else if (m_context.request.eligibleChannels.isEmpty())
            m_status = QStringLiteral("no-channels");
        else {
            m_status = QStringLiteral("loading");
            m_busy = true;
            m_debounce.start();
        }
    }
    emit stateChanged();
}
void EpgSearchController::refreshContext()
{
    const auto next = m_provider();
    const bool source = next.request.profileId != m_context.request.profileId;
    const bool changed = source || next.request.epgGeneration != m_context.request.epgGeneration
        || next.snapshot != m_context.snapshot
        || next.request.channelRevision != m_context.request.channelRevision
        || next.preparing != m_context.preparing;
    m_context = next;
    m_model.setDateTimeFormat(next.format);
    m_model.setSummaries(next.snapshot && next.snapshot->store);
    if (!m_active)
        return;
    if (changed) {
        if (source) {
            m_query.clear();
            m_filter = QStringLiteral("all");
            m_expanded = false;
        }
        invalidate(source, !source);
        schedule();
    } else {
        refreshActions();
        emit stateChanged();
    }
}
void EpgSearchController::launch(bool more)
{
    if (!m_active || m_stopping)
        return;
    if (m_running) {
        m_pending = true;
        return;
    }
    if (m_context.preparing && (!m_context.snapshot || m_context.snapshot->totalEntries == 0)) {
        m_status = QStringLiteral("preparing");
        m_busy = false;
        m_preparingRetry.start();
        emit stateChanged();
        return;
    }
    m_pending = false;
    m_running = true;
    m_more = more;
    m_busy = !more;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    if (!more) {
        m_request = m_context.request;
        m_request.requestId = m_generation;
        m_request.query = m_query;
        m_request.nowUtc = QDateTime::currentDateTimeUtc();
        m_request.offset = 0;
        m_request.timeFilter = m_filter == QStringLiteral("now") ? EpgSearchTimeFilter::Now
            : m_filter == QStringLiteral("upcoming")             ? EpgSearchTimeFilter::Upcoming
            : m_filter == QStringLiteral("past")                 ? EpgSearchTimeFilter::Past
                                                                 : EpgSearchTimeFilter::All;
    } else
        m_request.offset = m_nextOffset;
    const auto request = m_request;
    const auto snapshot = m_context.snapshot;
    const auto cancel = m_cancel;
    const auto backend = m_backend;
    m_watcher.setFuture(QtConcurrent::run(&m_pool, [request, snapshot, cancel, backend] {
        try {
            return backend(snapshot, request, [cancel] { return cancel->load(); });
        } catch (const std::exception&) {
            EpgSearchResult r;
            r.request = request;
            r.status = EpgSearchStatus::Error;
            r.errorText = QCoreApplication::translate("EpgSearchController", "Cannot search programme data.");
            return r;
        }
    }));
    emit stateChanged();
}
void EpgSearchController::finished()
{
    const auto result = m_watcher.result();
    m_running = false;
    if (m_stopping || !m_active)
        return;
    if (result.request.requestId != m_generation || result.request.profileId != m_context.request.profileId
        || result.request.channelRevision != m_context.request.channelRevision
        || result.request.epgGeneration != m_context.request.epgGeneration) {
        if (m_pending) {
            m_pending = false;
            launch();
        }
        return;
    }
    const bool more = m_more;
    m_busy = false;
    m_more = false;
    if (result.status == EpgSearchStatus::Ready) {
        if (more)
            m_model.append(result.rows);
        else
            m_model.replace(result.rows);
        m_hasMore = result.hasMore;
        m_nextOffset = result.nextOffset;
        m_current = true;
        m_error.clear();
        m_status = m_model.rowCount() ? QStringLiteral("ready") : QStringLiteral("empty");
        if (!more) {
            m_selected = m_preserveKey.isEmpty() ? -1 : m_model.indexOfKey(m_preserveKey);
            if (m_selected < 0 && m_model.rowCount())
                m_selected = 0;
            updateDetails();
        }
    } else if (result.status != EpgSearchStatus::Cancelled) {
        m_error = result.errorText;
        if (more) {
            m_current = true;
            m_status = QStringLiteral("ready");
        } else {
            m_current = false;
            m_status = result.status == EpgSearchStatus::NoEpg ? QStringLiteral("no-epg")
                : result.status == EpgSearchStatus::Preparing  ? QStringLiteral("preparing")
                                                               : QStringLiteral("error");
            if (result.status == EpgSearchStatus::Preparing)
                m_preparingRetry.start();
        }
    }
    emit stateChanged();
    if (m_pending) {
        m_pending = false;
        launch();
    }
}
void EpgSearchController::fetchNextPage()
{
    if (m_current && m_hasMore && !m_running && !m_debounce.isActive())
        launch(true);
}
void EpgSearchController::retry()
{
    if (m_current && m_hasMore && !m_error.isEmpty()) {
        fetchNextPage();
        return;
    }
    m_context = m_provider();
    invalidate(false, true);
    schedule();
}
void EpgSearchController::selectIndex(int index)
{
    if (index < 0 || index >= m_model.rowCount() || index == m_selected)
        return;
    m_selected = index;
    updateDetails();
    emit stateChanged();
}
void EpgSearchController::moveSelection(int delta)
{
    if (!m_current || !m_model.rowCount())
        return;
    selectIndex(std::clamp(m_selected < 0 ? 0 : m_selected + delta, 0, m_model.rowCount() - 1));
}
void EpgSearchController::updateDetails()
{
    m_detailId = 0;
    m_detailsBusy = false;
    m_program.clear();
    m_details.clear();
    const auto* r = m_model.row(m_selected);
    if (!r || !m_current)
        return;
    m_program = toVariantMap(r->program, m_context.format);
    if (m_context.snapshot && m_context.snapshot->store)
        m_program.insert(QStringLiteral("detailsPending"), true);
    if (m_program.value(QStringLiteral("detailsPending")).toBool() && m_actions.details) {
        m_detailsBusy = true;
        m_detailKey = r->resultKey;
        m_detailGeneration = m_generation;
        m_detailId = m_actions.details(toVariantMap(r->channel), m_program);
    }
    refreshActions();
}
void EpgSearchController::completeDetails(quint64 id, const QVariantMap& program, const QString& error)
{
    if (!m_active || id != m_detailId || m_detailGeneration != m_generation || m_detailKey != selectedKey())
        return;
    m_detailsBusy = false;
    m_detailId = 0;
    if (error.isEmpty())
        m_program = program;
    else
        m_error = error;
    refreshActions();
}
void EpgSearchController::refreshActions()
{
    const auto* r = m_model.row(m_selected);
    if (!m_current || !r) {
        m_details.clear();
        emit stateChanged();
        return;
    }
    m_details = m_program;
    m_details.insert(QStringLiteral("channel"), toVariantMap(r->channel));
    m_details.insert(QStringLiteral("program"), m_program);
    m_details.insert(QStringLiteral("channelName"), r->channel.name);
    m_details.insert(QStringLiteral("timeLabel"), m_model.timeLabel(*r));
    if (m_actions.state) {
        const auto state = m_actions.state(toVariantMap(r->channel), m_program);
        for (auto it = state.cbegin(); it != state.cend(); ++it)
            m_details.insert(it.key(), it.value());
    }
    if (m_detailsBusy || m_program.value(QStringLiteral("detailsPending")).toBool()) {
        m_details.insert(QStringLiteral("primaryEnabled"), false);
        m_details.insert(QStringLiteral("fromBeginningEnabled"), false);
        m_details.insert(QStringLiteral("recordingEnabled"), false);
        m_details.insert(QStringLiteral("downloadEnabled"), false);
    }
    emit stateChanged();
}
bool EpgSearchController::actionReady()
{
    if (!m_active || !m_current || m_detailsBusy || m_actionPending || !m_model.row(m_selected))
        return false;
    refreshContext();
    return m_current && !m_detailsBusy && m_model.row(m_selected);
}
void EpgSearchController::play(bool beginning, bool fallbackToPrimary)
{
    if (!actionReady())
        return;
    refreshActions();
    if (beginning && fallbackToPrimary && !m_details.value(QStringLiteral("fromBeginningEnabled")).toBool())
        beginning = false;
    if (!m_details
            .value(beginning ? QStringLiteral("fromBeginningEnabled") : QStringLiteral("primaryEnabled"))
            .toBool())
        return;
    if (!beginning && m_details.value(QStringLiteral("actionKind")).toString() == QStringLiteral("details")) {
        emit showDetailsRequested();
        return;
    }
    m_actionPending = true;
    const auto* r = m_model.row(m_selected);
    const bool accepted = m_actions.play && m_actions.play(toVariantMap(r->channel), m_program, beginning);
    m_actionPending = false;
    if (accepted)
        emit closeRequested();
    else {
        m_error = tr("The selected playback request could not be accepted.");
        refreshActions();
    }
}
bool EpgSearchController::validateDownloadTarget(const QString& token, const QString& resultKey)
{
    if (!actionReady() || token != downloadActionToken() || resultKey != selectedKey())
        return false;
    refreshActions();
    return m_details.value(QStringLiteral("downloadEnabled")).toBool();
}
// QML supplies the message and its selected-result fence as positional strings.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void EpgSearchController::reportActionError(const QString& text, const QString& resultKey)
{
    if (!m_active || !m_current || selectedKey() != resultKey)
        return;
    m_error = text;
    emit stateChanged();
}
void EpgSearchController::activateSelected() { play(false); }
void EpgSearchController::activateSelectedFromBeginningOrDefault() { play(true, true); }
void EpgSearchController::playSelectedFromBeginning() { play(true); }
void EpgSearchController::toggleSelectedRecording()
{
    if (!actionReady())
        return;
    refreshActions();
    if (!m_details.value(QStringLiteral("recordingEnabled")).toBool())
        return;
    m_actionPending = true;
    const auto* r = m_model.row(m_selected);
    if (!m_actions.record || !m_actions.record(toVariantMap(r->channel), m_program))
        m_error = tr("Cannot change the programme recording.");
    m_actionPending = false;
    refreshActions();
}
void EpgSearchController::downloadSelected()
{
    if (!actionReady())
        return;
    refreshActions();
    if (m_details.value(QStringLiteral("downloadEnabled")).toBool())
        emit downloadRequested(toVariantMap(m_model.row(m_selected)->channel), m_program);
}
} // namespace OKILTV::App
