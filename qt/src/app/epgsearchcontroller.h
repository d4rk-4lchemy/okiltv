#pragma once
#include "../core/epgservice.h"
#include "epgsearchmodel.h"
#include <QFutureWatcher>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
namespace OKILTV::App {
class AppController;
class EpgSearchController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(EpgSearchModel* model READ model CONSTANT)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY stateChanged)
    Q_PROPERTY(QString timeFilter READ timeFilter WRITE setTimeFilter NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool fetchingMore READ fetchingMore NOTIFY stateChanged)
    Q_PROPERTY(bool resultsCurrent READ resultsCurrent NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY stateChanged)
    Q_PROPERTY(QString selectedKey READ selectedKey NOTIFY stateChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap selectedDetails READ selectedDetails NOTIFY stateChanged)
    Q_PROPERTY(bool detailsBusy READ detailsBusy NOTIFY stateChanged)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY stateChanged)
    Q_PROPERTY(QString dataAgeText READ dataAgeText NOTIFY stateChanged)
    Q_PROPERTY(bool expanded READ expanded NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
public:
    struct Context {
        Core::EpgSearchRequest request;
        std::shared_ptr<const Core::EpgService::Snapshot> snapshot;
        QString sourceName;
        Core::DateTimeFormatOptions format = Core::systemDateTimeFormat();
        bool preparing = false;
        QDateTime fetchedAt;
    };
    using ContextProvider = std::function<Context()>;
    using SearchBackend
        = std::function<Core::EpgSearchResult(std::shared_ptr<const Core::EpgService::Snapshot>,
            const Core::EpgSearchRequest&, const Core::EpgStore::Cancelled&)>;
    struct Actions {
        std::function<QVariantMap(const QVariantMap&, const QVariantMap&)> state;
        std::function<bool(const QVariantMap&, const QVariantMap&, bool)> play;
        std::function<bool(const QVariantMap&, const QVariantMap&)> record;
        std::function<quint64(const QVariantMap&, const QVariantMap&)> details;
    };
    explicit EpgSearchController(AppController* app, QObject* parent = nullptr);
    EpgSearchController(
        ContextProvider context, SearchBackend backend, Actions actions = { }, QObject* parent = nullptr);
    ~EpgSearchController() override;
    EpgSearchModel* model() { return &m_model; }
    QString query() const { return m_query; }
    QString timeFilter() const { return m_filter; }
    QString status() const { return m_status; }
    bool busy() const { return m_busy; }
    bool fetchingMore() const { return m_more; }
    bool resultsCurrent() const { return m_current; }
    QString errorText() const { return m_error; }
    bool hasMore() const { return m_hasMore; }
    QString selectedKey() const;
    int selectedIndex() const { return m_selected; }
    QVariantMap selectedDetails() const { return m_details; }
    bool detailsBusy() const { return m_detailsBusy; }
    QString sourceName() const { return m_context.sourceName; }
    QString dataAgeText() const;
    bool expanded() const { return m_expanded; }
    bool active() const { return m_active; }
    Q_INVOKABLE void openSession();
    Q_INVOKABLE void closeSession();
    Q_INVOKABLE void setQuery(const QString&);
    Q_INVOKABLE void setTimeFilter(const QString&);
    Q_INVOKABLE void selectIndex(int);
    Q_INVOKABLE void moveSelection(int);
    Q_INVOKABLE void fetchNextPage();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void activateSelected();
    Q_INVOKABLE void activateSelectedFromBeginningOrDefault();
    Q_INVOKABLE void toggleSelectedRecording();
    Q_INVOKABLE void downloadSelected();
    Q_INVOKABLE void playSelectedFromBeginning();
    Q_INVOKABLE void reportActionError(const QString& text, const QString& resultKey);
    Q_INVOKABLE QString downloadActionToken() const { return QString::number(m_generation); }
    Q_INVOKABLE bool validateDownloadTarget(const QString& token, const QString& resultKey);
    void refreshContext();
    void refreshActions();
    void completeDetails(quint64, const QVariantMap&, const QString&);
    void shutdown();
signals:
    void stateChanged();
    void closeRequested();
    void showDetailsRequested();
    void downloadRequested(const QVariantMap& channel, const QVariantMap& program);

private:
    void invalidate(bool clear, bool preserve = false);
    void schedule();
    void launch(bool more = false);
    void finished();
    void updateDetails();
    void play(bool fromBeginning, bool fallbackToPrimary = false);
    bool actionReady();
    EpgSearchModel m_model;
    ContextProvider m_provider;
    SearchBackend m_backend;
    Actions m_actions;
    Context m_context;
    QThreadPool m_pool;
    QFutureWatcher<Core::EpgSearchResult> m_watcher;
    QTimer m_debounce, m_clock, m_preparingRetry;
    std::shared_ptr<std::atomic_bool> m_cancel;
    Core::EpgSearchRequest m_request;
    QString m_query, m_filter = QStringLiteral("all"), m_status = QStringLiteral("idle"), m_error,
                     m_preserveKey;
    QVariantMap m_details, m_program;
    bool m_active = false, m_expanded = false, m_busy = false, m_more = false, m_current = false,
         m_hasMore = false;
    bool m_running = false, m_pending = false, m_stopping = false, m_detailsBusy = false,
         m_actionPending = false;
    quint64 m_generation = 0, m_detailId = 0, m_detailGeneration = 0;
    QString m_detailKey;
    int m_selected = -1, m_nextOffset = 0;
};
} // namespace OKILTV::App
