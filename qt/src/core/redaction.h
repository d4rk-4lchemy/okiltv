#pragma once

#include <QJsonValue>
#include <QString>
#include <QUrl>

namespace OKILTV::Core {

QString redactSensitiveUrl(const QString &rawUrl);
QString redactSensitiveText(const QString &text);
QJsonValue redactSensitiveJson(const QJsonValue &value);
QString networkCategoryForUrl(const QUrl &url);

} // namespace OKILTV::Core
