#pragma once

#include "models.h"

#include <QList>
#include <QString>

#include <optional>

namespace OKILTV::Core {

class SourceStore
{
public:
    explicit SourceStore(QString summariesFilePath = {}, QString sourceDetailsDirectory = {});

    QList<SourceSummary> loadSummaries() const;
    bool saveSummaries(const QList<SourceSummary> &summaries, QString *errorText = nullptr) const;

    std::optional<ServerProfile> loadDetail(const QUuid &profileId) const;
    bool saveDetail(const ServerProfile &profile, QString *errorText = nullptr) const;

    bool removeDetail(const QUuid &profileId, QString *errorText = nullptr) const;

private:
    QString detailFilePath(const QUuid &profileId) const;

    QString m_summariesFilePath;
    QString m_sourceDetailsDirectory;
};

} // namespace OKILTV::Core
