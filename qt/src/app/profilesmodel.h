#pragma once

#include "../core/settingsmanager.h"

#include <QAbstractListModel>

namespace OKILTV::App {

class ProfilesModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString activeProfileId READ activeProfileId NOTIFY activeProfileIdChanged)

public:
    enum Roles
    {
        IdRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        TypeLabelRole,
        IsActiveRole,
        LastRefreshedRole
    };
    Q_ENUM(Roles)

    explicit ProfilesModel(Core::SettingsManager *settings, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString activeProfileId() const;

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QString addXtreamProfile(
        const QString &name,
        const QString &baseUrl,
        const QString &username,
        const QString &password,
        const QString &xmltvUrl = {},
        int autoRefreshIntervalHours = 24);
    Q_INVOKABLE QString addM3uUrlProfile(
        const QString &name,
        const QString &m3uUrl,
        const QString &xmltvUrl = {},
        int autoRefreshIntervalHours = 24);
    Q_INVOKABLE QString addM3uFileProfile(
        const QString &name,
        const QString &filePath,
        const QString &xmltvUrl = {});
    Q_INVOKABLE bool replaceProfile(const QString &profileId, const QVariantMap &changes);
    Q_INVOKABLE bool removeProfile(const QString &profileId);
    Q_INVOKABLE bool selectProfile(const QString &profileId);
    Q_INVOKABLE void reload();

signals:
    void activeProfileIdChanged();
    void profileSelectionRequested(const QString &profileId);

private:
    int indexOfProfile(const QUuid &profileId) const;

    Core::SettingsManager *m_settings;
};

} // namespace OKILTV::App
