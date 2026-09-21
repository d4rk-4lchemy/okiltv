#pragma once

#include <QString>

namespace OKILTV::Core {

// Persistence boundary only: model objects always contain usable plaintext.
// Throws a generic, credential-free error on unavailable/locked storage or tampering.
QString protectSecret(const QString &plaintext);
QString unprotectSecret(const QString &stored);
bool isProtectedSecret(const QString &stored);

#ifdef OKILTV_SECURITY_TESTS
// Compiled only into test executables; never a runtime/environment bypass.
void useIsolatedSecretKeyForTests();
#endif

} // namespace OKILTV::Core
