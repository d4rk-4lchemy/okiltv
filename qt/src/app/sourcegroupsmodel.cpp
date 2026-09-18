#include "sourcegroupsmodel.h"
#include "../core/sourcegrouppreferences.h"

#include <QtConcurrent>

#include <algorithm>
#include <QSet>

namespace OKILTV::App {

using namespace Core;

namespace {

constexpr qint64 kFavouritesEligibleWatchSeconds = static_cast<qint64>(6) * 60 * 60;
constexpr auto kFavouritesGroupId = "__favourites__";

struct ReloadState
{
    QStringList hiddenGroups;
    QStringList groupOrder;
    bool hideUnchecked { false };
};

struct ReloadGroup
{
    QString id;
    QString name;
    int count { 0 };
};

struct ReloadResult
{
    QString profileId;
    QList<ReloadGroup> groups;
    bool hideUnchecked { false };
    QString errorText;
};

struct HiddenGroupMergeData
{
    const QStringList &draftHiddenGroups;
    const QStringList &persistedHiddenGroups;
    const QStringList &knownOrder;
};

QStringList mergeOrder(const QStringList &primary, const QStringList &secondary)
{
    QStringList merged;
    merged.reserve(primary.size() + secondary.size());
    for (const auto &groupId : primary) {
        if (!groupId.isEmpty() && !merged.contains(groupId)) {
            merged.push_back(groupId);
        }
    }
    for (const auto &groupId : secondary) {
        if (!groupId.isEmpty() && !merged.contains(groupId)) {
            merged.push_back(groupId);
        }
    }
    return merged;
}

QStringList mergeHiddenGroups(const HiddenGroupMergeData &data)
{
    QStringList merged;
    merged.reserve(data.draftHiddenGroups.size() + data.persistedHiddenGroups.size());
    for (const auto &groupId : data.draftHiddenGroups) {
        if (!groupId.isEmpty() && !merged.contains(groupId)) {
            merged.push_back(groupId);
        }
    }
    for (const auto &groupId : data.persistedHiddenGroups) {
        if (!groupId.isEmpty() && !data.knownOrder.contains(groupId) && !merged.contains(groupId)) {
            merged.push_back(groupId);
        }
    }
    return merged;
}

ReloadState buildEffectiveState(
    const QStringList &discoveredIds,
    const ReloadState &persistedState,
    const std::optional<ReloadState> &draftState,
    bool *settingsChanged)
{
    const auto reconciled = reconcileSourceGroups(discoveredIds, { persistedState.hiddenGroups, persistedState.groupOrder });
    ReloadState effectiveState { reconciled.hiddenGroups, reconciled.groupOrder, persistedState.hideUnchecked };
    if (settingsChanged != nullptr) {
        *settingsChanged = persistedState.hiddenGroups != effectiveState.hiddenGroups
            || persistedState.groupOrder != effectiveState.groupOrder;
    }
    if (draftState.has_value()) {
        const auto &draft = draftState.value();
        effectiveState.hiddenGroups = mergeHiddenGroups({ draft.hiddenGroups, effectiveState.hiddenGroups, draft.groupOrder });
        effectiveState.groupOrder = mergeOrder(draft.groupOrder, effectiveState.groupOrder);
        effectiveState.hideUnchecked = draft.hideUnchecked;
    }

    return effectiveState;
}

} // namespace

SourceGroupsModel::SourceGroupsModel(SettingsManager *settings, DatabaseService *database, QObject *parent)
    : QAbstractListModel(parent)
    , m_settings(settings)
    , m_database(database)
{
}

int SourceGroupsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_groups.size());
}

QVariant SourceGroupsModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_groups.size()) {
        return {};
    }

    const auto &group = m_groups.at(index.row());
    switch (role) {
    case IdRole:
        return group.id;
    case NameRole:
        return group.name;
    case CountRole:
        return group.count;
    case SelectedRole:
        return group.selected;
    default:
        return {};
    }
}

QHash<int, QByteArray> SourceGroupsModel::roleNames() const
{
    return {
        { IdRole, "id" },
        { NameRole, "name" },
        { CountRole, "count" },
        { SelectedRole, "selected" }
    };
}

QString SourceGroupsModel::profileId() const
{
    return m_profileId;
}

void SourceGroupsModel::setProfileId(const QString &value)
{
    const auto normalizedValue = value.trimmed();
    if (m_profileId == normalizedValue) {
        return;
    }

    m_profileId = normalizedValue;
    emit profileIdChanged();
    reload();
}

bool SourceGroupsModel::hasGroups() const
{
    return !m_groups.isEmpty();
}

int SourceGroupsModel::selectedCount() const
{
    return static_cast<int>(std::count_if(m_groups.cbegin(), m_groups.cend(), [](const GroupEntry &group) {
        return group.selected;
    }));
}

int SourceGroupsModel::totalCount() const
{
    return static_cast<int>(m_groups.size());
}

bool SourceGroupsModel::loading() const
{
    return m_loading;
}

bool SourceGroupsModel::dirty() const
{
    return !m_draftsByProfile.isEmpty();
}

bool SourceGroupsModel::autoPersist() const
{
    return m_autoPersist;
}

void SourceGroupsModel::setAutoPersist(const bool value)
{
    if (m_autoPersist == value) {
        return;
    }

    m_autoPersist = value;
    emit autoPersistChanged();
}

bool SourceGroupsModel::hideUnchecked() const
{
    return m_profileId.isEmpty() ? false : m_hideUnchecked;
}

void SourceGroupsModel::setHideUnchecked(const bool value)
{
    if (m_profileId.isEmpty() || m_hideUnchecked == value) {
        return;
    }

    const auto wasDirty = dirty();
    m_hideUnchecked = value;
    if (m_autoPersist) {
        m_settings->current().hideUncheckedGroupsByProfile[m_profileId] = value;
        m_settings->save();
        m_draftsByProfile.remove(m_profileId);
    } else {
        updateDraftStateForCurrentProfile();
    }

    rebuildVisibleGroups();
    emit hideUncheckedChanged();
    if (wasDirty != dirty()) {
        emit dirtyChanged();
    }
}

QString SourceGroupsModel::searchText() const
{
    return m_searchText;
}

void SourceGroupsModel::setSearchText(const QString &value)
{
    const auto normalized = value.trimmed();
    if (m_searchText == normalized) {
        return;
    }

    m_searchText = normalized;
    rebuildVisibleGroups();
    emit searchTextChanged();
}

QVariantList SourceGroupsModel::visibleGroups() const
{
    return m_visibleGroups;
}

QStringList SourceGroupsModel::visibleGroupIds() const
{
    return m_visibleGroupIds;
}

QVariantMap SourceGroupsModel::get(const int row) const
{
    if (row < 0 || row >= m_groups.size()) {
        return {};
    }

    const auto &group = m_groups.at(row);
    return QVariantMap {
        { QStringLiteral("id"), group.id },
        { QStringLiteral("name"), group.name },
        { QStringLiteral("count"), group.count },
        { QStringLiteral("selected"), group.selected }
    };
}

void SourceGroupsModel::reload()
{
    const auto generation = ++m_reloadGeneration;

    if (m_profileId.isEmpty()) {
        setLoading(false);
        clearLoadedGroups();
        return;
    }

    const auto profileUuid = parseGuid(m_profileId);
    if (profileUuid.isNull()) {
        setLoading(false);
        clearLoadedGroups();
        return;
    }

    const auto profileId = m_profileId;
    const auto favouriteChannelIds = m_settings->current().favoriteChannelIdsByProfile.value(profileId);
    const auto favouritesId = QString::fromUtf8(kFavouritesGroupId);
    const auto persisted = persistedDraftState(profileId);
    const auto draft = m_draftsByProfile.value(profileId);
    const auto retainFavourites = persisted.hiddenGroups.contains(favouritesId) || persisted.groupOrder.contains(favouritesId)
        || draft.hiddenGroups.contains(favouritesId) || draft.groupOrder.contains(favouritesId);

    setLoading(true);
    m_backgroundTasks.addFuture(QtConcurrent::run([this, generation, profileId, profileUuid,
                                                  favouriteChannelIds, retainFavourites]() {
        ReloadResult result;
        result.profileId = profileId;
        try {
            const auto channels = m_database->loadChannels(profileUuid);
            const auto watchSecondsByChannelId = m_database->loadWatchSecondsByProfile(profileUuid);
            QSet<int> manualFavouriteChannelIds;
            for (const auto channelId : favouriteChannelIds) {
                manualFavouriteChannelIds.insert(channelId);
            }

            QHash<QString, QString> categoryNameById;
            QHash<QString, int> countsByGroupId;
            QStringList discoveredIds;
            QSet<QString> discoveredIdSet;
            auto favouritesCount = 0;

            for (const auto &channel : channels) {
                const auto groupId = normalizeChannelCategoryId(channel.categoryId);
                if (!discoveredIdSet.contains(groupId)) {
                    discoveredIdSet.insert(groupId);
                    discoveredIds.push_back(groupId);
                }
                countsByGroupId[groupId] += 1;
                if (!channel.categoryName.isEmpty() && !categoryNameById.contains(groupId)) {
                    categoryNameById.insert(groupId, channel.categoryName);
                }
                if (watchSecondsByChannelId.value(channel.id, 0) >= kFavouritesEligibleWatchSeconds
                    || manualFavouriteChannelIds.contains(channel.id)) {
                    favouritesCount += 1;
                }
            }

            const auto favouritesGroupId = QString::fromUtf8(kFavouritesGroupId);
            if ((!channels.isEmpty() || retainFavourites) && !discoveredIds.contains(favouritesGroupId)) {
                discoveredIds.prepend(favouritesGroupId);
            }

            for (const auto &groupId : discoveredIds) {
                const auto count =
                    groupId == favouritesGroupId ? favouritesCount : countsByGroupId.value(groupId, 0);
                if (count <= 0 && groupId != favouritesGroupId) {
                    continue;
                }

                result.groups.push_back(ReloadGroup {
                    groupId,
                    groupId == favouritesGroupId
                        ? QStringLiteral("Favourites")
                        : categoryNameById.value(groupId, displayNameForCategoryId(groupId)),
                    count
                });
            }
        } catch (const std::exception &exception) {
            result.errorText = QString::fromUtf8(exception.what());
        } catch (...) {
            result.errorText = QStringLiteral("Unknown source groups reload failure");
        }

        QMetaObject::invokeMethod(
            this,
            [this, generation, result = std::move(result)]() mutable {
                if (generation != m_reloadGeneration || result.profileId != m_profileId) {
                    return;
                }

                if (!result.errorText.isEmpty()) {
                    qWarning() << "Source groups reload failed for profile" << result.profileId << ":" << result.errorText;
                    setLoading(false);
                    return;
                }

                QStringList discoveredIds;
                QHash<QString, ReloadGroup> groupsById;
                for (const auto &group : result.groups) {
                    discoveredIds.push_back(group.id);
                    groupsById.insert(group.id, group);
                }
                const auto persisted = persistedDraftState(result.profileId);
                const auto draft = m_draftsByProfile.constFind(result.profileId);
                const auto currentDraft = draft != m_draftsByProfile.cend()
                    ? std::optional<ReloadState>({ draft->hiddenGroups, draft->groupOrder, draft->hideUnchecked })
                    : std::nullopt;
                auto settingsChanged = false;
                const auto effective = buildEffectiveState(discoveredIds,
                    { persisted.hiddenGroups, persisted.groupOrder, persisted.hideUnchecked }, currentDraft, &settingsChanged);
                // Persist discovery independently of the user's unsaved edits.
                if (settingsChanged) {
                    const auto generated = reconcileSourceGroups(discoveredIds, { persisted.hiddenGroups, persisted.groupOrder });
                    auto &settings = m_settings->current();
                    settings.hiddenGroupsByProfile[result.profileId] = generated.hiddenGroups;
                    settings.groupOrderByProfile[result.profileId] = generated.groupOrder;
                    m_settings->save();
                }
                if (currentDraft.has_value()) {
                    m_draftsByProfile[result.profileId] = { effective.hiddenGroups, effective.groupOrder, effective.hideUnchecked };
                }
                QList<GroupEntry> groups;
                groups.reserve(result.groups.size());
                for (const auto &id : effective.groupOrder) {
                    const auto it = groupsById.constFind(id);
                    if (it != groupsById.cend()) {
                        groups.push_back(GroupEntry { id, it->name, it->count, !effective.hiddenGroups.contains(id) });
                    }
                }
                result.hideUnchecked = effective.hideUnchecked;

                const auto hideUncheckedStateChanged = m_hideUnchecked != result.hideUnchecked;
                beginResetModel();
                m_groups = std::move(groups);
                endResetModel();
                m_hideUnchecked = result.hideUnchecked;
                rebuildVisibleGroups();
                if (hideUncheckedStateChanged) {
                    emit hideUncheckedChanged();
                }
                emit groupsChanged();
                setLoading(false);
            },
            Qt::QueuedConnection);
    }));
}

bool SourceGroupsModel::setGroupSelected(const QString &groupId, const bool selected)
{
    return updateSelection({ groupId.trimmed() }, selected);
}

bool SourceGroupsModel::setGroupsSelected(const QStringList &groupIds, const bool selected)
{
    QStringList normalizedIds;
    normalizedIds.reserve(groupIds.size());
    for (const auto &groupId : groupIds) {
        const auto normalized = groupId.trimmed();
        if (!normalized.isEmpty() && !normalizedIds.contains(normalized)) {
            normalizedIds.push_back(normalized);
        }
    }
    return updateSelection(normalizedIds, selected);
}

bool SourceGroupsModel::selectAll()
{
    QStringList groupIds;
    groupIds.reserve(m_groups.size());
    for (const auto &group : m_groups) {
        groupIds.push_back(group.id);
    }
    return updateSelection(groupIds, true);
}

bool SourceGroupsModel::deselectAll()
{
    QStringList groupIds;
    groupIds.reserve(m_groups.size());
    for (const auto &group : m_groups) {
        groupIds.push_back(group.id);
    }
    return updateSelection(groupIds, false);
}

bool SourceGroupsModel::moveGroup(const QString &groupId, int targetIndex)
{
    if (m_profileId.isEmpty()) {
        return false;
    }

    const auto normalizedId = groupId.trimmed();
    if (normalizedId.isEmpty() || m_groups.isEmpty()) {
        return false;
    }

    const auto sourceIndex = indexOfGroup(normalizedId);
    if (sourceIndex < 0) {
        return false;
    }

    targetIndex = std::clamp(targetIndex, 0, static_cast<int>(m_groups.size()) - 1);
    if (sourceIndex == targetIndex) {
        return false;
    }

    const auto destinationRow = targetIndex > sourceIndex ? targetIndex + 1 : targetIndex;
    if (!beginMoveRows({}, sourceIndex, sourceIndex, {}, destinationRow)) {
        return false;
    }
    m_groups.move(sourceIndex, targetIndex);
    endMoveRows();

    const auto wasDirty = dirty();
    if (m_autoPersist) {
        auto &settings = m_settings->current();
        const auto previousOrder = settings.groupOrderByProfile.value(m_profileId);
        settings.groupOrderByProfile[m_profileId] = mergeOrder(currentDraftState().groupOrder, previousOrder);
        m_settings->save();
        m_draftsByProfile.remove(m_profileId);
    } else {
        updateDraftStateForCurrentProfile();
    }

    emit groupsChanged();
    rebuildVisibleGroups();
    if (wasDirty != dirty()) {
        emit dirtyChanged();
    }
    return true;
}

bool SourceGroupsModel::reorderVisibleGroups(const QStringList &visibleOrderedIds)
{
    if (m_profileId.isEmpty() || m_groups.isEmpty()) {
        return false;
    }

    QStringList normalizedVisibleIds;
    normalizedVisibleIds.reserve(visibleOrderedIds.size());
    for (const auto &groupId : visibleOrderedIds) {
        const auto normalizedId = groupId.trimmed();
        if (normalizedId.isEmpty() || normalizedVisibleIds.contains(normalizedId) || indexOfGroup(normalizedId) < 0) {
            continue;
        }
        normalizedVisibleIds.push_back(normalizedId);
    }
    if (normalizedVisibleIds.isEmpty()) {
        return false;
    }

    QStringList hiddenIds;
    hiddenIds.reserve(m_groups.size());
    for (const auto &group : m_groups) {
        if (!normalizedVisibleIds.contains(group.id)) {
            hiddenIds.push_back(group.id);
        }
    }

    const auto finalOrder = normalizedVisibleIds + hiddenIds;
    if (finalOrder.size() != m_groups.size()) {
        return false;
    }

    QList<GroupEntry> reorderedGroups;
    reorderedGroups.reserve(m_groups.size());
    for (const auto &groupId : finalOrder) {
        const auto sourceIndex = indexOfGroup(groupId);
        if (sourceIndex < 0) {
            return false;
        }
        reorderedGroups.push_back(m_groups.at(sourceIndex));
    }

    auto unchanged = true;
    for (auto index = 0; index < m_groups.size(); ++index) {
        if (m_groups.at(index).id != reorderedGroups.at(index).id) {
            unchanged = false;
            break;
        }
    }
    if (unchanged) {
        return false;
    }

    beginResetModel();
    m_groups = std::move(reorderedGroups);
    endResetModel();

    const auto wasDirty = dirty();
    if (m_autoPersist) {
        auto &settings = m_settings->current();
        const auto previousOrder = settings.groupOrderByProfile.value(m_profileId);
        settings.groupOrderByProfile[m_profileId] = mergeOrder(currentDraftState().groupOrder, previousOrder);
        m_settings->save();
        m_draftsByProfile.remove(m_profileId);
    } else {
        updateDraftStateForCurrentProfile();
    }

    emit groupsChanged();
    rebuildVisibleGroups();
    if (wasDirty != dirty()) {
        emit dirtyChanged();
    }
    return true;
}

void SourceGroupsModel::saveDraftChanges()
{
    if (m_draftsByProfile.isEmpty()) {
        return;
    }

    auto &settings = m_settings->current();
    for (auto it = m_draftsByProfile.cbegin(); it != m_draftsByProfile.cend(); ++it) {
        const auto &profileId = it.key();
        const auto &draft = it.value();
        const auto persistedOrder = settings.groupOrderByProfile.value(profileId);
        const auto persistedHidden = settings.hiddenGroupsByProfile.value(profileId);
        settings.groupOrderByProfile[profileId] = mergeOrder(draft.groupOrder, persistedOrder);
        settings.hiddenGroupsByProfile[profileId] = mergeHiddenGroups(HiddenGroupMergeData {
            draft.hiddenGroups,
            persistedHidden,
            draft.groupOrder });
        settings.hideUncheckedGroupsByProfile[profileId] = draft.hideUnchecked;
    }

    m_settings->save();
    const auto wasDirty = dirty();
    m_draftsByProfile.clear();
    if (wasDirty) {
        emit dirtyChanged();
    }
}

void SourceGroupsModel::discardDraftChanges()
{
    const auto wasDirty = dirty();
    if (!wasDirty) {
        reload();
        return;
    }

    m_draftsByProfile.clear();
    emit dirtyChanged();
    reload();
}

int SourceGroupsModel::indexOfGroup(const QString &groupId) const
{
    for (auto index = 0; index < m_groups.size(); ++index) {
        if (m_groups.at(index).id == groupId) {
            return index;
        }
    }

    return -1;
}

bool SourceGroupsModel::updateSelection(const QStringList &groupIds, const bool selected)
{
    if (m_profileId.isEmpty() || groupIds.isEmpty()) {
        return false;
    }

    QList<int> changedRows;
    changedRows.reserve(groupIds.size());
    auto changed = false;
    for (const auto &groupId : groupIds) {
        const auto row = indexOfGroup(groupId);
        if (row < 0 || m_groups[row].selected == selected) {
            continue;
        }

        m_groups[row].selected = selected;
        changedRows.push_back(row);
        changed = true;
    }

    if (!changed) {
        return false;
    }

    const auto wasDirty = dirty();
    if (m_autoPersist) {
        auto &settings = m_settings->current();
        const auto current = currentDraftState();
        settings.hiddenGroupsByProfile[m_profileId] = mergeHiddenGroups({ current.hiddenGroups,
            settings.hiddenGroupsByProfile.value(m_profileId), current.groupOrder });
        m_settings->save();
        m_draftsByProfile.remove(m_profileId);
    } else {
        updateDraftStateForCurrentProfile();
    }

    for (const auto row : changedRows) {
        emit dataChanged(index(row, 0), index(row, 0), { SelectedRole });
    }
    rebuildVisibleGroups();
    emit groupsChanged();
    emit selectionEdited(m_profileId);
    if (wasDirty != dirty()) {
        emit dirtyChanged();
    }
    return true;
}

void SourceGroupsModel::setLoading(const bool value)
{
    if (m_loading == value) {
        return;
    }

    m_loading = value;
    emit loadingChanged();
}

void SourceGroupsModel::clearLoadedGroups()
{
    const auto hadGroups = !m_groups.isEmpty();
    const auto hideUncheckedStateChanged = m_hideUnchecked;
    beginResetModel();
    m_groups.clear();
    endResetModel();
    m_searchText.clear();
    rebuildVisibleGroups();
    m_hideUnchecked = false;
    if (hideUncheckedStateChanged) {
        emit hideUncheckedChanged();
    }
    if (hadGroups || hideUncheckedStateChanged) {
        emit groupsChanged();
    }
}

void SourceGroupsModel::applyDraftStateForCurrentProfile()
{
    reload();
}

void SourceGroupsModel::rebuildVisibleGroups()
{
    QVariantList visibleGroups;
    QStringList visibleIds;
    const auto query = m_searchText.trimmed().toLower();
    for (const auto &group : m_groups) {
        const auto matchesSearch = query.isEmpty() || group.name.toLower().contains(query);
        const auto passesHideFilter = !m_hideUnchecked || group.selected;
        if (!matchesSearch || !passesHideFilter) {
            continue;
        }

        visibleGroups.push_back(QVariantMap {
            { QStringLiteral("id"), group.id },
            { QStringLiteral("name"), group.name },
            { QStringLiteral("count"), group.count },
            { QStringLiteral("selected"), group.selected }
        });
        visibleIds.push_back(group.id);
    }

    m_visibleGroups = std::move(visibleGroups);
    m_visibleGroupIds = std::move(visibleIds);
    emit visibleGroupsChanged();
}

void SourceGroupsModel::updateDraftStateForCurrentProfile()
{
    if (m_profileId.isEmpty()) {
        return;
    }

    const auto currentState = currentDraftState();
    if (draftStatesEqual(currentState, persistedDraftState(m_profileId))) {
        m_draftsByProfile.remove(m_profileId);
        return;
    }

    m_draftsByProfile.insert(m_profileId, currentState);
}

void SourceGroupsModel::clearDraftStateIfPersisted(const QString &profileId)
{
    if (!m_draftsByProfile.contains(profileId)) {
        return;
    }

    if (draftStatesEqual(m_draftsByProfile.value(profileId), persistedDraftState(profileId))) {
        m_draftsByProfile.remove(profileId);
    }
}

SourceGroupsModel::DraftState SourceGroupsModel::currentDraftState() const
{
    DraftState state;
    state.hideUnchecked = m_hideUnchecked;
    state.groupOrder.reserve(m_groups.size());
    for (const auto &group : m_groups) {
        state.groupOrder.push_back(group.id);
        if (!group.selected) {
            state.hiddenGroups.push_back(group.id);
        }
    }
    return state;
}

SourceGroupsModel::DraftState SourceGroupsModel::persistedDraftState(const QString &profileId) const
{
    return DraftState {
        m_settings->current().hiddenGroupsByProfile.value(profileId),
        m_settings->current().groupOrderByProfile.value(profileId),
        m_settings->current().hideUncheckedGroupsByProfile.value(profileId, false)
    };
}

bool SourceGroupsModel::draftStatesEqual(const DraftState &left, const DraftState &right) const
{
    return left.hiddenGroups == right.hiddenGroups
        && left.groupOrder == right.groupOrder
        && left.hideUnchecked == right.hideUnchecked;
}

} // namespace OKILTV::App
