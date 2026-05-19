#pragma once

#include "../core/database_service.h"
#include "../core/settingsmanager.h"

#include <QAbstractListModel>
#include <QFutureSynchronizer>
#include <QHash>

namespace OKILTV::App {

class SourceGroupsModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString profileId READ profileId WRITE setProfileId NOTIFY profileIdChanged)
    Q_PROPERTY(bool hasGroups READ hasGroups NOTIFY groupsChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY groupsChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY groupsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool autoPersist READ autoPersist WRITE setAutoPersist NOTIFY autoPersistChanged)
    Q_PROPERTY(bool hideUnchecked READ hideUnchecked WRITE setHideUnchecked NOTIFY hideUncheckedChanged)

public:
    enum Roles
    {
        IdRole = Qt::UserRole + 1,
        NameRole,
        CountRole,
        SelectedRole
    };
    Q_ENUM(Roles)

    explicit SourceGroupsModel(
        Core::SettingsManager *settings,
        Core::DatabaseService *database,
        QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString profileId() const;
    void setProfileId(const QString &value);

    bool hasGroups() const;
    int selectedCount() const;
    int totalCount() const;
    bool loading() const;
    bool dirty() const;
    bool autoPersist() const;
    void setAutoPersist(bool value);
    bool hideUnchecked() const;
    void setHideUnchecked(bool value);

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE void reload();
    Q_INVOKABLE bool setGroupSelected(const QString &groupId, bool selected);
    Q_INVOKABLE bool setGroupsSelected(const QStringList &groupIds, bool selected);
    Q_INVOKABLE bool selectAll();
    Q_INVOKABLE bool deselectAll();
    Q_INVOKABLE bool moveGroup(const QString &groupId, int targetIndex);
    Q_INVOKABLE bool reorderVisibleGroups(const QStringList &visibleOrderedIds);
    Q_INVOKABLE void saveDraftChanges();
    Q_INVOKABLE void discardDraftChanges();

signals:
    void profileIdChanged();
    void groupsChanged();
    void loadingChanged();
    void dirtyChanged();
    void autoPersistChanged();
    void hideUncheckedChanged();

private:
    struct GroupEntry
    {
        QString id;
        QString name;
        int count { 0 };
        bool selected { false };
    };

    struct DraftState
    {
        QStringList hiddenGroups;
        QStringList groupOrder;
        bool hideUnchecked { false };
    };

    int indexOfGroup(const QString &groupId) const;
    bool updateSelection(const QStringList &groupIds, bool selected);
    void setLoading(bool value);
    void clearLoadedGroups();
    void applyDraftStateForCurrentProfile();
    void updateDraftStateForCurrentProfile();
    void clearDraftStateIfPersisted(const QString &profileId);
    DraftState currentDraftState() const;
    DraftState persistedDraftState(const QString &profileId) const;
    bool draftStatesEqual(const DraftState &left, const DraftState &right) const;

    Core::SettingsManager *m_settings;
    Core::DatabaseService *m_database;
    QString m_profileId;
    QList<GroupEntry> m_groups;
    QHash<QString, DraftState> m_draftsByProfile;
    bool m_loading { false };
    bool m_autoPersist { true };
    bool m_hideUnchecked { false };
    QFutureSynchronizer<void> m_backgroundTasks;
    quint64 m_reloadGeneration { 0 };
};

} // namespace OKILTV::App
