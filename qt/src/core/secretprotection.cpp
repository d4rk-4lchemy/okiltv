#include "secretprotection.h"

#include <QByteArray>
#include <QLibrary>
#include <QLockFile>
#include <QDir>
#include <QStandardPaths>
#include <QMutex>
#include <QMutexLocker>

#include <memory>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dpapi.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace OKILTV::Core {
namespace {
const QString prefix = QStringLiteral("okiltv-secret:v1:");

[[noreturn]] void unavailable()
{
    throw std::runtime_error("Cannot unlock protected application data. Use the original system account and unlock its secret store. No plaintext fallback is allowed.");
}

#ifndef Q_OS_WIN
QMutex keyMutex;
QByteArray cachedKey;

QByteArray systemKey(bool create)
{
    QMutexLocker lock(&keyMutex);
    if (!cachedKey.isEmpty()) {
        return cachedKey;
    }
    const auto lockDirectory = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/OKILTV");
    if (!QDir().mkpath(lockDirectory)) unavailable();
    QLockFile keyLock(lockDirectory + QStringLiteral("/secret-key.lock"));
    if (!keyLock.tryLock(5000)) unavailable();
    // Resolve the public libsecret/glib ABI at runtime so absence is a controlled
    // storage error rather than a loader failure. Only the AES key enters keyring.
    QLibrary secret(QStringLiteral("secret-1"), 0);
    QLibrary glib(QStringLiteral("glib-2.0"), 0);
    using HashNew = void *(*)(unsigned int (*)(const void *), int (*)(const void *, const void *));
    const auto hashNew = reinterpret_cast<HashNew>(glib.resolve("g_hash_table_new"));
    const auto hash = reinterpret_cast<unsigned int (*)(const void *)>(glib.resolve("g_str_hash"));
    const auto equal = reinterpret_cast<int (*)(const void *, const void *)>(glib.resolve("g_str_equal"));
    const auto insert = reinterpret_cast<int (*)(void *, void *, void *)>(glib.resolve("g_hash_table_insert"));
    const auto unref = reinterpret_cast<void (*)(void *)>(glib.resolve("g_hash_table_unref"));
    const auto freeError = reinterpret_cast<void (*)(void *)>(glib.resolve("g_error_free"));
    const auto lookup = reinterpret_cast<char *(*)(const void *, void *, void *, void **)>(secret.resolve("secret_password_lookupv_sync"));
    const auto store = reinterpret_cast<int (*)(const void *, void *, const char *, const char *, const char *, void *, void **)>(secret.resolve("secret_password_storev_sync"));
    const auto freePassword = reinterpret_cast<void (*)(char *)>(secret.resolve("secret_password_free"));
    if (!hashNew || !hash || !equal || !insert || !unref || !freeError || !lookup || !store || !freePassword) {
        unavailable();
    }
    auto *attributes = hashNew(hash, equal);
    insert(attributes, const_cast<char *>("application"), const_cast<char *>("org.okiltv.storage"));
    insert(attributes, const_cast<char *>("purpose"), const_cast<char *>("encryption-key-v1"));
    const std::unique_ptr<void, decltype(unref)> guard(attributes, unref);
    void *error = nullptr;
    auto *password = lookup(nullptr, attributes, nullptr, &error);
    if (error != nullptr) {
        freeError(error);
        if (password != nullptr) freePassword(password);
        unavailable();
    }
    if (password != nullptr) {
        const auto result = QByteArray::fromBase64Encoding(QByteArray(password), QByteArray::AbortOnBase64DecodingErrors);
        freePassword(password);
        if (!result || result.decoded.size() != 32) unavailable();
        cachedKey = result.decoded;
        return cachedKey;
    }
    if (!create) unavailable();
    QByteArray key(32, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(key.data()), 32) != 1) unavailable();
    const auto encoded = key.toBase64();
    const auto saved = store(nullptr, attributes, "default", "OKILTV protected data", encoded.constData(), nullptr, &error);
    if (error != nullptr) freeError(error);
    if (!saved || error != nullptr) unavailable();
    // Verify durable storage before permitting migration to replace old data.
    password = lookup(nullptr, attributes, nullptr, &error);
    const bool verified = error == nullptr && password != nullptr && QByteArray(password) == encoded;
    if (password != nullptr) freePassword(password);
    if (error != nullptr) freeError(error);
    if (!verified) unavailable();
    cachedKey = key;
    return cachedKey;
}
#endif
} // namespace

bool isProtectedSecret(const QString &stored)
{
    return stored.startsWith(QStringLiteral("okiltv-secret:"));
}

QString protectSecret(const QString &plaintext)
{
    if (plaintext.isEmpty()) return plaintext;
    const auto bytes = plaintext.toUtf8();
#ifdef Q_OS_WIN
    DATA_BLOB input { static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE *>(const_cast<char *>(bytes.constData())) };
    DATA_BLOB output {};
    if (!CryptProtectData(&input, L"OKILTV", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) unavailable();
    const QByteArray encrypted(reinterpret_cast<const char *>(output.pbData), static_cast<qsizetype>(output.cbData));
    LocalFree(output.pbData);
    return prefix + QStringLiteral("dpapi:") + QString::fromLatin1(encrypted.toBase64());
#else
    const auto key = systemKey(true);
    QByteArray nonce(12, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), 12) != 1) unavailable();
    const std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData())) != 1) unavailable();
    QByteArray encrypted(bytes.size() + 16, Qt::Uninitialized);
    int count = 0;
    if (EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(encrypted.data()), &count,
        reinterpret_cast<const unsigned char *>(bytes.constData()), static_cast<int>(bytes.size())) != 1) unavailable();
    int tail = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(encrypted.data()) + count, &tail) != 1) unavailable();
    encrypted.resize(count + tail);
    QByteArray tag(16, Qt::Uninitialized);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) unavailable();
    return prefix + QStringLiteral("keyring:") + QString::fromLatin1((nonce + tag + encrypted).toBase64());
#endif
}

QString unprotectSecret(const QString &stored)
{
    if (!isProtectedSecret(stored)) return stored; // Legacy plaintext, rewritten by migration.
#ifdef Q_OS_WIN
    const auto header = prefix + QStringLiteral("dpapi:");
#else
    const auto header = prefix + QStringLiteral("keyring:");
#endif
    if (!stored.startsWith(header)) unavailable();
    const auto decoded = QByteArray::fromBase64Encoding(stored.mid(header.size()).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) unavailable();
    const auto &bytes = decoded.decoded;
#ifdef Q_OS_WIN
    DATA_BLOB input { static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE *>(const_cast<char *>(bytes.constData())) };
    DATA_BLOB output {};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) unavailable();
    const auto result = QString::fromUtf8(reinterpret_cast<const char *>(output.pbData), static_cast<qsizetype>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
#else
    if (bytes.size() < 28) unavailable();
    const auto key = systemKey(false);
    const std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(bytes.constData())) != 1) unavailable();
    QByteArray plaintext(bytes.size(), Qt::Uninitialized);
    int count = 0;
    if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(plaintext.data()), &count,
        reinterpret_cast<const unsigned char *>(bytes.constData()) + 28, static_cast<int>(bytes.size() - 28)) != 1) unavailable();
    auto tag = bytes.mid(12, 16);
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1) unavailable();
    int tail = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(plaintext.data()) + count, &tail) != 1) unavailable();
    return QString::fromUtf8(plaintext.constData(), count + tail);
#endif
}

#ifdef OKILTV_SECURITY_TESTS
void useIsolatedSecretKeyForTests()
{
#ifndef Q_OS_WIN
    QMutexLocker lock(&keyMutex);
    cachedKey = QByteArray(32, 'T');
#endif
}
#endif
} // namespace OKILTV::Core
