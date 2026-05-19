#pragma once

#include <QString>
#include <QUrl>

namespace OKILTV::Core {

QString redactSensitiveUrl(const QString &rawUrl);
QString redactSensitiveText(const QString &text);
QString networkCategoryForUrl(const QUrl &url);

} // namespace OKILTV::Core
