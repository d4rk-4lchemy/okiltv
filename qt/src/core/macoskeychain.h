#pragma once

#include <QByteArray>
#include <QString>
#include <CoreFoundation/CoreFoundation.h>

namespace OKILTV::Core {
// Stable service org.okiltv.storage. A separate account permits disposable
// native integration tests without reading or deleting the application key.
QByteArray macosKeychainKey(bool create, const QString &account);
// Retained LAContext with authentication UI forbidden; caller releases it.
CFTypeRef macosAuthenticationContext() CF_RETURNS_RETAINED;
} // namespace OKILTV::Core
