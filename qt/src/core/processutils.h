#pragma once

#include <QString>
#include <QStringView>

namespace OKILTV::Core {

QString resolveProcessBinary(QStringView baseName);
bool processBinaryAvailable(QStringView baseName);
bool ffmpegToolsAvailable();

} // namespace OKILTV::Core
