#include "portableruntimecontroller.h"

#include "../core/appdatapaths.h"

#include <QCoreApplication>
#include <QProcess>

namespace OKILTV::App {

using namespace Core;

PortableRuntimeController::PortableRuntimeController(QObject *parent)
    : QObject(parent)
{
    reload();
}

bool PortableRuntimeController::portableModeEnabled() const
{
    return AppDataPaths::runtimeContext().launchMode == LaunchMode::Portable;
}

QString PortableRuntimeController::effectiveDataRoot() const
{
    return AppDataPaths::dataDirectory();
}

QString PortableRuntimeController::customDataRoot() const
{
    return m_customDataRoot;
}

void PortableRuntimeController::setCustomDataRoot(const QString &value)
{
    const auto normalized = value.trimmed();
    if (m_customDataRoot == normalized) {
        return;
    }

    m_customDataRoot = normalized;
    m_restartRequired = false;
    const auto error = PortableBootstrap::dataRootOverrideError(m_customDataRoot);
    if (!error.isEmpty()) {
        setStatus(error);
    } else if (m_customDataRoot.isEmpty()) {
        setStatus(QStringLiteral("Portable build will use %%APPDATA%%\\OKILTV after restart."));
    } else {
        setStatus(QStringLiteral("Portable build will switch to %1 after restart.").arg(m_customDataRoot));
    }
    emit stateChanged();
}

QString PortableRuntimeController::dataRootStatus() const
{
    return m_dataRootStatus;
}

bool PortableRuntimeController::restartRequired() const
{
    return m_restartRequired;
}

void PortableRuntimeController::reload()
{
    const auto context = AppDataPaths::runtimeContext();
    if (context.launchMode != LaunchMode::Portable || context.portableBootstrapPath.trimmed().isEmpty()) {
        m_customDataRoot.clear();
        m_restartRequired = false;
        setStatus(QStringLiteral("Portable runtime controls are unavailable in this build."));
        emit stateChanged();
        return;
    }

    const auto config = PortableBootstrap::load(context.portableBootstrapPath);
    m_customDataRoot = config.dataRootOverride;
    m_restartRequired = false;
    if (m_customDataRoot.isEmpty()) {
        setStatus(QStringLiteral("Portable build is currently using %%APPDATA%%\\OKILTV."));
    } else {
        setStatus(QStringLiteral("Portable build is currently using %1.").arg(AppDataPaths::dataDirectory()));
    }
    emit stateChanged();
}

void PortableRuntimeController::applyCustomDataRootAndRestart()
{
    const auto context = AppDataPaths::runtimeContext();
    if (context.launchMode != LaunchMode::Portable || context.portableBootstrapPath.trimmed().isEmpty()) {
        setStatus(QStringLiteral("Portable restart is unavailable in this build."));
        emit stateChanged();
        return;
    }

    PortableBootstrapConfig config;
    config.dataRootOverride = m_customDataRoot;

    QString errorText;
    if (!PortableBootstrap::save(context.portableBootstrapPath, config, &errorText)) {
        m_restartRequired = false;
        setStatus(errorText);
        emit stateChanged();
        return;
    }

    QStringList args = QCoreApplication::arguments();
    if (!args.isEmpty()) {
        args.removeFirst();
    }

    for (int index = 0; index < args.size();) {
        if (args.at(index) == QStringLiteral("--portable-bootstrap")) {
            args.removeAt(index);
            if (index < args.size()) {
                args.removeAt(index);
            }
            continue;
        }
        ++index;
    }

    args << QStringLiteral("--portable-bootstrap") << context.portableBootstrapPath;
    if (qEnvironmentVariableIsSet("OKILTV_SKIP_PORTABLE_RESTART")) {
        m_restartRequired = true;
        setStatus(QStringLiteral("Portable data directory saved. Restart the app to apply it."));
        emit stateChanged();
        return;
    }

    if (QProcess::startDetached(QCoreApplication::applicationFilePath(), args)) {
        setStatus(QStringLiteral("Restarting with updated portable data directory."));
        emit stateChanged();
        QCoreApplication::quit();
        return;
    }

    m_restartRequired = true;
    setStatus(QStringLiteral("Portable data directory saved. Restart the app to apply it."));
    emit stateChanged();
}

void PortableRuntimeController::resetDataRootOverride()
{
    setCustomDataRoot({});
}

void PortableRuntimeController::setStatus(const QString &value)
{
    m_dataRootStatus = value;
}

} // namespace OKILTV::App
