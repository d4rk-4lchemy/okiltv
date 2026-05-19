#pragma once

#include "../core/portablebootstrap.h"

#include <QObject>

namespace OKILTV::App {

class PortableRuntimeController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool portableModeEnabled READ portableModeEnabled CONSTANT)
    Q_PROPERTY(QString effectiveDataRoot READ effectiveDataRoot NOTIFY stateChanged)
    Q_PROPERTY(QString customDataRoot READ customDataRoot WRITE setCustomDataRoot NOTIFY stateChanged)
    Q_PROPERTY(QString dataRootStatus READ dataRootStatus NOTIFY stateChanged)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY stateChanged)

public:
    explicit PortableRuntimeController(QObject *parent = nullptr);

    bool portableModeEnabled() const;
    QString effectiveDataRoot() const;
    QString customDataRoot() const;
    void setCustomDataRoot(const QString &value);
    QString dataRootStatus() const;
    bool restartRequired() const;

    Q_INVOKABLE void reload();
    Q_INVOKABLE void applyCustomDataRootAndRestart();
    Q_INVOKABLE void resetDataRootOverride();

signals:
    void stateChanged();

private:
    void setStatus(const QString &value);

    QString m_customDataRoot;
    QString m_dataRootStatus;
    bool m_restartRequired { false };
};

} // namespace OKILTV::App
