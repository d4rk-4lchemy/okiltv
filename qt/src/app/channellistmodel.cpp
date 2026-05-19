#include "channellistmodel.h"

#include <algorithm>

namespace OKILTV::App {

using namespace Core;

namespace {

int displayNumberForChannel(const Channel &channel, const int sortedIndex)
{
    if (channel.source == ChannelSource::M3U) {
        return std::max(1, channel.sortOrder);
    }
    if (channel.sortOrder > 0) {
        return channel.sortOrder;
    }
    return sortedIndex + 1;
}

} // namespace

ChannelListModel::ChannelListModel(SettingsManager *settings, QObject *parent)
    : QAbstractListModel(parent)
    , m_settings(settings)
{
}

int ChannelListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_filteredRows.size());
}

QVariant ChannelListModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_filteredRows.size()) {
        return {};
    }

    const auto &channel = m_allChannels.at(m_filteredRows.at(index.row()));
    switch (role) {
    case IdRole:
        return channel.id;
    case NameRole:
        return channel.name;
    case CategoryIdRole:
        return channel.categoryId;
    case TvgIdRole:
        return channel.tvgId;
    case TvgNameRole:
        return channel.tvgName;
    case IconUrlRole:
        return channel.iconUrl;
    case CachedIconPathRole:
        return channel.cachedIconPath;
    case SortOrderRole:
        return channel.sortOrder;
    case SourceRole:
        return channelSourceToString(channel.source);
    case IsSelectedRole:
        return channel.id == m_selectedChannelId;
    case IsFavoriteRole:
        return isFavorite(channel.id);
    case IsDvrRecordingRole:
        return m_dvrRecordingChannelIdsByProfile.value(m_activeProfileId).contains(channel.id);
    case CurrentProgramTitleRole:
        return m_currentProgramInfoByChannelId.value(channel.id).value(QStringLiteral("title")).toString();
    case CurrentProgramTimeRangeRole:
        return m_currentProgramInfoByChannelId.value(channel.id).value(QStringLiteral("timeRange")).toString();
    default:
        return {};
    }
}

QHash<int, QByteArray> ChannelListModel::roleNames() const
{
    return {
        { IdRole, "id" },
        { NameRole, "name" },
        { CategoryIdRole, "categoryId" },
        { TvgIdRole, "tvgId" },
        { TvgNameRole, "tvgName" },
        { IconUrlRole, "iconUrl" },
        { CachedIconPathRole, "cachedIconPath" },
        { SortOrderRole, "sortOrder" },
        { SourceRole, "source" },
        { IsSelectedRole, "isSelected" },
        { IsFavoriteRole, "isFavorite" },
        { IsDvrRecordingRole, "isDvrRecording" },
        { CurrentProgramTitleRole, "currentProgramTitle" },
        { CurrentProgramTimeRangeRole, "currentProgramTimeRange" }
    };
}

QString ChannelListModel::searchText() const
{
    return m_searchText;
}

void ChannelListModel::setSearchText(const QString &value)
{
    if (m_searchText == value) {
        return;
    }

    m_searchText = value;
    emit searchTextChanged();
    rebuildFilter();
}

int ChannelListModel::filteredCount() const
{
    return static_cast<int>(m_filteredRows.size());
}

int ChannelListModel::totalCount() const
{
    return static_cast<int>(m_allChannels.size());
}

QString ChannelListModel::selectedCategoryId() const
{
    return m_selectedCategoryId;
}

void ChannelListModel::setSelectedCategoryId(const QString &value)
{
    const auto normalized = value.trimmed() == QString::fromUtf8(kFavouritesCategoryId) || value.trimmed().isEmpty()
        ? value.trimmed()
        : normalizeChannelCategoryId(value);
    if (m_selectedCategoryId == normalized) {
        return;
    }

    m_selectedCategoryId = normalized;
    emit selectedCategoryIdChanged();
    rebuildFilter();
}

int ChannelListModel::selectedChannelId() const
{
    return m_selectedChannelId;
}

QVariantList ChannelListModel::categories() const
{
    rebuildCategoriesCache();
    return m_cachedCategories;
}

QString ChannelListModel::activeProfileId() const
{
    return m_activeProfileId;
}

void ChannelListModel::setChannels(const QList<Channel> &channels, const QList<ChannelCategory> &categories)
{
    beginResetModel();
    m_allChannels = channels;
    for (auto &channel : m_allChannels) {
        channel.categoryId = normalizeChannelCategoryId(channel.categoryId);
    }
    std::sort(
        m_allChannels.begin(),
        m_allChannels.end(),
        [](const Channel &left, const Channel &right) {
            if (left.sortOrder != right.sortOrder) {
                return left.sortOrder < right.sortOrder;
            }
            const auto nameCompare = QString::localeAwareCompare(left.name, right.name);
            if (nameCompare != 0) {
                return nameCompare < 0;
            }
            return left.id < right.id;
        });
    m_categories = categories;
    for (auto &category : m_categories) {
        category.id = normalizeChannelCategoryId(category.id);
        if (category.name.trimmed().isEmpty()) {
            category.name = displayNameForCategoryId(category.id);
        }
    }
    m_selectedChannelId = -1;
    m_currentProgramInfoByChannelId.clear();
    m_watchSecondsByChannelId.clear();
    reloadManualFavourites();
    invalidateCategoriesCache();
    endResetModel();
    emit totalCountChanged();
    emit selectedChannelIdChanged();
    emit categoriesChanged();
    rebuildFilter();
}

void ChannelListModel::clear()
{
    beginResetModel();
    m_allChannels.clear();
    m_categories.clear();
    m_watchSecondsByChannelId.clear();
    m_manualFavouriteChannelIds.clear();
    m_filteredRows.clear();
    m_searchText.clear();
    m_selectedCategoryId.clear();
    m_selectedChannelId = -1;
    m_currentProgramInfoByChannelId.clear();
    m_dvrRecordingChannelIdsByProfile.clear();
    invalidateCategoriesCache();
    endResetModel();
    emit totalCountChanged();
    emit filteredCountChanged();
    emit categoriesChanged();
    emit searchTextChanged();
    emit selectedCategoryIdChanged();
    emit selectedChannelIdChanged();
}

void ChannelListModel::setCachedIconPath(const int channelId, const QString &path)
{
    for (auto index = 0; index < m_allChannels.size(); ++index) {
        if (m_allChannels[index].id != channelId) {
            continue;
        }

        m_allChannels[index].cachedIconPath = path;
        const auto row = filteredRowForChannel(channelId);
        if (row >= 0) {
            emit dataChanged(this->index(row, 0), this->index(row, 0), { CachedIconPathRole });
        }
        return;
    }
}

void ChannelListModel::setActiveProfileId(const QString &profileId)
{
    if (m_activeProfileId == profileId) {
        return;
    }

    const auto hadRows = !m_filteredRows.isEmpty();
    const auto previousProfileId = m_activeProfileId;
    m_activeProfileId = profileId;
    m_watchSecondsByChannelId.clear();
    reloadManualFavourites();
    invalidateCategoriesCache();
    if (!m_selectedCategoryId.isEmpty()) {
        const auto ids = orderedVisibleCategoryIds();
        if (!ids.contains(m_selectedCategoryId)) {
            m_selectedCategoryId.clear();
            emit selectedCategoryIdChanged();
        }
    }
    emit activeProfileIdChanged();
    emit categoriesChanged();
    rebuildFilter();
    if (hadRows && previousProfileId != m_activeProfileId && !m_filteredRows.isEmpty()) {
        emit dataChanged(
            index(0, 0),
            index(static_cast<int>(m_filteredRows.size()) - 1, 0),
            { IsDvrRecordingRole });
    }
}

void ChannelListModel::setWatchSeconds(const QHash<int, qint64> &watchSecondsByChannelId)
{
    if (m_watchSecondsByChannelId == watchSecondsByChannelId) {
        return;
    }

    const auto previousWatchSecondsByChannelId = m_watchSecondsByChannelId;
    const auto isAutoFavouriteEligibleFor = [](const QHash<int, qint64> &watchSecondsMap, const int channelId) {
        return std::max<qint64>(0, watchSecondsMap.value(channelId, 0)) / 60 >= kFavouritesEligibilityMinutes;
    };

    auto favouritesEligibilityChanged = false;
    QSet<int> favouriteRoleChangedChannelIds;
    for (const auto &channel : m_allChannels) {
        const auto wasAutoFavourite = isAutoFavouriteEligibleFor(previousWatchSecondsByChannelId, channel.id);
        const auto isAutoFavourite = isAutoFavouriteEligibleFor(watchSecondsByChannelId, channel.id);
        if (wasAutoFavourite == isAutoFavourite) {
            continue;
        }

        favouritesEligibilityChanged = true;
        if (!m_manualFavouriteChannelIds.contains(channel.id)) {
            favouriteRoleChangedChannelIds.insert(channel.id);
        }
    }

    m_watchSecondsByChannelId = watchSecondsByChannelId;
    const auto favouritesCategoryId = QString::fromUtf8(kFavouritesCategoryId);
    if (m_selectedCategoryId == favouritesCategoryId) {
        invalidateCategoriesCache();
        emit categoriesChanged();
        rebuildFilter();
        return;
    }

    if (favouritesEligibilityChanged) {
        invalidateCategoriesCache();
        emit categoriesChanged();
    }

    for (auto row = 0; row < m_filteredRows.size(); ++row) {
        const auto channelId = m_allChannels.at(m_filteredRows.at(row)).id;
        if (!favouriteRoleChangedChannelIds.contains(channelId)) {
            continue;
        }

        emit dataChanged(index(row, 0), index(row, 0), { IsFavoriteRole });
    }
}

void ChannelListModel::setDvrRecordingChannelsForProfile(const QString &profileId, const QList<int> &channelIds)
{
    const auto normalizedProfileId = profileId.trimmed();
    QSet<int> normalizedIds;
    for (const auto channelId : channelIds) {
        if (channelId >= 0) {
            normalizedIds.insert(channelId);
        }
    }

    const auto previousIds = m_dvrRecordingChannelIdsByProfile.value(normalizedProfileId);
    if (previousIds == normalizedIds) {
        return;
    }

    m_dvrRecordingChannelIdsByProfile.insert(normalizedProfileId, normalizedIds);
    if (normalizedProfileId != m_activeProfileId || m_filteredRows.isEmpty()) {
        return;
    }

    emit dataChanged(
        index(0, 0),
        index(static_cast<int>(m_filteredRows.size()) - 1, 0),
        { IsDvrRecordingRole });
}

void ChannelListModel::setCurrentProgramInfo(const QHash<int, QVariantMap> &infoByChannelId)
{
    QList<int> changedRows;
    changedRows.reserve(m_filteredRows.size());

    for (auto row = 0; row < m_filteredRows.size(); ++row) {
        const auto channelId = m_allChannels.at(m_filteredRows.at(row)).id;
        if (m_currentProgramInfoByChannelId.value(channelId) != infoByChannelId.value(channelId)) {
            changedRows.push_back(row);
        }
    }

    m_currentProgramInfoByChannelId = infoByChannelId;
    for (const auto row : changedRows) {
        emit dataChanged(
            index(row, 0),
            index(row, 0),
            { CurrentProgramTitleRole, CurrentProgramTimeRangeRole });
    }
}

std::optional<Channel> ChannelListModel::selectedChannel() const
{
    return channelById(m_selectedChannelId);
}

std::optional<Channel> ChannelListModel::channelById(const int channelId) const
{
    for (const auto &channel : m_allChannels) {
        if (channel.id == channelId) {
            return channel;
        }
    }

    return std::nullopt;
}

QList<Channel> ChannelListModel::allChannels() const
{
    return m_allChannels;
}

QVariantMap ChannelListModel::currentChannel() const
{
    const auto channel = selectedChannel();
    return channel.has_value() ? toVariantMap(channel.value()) : QVariantMap {};
}

bool ChannelListModel::activateById(const int channelId)
{
    if (!selectById(channelId)) {
        return false;
    }

    emit channelActivated(channelId);
    return true;
}

bool ChannelListModel::activateByDisplayNumber(const int displayNumber)
{
    if (displayNumber <= 0) {
        return false;
    }

    for (auto index = 0; index < m_allChannels.size(); ++index) {
        const auto &channel = m_allChannels.at(index);
        if (displayNumberForChannel(channel, index) != displayNumber) {
            continue;
        }
        return activateById(channel.id);
    }

    return false;
}

bool ChannelListModel::activateAt(const int row)
{
    if (row < 0 || row >= m_filteredRows.size()) {
        return false;
    }

    return activateById(m_allChannels.at(m_filteredRows.at(row)).id);
}

bool ChannelListModel::activateRelative(const int delta)
{
    if (m_filteredRows.isEmpty()) {
        return false;
    }

    auto row = filteredRowForChannel(m_selectedChannelId);
    if (row < 0) {
        row = 0;
    } else {
        row = std::clamp(row + delta, 0, static_cast<int>(m_filteredRows.size()) - 1);
    }

    return activateAt(row);
}

int ChannelListModel::rowForChannelId(const int channelId) const
{
    return filteredRowForChannel(channelId);
}

bool ChannelListModel::selectAt(const int row)
{
    if (row < 0 || row >= m_filteredRows.size()) {
        return false;
    }

    return selectById(m_allChannels.at(m_filteredRows.at(row)).id);
}

bool ChannelListModel::selectRelativeWrapped(const int delta)
{
    if (m_filteredRows.isEmpty()) {
        return false;
    }

    auto row = filteredRowForChannel(m_selectedChannelId);
    if (row < 0) {
        row = 0;
    } else if (delta != 0) {
        const auto size = static_cast<int>(m_filteredRows.size());
        row = (row + (delta % size) + size) % size;
    }

    return selectAt(row);
}

bool ChannelListModel::selectById(const int channelId)
{
    if (!channelById(channelId).has_value()) {
        return false;
    }

    if (m_selectedChannelId == channelId) {
        return true;
    }

    const auto previous = m_selectedChannelId;
    m_selectedChannelId = channelId;
    emitSelectionChanged(previous, m_selectedChannelId);
    return true;
}

bool ChannelListModel::isFavorite(const int channelId) const
{
    return isFavouriteEligible(channelId);
}

bool ChannelListModel::toggleFavorite(const int channelId)
{
    if (m_activeProfileId.isEmpty() || !channelById(channelId).has_value()) {
        return false;
    }

    const auto previousRow = filteredRowForChannel(channelId);
    const auto favouritesCategoryId = QString::fromUtf8(kFavouritesCategoryId);
    const auto favouritesCategorySelected = m_selectedCategoryId == favouritesCategoryId;

    auto &favouriteChannelIds = m_settings->current().favoriteChannelIdsByProfile[m_activeProfileId];
    const auto currentlyFavourite = m_manualFavouriteChannelIds.contains(channelId);
    favouriteChannelIds.removeAll(channelId);
    if (!currentlyFavourite) {
        favouriteChannelIds.push_back(channelId);
        m_manualFavouriteChannelIds.insert(channelId);
    } else {
        m_manualFavouriteChannelIds.remove(channelId);
    }

    m_settings->save();
    invalidateCategoriesCache();
    emit categoriesChanged();
    if (favouritesCategorySelected) {
        rebuildFilter();
        return true;
    }

    if (previousRow >= 0) {
        emit dataChanged(index(previousRow, 0), index(previousRow, 0), { IsFavoriteRole });
    }
    return true;
}

bool ChannelListModel::setCategoryHidden(const QString &categoryId, const bool hidden)
{
    const auto normalizedCategoryId = normalizeChannelCategoryId(categoryId);
    if (m_activeProfileId.isEmpty() || normalizedCategoryId.isEmpty()) {
        return false;
    }

    auto &hiddenGroups = m_settings->current().hiddenGroupsByProfile[m_activeProfileId];
    const auto contains = hiddenGroups.contains(normalizedCategoryId);
    if (hidden && !contains) {
        hiddenGroups.push_back(normalizedCategoryId);
    } else if (!hidden && contains) {
        hiddenGroups.removeAll(normalizedCategoryId);
    } else {
        return false;
    }

    if (m_selectedCategoryId == normalizedCategoryId && hidden) {
        m_selectedCategoryId.clear();
        emit selectedCategoryIdChanged();
    }

    m_settings->save();
    invalidateCategoriesCache();
    emit categoriesChanged();
    rebuildFilter();
    return true;
}

bool ChannelListModel::moveCategory(const QString &categoryId, int targetIndex)
{
    const auto normalizedCategoryId = normalizeChannelCategoryId(categoryId);
    if (m_activeProfileId.isEmpty() || normalizedCategoryId.isEmpty()) {
        return false;
    }

    auto order = orderedVisibleCategoryIds();
    const auto currentIndex = order.indexOf(normalizedCategoryId);
    if (currentIndex < 0) {
        return false;
    }

    order.removeAt(currentIndex);
    targetIndex = std::clamp(targetIndex, 0, static_cast<int>(order.size()));
    order.insert(targetIndex, normalizedCategoryId);
    m_settings->current().groupOrderByProfile[m_activeProfileId] = order;
    m_settings->save();
    invalidateCategoriesCache();
    emit categoriesChanged();
    return true;
}

void ChannelListModel::refreshFilter()
{
    if (!m_selectedCategoryId.isEmpty()) {
        const auto ids = orderedVisibleCategoryIds();
        if (!ids.contains(m_selectedCategoryId)) {
            m_selectedCategoryId.clear();
            emit selectedCategoryIdChanged();
        }
    }
    rebuildFilter();
    invalidateCategoriesCache();
    emit categoriesChanged();
}

void ChannelListModel::invalidateCategoriesCache()
{
    m_categoriesCacheDirty = true;
}

void ChannelListModel::rebuildCategoriesCache() const
{
    if (!m_categoriesCacheDirty) {
        return;
    }

    QHash<QString, int> countByCategoryId;
    QHash<QString, QString> nameByCategoryId;
    auto favouritesCount = 0;
    for (const auto &category : m_categories) {
        nameByCategoryId.insert(
            category.id,
            category.name.trimmed().isEmpty() ? displayNameForCategoryId(category.id) : category.name);
    }
    for (const auto &channel : m_allChannels) {
        countByCategoryId[channel.categoryId] += 1;
        if (isFavouriteEligible(channel.id)) {
            favouritesCount += 1;
        }
    }

    QVariantList categoryList;
    for (const auto &categoryId : orderedVisibleCategoryIds()) {
        if (categoryId == QString::fromUtf8(kFavouritesCategoryId)) {
            categoryList.push_back(QVariantMap {
                { QStringLiteral("id"), QString::fromUtf8(kFavouritesCategoryId) },
                { QStringLiteral("name"), QStringLiteral("Favourites") },
                { QStringLiteral("count"), favouritesCount },
                { QStringLiteral("isFavorites"), true }
            });
            continue;
        }

        categoryList.push_back(QVariantMap {
            { QStringLiteral("id"), categoryId },
            { QStringLiteral("name"), nameByCategoryId.value(categoryId, displayNameForCategoryId(categoryId)) },
            { QStringLiteral("count"), countByCategoryId.value(categoryId, 0) },
            { QStringLiteral("isFavorites"), false }
        });
    }

    m_cachedCategories = std::move(categoryList);
    m_categoriesCacheDirty = false;
}

void ChannelListModel::rebuildFilter()
{
    beginResetModel();
    m_filteredRows.clear();
    const auto query = m_searchText.trimmed();
    const auto favouritesCategoryId = QString::fromUtf8(kFavouritesCategoryId);
    for (auto index = 0; index < m_allChannels.size(); ++index) {
        const auto &channel = m_allChannels.at(index);
        if (m_selectedCategoryId == favouritesCategoryId && !isFavouriteEligible(channel.id)) {
            continue;
        }
        if (m_selectedCategoryId.isEmpty() && isCategoryHidden(channel.categoryId)) {
            continue;
        }
        if (!m_selectedCategoryId.isEmpty()
            && m_selectedCategoryId != favouritesCategoryId
            && channel.categoryId != m_selectedCategoryId) {
            continue;
        }
        if (!query.isEmpty()
            && !channel.name.contains(query, Qt::CaseInsensitive)
            && !channel.tvgName.contains(query, Qt::CaseInsensitive)) {
            continue;
        }
        m_filteredRows.push_back(index);
    }

    if (m_selectedCategoryId == favouritesCategoryId) {
        std::sort(
            m_filteredRows.begin(),
            m_filteredRows.end(),
            [this](const int leftIndex, const int rightIndex) {
                const auto &leftChannel = m_allChannels.at(leftIndex);
                const auto &rightChannel = m_allChannels.at(rightIndex);
                const auto leftMinutes = watchMinutesForChannel(leftChannel.id);
                const auto rightMinutes = watchMinutesForChannel(rightChannel.id);
                if (leftMinutes != rightMinutes) {
                    return leftMinutes > rightMinutes;
                }
                return leftIndex < rightIndex;
            });
    }

    endResetModel();
    emit filteredCountChanged();
}

int ChannelListModel::filteredRowForChannel(const int channelId) const
{
    for (auto row = 0; row < m_filteredRows.size(); ++row) {
        if (m_allChannels.at(m_filteredRows.at(row)).id == channelId) {
            return row;
        }
    }

    return -1;
}

void ChannelListModel::emitSelectionChanged(const int previousChannelId, const int nextChannelId)
{
    const auto previousRow = filteredRowForChannel(previousChannelId);
    const auto nextRow = filteredRowForChannel(nextChannelId);
    if (previousRow >= 0) {
        emit dataChanged(index(previousRow, 0), index(previousRow, 0), { IsSelectedRole });
    }
    if (nextRow >= 0) {
        emit dataChanged(index(nextRow, 0), index(nextRow, 0), { IsSelectedRole });
    }
    emit selectedChannelIdChanged();
}

QStringList ChannelListModel::orderedVisibleCategoryIds() const
{
    const auto favouritesCategoryId = QString::fromUtf8(kFavouritesCategoryId);
    QStringList sourceIds;
    sourceIds.reserve(m_categories.size() + 1);

    if (!m_activeProfileId.isEmpty() && !isCategoryHidden(favouritesCategoryId)) {
        sourceIds.push_back(favouritesCategoryId);
    }

    for (const auto &category : m_categories) {
        if (category.id.isEmpty() || isCategoryHidden(category.id)) {
            continue;
        }
        if (!sourceIds.contains(category.id)) {
            sourceIds.push_back(category.id);
        }
    }

    const auto orderedIds = m_settings->current().groupOrderByProfile.value(m_activeProfileId);
    QStringList result;
    result.reserve(sourceIds.size());
    for (const auto &orderedId : orderedIds) {
        if (sourceIds.contains(orderedId) && !result.contains(orderedId)) {
            result.push_back(orderedId);
        }
    }
    for (const auto &id : sourceIds) {
        if (result.contains(id)) {
            continue;
        }
        if (id == favouritesCategoryId && !orderedIds.contains(id)) {
            result.prepend(id);
            continue;
        }
        result.push_back(id);
    }
    return result;
}

bool ChannelListModel::isCategoryHidden(const QString &categoryId) const
{
    return m_settings->current().hiddenGroupsByProfile.value(m_activeProfileId).contains(categoryId);
}

bool ChannelListModel::isAutoFavouriteEligible(const int channelId) const
{
    return watchMinutesForChannel(channelId) >= kFavouritesEligibilityMinutes;
}

bool ChannelListModel::isFavouriteEligible(const int channelId) const
{
    return isAutoFavouriteEligible(channelId) || m_manualFavouriteChannelIds.contains(channelId);
}

void ChannelListModel::reloadManualFavourites()
{
    m_manualFavouriteChannelIds.clear();
    if (m_activeProfileId.isEmpty()) {
        return;
    }

    const auto favouriteChannelIds = m_settings->current().favoriteChannelIdsByProfile.value(m_activeProfileId);
    for (const auto channelId : favouriteChannelIds) {
        m_manualFavouriteChannelIds.insert(channelId);
    }
}

qint64 ChannelListModel::watchSecondsForChannel(const int channelId) const
{
    return std::max<qint64>(0, m_watchSecondsByChannelId.value(channelId, 0));
}

qint64 ChannelListModel::watchMinutesForChannel(const int channelId) const
{
    return watchSecondsForChannel(channelId) / 60;
}

} // namespace OKILTV::App
