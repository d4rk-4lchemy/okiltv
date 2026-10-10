#include "macoskeychain.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <openssl/rand.h>

#include <memory>
#include <stdexcept>

namespace OKILTV::Core {
namespace {
struct CfRelease {
    void operator()(const void *value) const { if (value != nullptr) CFRelease(value); }
};
template<class T> using CfPtr = std::unique_ptr<T, CfRelease>;

[[noreturn]] void keychainError()
{
    throw std::runtime_error("Cannot unlock protected application data in macOS Keychain. Unlock the original account's login keychain and allow OKILTV access, then retry. No plaintext fallback is allowed.");
}

QByteArray readKey(CFMutableDictionaryRef query, bool *missing)
{
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const auto status = SecItemCopyMatching(query, &result);
    const CfPtr<const void> guard(result);
    *missing = status == errSecItemNotFound;
    if (*missing) return {};
    if (status != errSecSuccess || result == nullptr || CFGetTypeID(result) != CFDataGetTypeID()) keychainError();
    const auto data = static_cast<CFDataRef>(result);
    if (CFDataGetLength(data) != 32) keychainError();
    return {reinterpret_cast<const char *>(CFDataGetBytePtr(data)), 32};
}
} // namespace

QByteArray macosKeychainKey(const bool create, const QString &account)
{
    const auto bytes = account.toUtf8();
    const CfPtr<const __CFString> accountRef(CFStringCreateWithBytes(nullptr,
        reinterpret_cast<const UInt8 *>(bytes.constData()), static_cast<CFIndex>(bytes.size()), kCFStringEncodingUTF8, false));
    const CfPtr<__CFDictionary> query(CFDictionaryCreateMutable(nullptr, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (!query || !accountRef) keychainError();
    const CfPtr<const void> authentication(macosAuthenticationContext());
    if (!authentication) keychainError();
    CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query.get(), kSecAttrService, CFSTR("org.okiltv.storage"));
    CFDictionarySetValue(query.get(), kSecAttrAccount, accountRef.get());
    // Background storage workers never open an unexpected authentication dialog.
    CFDictionarySetValue(query.get(), kSecUseAuthenticationContext, authentication.get());
    bool missing = false;
    auto existing = readKey(query.get(), &missing);
    if (!missing) return existing;
    if (!create) keychainError();

    QByteArray generated(32, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(generated.data()), 32) != 1) keychainError();
    const CfPtr<const __CFData> data(CFDataCreate(nullptr,
        reinterpret_cast<const UInt8 *>(generated.constData()), 32));
    if (!data) keychainError();
    CFDictionaryRemoveValue(query.get(), kSecReturnData);
    CFDictionaryRemoveValue(query.get(), kSecMatchLimit);
    CFDictionarySetValue(query.get(), kSecAttrLabel, CFSTR("OKILTV protected data"));
    CFDictionarySetValue(query.get(), kSecValueData, data.get());
    const auto added = SecItemAdd(query.get(), nullptr);
    if (added != errSecSuccess && added != errSecDuplicateItem) keychainError();
    CFDictionaryRemoveValue(query.get(), kSecValueData);
    CFDictionaryRemoveValue(query.get(), kSecAttrLabel);
    const auto durable = readKey(query.get(), &missing);
    if (missing || (added == errSecSuccess && durable != generated)) keychainError();
    return durable; // A competing process may have created the winning key.
}
} // namespace OKILTV::Core
