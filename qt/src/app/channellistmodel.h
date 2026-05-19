#pragma once

#include "../core/models.h"
#include "../core/settingsmanager.h"

#include <QAbstractListModel>
#include <QHash>
#include <QSet>

#include <optional>

namespace OKILTV::App {

class ChannelListModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(int filteredCount READ filteredCount NOTIFY filteredCountChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY totalCountChanged)
    Q_PROPERTY(QString selectedCategoryId READ selectedCategoryId WRITE setSelectedCategoryId NOTIFY selectedCategoryIdChanged)
    Q_PROPERTY(int selectedChannelId READ selectedChannelId NOTIFY selectedChannelIdChanged)
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
    Q_PROPERTY(QString activeProfileId READ activeProfileId NOTIFY activeProfileIdChanged)

public:
    enum Roles
    {
        IdRole = Qt::UserRole + 1,
        NameRole,
        CategoryIdRole,
        TvgIdRole,
        TvgNameRole,
        IconUrlRole,
        CachedIconPathRole,
        SortOrderRole,
        SourceRole,
        IsSelectedRole,
        IsFavoriteRole,
        IsDvrRecordingRole,
        CurrentProgramTitleRole,
        CurrentProgramTimeRangeRole
    };
    Q_ENUM(Roles)

    explicit ChannelListModel(Core::SettingsManager *settings, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString searchText() const;
    void setSearchText(const QString &value);
    int filteredCount() const;
    int totalCount() const;

    QString selectedCategoryId() const;
    void setSelectedCategoryId(const QString &value);

    int selectedChannelId() const;
    QVariantList categories() const;
    QString activeProfileId() const;

    void setChannels(const QList<Core::Channel> &channels, const QList<Core::ChannelCategory> &categories);
    void clear();
    void setCachedIconPath(int channelId, const QString &path);
    void setActiveProfileId(const QString &profileId);
    void setWatchSeconds(const QHash<int, qint64> &watchSecondsByChannelId);
    void setDvrRecordingChannelsForProfile(const QString &profileId, const QList<int> &channelIds);
    void setCurrentProgramInfo(const QHash<int, QVariantMap> &infoByChannelId);

    std::optional<Core::Channel> selectedChannel() const;
    std::optional<Core::Channel> channelById(int channelId) const;
    QList<Core::Channel> allChannels() const;

    Q_INVOKABLE QVariantMap currentChannel() const;
    Q_INVOKABLE bool activateById(int channelId);
    Q_INVOKABLE bool activateByDisplayNumber(int displayNumber);
    Q_INVOKABLE bool activateAt(int row);
    Q_INVOKABLE bool activateRelative(int delta);
    Q_INVOKABLE int rowForChannelId(int channelId) const;
    Q_INVOKABLE bool selectAt(int row);
    Q_INVOKABLE bool selectRelativeWrapped(int delta);
    Q_INVOKABLE bool selectById(int channelId);
    Q_INVOKABLE bool isFavorite(int channelId) const;
    Q_INVOKABLE bool toggleFavorite(int channelId);
    Q_INVOKABLE bool setCategoryHidden(const QString &categoryId, bool hidden);
    Q_INVOKABLE bool moveCategory(const QString &categoryId, int targetIndex);
    Q_INVOKABLE void refreshFilter();

signals:
    void searchTextChanged();
    void filteredCountChanged();
    void totalCountChanged();
    void selectedCategoryIdChanged();
    void selectedChannelIdChanged();
    void categoriesChanged();
    void activeProfileIdChanged();
    void channelActivated(int channelId);

private:
    static constexpr auto kFavouritesCategoryId = "__favourites__";
    static constexpr qint64 kFavouritesEligibilityMinutes = 360;

    void rebuildFilter();
    void invalidateCategoriesCache();
    void rebuildCategoriesCache() const;
    int filteredRowForChannel(int channelId) const;
    void emitSelectionChanged(int previousChannelId, int nextChannelId);
    QStringList orderedVisibleCategoryIds() const;
    bool isCategoryHidden(const QString &categoryId) const;
    bool isAutoFavouriteEligible(int channelId) const;
    bool isFavouriteEligible(int channelId) const;
    void reloadManualFavourites();
    qint64 watchSecondsForChannel(int channelId) const;
    qint64 watchMinutesForChannel(int channelId) const;

    Core::SettingsManager *m_settings;
    QList<Core::Channel> m_allChannels;
    QList<Core::ChannelCategory> m_categories;
    QHash<int, qint64> m_watchSecondsByChannelId;
    QSet<int> m_manualFavouriteChannelIds;
    QList<int> m_filteredRows;
    QString m_searchText;
    QString m_selectedCategoryId;
    int m_selectedChannelId { -1 };
    QString m_activeProfileId;
    QHash<int, QVariantMap> m_currentProgramInfoByChannelId;
    QHash<QString, QSet<int>> m_dvrRecordingChannelIdsByProfile;
    mutable QVariantList m_cachedCategories;
    mutable bool m_categoriesCacheDirty { true };
};

} // namespace OKILTV::App
