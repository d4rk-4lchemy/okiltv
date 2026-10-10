#include "core/macoskeychain.h"
#include "core/appdatapaths.h"
#include "core/processutils.h"
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <QDir>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <future>
#include <stdexcept>

using namespace OKILTV::Core;
namespace {
class DisposableItem
{
public:
    const QString account = QStringLiteral("integration-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(nullptr, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    DisposableItem()
    {
        const auto bytes = account.toUtf8();
        const auto name = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(bytes.constData()),
            static_cast<CFIndex>(bytes.size()), kCFStringEncodingUTF8, false);
        CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(query, kSecAttrService, CFSTR("org.okiltv.storage"));
        CFDictionarySetValue(query, kSecAttrAccount, name);
        const auto authentication = macosAuthenticationContext();
        CFDictionarySetValue(query, kSecUseAuthenticationContext, authentication);
        CFRelease(authentication);
        CFRelease(name);
    }
    ~DisposableItem()
    {
        CFDictionaryRemoveValue(query, kSecReturnData);
        SecItemDelete(query);
        CFRelease(query);
    }
};
} // namespace

class MacosPlatformTests final : public QObject
{
    Q_OBJECT
private slots:
    void missingLookupNeverCreatesKey()
    {
        DisposableItem item;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, macosKeychainKey(false, item.account));
        QCOMPARE(SecItemCopyMatching(item.query, nullptr), errSecItemNotFound);
    }
    void nativeKeyCreationRaceAndDurableRead()
    {
        DisposableItem item;
        auto first = std::async(std::launch::async, [&]() { return macosKeychainKey(true, item.account); });
        auto second = std::async(std::launch::async, [&]() { return macosKeychainKey(true, item.account); });
        const auto key = first.get();
        QCOMPARE(key.size(), 32);
        QCOMPARE(second.get(), key);
        QCOMPARE(macosKeychainKey(false, item.account), key);
        QCOMPARE(macosKeychainKey(true, item.account), key);
    }
    void malformedKeyIsNeverReplaced()
    {
        DisposableItem item;
        const auto value = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>("invalid"), 7);
        CFDictionarySetValue(item.query, kSecValueData, value);
        QCOMPARE(SecItemAdd(item.query, nullptr), errSecSuccess);
        CFDictionaryRemoveValue(item.query, kSecValueData);
        CFRelease(value);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, macosKeychainKey(false, item.account));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, macosKeychainKey(true, item.account));
        CFDictionarySetValue(item.query, kSecReturnData, kCFBooleanTrue);
        CFTypeRef result = nullptr;
        QCOMPARE(SecItemCopyMatching(item.query, &result), errSecSuccess);
        const auto cleanup = qScopeGuard([&]() { if (result != nullptr) CFRelease(result); });
        QVERIFY(result != nullptr);
        QCOMPARE(CFDataGetLength(static_cast<CFDataRef>(result)), 7);
    }
    void shippedSqliteSupportsFts5()
    {
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("macos-fts5-test"));
            database.setDatabaseName(QStringLiteral(":memory:"));
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral("CREATE VIRTUAL TABLE guide USING fts5(title)")));
            QVERIFY(query.exec(QStringLiteral("INSERT INTO guide VALUES ('Local movie fixture')")));
            QVERIFY(query.exec(QStringLiteral("SELECT title FROM guide WHERE guide MATCH 'movie'")));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), QStringLiteral("Local movie fixture"));
            database.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("macos-fts5-test"));
    }
    void packagedHelpersResolveWithoutPath()
    {
        if (!QCoreApplication::applicationDirPath().endsWith(QStringLiteral("/Contents/MacOS"))) {
            QSKIP("Helper isolation requires the test executable inside the packaged app.");
        }
        const auto previousPath = qgetenv("PATH");
        const auto cleanup = qScopeGuard([&]() { qputenv("PATH", previousPath); });
        qputenv("PATH", QByteArray());
        QVERIFY(ffmpegToolsAvailable());
        for (const auto &name : {QStringLiteral("ffmpeg"), QStringLiteral("ffprobe")}) {
            QCOMPARE(QFileInfo(resolveProcessBinary(name)).absolutePath(), QCoreApplication::applicationDirPath());
        }
    }
};
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const auto plugins = QCoreApplication::applicationDirPath() + QStringLiteral("/../PlugIns");
    if (QDir(plugins).exists()) application.setLibraryPaths({plugins});
    MacosPlatformTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "tst_macos_platform.moc"
