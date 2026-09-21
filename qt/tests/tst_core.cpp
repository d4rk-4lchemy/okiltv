#include "../src/core/secretprotection.h"
#include "../src/core/appdatapaths.h"
#include "../src/core/catchupurlresolver.h"
#include "../src/core/database_service.h"
#include "../src/core/debuglogger.h"
#include "../src/core/epgcache_service.h"
#include "../src/core/epgservice.h"
#include "../src/core/m3uservice.h"
#include "../src/core/portablebootstrap.h"
#include "../src/core/redaction.h"
#include "../src/core/settingsmanager.h"
#include "../src/core/trackpreferences.h"
#include "../src/core/xtreamservice.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <ctime>
#include <QtTest>

#include <cmath>
#include <stdexcept>
#if __has_include(<zlib.h>)
#include <zlib.h>
#elif __has_include(<QtZlib/zlib.h>)
#include <QtZlib/zlib.h>
#else
#error "zlib headers are required for XMLTV gzip/zlib tests."
#endif

using namespace OKILTV::Core;

namespace {

class ScopedAppDataEnv
{
public:
    explicit ScopedAppDataEnv(const QString &path)
        : m_previous(qgetenv("APPDATA"))
    {
        qputenv("APPDATA", path.toUtf8());
    }

    ~ScopedAppDataEnv()
    {
        if (m_previous.isEmpty()) {
            qunsetenv("APPDATA");
        } else {
            qputenv("APPDATA", m_previous);
        }
    }

private:
    QByteArray m_previous;
};

class ScopedRuntimeContext
{
public:
    ScopedRuntimeContext()
        : m_previous(AppDataPaths::runtimeContext())
    {
        AppDataPaths::resetRuntimeForTests();
    }

    ~ScopedRuntimeContext()
    {
        AppDataPaths::initializeRuntime(m_previous);
    }

private:
    RuntimeContext m_previous;
};

class MockNetworkAccess final : public NetworkAccess
{
public:
    void setResponse(const QUrl &url, const QByteArray &payload)
    {
        QMutexLocker locker(&m_mutex);
        m_responses.insert(url.toString(), payload);
    }

    QByteArray get(const QUrl &url) const override
    {
        QMutexLocker locker(&m_mutex);
        return m_responses.value(url.toString());
    }

private:
    mutable QMutex m_mutex;
    QHash<QString, QByteArray> m_responses;
};

QByteArray gzipCompress(const QByteArray &input)
{
    z_stream stream {};
    if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }

    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());

    QByteArray output;
    output.reserve(input.size());
    char buffer[16 * 1024];
    int code = Z_OK;
    do {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);

        code = deflate(&stream, Z_FINISH);
        if (code != Z_OK && code != Z_STREAM_END) {
            deflateEnd(&stream);
            return {};
        }

        output.append(buffer, static_cast<int>(sizeof(buffer) - stream.avail_out));
    } while (code != Z_STREAM_END);

    deflateEnd(&stream);
    return output;
}

} // namespace

class CoreTests final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { OKILTV::Core::useIsolatedSecretKeyForTests(); }
    void channelTrackPreferencesRoundTrip();
    void trackPreferencesMatchIdentity();
    void uiTransparencySettingsCompatibility();
    void dateTimeFormatDetection_data();
    void dateTimeFormatDetection();
    void dateTimeDisplayFormatting();
    void dateTimeDisplayHonorsLocalDst();
    void dateTimeSettingsCompatibility();
    void settingsCompatibilityRoundTrips();
    void settingsVolumeCompatibility();
    void settingsCompatibilityDefaultsNewPlayerTuningFields();
    void portableBootstrapRoundTripsIndependentOfSettings();
    void portableBootstrapDefaultsDataBesideMarker();
    void appDataPathsSupportPortableOverridesAndFallback();
    void appDataPathsMigratesLegacyRootIntoOKILTVDirectory();
    void debugLoggerOnlyWritesFilesForExplicitDump();
    void debugLoggerCursorAndSubscribersStayOrdered();
    void protectedStorageRoundTripAndTampering();
    void credentialMigrationPreservesSettingsAndSources();
    void credentialMigrationFailurePreservesLegacyFile();
    void credentialMigrationProtectsDatabase();
    void lockedSettingsPreserveRecoveryData();
    void lockedSourceRequiresExplicitReplacement();
    void sourceRemovalCleansProtectedCopies();
    void databaseCredentialMigrationRollsBackOnFailure();
    void redactionMasksStructuredSecrets();
    void redactionMasksXtreamSecrets();
    void redactionMasksAuthorizationAndTokenSecrets();
    void m3uParserKeepsFieldParity();
    void m3uParserCapturesSupportedCatchupMetadata();
    void m3uParserAcceptsFlexibleAttributeSyntax();
    void m3uParserSupportsLegacyTimeshiftCatchupMetadata();
    void m3uParserFallsBackWhenTvgNameIsMissing();
    void m3uParserUsesDisplayCategoryNameWhenGroupMissing();
    void xtreamServiceParsesCatchupFields();
    void xtreamServiceRejectsNonArrayPayloads();
    void epgParserHandlesOffsetsAndOrdering();
    void epgParserRejectsMalformedPayloads();
    void epgParserAcceptsValidEmptyDocument();
    void epgParserSupportsGzipPayloads();
    void epgCacheRoundTripsWithMetadata();
    void epgCacheAgeCalculationsStayStable();
    void epgCacheFingerprintInvalidatesChangedSource();
    void epgSnapshotAppliesPrebuiltIndex();
    void epgRangeLookupPreservesOverlapsAndBoundaries();
    void databaseKeepsSchemaCompatible();
    void databasePersistsWatchSecondsForChannelIdZero();
    void catchupProgressRoundsAndCompletes_data();
    void catchupProgressRoundsAndCompletes();
    void databaseCatchupProgressPersistsAndIsolatesEmissions();
    void databaseUpsertRefreshesTvgAndSourceFields();
    void databaseReplaceChannelsForProfilePrunesStaleRows();
    void catchupUrlResolverBuildsXtreamAndM3uTargets();
    void catchupUrlResolverBuildsIndependentWindows();
    void catchupUrlResolverRejectsUnavailableTargets();
    void settingsLoadInvalidJsonCreatesBackupAndReportsError();
    void settingsSaveReportsErrorAndCreatesParentDirectory();
    void settingsReloadCommitsExistingFile();
    void channelGroupIdsDoNotDecryptChannels();
};

void CoreTests::dateTimeFormatDetection_data()
{
    QTest::addColumn<QString>("datePattern");
    QTest::addColumn<QString>("timePattern");
    QTest::addColumn<bool>("monthFirst");
    QTest::addColumn<bool>("twelveHour");
    QTest::newRow("Poland") << "dd.MM.yyyy" << "HH:mm" << false << false;
    QTest::newRow("UK") << "dd/MM/yyyy" << "HH:mm" << false << false;
    QTest::newRow("US") << "M/d/yy" << "h:mm AP" << true << true;
    QTest::newRow("year-first") << "yyyy-MM-dd" << "h:mm ap" << true << true;
    QTest::newRow("weekday") << "ddd MMM d yyyy" << "HH:mm" << true << false;
    QTest::newRow("quoted") << "'MM' dd/MM/yyyy" << "HH:mm 'AP'" << false << false;
    QTest::newRow("escaped-quote") << "'it''s dd' MM/dd" << "h:mm 'o''clock' AP" << true << true;
    QTest::newRow("fallback") << "yyyy" << "" << false << false;
}

void CoreTests::dateTimeFormatDetection()
{
    QFETCH(QString, datePattern);
    QFETCH(QString, timePattern);
    QFETCH(bool, monthFirst);
    QFETCH(bool, twelveHour);
    const auto options = detectDateTimeFormat(datePattern, timePattern);
    QCOMPARE(options.monthFirst, monthFirst);
    QCOMPARE(options.twelveHour, twelveHour);
}

void CoreTests::dateTimeDisplayFormatting()
{
    const DateTimeFormatOptions twelve { true, true };
    const DateTimeFormatOptions twentyFour { false, false };
    const QDateTime evening(QDate(2026, 9, 18), QTime(18, 5));
    QCOMPARE(formatDisplayTime(evening, twelve), QStringLiteral("6:05 PM"));
    QCOMPARE(formatDisplayTime(evening, twentyFour), QStringLiteral("18:05"));
    QCOMPARE(formatDisplayDateTime(evening, twelve.guideDatePattern()), QStringLiteral("Friday, 09.18"));
    QCOMPARE(formatDisplayDateTime(evening, twentyFour.guideDatePattern()), QStringLiteral("Friday, 18.09"));
    QCOMPARE(formatDisplayDateTime(evening, twelve.clockPattern()), QStringLiteral("Fri Sep 18  6:05 PM"));
    QCOMPARE(formatDisplayDateTime(evening, twentyFour.clockPattern()), QStringLiteral("Fri 18 Sep  18:05"));
    QCOMPARE(formatDisplayDateTime(evening, twelve.dateTimePattern()), QStringLiteral("09-18-2026 6:05 PM"));
    const QDateTime midnight(QDate(2027, 1, 1), QTime(0, 0));
    const QDateTime noon(QDate(2027, 1, 1), QTime(12, 0));
    QCOMPARE(formatDisplayTime(midnight, twelve), QStringLiteral("12:00 AM"));
    QCOMPARE(formatDisplayTime(noon, twelve), QStringLiteral("12:00 PM"));
    QCOMPARE(formatDisplayTimeRange(midnight.addSecs(-60), midnight, twelve), QStringLiteral("11:59 PM - 12:00 AM"));
    QCOMPARE(formatDisplayTime({}, twelve), QString {});
    QCOMPARE(formatDisplayTimeRange({}, noon, twelve), QString {});
    QCOMPARE(formatDisplayDateTime(midnight, twentyFour.dateTimePattern()), QStringLiteral("01-01-2027 00:00"));
    // UTC and local representations of an instant must have identical display text.
    QCOMPARE(formatDisplayTime(evening.toUTC(), twelve), formatDisplayTime(evening, twelve));
    EpgEntry entry;
    entry.start = evening;
    entry.stop = evening.addSecs(3600);
    const auto raw = toVariantMap(entry);
    const auto formatted = formatProgramTimes(raw, twelve);
    QCOMPARE(formatted.value("timeRange").toString(), QStringLiteral("6:05 PM - 7:05 PM"));
    QCOMPARE(formatted.value("start"), raw.value("start"));
    QCOMPARE(formatted.value("stop"), raw.value("stop"));
    QCOMPARE(raw.value("timeRange").toString(), QStringLiteral("18:05 - 19:05"));
    QVERIFY(formatProgramTimes(QVariantMap {}, twelve).isEmpty());
}

void CoreTests::dateTimeDisplayHonorsLocalDst()
{
#ifdef Q_OS_UNIX
    const bool wasSet = qEnvironmentVariableIsSet("TZ");
    const auto previous = qgetenv("TZ");
    const auto restore = qScopeGuard([previous, wasSet]() {
        if (wasSet) qputenv("TZ", previous);
        else qunsetenv("TZ");
        tzset();
    });
    qputenv("TZ", "Europe/Warsaw");
    tzset();
    const DateTimeFormatOptions options { false, true };
    const auto before = QDateTime::fromString("2026-03-29T00:30:00Z", Qt::ISODate);
    const auto after = before.addSecs(3600);
    QCOMPARE(formatDisplayTime(before, options), QStringLiteral("1:30 AM"));
    QCOMPARE(formatDisplayTime(after, options), QStringLiteral("3:30 AM"));
    const auto first = QDateTime::fromString("2026-10-25T00:30:00Z", Qt::ISODate);
    QCOMPARE(formatDisplayTime(first, options), QStringLiteral("2:30 AM"));
    QCOMPARE(formatDisplayTime(first.addSecs(3600), options), QStringLiteral("2:30 AM"));
    QCOMPARE(before.toString(Qt::ISODate), QStringLiteral("2026-03-29T00:30:00Z"));
#else
    QSKIP("Local TZ environment override is tested on Unix; Windows regional settings require desktop validation.");
#endif
}

void CoreTests::dateTimeSettingsCompatibility()
{
    const auto defaults = appSettingsFromJson({});
    QCOMPARE(defaults.dateOrder, QStringLiteral("system"));
    QCOMPARE(defaults.timeFormat, QStringLiteral("system"));
    auto settings = defaults;
    settings.dateOrder = QStringLiteral("mdy");
    settings.timeFormat = QStringLiteral("12h");
    const auto restored = appSettingsFromJson(toJson(settings));
    QCOMPARE(restored.dateOrder, settings.dateOrder);
    QCOMPARE(restored.timeFormat, settings.timeFormat);
    const auto invalid = appSettingsFromJson({ { "dateOrder", "bad" }, { "timeFormat", 12 } });
    QCOMPARE(invalid.dateOrder, QStringLiteral("system"));
    QCOMPARE(invalid.timeFormat, QStringLiteral("system"));
    const auto system = systemDateTimeFormat();
    QCOMPARE(resolveDateTimeFormat("system", "system"), system);
    QCOMPARE(resolveDateTimeFormat("dmy", "system").twelveHour, system.twelveHour);
    QVERIFY(!resolveDateTimeFormat("dmy", "12h").monthFirst);
    QVERIFY(resolveDateTimeFormat("system", "12h").twelveHour);
    QCOMPARE(toJson(defaults).value("dateOrder").toString(), QStringLiteral("system"));
}

void CoreTests::uiTransparencySettingsCompatibility()
{
    QCOMPARE(appSettingsFromJson({}).uiTransparency, 100);
    QCOMPARE(appSettingsFromJson({ { "uiTransparency", "invalid" } }).uiTransparency, 100);
    QCOMPARE(appSettingsFromJson({ { "uiTransparency", -10 } }).uiTransparency, 0);
    QCOMPARE(appSettingsFromJson({ { "uiTransparency", 110 } }).uiTransparency, 100);
    for (const int value : { 0, 50, 100 }) {
        AppSettings settings;
        settings.uiTransparency = value;
        QCOMPARE(appSettingsFromJson(toJson(settings)).uiTransparency, value);
    }
}

void CoreTests::settingsVolumeCompatibility()
{
    QCOMPARE(appSettingsFromJson(QJsonObject {}).playerVolume, 100.0);
    QCOMPARE(appSettingsFromJson(QJsonObject { { QStringLiteral("playerVolume"), "invalid" } }).playerVolume, 100.0);
    QCOMPARE(appSettingsFromJson(QJsonObject { { QStringLiteral("playerVolume"), -5.0 } }).playerVolume, 0.0);
    QCOMPARE(appSettingsFromJson(QJsonObject { { QStringLiteral("playerVolume"), 150.0 } }).playerVolume, 100.0);
    AppSettings settings;
    settings.playerVolume = 37.5;
    QCOMPARE(appSettingsFromJson(toJson(settings)).playerVolume, 37.5);
}

void CoreTests::channelTrackPreferencesRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("settings.json"));
    SettingsManager settings(path);
    const auto profile = guidToString(QUuid::createUuid());
    const auto otherProfile = guidToString(QUuid::createUuid());
    const QJsonObject off { { QStringLiteral("mode"), QStringLiteral("off") } };
    const QJsonObject audio { { QStringLiteral("mode"), QStringLiteral("track") },
        { QStringLiteral("id"), 2 }, { QStringLiteral("lang"), QStringLiteral("eng") } };
    settings.setChannelTrackPreference(profile, QStringLiteral("xtream:1"), QStringLiteral("audio"), audio);
    settings.setChannelTrackPreference(profile, QStringLiteral("xtream:1"), QStringLiteral("sub"), off);
    settings.setChannelTrackPreference(otherProfile, QStringLiteral("xtream:1"), QStringLiteral("sub"), off);
    QVERIFY(settings.lastSaveError().isEmpty());
    SettingsManager reloaded(path);
    reloaded.load();
    QCOMPARE(reloaded.channelTrackPreferences(profile, QStringLiteral("xtream:1"))
                 .value(QStringLiteral("audio")).toObject(), audio);
    QVERIFY(reloaded.channelTrackPreferences(profile, QStringLiteral("xtream:2")).isEmpty());
    reloaded.setChannelTrackPreference(profile, QStringLiteral("xtream:1"), QStringLiteral("audio"), {});
    reloaded.current().theme = QStringLiteral("Light");
    reloaded.save();
    SettingsManager again(path);
    again.load();
    const QJsonObject subtitlesOnly { { QStringLiteral("sub"), off } };
    QCOMPARE(again.channelTrackPreferences(profile, QStringLiteral("xtream:1")), subtitlesOnly);
    QCOMPARE(again.channelTrackPreferences(otherProfile, QStringLiteral("xtream:1")), subtitlesOnly);
    QVERIFY(appSettingsFromJson(QJsonObject {}).channelTrackPreferences.isEmpty());

    Channel channel;
    channel.source = ChannelSource::M3U;
    channel.streamUrl = QStringLiteral("https://example.test/private-user/secret.ts");
    channel.id = 1;
    const auto key = trackPreferenceChannelKey(channel);
    channel.id = 50;
    QCOMPARE(trackPreferenceChannelKey(channel), key);
    QVERIFY(!key.contains(QStringLiteral("secret")));
    channel.streamUrl += QStringLiteral("?other");
    QVERIFY(trackPreferenceChannelKey(channel) != key);
}

void CoreTests::trackPreferencesMatchIdentity()
{
    const QString audio = QStringLiteral("audio");
    const auto track = [&audio](int id, const QString &title, const QString &lang) -> QVariant {
        return QVariantMap { { QStringLiteral("type"), audio }, { QStringLiteral("id"), id },
            { QStringLiteral("title"), title }, { QStringLiteral("lang"), lang } };
    };
    const QVariantList original { track(1, QStringLiteral("Main"), QStringLiteral("pol")),
        track(2, QStringLiteral(" Original "), QStringLiteral("ENG")) };
    const auto preference = makeTrackPreference(original, audio, 2);
    QCOMPARE(matchTrackPreference({ track(5, QStringLiteral("original"), QStringLiteral("eng")) }, audio, preference), 5);
    QCOMPARE(matchTrackPreference({ track(2, QStringLiteral("Commentary"), QStringLiteral("eng")) }, audio, preference), -1);
    QCOMPARE(matchTrackPreference({ track(3, QStringLiteral("Original"), QStringLiteral("eng")),
        track(4, QStringLiteral("Original"), QStringLiteral("eng")) }, audio, preference), -1);
    QCOMPARE(matchTrackPreference({ track(2, QStringLiteral("Original"), QStringLiteral("eng")),
        track(4, QStringLiteral("Original"), QStringLiteral("eng")) }, audio, preference), 2);
    const QVariantList anonymous { track(1, {}, {}), track(2, {}, {}) };
    const auto unnamed = makeTrackPreference(anonymous, audio, 2);
    QCOMPARE(matchTrackPreference(anonymous, audio, unnamed), 2);
    QCOMPARE(matchTrackPreference({ track(2, {}, {}) }, audio, unnamed), -1);
    QCOMPARE(matchTrackPreference({}, QStringLiteral("sub"), makeTrackPreference({}, QStringLiteral("sub"), 0)), 0);
    QVERIFY(makeTrackPreference(original, audio, 99).isEmpty());
    QCOMPARE(matchTrackPreference(original, audio, QJsonObject {}), -1);
}

void CoreTests::settingsCompatibilityRoundTrips()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto settingsPath = tempDir.filePath(QStringLiteral("settings.json"));
    QFile file(settingsPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"json({
  "activeProfileId": "0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9",
  "profiles": [
    {
      "id": "0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9",
      "name": "Office IPTV",
      "type": 1,
      "xtreamBaseUrl": "",
      "xtreamUsername": "",
      "xtreamPassword": "",
      "xtreamServerTimezone": "Asia/Dubai",
      "m3UUrl": "https://example.com/playlist.m3u",
      "m3UFilePath": "",
      "xmltvUrl": "https://example.com/guide.xml",
      "autoRefreshIntervalHours": 12,
      "lastRefreshed": "2026-03-17T15:00:00Z",
      "isActive": true
    }
  ],
  "theme": "Dark",
  "lastSection": "guide",
  "preventDisplaySleep": false,
  "guidePreviewEnabled": false,
  "overlayAutoHide": false,
  "guidePastHours": 5,
  "epgLookAheadHours": 6,
  "autoRefreshEpg": true,
  "refreshIntervalMinutes": 360,
  "playerWaitForStreamSeconds": 7.36,
  "playerDeinterlaceEnabled": false,
  "playerImageSmoothingEnabled": true,
  "playerPicturePreset": "warm",
  "playerBufferSeconds": 2.24,
  "playerUserAgent": "OKILTV-Test-Agent/1.0",
  "mpvDllPath": "C:/mpv/mpv-2.dll",
  "mpvOptions": {
    "cache": "yes"
  },
  "multiviewEnabled": false,
  "multiviewDefaultLayout": "grid2x2",
  "multiviewMaxTiles": 11,
  "multiviewPreferHwdec": false,
  "multiviewRetainSelectionOnPromotion": true,
  "minimizeToTrayOnMinimize": false,
  "dvrRecordingsDirectory": "/tmp/dvr",
  "dvrRemuxToMkv": false,
  "dvrStartOffsetMinutes": -3,
  "dvrEndOffsetMinutes": 7,
  "dvrSchedules": [
    {
      "id": "profile|42|2026-03-17T15:00:00.000Z|2026-03-17T16:00:00.000Z|Show",
      "profileId": "0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9",
      "channelId": 42,
      "channelName": "News",
      "streamUrl": "http://example.com/stream",
      "tvgId": "channel.news",
      "title": "Show",
      "subTitle": "Episode",
      "description": "Desc",
      "start": "2026-03-17T15:00:00.000Z",
      "stop": "2026-03-17T16:00:00.000Z",
      "createdAt": "2026-03-17T14:59:00.000Z"
    }
  ],
  "lastWatchedChannelId": {
    "0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9": 42
  },
  "favoriteChannelIdsByProfile": {
    "0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9": [42, 84]
  },
  "hiddenGroupsByProfile": {
    "0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9": ["Hidden Group"]
  },
  "groupOrderByProfile": {
    "0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9": ["News", "Sports"]
  },
  "hideUncheckedGroupsByProfile": {
    "0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9": true
  }
})json");
    file.close();

    SettingsManager settings(settingsPath);
    settings.load();
    const auto activeProfile = settings.activeProfile();

    QVERIFY(settings.current().activeProfileId.has_value());
    QCOMPARE(guidToString(settings.current().activeProfileId.value()), QStringLiteral("0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9"));
    QVERIFY(activeProfile.has_value());
    QCOMPARE(activeProfile->m3uUrl, QStringLiteral("https://example.com/playlist.m3u"));
    QCOMPARE(activeProfile->type, ProfileType::M3UUrl);
    QCOMPARE(activeProfile->xtreamServerTimezone, QStringLiteral("Asia/Dubai"));
    QCOMPARE(activeProfile->autoRefreshIntervalHours, 12);
    QCOMPARE(settings.current().lastSection, QStringLiteral("guide"));
    QCOMPARE(settings.current().preventDisplaySleep, false);
    QCOMPARE(settings.current().guidePreviewEnabled, false);
    QCOMPARE(settings.current().overlayAutoHide, false);
    QCOMPARE(settings.current().overlayInactivitySeconds, 60);
    QCOMPARE(settings.current().guidePastHours, 5);
    QVERIFY(std::abs(settings.current().playerWaitForStreamSeconds - 7.4) < 0.0001);
    QCOMPARE(settings.current().playerDeinterlaceEnabled, false);
    QCOMPARE(settings.current().playerImageSmoothingEnabled, true);
    QCOMPARE(settings.current().playerPicturePreset, QStringLiteral("warm"));
    QVERIFY(std::abs(settings.current().playerBufferSeconds - 2.2) < 0.0001);
    QCOMPARE(settings.current().playerUserAgent, QStringLiteral("OKILTV-Test-Agent/1.0"));
    QCOMPARE(settings.current().multiviewEnabled, false);
    QCOMPARE(settings.current().multiviewMaxTiles, 9);
    QCOMPARE(settings.current().multiviewPreferHwdec, false);
    QCOMPARE(settings.current().multiviewRetainSelectionOnPromotion, true);
    QCOMPARE(settings.current().minimizeToTrayOnMinimize, false);
    QCOMPARE(settings.current().dvrRecordingsDirectory, QStringLiteral("/tmp/dvr"));
    QCOMPARE(settings.current().dvrRemuxToMkv, false);
    QCOMPARE(settings.current().dvrStartOffsetMinutes, -3);
    QCOMPARE(settings.current().dvrEndOffsetMinutes, 7);
    QCOMPARE(settings.current().dvrSchedules.size(), 1);
    QCOMPARE(settings.current().dvrSchedules.first().channelId, 42);
    QCOMPARE(settings.current().dvrSchedules.first().title, QStringLiteral("Show"));
    QCOMPARE(settings.current().lastWatchedChannelId.value(QStringLiteral("0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9")), 42);
    QCOMPARE(settings.current().favoriteChannelIdsByProfile.value(QStringLiteral("0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9")).size(), 2);
    QCOMPARE(settings.current().hiddenGroupsByProfile.value(QStringLiteral("0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9")).size(), 1);
    QCOMPARE(settings.current().groupOrderByProfile.value(QStringLiteral("0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9")).size(), 2);
    QCOMPARE(settings.current().hideUncheckedGroupsByProfile.value(QStringLiteral("0d9e36cc-4d10-4f54-bf08-326a5ca2d7f9")), true);

    settings.save();

    QFile roundTrip(settingsPath);
    QVERIFY(roundTrip.open(QIODevice::ReadOnly));
    const auto storedDocument = QJsonDocument::fromJson(roundTrip.readAll());
    QVERIFY(storedDocument.isObject());
    QVERIFY(storedDocument.object().contains(QStringLiteral("protectedSettings")));
    auto combined = storedDocument.object();
    const auto secretSettings = QJsonDocument::fromJson(unprotectSecret(combined.value(QStringLiteral("protectedSettings")).toString()).toUtf8()).object();
    for (auto it = secretSettings.begin(); it != secretSettings.end(); ++it) combined.insert(it.key(), it.value());
    const QJsonDocument document(combined);
    QVERIFY(document.isObject());
    QVERIFY(document.object().contains(QStringLiteral("mpvDllPath")));
    QVERIFY(document.object().contains(QStringLiteral("lastSection")));
    QVERIFY(document.object().contains(QStringLiteral("preventDisplaySleep")));
    QVERIFY(document.object().contains(QStringLiteral("guidePreviewEnabled")));
    QVERIFY(document.object().contains(QStringLiteral("overlayAutoHide")));
    QVERIFY(document.object().contains(QStringLiteral("guidePastHours")));
    QVERIFY(document.object().contains(QStringLiteral("playerWaitForStreamSeconds")));
    QVERIFY(document.object().contains(QStringLiteral("playerDeinterlaceEnabled")));
    QVERIFY(document.object().contains(QStringLiteral("playerBufferSeconds")));
    QVERIFY(document.object().contains(QStringLiteral("playerUserAgent")));
    QVERIFY(document.object().contains(QStringLiteral("multiviewEnabled")));
    QVERIFY(document.object().contains(QStringLiteral("multiviewMaxTiles")));
    QVERIFY(document.object().contains(QStringLiteral("multiviewPreferHwdec")));
    QVERIFY(document.object().contains(QStringLiteral("multiviewRetainSelectionOnPromotion")));
    QVERIFY(document.object().contains(QStringLiteral("minimizeToTrayOnMinimize")));
    QVERIFY(document.object().contains(QStringLiteral("dvrRecordingsDirectory")));
    QVERIFY(document.object().contains(QStringLiteral("dvrRemuxToMkv")));
    QVERIFY(document.object().contains(QStringLiteral("dvrStartOffsetMinutes")));
    QVERIFY(document.object().contains(QStringLiteral("dvrEndOffsetMinutes")));
    QVERIFY(document.object().contains(QStringLiteral("dvrSchedules")));
    QVERIFY(document.object().contains(QStringLiteral("favoriteChannelIdsByProfile")));
    QVERIFY(document.object().contains(QStringLiteral("hideUncheckedGroupsByProfile")));
    QVERIFY(std::abs(document.object().value(QStringLiteral("playerWaitForStreamSeconds")).toDouble() - 7.4) < 0.0001);
    QCOMPARE(document.object().value(QStringLiteral("playerDeinterlaceEnabled")).toBool(), false);
    QCOMPARE(document.object().value(QStringLiteral("playerImageSmoothingEnabled")).toBool(), true);
    QVERIFY(std::abs(document.object().value(QStringLiteral("playerBufferSeconds")).toDouble() - 2.2) < 0.0001);
    QCOMPARE(document.object().value(QStringLiteral("playerUserAgent")).toString(), QStringLiteral("OKILTV-Test-Agent/1.0"));
    QCOMPARE(document.object().value(QStringLiteral("preventDisplaySleep")).toBool(), false);
    QCOMPARE(document.object().value(QStringLiteral("guidePastHours")).toInt(), 5);
    QCOMPARE(document.object().value(QStringLiteral("timeshiftEnabled")).toBool(), false);
    QCOMPARE(document.object().value(QStringLiteral("timeshiftWindowMinutes")).toInt(), 90);
    QCOMPARE(document.object().value(QStringLiteral("timeshiftStorageDirectory")).toString(), QStringLiteral(""));
    QCOMPARE(document.object().value(QStringLiteral("timeshiftMaxDiskGb")).toInt(), 8);
    QCOMPARE(document.object().value(QStringLiteral("multiviewEnabled")).toBool(), false);
    QVERIFY(!document.object().contains(QStringLiteral("multiviewDefaultLayout")));
    QCOMPARE(document.object().value(QStringLiteral("multiviewMaxTiles")).toInt(), 9);
    QCOMPARE(document.object().value(QStringLiteral("multiviewPreferHwdec")).toBool(), false);
    QCOMPARE(document.object().value(QStringLiteral("multiviewRetainSelectionOnPromotion")).toBool(), true);
    QVERIFY(document.object().value(QStringLiteral("profilesMigratedToSourceStore")).toBool(false));
    QCOMPARE(document.object().value(QStringLiteral("profiles")).toArray().size(), 0);
}

void CoreTests::settingsCompatibilityDefaultsNewPlayerTuningFields()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto settingsPath = tempDir.filePath(QStringLiteral("settings.json"));
    QFile file(settingsPath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"json({
  "activeProfileId": null,
  "profiles": [
    {
      "id": "4f9fe15d-cc67-4dc4-a1f0-9300e7722d4e",
      "name": "Legacy URL Profile",
      "type": 1,
      "xtreamBaseUrl": "",
      "xtreamUsername": "",
      "xtreamPassword": "",
      "m3UUrl": "https://example.com/legacy.m3u",
      "m3UFilePath": "",
      "xmltvUrl": "",
      "lastRefreshed": "0001-01-01T00:00:00",
      "isActive": false
    }
  ],
  "theme": "Dark",
  "lastSection": "live",
  "guidePreviewEnabled": true,
  "overlayAutoHide": true,
  "overlayAutoHideSeconds": 3,
  "epgLookAheadHours": 6,
  "autoRefreshEpg": true,
  "refreshIntervalMinutes": 360,
  "mpvDllPath": "",
  "mpvOptions": {}
})json");
    file.close();

    SettingsManager settings(settingsPath);
    settings.load();

    QVERIFY(std::abs(settings.current().playerWaitForStreamSeconds - 5.0) < 0.0001);
    QCOMPARE(settings.current().preventDisplaySleep, true);
    QCOMPARE(settings.current().guidePastHours, 6);
    QCOMPARE(settings.current().playerDeinterlaceEnabled, true);
    QCOMPARE(settings.current().playerImageSmoothingEnabled, false);
    QCOMPARE(settings.current().playerPicturePreset, QStringLiteral("standard"));
    QCOMPARE(normalizePlayerPicturePreset(QStringLiteral("unknown")), QStringLiteral("standard"));
    QVERIFY(std::abs(settings.current().playerBufferSeconds - 3.0) < 0.0001);
    QCOMPARE(settings.current().playerUserAgent, defaultPlayerUserAgent());
    QCOMPARE(settings.current().timeshiftEnabled, false);
    QCOMPARE(settings.current().timeshiftWindowMinutes, 90);
    QCOMPARE(settings.current().timeshiftStorageDirectory, QStringLiteral(""));
    QCOMPARE(settings.current().timeshiftMaxDiskGb, 8);
    QCOMPARE(settings.current().multiviewEnabled, true);
    QCOMPARE(settings.current().multiviewMaxTiles, 4);
    QCOMPARE(settings.current().multiviewPreferHwdec, true);
    QCOMPARE(settings.current().multiviewRetainSelectionOnPromotion, false);
    QCOMPARE(settings.current().minimizeToTrayOnMinimize, true);
    QCOMPARE(settings.current().dvrRecordingsDirectory, QStringLiteral(""));
    QCOMPARE(settings.current().dvrRemuxToMkv, true);
    QCOMPARE(settings.current().dvrStartOffsetMinutes, 2);
    QCOMPARE(settings.current().dvrEndOffsetMinutes, 2);
    QCOMPARE(settings.current().dvrSchedules.size(), 0);
    QCOMPARE(settings.current().hideUncheckedGroupsByProfile.value(QStringLiteral("missing-profile-id"), false), false);
    QCOMPARE(settings.current().profiles.first().autoRefreshIntervalHours, 24);
}

void CoreTests::portableBootstrapRoundTripsIndependentOfSettings()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto bootstrapPath = tempDir.filePath(QStringLiteral("OKILTV-portable.json"));
    const auto customRoot = tempDir.filePath(QStringLiteral("portable-data"));

    PortableBootstrapConfig config;
    config.dataRootOverride = customRoot;
    QString errorText;
    QVERIFY(PortableBootstrap::save(bootstrapPath, config, &errorText));
    QVERIFY2(errorText.isEmpty(), qPrintable(errorText));

    const auto loaded = PortableBootstrap::load(bootstrapPath);
    QCOMPARE(loaded.schemaVersion, 1);
    QCOMPARE(loaded.dataRootOverride, QDir::cleanPath(customRoot));
    QVERIFY(!QFile::exists(tempDir.filePath(QStringLiteral("settings.json"))));
}

void CoreTests::portableBootstrapDefaultsDataBesideMarker()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto markerPath = tempDir.filePath(QStringLiteral("OKILTV-portable.json"));
    QCOMPARE(
        PortableBootstrap::defaultDataRootForBootstrap(markerPath),
        tempDir.filePath(QStringLiteral("data")));
    QCOMPARE(PortableBootstrap::defaultDataRootForBootstrap({}), QString());
}

void CoreTests::appDataPathsSupportPortableOverridesAndFallback()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedAppDataEnv appData(tempDir.path());
    ScopedRuntimeContext runtimeContext;

    QCOMPARE(AppDataPaths::defaultDataDirectory(), tempDir.filePath(QStringLiteral("OKILTV")));
    QCOMPARE(AppDataPaths::dataDirectory(), tempDir.filePath(QStringLiteral("OKILTV")));

    RuntimeContext portableContext;
    portableContext.launchMode = LaunchMode::Portable;
    portableContext.portableBootstrapPath = tempDir.filePath(QStringLiteral("portable-bootstrap.json"));
    portableContext.dataRootOverride = QStringLiteral("relative/path");
    AppDataPaths::initializeRuntime(portableContext);
    QCOMPARE(AppDataPaths::dataDirectory(), tempDir.filePath(QStringLiteral("OKILTV")));

    portableContext.dataRootOverride = tempDir.filePath(QStringLiteral("portable-data"));
    AppDataPaths::initializeRuntime(portableContext);
    QCOMPARE(AppDataPaths::dataDirectory(), tempDir.filePath(QStringLiteral("portable-data")));
    QCOMPARE(AppDataPaths::settingsFile(), tempDir.filePath(QStringLiteral("portable-data/settings.json")));
}

void CoreTests::appDataPathsMigratesLegacyRootIntoOKILTVDirectory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedAppDataEnv appData(tempDir.path());
    ScopedRuntimeContext runtimeContext;

    const auto legacyRoot = tempDir.filePath(QStringLiteral("IptvPlayer"));
    const auto newRoot = tempDir.filePath(QStringLiteral("OKILTV"));
    QVERIFY(QDir().mkpath(legacyRoot));
    QVERIFY(!QFile::exists(newRoot + QStringLiteral("/settings.json")));

    QFile legacySettings(QDir(legacyRoot).filePath(QStringLiteral("settings.json")));
    QVERIFY(legacySettings.open(QIODevice::WriteOnly | QIODevice::Truncate));
    legacySettings.write("{\"theme\":\"Dark\"}");
    legacySettings.close();

    QCOMPARE(AppDataPaths::dataDirectory(), newRoot);
    QVERIFY(QFile::exists(QDir(newRoot).filePath(QStringLiteral("settings.json"))));
    QVERIFY(QFile::exists(QDir(legacyRoot).filePath(QStringLiteral("settings.json"))));
}

void CoreTests::debugLoggerOnlyWritesFilesForExplicitDump()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedAppDataEnv appData(tempDir.path());

    auto &logger = DebugLogger::instance();
    logger.startSessionLogging();
    logger.log(QStringLiteral("test"), QStringLiteral("Manual dump should keep this entry."));
    logger.log(
        QStringLiteral("test"),
        QStringLiteral(
            "Sensitive sample Authorization: Bearer test_token_20260519 url=https://provider.test/live/alice/secret/9.ts?token=test_token_20260519"));

    QCOMPARE(logger.sessionLogPath(), QString());

    const QDir dumpDir(AppDataPaths::debugDumpDirectory());
    QCOMPARE(dumpDir.entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 0);

    const auto dumpPath = logger.writeDump(QStringLiteral("Diagnostics summary"));
    QVERIFY(!dumpPath.isEmpty());
    QVERIFY(QFile::exists(dumpPath));

    QCOMPARE(dumpDir.entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 1);

    QFile dumpFile(dumpPath);
    QVERIFY(dumpFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto dumpContents = QString::fromUtf8(dumpFile.readAll());
    QVERIFY(dumpContents.contains(QStringLiteral("Diagnostics summary")));
    QVERIFY(dumpContents.contains(QStringLiteral("Manual dump should keep this entry.")));
    QVERIFY(dumpContents.contains(QStringLiteral("Session Log: <disabled>")));
    QVERIFY(!dumpContents.contains(QStringLiteral("test_token_20260519")));
    QVERIFY(!dumpContents.contains(QStringLiteral("alice")));
    QVERIFY(!dumpContents.contains(QStringLiteral("secret")));
    QVERIFY(dumpContents.contains(QStringLiteral("Authorization=***")));
}

void CoreTests::debugLoggerCursorAndSubscribersStayOrdered()
{
    auto &logger = DebugLogger::instance();
    logger.startSessionLogging();

    QList<DebugLogger::Entry> observedEntries;
    const auto subscriptionId = logger.subscribe([&observedEntries](const DebugLogger::Entry &entry) {
        observedEntries.push_back(entry);
    });
    QVERIFY(subscriptionId > 0);

    const auto cursorBefore = logger.latestCursor();
    logger.log(QStringLiteral("ui-test"), QStringLiteral("first-entry"));
    logger.log(QStringLiteral("ui-test"), QStringLiteral("second-entry"));

    const auto entries = logger.entriesSince(cursorBefore);
    QVERIFY(entries.size() >= 2);
    QCOMPARE(entries.at(entries.size() - 2).message, QStringLiteral("first-entry"));
    QCOMPARE(entries.at(entries.size() - 1).message, QStringLiteral("second-entry"));
    QVERIFY(entries.at(entries.size() - 2).cursor < entries.at(entries.size() - 1).cursor);

    QVERIFY(observedEntries.size() >= 2);
    QCOMPARE(observedEntries.at(observedEntries.size() - 2).message, QStringLiteral("first-entry"));
    QCOMPARE(observedEntries.at(observedEntries.size() - 1).message, QStringLiteral("second-entry"));

    logger.unsubscribe(subscriptionId);
}


void CoreTests::protectedStorageRoundTripAndTampering()
{
    const auto secret = QStringLiteral("żółć-audit-secret-123");
    const auto first = protectSecret(secret);
    const auto second = protectSecret(secret);
    QVERIFY(isProtectedSecret(first));
    QVERIFY(!first.contains(QStringLiteral("audit-secret")));
    QVERIFY(first != second);
    QCOMPARE(unprotectSecret(first), secret);
    QCOMPARE(unprotectSecret(QStringLiteral("legacy")), QStringLiteral("legacy"));
    const auto separator = first.lastIndexOf(u':');
    auto bytes = QByteArray::fromBase64(first.mid(separator + 1).toLatin1());
    bytes[bytes.size() - 1] = static_cast<char>(bytes.back() ^ 1);
    const auto damaged = first.left(separator + 1) + QString::fromLatin1(bytes.toBase64());
    QVERIFY_EXCEPTION_THROWN(unprotectSecret(damaged), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(unprotectSecret(QStringLiteral("okiltv-secret:v99:unknown")), std::runtime_error);
}

void CoreTests::credentialMigrationPreservesSettingsAndSources()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ServerProfile profile;
    profile.id = QUuid::createUuid();
    profile.name = QStringLiteral("Migrated source");
    profile.xtreamBaseUrl = QStringLiteral("https://provider.invalid");
    profile.xtreamUsername = QStringLiteral("audit-login");
    profile.xtreamPassword = QStringLiteral("audit-password");
    AppSettings legacy;
    legacy.profiles = { profile };
    legacy.activeProfileId = profile.id;
    legacy.playerVolume = 43;
    legacy.mpvOptions.insert(QStringLiteral("http-header-fields"), QStringLiteral("Authorization: Bearer audit-token"));
    DvrScheduleEntry recording;
    recording.id = QStringLiteral("recording");
    recording.profileId = guidToString(profile.id);
    recording.channelId = 5;
    recording.streamUrl = QStringLiteral("https://provider.invalid/live/audit-login/audit-password/5.ts");
    recording.start = QDateTime::currentDateTimeUtc().addSecs(3600);
    recording.stop = recording.start.addSecs(3600);
    legacy.dvrSchedules = { recording };
    const auto path = dir.filePath(QStringLiteral("settings.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const auto original = QJsonDocument(toJson(legacy)).toJson();
    QCOMPARE(file.write(original), original.size());
    file.close();
    QFile backup(dir.filePath(QStringLiteral("settings.json.invalid-old.bak")));
    QVERIFY(backup.open(QIODevice::WriteOnly));
    backup.write(original);
    backup.close();
    SettingsManager manager(path);
    manager.load();
    QVERIFY(manager.activeProfile().has_value());
    QCOMPARE(manager.activeProfile()->xtreamPassword, profile.xtreamPassword);
    QCOMPARE(manager.current().dvrSchedules.first().streamUrl, recording.streamUrl);
    QCOMPARE(manager.current().playerVolume, 43.0);
    const auto sourcePath = dir.filePath(QStringLiteral("sources/%1.json").arg(guidToString(profile.id)));
    for (const auto &artifact : { path, sourcePath, backup.fileName() }) {
        QFile persisted(artifact);
        QVERIFY2(persisted.open(QIODevice::ReadOnly), qPrintable(artifact));
        const auto bytes = persisted.readAll();
        QVERIFY(!bytes.contains("audit-password"));
        QVERIFY(!bytes.contains("audit-login"));
        QVERIFY(!bytes.contains("audit-token"));
    }
    SettingsManager restart(path);
    restart.load();
    QCOMPARE(restart.activeProfile()->xtreamPassword, profile.xtreamPassword);
    QCOMPARE(restart.current().dvrSchedules.first().streamUrl, recording.streamUrl);
    QCOMPARE(restart.current().mpvOptions, legacy.mpvOptions);
    // A standalone legacy source file (already split from settings) is migrated too.
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
    source.write(QJsonDocument(toJson(profile)).toJson());
    source.close();
    SettingsManager splitLegacy(path);
    splitLegacy.load();
    QVERIFY(source.open(QIODevice::ReadOnly));
    QVERIFY(!source.readAll().contains("audit-password"));
}

void CoreTests::credentialMigrationFailurePreservesLegacyFile()
{
    QTemporaryDir dir;
    ServerProfile profile;
    profile.id = QUuid::createUuid();
    profile.xtreamPassword = QStringLiteral("audit-password");
    AppSettings legacy;
    legacy.profiles = { profile };
    const auto bytes = QJsonDocument(toJson(legacy)).toJson();
    const auto path = dir.filePath(QStringLiteral("settings.json"));
    QFile settingsFile(path);
    QVERIFY(settingsFile.open(QIODevice::WriteOnly));
    settingsFile.write(bytes);
    settingsFile.close();
    QFile obstruction(dir.filePath(QStringLiteral("sources")));
    QVERIFY(obstruction.open(QIODevice::WriteOnly));
    obstruction.write("not-a-directory");
    obstruction.close();
    SettingsManager manager(path);
    QVERIFY_EXCEPTION_THROWN(manager.load(), std::runtime_error);
    QVERIFY(settingsFile.open(QIODevice::ReadOnly));
    QCOMPARE(settingsFile.readAll(), bytes);
    settingsFile.close();
    QVERIFY(obstruction.remove());
    manager.load();
    QCOMPARE(manager.profileById(profile.id)->xtreamPassword, profile.xtreamPassword);
}

void CoreTests::credentialMigrationProtectsDatabase()
{
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("iptv.db"));
    const auto id = QUuid::createUuid();
    int rebuilds = 0;
    const auto rebuildStarted = [&]() { ++rebuilds; };
    DatabaseService initial(path, rebuildStarted);
    QCOMPARE(rebuilds, 0);
    Channel channel;
    channel.id = 7;
    channel.profileId = id;
    channel.name = QStringLiteral("Fixture");
    channel.streamUrl = QStringLiteral("https://provider.invalid/live/user/audit-db-secret/7.ts");
    initial.upsertChannels({channel});
    QCOMPARE(initial.loadChannels(id).first().streamUrl, channel.streamUrl);
    const auto connectionName = QStringLiteral("credential-migration-fixture");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        query.prepare(QStringLiteral("UPDATE channels SET stream_url=?, icon_url=?, catchup_source_template=?"));
        query.addBindValue(channel.streamUrl);
        query.addBindValue(QStringLiteral("https://provider.invalid/audit-icon-secret"));
        query.addBindValue(QStringLiteral("https://provider.invalid/audit-catchup-secret/{utc}"));
        QVERIFY(query.exec());
    }
    QSqlDatabase::removeDatabase(connectionName);
    DatabaseService migrated(path, rebuildStarted);
    QCOMPARE(rebuilds, 1);
    QCOMPARE(migrated.loadChannels(id).first().streamUrl, channel.streamUrl);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    QVERIFY(!bytes.contains("audit-db-secret"));
    QVERIFY(!bytes.contains("audit-icon-secret"));
    QVERIFY(!bytes.contains("audit-catchup-secret"));
    DatabaseService again(path, rebuildStarted);
    QCOMPARE(rebuilds, 1);
    QCOMPARE(again.loadChannels(id).first().streamUrl, channel.streamUrl);
    file.close();
    {
        auto raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        raw.setDatabaseName(path);
        QVERIFY(raw.open());
        QSqlQuery query(raw);
        QVERIFY(query.exec(QStringLiteral("INSERT INTO security_migrations VALUES ('channel-cleanup-v1')")));
    }
    QSqlDatabase::removeDatabase(connectionName);
    DatabaseService resumed(path, rebuildStarted);
    QCOMPARE(rebuilds, 2);
    QCOMPARE(resumed.loadChannels(id).first().streamUrl, channel.streamUrl);
}


void CoreTests::sourceRemovalCleansProtectedCopies()
{
    QTemporaryDir dir;
    SettingsManager settings(dir.filePath(QStringLiteral("settings.json")));
    ServerProfile profile;
    profile.id = QUuid::createUuid();
    profile.xtreamPassword = QStringLiteral("remove-secret");
    QVERIFY(settings.addProfile(profile));
    DatabaseService database(dir.filePath(QStringLiteral("iptv.db")));
    Channel channel;
    channel.id = 1;
    channel.profileId = profile.id;
    channel.name = QStringLiteral("Fixture");
    channel.streamUrl = QStringLiteral("https://provider.invalid/remove-secret");
    database.upsertChannels({channel});
    DvrScheduleEntry entry;
    entry.profileId = guidToString(profile.id);
    entry.streamUrl = channel.streamUrl;
    settings.current().dvrSchedules.append(entry);
    settings.save();
    QVERIFY(settings.removeProfile(profile.id));
    QVERIFY(database.loadChannels(profile.id).isEmpty());
    QVERIFY(settings.current().dvrSchedules.isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("sources/%1.json").arg(guidToString(profile.id)))));
    SettingsManager restart(settings.settingsFilePath());
    restart.load();
    QVERIFY(restart.sourceSummaries().isEmpty());
    QVERIFY(restart.current().dvrSchedules.isEmpty());
}

void CoreTests::databaseCredentialMigrationRollsBackOnFailure()
{
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("iptv.db"));
    const auto profileId = QUuid::createUuid();
    DatabaseService database(path);
    Channel channel;
    channel.id = 1;
    channel.profileId = profileId;
    channel.name = QStringLiteral("Fixture");
    channel.streamUrl = QStringLiteral("https://provider.invalid/rollback-secret");
    database.upsertChannels({channel});
    const auto connectionName = QStringLiteral("migration-failure-fixture");
    {
        auto raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        raw.setDatabaseName(path);
        QVERIFY(raw.open());
        QSqlQuery query(raw);
        query.prepare(QStringLiteral("UPDATE channels SET stream_url=?"));
        query.addBindValue(channel.streamUrl);
        QVERIFY(query.exec());
        QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER refuse_migration BEFORE UPDATE ON channels BEGIN SELECT RAISE(ABORT, 'fixture failure'); END")));
        QVERIFY_EXCEPTION_THROWN(database.ensureSchema(), std::runtime_error);
        QVERIFY(query.exec(QStringLiteral("SELECT stream_url FROM channels")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), channel.streamUrl);
        query.finish();
        QVERIFY(query.exec(QStringLiteral("DROP TRIGGER refuse_migration")));
    }
    QSqlDatabase::removeDatabase(connectionName);
    database.ensureSchema();
    QCOMPARE(database.loadChannels(profileId).first().streamUrl, channel.streamUrl);
}


void CoreTests::lockedSourceRequiresExplicitReplacement()
{
    QTemporaryDir dir;
    SettingsManager original(dir.filePath(QStringLiteral("settings.json")));
    ServerProfile profile;
    profile.id = QUuid::createUuid();
    profile.xtreamPassword = QStringLiteral("original-secret");
    QVERIFY(original.addProfile(profile));
    original.setActiveProfileId(profile.id);
    QFile source(dir.filePath(QStringLiteral("sources/%1.json").arg(guidToString(profile.id))));
    const auto locked = QJsonDocument(QJsonObject {
        { QStringLiteral("protectedProfile"), QStringLiteral("okiltv-secret:v1:other-account:abc") }
    }).toJson();
    QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
    source.write(locked);
    source.close();
    SettingsManager moved(original.settingsFilePath());
    moved.load();
    QVERIFY(!moved.activeProfile().has_value());
    QVERIFY(!moved.lastLoadError().isEmpty());
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), locked);
    source.close();
    profile.xtreamPassword = QStringLiteral("reentered-secret");
    QVERIFY(moved.replaceProfile(profile.id, profile));
    QCOMPARE(moved.activeProfile()->xtreamPassword, profile.xtreamPassword);
    SettingsManager restart(original.settingsFilePath());
    restart.load();
    QCOMPARE(restart.activeProfile()->xtreamPassword, profile.xtreamPassword);
}

void CoreTests::lockedSettingsPreserveRecoveryData()
{
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("settings.json"));
    const auto unavailable = QStringLiteral("okiltv-secret:v1:another-account:abc");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject {{QStringLiteral("protectedSettings"), unavailable}}).toJson());
    file.close();
    SettingsManager manager(path);
    manager.load();
    QVERIFY(!manager.lastLoadError().isEmpty());
    manager.current().mpvOptions.insert(QStringLiteral("http-header-fields"), QStringLiteral("Authorization: Bearer new-secret"));
    manager.save();
    QVERIFY(manager.lastSaveError().isEmpty());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    QVERIFY(object.value(QStringLiteral("unavailableProtectedSettings")).toArray().contains(unavailable));
    SettingsManager restart(path);
    restart.load();
    QCOMPARE(restart.current().mpvOptions, manager.current().mpvOptions);
    file.close();
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const auto recoverable = protectSecret(QString::fromUtf8(QJsonDocument(QJsonObject {
        { QStringLiteral("playerUserAgent"), QStringLiteral("Recovered-Agent") }
    }).toJson()));
    file.write(QJsonDocument(QJsonObject {
        { QStringLiteral("unavailableProtectedSettings"), QJsonArray { recoverable } }
    }).toJson());
    file.close();
    SettingsManager unlocked(path);
    unlocked.load();
    QCOMPARE(unlocked.current().playerUserAgent, QStringLiteral("Recovered-Agent"));

}

void CoreTests::redactionMasksStructuredSecrets()
{
    const auto url = QStringLiteral("https://audit-user:audit-secret@example.invalid/video.ts");
    QVERIFY(!redactSensitiveUrl(url).contains(QStringLiteral("audit-secret")));
    QVERIFY(!redactSensitiveText(url).contains(QStringLiteral("audit-user")));
    const auto raw = QStringLiteral(R"({"username":"audit-user","password":"audit-secret","nested":{"xtreamPassword":"escaped\"secret"}})");
    const auto text = redactSensitiveText(QStringLiteral("API sample=") + raw);
    QVERIFY(!text.contains(QStringLiteral("audit-user")));
    QVERIFY(!text.contains(QStringLiteral("audit-secret")));
    QVERIFY(!text.contains(QStringLiteral("escaped")));
    const QJsonObject object {
        { QStringLiteral("password"), QStringLiteral("plain-secret") },
        { QStringLiteral("nested"), QJsonArray { QJsonObject {
            { QStringLiteral("xtreamPassword"), QStringLiteral("nested-secret") },
            { QStringLiteral("title"), QStringLiteral("Programme") } } } }
    };
    const auto sanitized = redactSensitiveJson(object).toObject();
    QCOMPARE(sanitized.value(QStringLiteral("password")).toString(), QStringLiteral("***"));
    const auto nested = sanitized.value(QStringLiteral("nested")).toArray().first().toObject();
    QCOMPARE(nested.value(QStringLiteral("xtreamPassword")).toString(), QStringLiteral("***"));
    QCOMPARE(nested.value(QStringLiteral("title")).toString(), QStringLiteral("Programme"));
}

void CoreTests::redactionMasksXtreamSecrets()
{
    const auto streamUrl = QStringLiteral("https://example.test/live/user1/pass1/12345.m3u8");
    const auto apiUrl =
        QStringLiteral("https://example.test/player_api.php?username=user1&password=pass1&action=get_live_streams");
    const auto logLine = QStringLiteral("GET %1 and %2").arg(streamUrl, apiUrl);

    const auto redactedStreamUrl = redactSensitiveUrl(streamUrl);
    const auto redactedApiUrl = redactSensitiveUrl(apiUrl);
    const auto redactedLine = redactSensitiveText(logLine);

    QVERIFY(!redactedStreamUrl.contains(QStringLiteral("user1")));
    QVERIFY(!redactedStreamUrl.contains(QStringLiteral("pass1")));
    QVERIFY(!redactedApiUrl.contains(QStringLiteral("username=user1")));
    QVERIFY(!redactedApiUrl.contains(QStringLiteral("password=pass1")));
    QVERIFY(!redactedLine.contains(QStringLiteral("user1")));
    QVERIFY(!redactedLine.contains(QStringLiteral("pass1")));
    QVERIFY(redactedApiUrl.contains(QStringLiteral("username=")));
    QVERIFY(redactedApiUrl.contains(QStringLiteral("password=")));
}

void CoreTests::redactionMasksAuthorizationAndTokenSecrets()
{
    const auto timeshiftUrl =
        QStringLiteral("https://provider.test/timeshift/alice/supersecret/61/2026-05-18:12-00/73.ts?token=abc123&auth=topsecret");
    const auto rawLog = QStringLiteral("Authorization: Bearer hi_there secret=%1").arg(timeshiftUrl);
    const auto optionLog = QStringLiteral("http-header-fields=Authorization: Bearer abc.def.ghi");

    const auto redactedUrl = redactSensitiveUrl(timeshiftUrl);
    const auto redactedLog = redactSensitiveText(rawLog);
    const auto redactedOptionLog = redactSensitiveText(optionLog);

    QVERIFY(!redactedUrl.contains(QStringLiteral("alice")));
    QVERIFY(!redactedUrl.contains(QStringLiteral("supersecret")));
    QVERIFY(!redactedUrl.contains(QStringLiteral("abc123")));
    QVERIFY(!redactedUrl.contains(QStringLiteral("topsecret")));
    QVERIFY(redactedUrl.contains(QStringLiteral("/timeshift/***/***/")));
    QVERIFY(!redactedLog.contains(QStringLiteral("hi_there")));
    QVERIFY(redactedLog.contains(QStringLiteral("Authorization=***")));
    QVERIFY(!redactedOptionLog.contains(QStringLiteral("abc.def.ghi")));
    QVERIFY(redactedOptionLog.contains(QStringLiteral("Authorization=***")));
}

void CoreTests::m3uParserKeepsFieldParity()
{
    const auto profileId = QUuid::createUuid();
    M3UService service;
    const auto channels = service.parse(
        QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXTINF:-1 tvg-id=\"BBC1.uk\" tvg-name=\"BBC One\" tvg-logo=\"http://logo.png\" group-title=\"UK\",BBC One HD\n"
            "http://server/1\n"),
        profileId);

    QCOMPARE(channels.size(), 1);
    QCOMPARE(channels.first().name, QStringLiteral("BBC One HD"));
    QCOMPARE(channels.first().tvgId, QStringLiteral("BBC1.uk"));
    QCOMPARE(channels.first().tvgName, QStringLiteral("BBC One"));
    QCOMPARE(channels.first().iconUrl, QStringLiteral("http://logo.png"));
    QCOMPARE(channels.first().categoryId, QStringLiteral("UK"));
    QCOMPARE(channels.first().categoryName, QStringLiteral("UK"));
    QCOMPARE(channels.first().streamUrl, QStringLiteral("http://server/1"));
}

void CoreTests::m3uParserCapturesSupportedCatchupMetadata()
{
    const auto profileId = QUuid::createUuid();
    M3UService service;
    const auto channels = service.parse(
        QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXTINF:-1 tvg-id=\"archive.one\" catchup=\"append\" catchup-days=\"3\" catchup-source=\"?utc={utc}&dur={duration}\" group-title=\"News\",Archive One\n"
            "http://archive.example/live.m3u8\n"
            "#EXTINF:-1 tvg-id=\"archive.two\" catchup=\"shift\" catchup-days=\"5\" catchup-source=\"http://ignored\"\n"
            "http://archive.example/unsupported.m3u8\n"),
        profileId);

    QCOMPARE(channels.size(), 2);
    QVERIFY(channels.first().catchupSupported);
    QCOMPARE(channels.first().catchupWindowHours, 72);
    QCOMPARE(channels.first().catchupMode, QStringLiteral("append"));
    QCOMPARE(channels.first().catchupSourceTemplate, QStringLiteral("?utc={utc}&dur={duration}"));
    QVERIFY(!channels.last().catchupSupported);
}

void CoreTests::m3uParserAcceptsFlexibleAttributeSyntax()
{
    const auto profileId = QUuid::createUuid();
    M3UService service;
    const auto channels = service.parse(
        QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXTINF:-1 TVG-ID='archive.one' tvg-name=\"Archive \\\"One\\\"\" TVG-LOGO=http://logo.one.png group-title='News Mix' CATCHUP=Append catchup-days=1.5 catchup-source='?utc={utc}&name=One, Two',Archive One, East\n"
            "http://archive.example/one.m3u8\n"
            "#EXTINF:-1 tvg-id=archive.two tvg-name='Archive Two' group-title=Sports catchup=\"default\" catchup-days='2' catchup-source=?utc={utc}&dur={duration},Archive Two\n"
            "http://archive.example/two.m3u8\n"),
        profileId);

    QCOMPARE(channels.size(), 2);

    QCOMPARE(channels.at(0).name, QStringLiteral("Archive One, East"));
    QCOMPARE(channels.at(0).tvgId, QStringLiteral("archive.one"));
    QCOMPARE(channels.at(0).tvgName, QStringLiteral("Archive \"One\""));
    QCOMPARE(channels.at(0).iconUrl, QStringLiteral("http://logo.one.png"));
    QCOMPARE(channels.at(0).categoryName, QStringLiteral("News Mix"));
    QVERIFY(channels.at(0).catchupSupported);
    QCOMPARE(channels.at(0).catchupWindowHours, 36);
    QCOMPARE(channels.at(0).catchupMode, QStringLiteral("append"));
    QCOMPARE(channels.at(0).catchupSourceTemplate, QStringLiteral("?utc={utc}&name=One, Two"));

    QCOMPARE(channels.at(1).tvgId, QStringLiteral("archive.two"));
    QCOMPARE(channels.at(1).tvgName, QStringLiteral("Archive Two"));
    QCOMPARE(channels.at(1).categoryName, QStringLiteral("Sports"));
    QVERIFY(channels.at(1).catchupSupported);
    QCOMPARE(channels.at(1).catchupWindowHours, 48);
    QCOMPARE(channels.at(1).catchupMode, QStringLiteral("default"));
    QCOMPARE(channels.at(1).catchupSourceTemplate, QStringLiteral("?utc={utc}&dur={duration}"));
}

void CoreTests::m3uParserSupportsLegacyTimeshiftCatchupMetadata()
{
    const auto profileId = QUuid::createUuid();
    M3UService service;
    const auto channels = service.parse(
        QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXTINF:-1 tvg-id=\"archive.legacy\" timeshift=\"3\",Legacy Archive\n"
            "http://archive.example/live.m3u8\n"
            "#EXTINF:-1 tvg-id=\"archive.precedence\" catchup=\"append\" catchup-days=\"2\" catchup-source=\"?utc={utc}\" timeshift=\"9\",Explicit Archive\n"
            "http://archive.example/explicit.m3u8\n"
            "#EXTINF:-1 tvg-id=\"archive.invalid.zero\" timeshift=\"0\",Zero Archive\n"
            "http://archive.example/zero.m3u8\n"
            "#EXTINF:-1 tvg-id=\"archive.invalid.text\" timeshift=\"nope\",Invalid Archive\n"
            "http://archive.example/invalid.m3u8\n"),
        profileId);

    QCOMPARE(channels.size(), 4);

    QVERIFY(channels.at(0).catchupSupported);
    QCOMPARE(channels.at(0).catchupWindowHours, 72);
    QCOMPARE(channels.at(0).catchupMode, QStringLiteral("append"));
    QCOMPARE(channels.at(0).catchupSourceTemplate, QStringLiteral("utc={utc}&lutc={lutc}"));

    QVERIFY(channels.at(1).catchupSupported);
    QCOMPARE(channels.at(1).catchupWindowHours, 48);
    QCOMPARE(channels.at(1).catchupMode, QStringLiteral("append"));
    QCOMPARE(channels.at(1).catchupSourceTemplate, QStringLiteral("?utc={utc}"));

    QVERIFY(!channels.at(2).catchupSupported);
    QVERIFY(!channels.at(3).catchupSupported);
}

void CoreTests::m3uParserFallsBackWhenTvgNameIsMissing()
{
    const auto profileId = QUuid::createUuid();
    M3UService service;
    const auto channels = service.parse(
        QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXTINF:-1 tvg-id=\"ABC.us@East\",ABC\n"
            "http://41.205.93.154/ABC/index.m3u8\n"),
        profileId);

    QCOMPARE(channels.size(), 1);
    QCOMPARE(channels.first().name, QStringLiteral("ABC"));
    QCOMPARE(channels.first().tvgId, QStringLiteral("ABC.us@East"));
    QCOMPARE(channels.first().tvgName, QStringLiteral("ABC"));
    QCOMPARE(channels.first().streamUrl, QStringLiteral("http://41.205.93.154/ABC/index.m3u8"));
}

void CoreTests::m3uParserUsesDisplayCategoryNameWhenGroupMissing()
{
    const auto profileId = QUuid::createUuid();
    M3UService service;
    const auto channels = service.parse(
        QByteArrayLiteral(
            "#EXTM3U\n"
            "#EXTINF:-1 tvg-id=\"NO_GROUP\",No Group Channel\n"
            "http://server/no-group.m3u8\n"),
        profileId);

    QCOMPARE(channels.size(), 1);
    QCOMPARE(channels.first().categoryId, QStringLiteral("__ungrouped__"));
    QCOMPARE(channels.first().categoryName, QStringLiteral("Ungrouped"));
}

void CoreTests::xtreamServiceParsesCatchupFields()
{
    auto network = std::make_shared<MockNetworkAccess>();

    ServerProfile profile;
    profile.xtreamBaseUrl = QStringLiteral("https://xtream.example");
    profile.xtreamUsername = QStringLiteral("alice");
    profile.xtreamPassword = QStringLiteral("secret");

    const QUrl authUrl(QStringLiteral("https://xtream.example/player_api.php?username=alice&password=secret"));
    network->setResponse(
        authUrl,
        QByteArrayLiteral(R"json({
            "user_info": { "auth": 1 },
            "server_info": { "timezone": "Europe/Warsaw" }
        })json"));

    const QUrl url(QStringLiteral("https://xtream.example/player_api.php?username=alice&password=secret&action=get_live_streams"));
    network->setResponse(
        url,
        QByteArrayLiteral(R"json([
            {
                "stream_id": 55,
                "name": "Archive News",
                "epg_channel_id": "archive.news",
                "category_id": "12",
                "stream_icon": "http://logo.png",
                "num": "9",
                "container_extension": "m3u8",
                "tv_archive": 1,
                "tv_archive_duration": 7
            },
            {
                "stream_id": 56,
                "name": "Archive Sports",
                "epg_channel_id": "archive.sports",
                "category_id": "13",
                "stream_icon": "http://logo2.png",
                "num": "10",
                "container_extension": "###",
                "tv_archive": 0,
                "tv_archive_duration": 0
            }
        ])json"));

    XtreamService service(network);
    service.setProfile(profile);
    const auto authInfo = service.authenticate();
    QVERIFY(authInfo.authenticated);
    QCOMPARE(authInfo.serverTimezone, QStringLiteral("Europe/Warsaw"));
    const auto channels = service.getLiveStreams();

    QCOMPARE(channels.size(), 2);
    QVERIFY(channels.first().catchupSupported);
    QCOMPARE(channels.first().catchupWindowHours, 168);
    QCOMPARE(channels.first().streamUrl, QStringLiteral("https://xtream.example/live/alice/secret/55.m3u8"));
    QCOMPARE(channels.last().streamUrl, QStringLiteral("https://xtream.example/live/alice/secret/56.ts"));
}

void CoreTests::xtreamServiceRejectsNonArrayPayloads()
{
    auto network = std::make_shared<MockNetworkAccess>();

    ServerProfile profile;
    profile.xtreamBaseUrl = QStringLiteral("https://xtream.example");
    profile.xtreamUsername = QStringLiteral("alice");
    profile.xtreamPassword = QStringLiteral("secret");

    const QUrl authUrl(QStringLiteral("https://xtream.example/player_api.php?username=alice&password=secret"));
    network->setResponse(
        authUrl,
        QByteArrayLiteral(R"json({
            "user_info": { "auth": 1 }
        })json"));

    const QUrl categoriesUrl(QStringLiteral("https://xtream.example/player_api.php?username=alice&password=secret&action=get_live_categories"));
    const QUrl streamsUrl(QStringLiteral("https://xtream.example/player_api.php?username=alice&password=secret&action=get_live_streams"));
    network->setResponse(
        categoriesUrl,
        QByteArrayLiteral(R"json({
            "error": "forbidden"
        })json"));
    network->setResponse(
        streamsUrl,
        QByteArrayLiteral(R"json({
            "error": "temporarily unavailable"
        })json"));

    XtreamService service(network);
    service.setProfile(profile);
    const auto authInfo = service.authenticate();
    QVERIFY(authInfo.authenticated);

    try {
        service.getLiveCategories();
        QFAIL("Expected categories shape failure.");
    } catch (const std::runtime_error &error) {
        const auto text = QString::fromUtf8(error.what());
        QVERIFY(text.contains(QStringLiteral("expected array")));
        QVERIFY(text.contains(QStringLiteral("sample=")));
    }

    try {
        service.getLiveStreams();
        QFAIL("Expected streams shape failure.");
    } catch (const std::runtime_error &error) {
        const auto text = QString::fromUtf8(error.what());
        QVERIFY(text.contains(QStringLiteral("expected array")));
        QVERIFY(text.contains(QStringLiteral("sample=")));
    }
}

void CoreTests::epgParserHandlesOffsetsAndOrdering()
{
    EpgService service;
    service.loadFromBytes(QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<tv>"
        "  <programme start=\"20260317190000 +0000\" stop=\"20260317200000 +0000\" channel=\"BBC1.uk\"><title>B</title><sub-title>Episode B</sub-title></programme>"
        "  <programme start=\"20260317180000 +0000\" stop=\"20260317190000 +0000\" channel=\"BBC1.uk\"><title>A</title></programme>"
        "  <programme start=\"20260317200000\" stop=\"20260317210000\" channel=\"BBC1.uk\"><title>C</title></programme>"
        "</tv>"));

    QCOMPARE(service.totalEntries(), 3);
    const auto entries = service.programsInRange(
        QStringLiteral("BBC1.uk"),
        QDateTime::fromString(QStringLiteral("2026-03-17T17:00:00Z"), Qt::ISODate),
        QDateTime::fromString(QStringLiteral("2026-03-17T22:00:00Z"), Qt::ISODate));
    QCOMPARE(entries.size(), 3);
    QCOMPARE(entries.at(0).title, QStringLiteral("A"));
    QCOMPARE(entries.at(1).title, QStringLiteral("B"));
    QCOMPARE(entries.at(1).subTitle, QStringLiteral("Episode B"));
    QCOMPARE(entries.at(2).title, QStringLiteral("C"));
    QVERIFY(entries.at(2).subTitle.isEmpty());
}

void CoreTests::epgParserRejectsMalformedPayloads()
{
    QVERIFY_EXCEPTION_THROWN(EpgService::parseEntries(QByteArray {}), std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(
        EpgService::parseEntries(QByteArrayLiteral("<html><body>forbidden</body></html>")),
        std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(
        EpgService::parseEntries(QByteArrayLiteral("<tv><programme start=\"20260317190000 +0000\"")),
        std::runtime_error);
}

void CoreTests::epgParserAcceptsValidEmptyDocument()
{
    const auto entries = EpgService::parseEntries(
        QByteArrayLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?><tv></tv>"));
    QCOMPARE(entries.size(), 0);
}

void CoreTests::epgParserSupportsGzipPayloads()
{
    const auto xml = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<tv>"
        "  <programme start=\"20260317190000 +0000\" stop=\"20260317200000 +0000\" channel=\"BBC1.uk\"><title>Compressed Programme</title></programme>"
        "</tv>");
    const auto compressed = gzipCompress(xml);
    QVERIFY(!compressed.isEmpty());

    const auto entries = EpgService::parseEntries(compressed);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().title, QStringLiteral("Compressed Programme"));
}

void CoreTests::epgCacheRoundTripsWithMetadata()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ScopedAppDataEnv appData(tempDir.path());

    ServerProfile profile;
    profile.id = QUuid::createUuid();
    profile.type = ProfileType::M3UUrl;
    profile.xmltvUrl = QStringLiteral("https://example.com/guide.xml");

    EpgCacheService cache;
    EpgCacheService::CacheData data;
    data.profileId = profile.id;
    data.sourceFingerprint = EpgCacheService::sourceFingerprint(profile);
    data.fetchedAt = QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate);
    data.snapshot = EpgService::buildSnapshot({
        EpgEntry {
            QStringLiteral("channel.one"),
            QStringLiteral("Morning News"),
            QStringLiteral("Latest headlines"),
            QStringLiteral("S1E1"),
            QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate),
            QDateTime::fromString(QStringLiteral("2026-03-17T19:00:00Z"), Qt::ISODate),
            QStringLiteral("Top Story")
        }
    });

    cache.save(data);

    const auto loaded = cache.load(profile.id);
    QCOMPARE(loaded.status, EpgCacheService::LoadStatus::Loaded);
    QCOMPARE(loaded.data.profileId, profile.id);
    QCOMPARE(loaded.data.sourceFingerprint, data.sourceFingerprint);
    QCOMPARE(loaded.data.fetchedAt, data.fetchedAt);
    QCOMPARE(loaded.data.snapshot.allEntries.size(), 1);
    QCOMPARE(loaded.data.snapshot.allEntries.first().title, QStringLiteral("Morning News"));
    QCOMPARE(loaded.data.snapshot.allEntries.first().subTitle, QStringLiteral("Top Story"));
    QCOMPARE(loaded.data.snapshot.totalEntries, 1);
}

void CoreTests::epgCacheAgeCalculationsStayStable()
{
    const auto fetchedAt = QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate);
    const auto now = QDateTime::fromString(QStringLiteral("2026-03-17T19:00:00Z"), Qt::ISODate);

    QCOMPARE(EpgCacheService::ageSeconds(fetchedAt, now), 3600);
    QCOMPARE(
        EpgCacheService::nextRefreshAt(fetchedAt, 90),
        QDateTime::fromString(QStringLiteral("2026-03-17T19:30:00Z"), Qt::ISODate));
    QVERIFY(!EpgCacheService::isStale(fetchedAt, 61, now));
    QVERIFY(EpgCacheService::isStale(fetchedAt, 60, now));
}

void CoreTests::epgCacheFingerprintInvalidatesChangedSource()
{
    ServerProfile profile;
    profile.id = QUuid::createUuid();
    profile.type = ProfileType::M3UUrl;
    profile.xmltvUrl = QStringLiteral("https://example.com/guide-a.xml");

    EpgCacheService::CacheData data;
    data.profileId = profile.id;
    data.sourceFingerprint = EpgCacheService::sourceFingerprint(profile);

    QVERIFY(EpgCacheService::matchesProfile(data, profile));

    profile.xmltvUrl = QStringLiteral("https://example.com/guide-b.xml");
    QVERIFY(!EpgCacheService::matchesProfile(data, profile));
}

void CoreTests::epgSnapshotAppliesPrebuiltIndex()
{
    const auto entries = EpgService::parseEntries(QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<tv>"
        "  <programme start=\"20260317190000 +0000\" stop=\"20260317200000 +0000\" channel=\"BBC1.uk\"><title>B</title></programme>"
        "  <programme start=\"20260317180000 +0000\" stop=\"20260317190000 +0000\" channel=\"BBC1.uk\"><title>A</title></programme>"
        "</tv>"));

    EpgService service;
    service.applySnapshot(EpgService::buildSnapshot(entries));

    QCOMPARE(service.totalEntries(), 2);
    const auto range = service.programsInRange(
        QStringLiteral("BBC1.uk"),
        QDateTime::fromString(QStringLiteral("2026-03-17T17:00:00Z"), Qt::ISODate),
        QDateTime::fromString(QStringLiteral("2026-03-17T21:00:00Z"), Qt::ISODate));
    QCOMPARE(range.size(), 2);
    QCOMPARE(range.first().title, QStringLiteral("A"));
    QCOMPARE(service.allEntries().first().title, QStringLiteral("A"));
}

void CoreTests::epgRangeLookupPreservesOverlapsAndBoundaries()
{
    EpgService service;
    const auto start = QDateTime::fromString(QStringLiteral("2026-03-17T00:00:00Z"), Qt::ISODate);
    const auto channel = QStringLiteral("test");
    const QList<EpgEntry> entries {
        { channel, QStringLiteral("Long"), {}, {}, start, start.addSecs(10 * 3600) },
        { channel, QStringLiteral("Short"), {}, {}, start.addSecs(3600), start.addSecs(2 * 3600) },
        { channel, QStringLiteral("Later"), {}, {}, start.addSecs(8 * 3600), start.addSecs(9 * 3600) }
    };
    service.loadFromEntries(entries);
    const auto from = start.addSecs(8 * 3600);
    const auto to = start.addSecs(9 * 3600);
    const auto result = service.programsInRange(QStringLiteral("TEST"), from, to);
    QCOMPARE(result.size(), 2);
    QCOMPARE(result.first().title, QStringLiteral("Long"));
    QCOMPARE(result.last().title, QStringLiteral("Later"));
    QCOMPARE(service.programsInRange(channel, from, to, 1).size(), 1);
    QVERIFY(service.programsInRange(channel, from, to, 0).isEmpty());
    QVERIFY(service.programsInRange(channel, to, from).isEmpty());
    QVERIFY(service.programsInRange(channel, from, from).isEmpty());
    QVERIFY(service.programsInRange(channel, start.addSecs(10 * 3600), start.addDays(1)).isEmpty());
    QVERIFY(service.programsInRange(channel, start.addSecs(-1), start).isEmpty());
    QCOMPARE(service.programsInRange(channel, to, start.addSecs(10 * 3600)).size(), 1);

    // Replacing a snapshot must also replace its range index.
    service.loadFromEntries({ entries.last() });
    QCOMPARE(service.programsInRange(channel, from, to).size(), 1);
    service.clear();
    QVERIFY(service.programsInRange(channel, from, to).isEmpty());
}

void CoreTests::databaseKeepsSchemaCompatible()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DatabaseService database(tempDir.filePath(QStringLiteral("iptv.db")));
    const auto profileId = QUuid::createUuid();

    Channel channel;
    channel.id = 42;
    channel.profileId = profileId;
    channel.name = QStringLiteral("BBC One");
    channel.streamUrl = QStringLiteral("http://server/1");
    channel.categoryId = QStringLiteral("UK");
    channel.tvgId = QStringLiteral("BBC1.uk");
    channel.tvgName = QStringLiteral("BBC One");
    channel.iconUrl = QStringLiteral("http://logo.png");
    channel.source = ChannelSource::M3U;
    channel.sortOrder = 0;
    database.upsertChannels({ channel });

    const auto loadedChannels = database.loadChannels(profileId);
    QCOMPARE(loadedChannels.size(), 1);
    QCOMPARE(loadedChannels.first().name, QStringLiteral("BBC One"));
    QVERIFY(!loadedChannels.first().catchupSupported);

    EpgEntry entry;
    entry.channelId = QStringLiteral("BBC1.uk");
    entry.title = QStringLiteral("News");
    entry.subTitle = QStringLiteral("Late Edition");
    entry.description = QStringLiteral("The news at six.");
    entry.start = QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate);
    entry.stop = QDateTime::fromString(QStringLiteral("2026-03-17T19:00:00Z"), Qt::ISODate);
    database.replaceEpg(profileId, { entry });

    const auto epgEntries = database.queryEpg(
        profileId,
        QStringLiteral("BBC1.uk"),
        QDateTime::fromString(QStringLiteral("2026-03-17T17:00:00Z"), Qt::ISODate),
        QDateTime::fromString(QStringLiteral("2026-03-17T20:00:00Z"), Qt::ISODate));
    QCOMPARE(epgEntries.size(), 1);
    QCOMPARE(epgEntries.first().title, QStringLiteral("News"));
    QCOMPARE(epgEntries.first().subTitle, QStringLiteral("Late Edition"));

    database.incrementWatchSeconds(profileId, channel.id, 61);
    database.incrementWatchSeconds(profileId, channel.id, 59);
    const auto watchSeconds = database.loadWatchSecondsByProfile(profileId);
    QCOMPARE(watchSeconds.value(channel.id, 0), 120);
}

void CoreTests::catchupProgressRoundsAndCompletes_data()
{
    QTest::addColumn<qint64>("positionMs");
    QTest::addColumn<qint64>("expectedSeconds");
    QTest::newRow("first-minute") << qint64(59999) << qint64(0);
    QTest::newRow("minute-boundary") << qint64(60000) << qint64(60);
    QTest::newRow("round-down") << qint64(1427000) << qint64(1380);
    QTest::newRow("before-completion") << qint64(3539999) << qint64(3480);
    QTest::newRow("last-minute") << qint64(3540000) << qint64(0);
    QTest::newRow("completed") << qint64(3600000) << qint64(0);
    QTest::newRow("beyond-end") << qint64(3700000) << qint64(0);
}

void CoreTests::catchupProgressRoundsAndCompletes()
{
    QFETCH(qint64, positionMs);
    QFETCH(qint64, expectedSeconds);
    CatchupProgress progress;
    progress.programStartMs = 10000000;
    progress.positionMs = positionMs;
    QCOMPARE(progress.resumeSeconds(progress.programStartMs + 3600000), expectedSeconds);
}

void CoreTests::databaseCatchupProgressPersistsAndIsolatesEmissions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("progress.sqlite"));
    DatabaseService database(path);
    database.ensureSchema();
    Channel channel;
    channel.id = 0;
    channel.profileId = QUuid::createUuid();
    channel.source = ChannelSource::M3U;
    channel.tvgId = QStringLiteral("station");
    channel.streamUrl = QStringLiteral("https://provider/live");
    const auto start = QDateTime::currentDateTimeUtc().addSecs(-7200);
    CatchupProgress progress { CatchupProgress::keyFor(channel, start), channel.profileId,
        start.toMSecsSinceEpoch(), start.addSecs(3600).toMSecsSinceEpoch(), 1427000,
        start.addDays(2).toMSecsSinceEpoch() };
    database.saveCatchupProgress(progress);
    DatabaseService reopened(path);
    const auto records = reopened.loadCatchupProgress();
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.first().positionMs, 1427000);
    QCOMPARE(records.first().resumeSeconds(progress.programStopMs), 1380);
    channel.name = QStringLiteral("Renamed station");
    QCOMPARE(CatchupProgress::keyFor(channel, start), progress.key);
    QVERIFY(CatchupProgress::keyFor(channel, start.addSecs(60)) != progress.key);
    channel.streamUrl += QStringLiteral("-different");
    QVERIFY(CatchupProgress::keyFor(channel, start) != progress.key);
    channel.streamUrl.chop(10);
    channel.profileId = QUuid::createUuid();
    QVERIFY(CatchupProgress::keyFor(channel, start) != progress.key);
    auto other = progress;
    other.key = CatchupProgress::keyFor(channel, start);
    other.profileId = channel.profileId;
    database.saveCatchupProgress(other);
    progress.positionMs = 600000; // A backward seek replaces the previous position.
    database.saveCatchupProgress(progress);
    QCOMPARE(database.loadCatchupProgress().size(), 2);
    progress.positionMs = 3540000;
    database.saveCatchupProgress(progress);
    QCOMPARE(database.loadCatchupProgress().size(), 1);
    QCOMPARE(database.loadCatchupProgress().first().key, other.key);
    database.removeCatchupProgress(other.key);
    QVERIFY(database.loadCatchupProgress().isEmpty());
}

void CoreTests::databasePersistsWatchSecondsForChannelIdZero()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DatabaseService database(tempDir.filePath(QStringLiteral("iptv.db")));
    const auto profileId = QUuid::createUuid();

    database.incrementWatchSeconds(profileId, 0, 30);
    database.incrementWatchSeconds(profileId, 0, 15);
    database.incrementWatchSeconds(profileId, -1, 90); // ignored invalid id

    const auto watchSeconds = database.loadWatchSecondsByProfile(profileId);
    QCOMPARE(watchSeconds.value(0, 0), 45);
    QVERIFY(!watchSeconds.contains(-1));
}

void CoreTests::databaseUpsertRefreshesTvgAndSourceFields()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DatabaseService database(tempDir.filePath(QStringLiteral("iptv.db")));
    const auto profileId = QUuid::createUuid();

    Channel channel;
    channel.id = 7;
    channel.profileId = profileId;
    channel.name = QStringLiteral("Example Channel");
    channel.streamUrl = QStringLiteral("http://server/original");
    channel.categoryId = QStringLiteral("news");
    channel.categoryName = QStringLiteral("News");
    channel.tvgId = QStringLiteral("old.tvg.id");
    channel.tvgName = QStringLiteral("Old TVG Name");
    channel.iconUrl = QStringLiteral("http://logo.png");
    channel.source = ChannelSource::M3U;
    channel.sortOrder = 1;
    channel.catchupSupported = true;
    channel.catchupWindowHours = 48;
    channel.catchupMode = QStringLiteral("append");
    channel.catchupSourceTemplate = QStringLiteral("?utc={utc}");
    database.upsertChannels({ channel });

    channel.tvgId = QStringLiteral("new.tvg.id");
    channel.tvgName = QStringLiteral("New TVG Name");
    channel.source = ChannelSource::Xtream;
    channel.catchupWindowHours = 72;
    database.upsertChannels({ channel });

    const auto loadedChannels = database.loadChannels(profileId);
    QCOMPARE(loadedChannels.size(), 1);
    QCOMPARE(loadedChannels.first().tvgId, QStringLiteral("new.tvg.id"));
    QCOMPARE(loadedChannels.first().tvgName, QStringLiteral("New TVG Name"));
    QCOMPARE(loadedChannels.first().source, ChannelSource::Xtream);
    QVERIFY(loadedChannels.first().catchupSupported);
    QCOMPARE(loadedChannels.first().catchupWindowHours, 72);
    QCOMPARE(loadedChannels.first().catchupMode, QStringLiteral("append"));
}

void CoreTests::databaseReplaceChannelsForProfilePrunesStaleRows()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DatabaseService database(tempDir.filePath(QStringLiteral("iptv.db")));
    const auto profileId = QUuid::createUuid();

    Channel first;
    first.id = 1;
    first.profileId = profileId;
    first.name = QStringLiteral("One");
    first.streamUrl = QStringLiteral("http://server/one");
    first.categoryId = QStringLiteral("News");
    first.tvgId = QStringLiteral("one");
    first.tvgName = QStringLiteral("One");

    Channel second = first;
    second.id = 2;
    second.name = QStringLiteral("Two");
    second.streamUrl = QStringLiteral("http://server/two");
    second.tvgId = QStringLiteral("two");
    second.tvgName = QStringLiteral("Two");

    database.replaceChannelsForProfile(profileId, { first, second });
    QCOMPARE(database.loadChannels(profileId).size(), 2);

    database.replaceChannelsForProfile(profileId, { second });
    const auto remaining = database.loadChannels(profileId);
    QCOMPARE(remaining.size(), 1);
    QCOMPARE(remaining.first().id, 2);

    database.replaceChannelsForProfile(profileId, {});
    QCOMPARE(database.loadChannels(profileId).size(), 0);
}

void CoreTests::settingsLoadInvalidJsonCreatesBackupAndReportsError()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto settingsPath = tempDir.filePath(QStringLiteral("settings.json"));
    QFile file(settingsPath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("{invalid-json");
    file.close();

    SettingsManager settings(settingsPath);
    settings.load();

    QVERIFY(!settings.lastLoadError().isEmpty());
    QCOMPARE(settings.current().profiles.size(), 0);

    const QDir dir(tempDir.path());
    const auto backups = dir.entryList(
        { QStringLiteral("settings.json.invalid-*.bak") },
        QDir::Files | QDir::NoDotAndDotDot);
    QVERIFY(!backups.isEmpty());
    QFile backup(dir.filePath(backups.first()));
    QVERIFY(backup.open(QIODevice::ReadOnly));
    const auto envelope = QJsonDocument::fromJson(backup.readAll()).object();
    const auto recovered = QByteArray::fromBase64(unprotectSecret(envelope.value(QStringLiteral("protectedBackup")).toString()).toLatin1());
    QCOMPARE(recovered, QByteArrayLiteral("{invalid-json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(QJsonDocument::fromJson(file.readAll()).isObject());
}

void CoreTests::channelGroupIdsDoNotDecryptChannels()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath(QStringLiteral("iptv.db"));
    DatabaseService database(path);
    const auto profile = QUuid::createUuid();
    const auto otherProfile = QUuid::createUuid();
    QList<Channel> channels;
    const QStringList categories { QStringLiteral("Sports"), QString(), QStringLiteral("News"), QStringLiteral("Sports") };
    for (int i = 0; i < categories.size(); ++i) {
        Channel channel;
        channel.id = i;
        channel.profileId = profile;
        channel.name = QStringLiteral("Fixture");
        channel.categoryId = categories[i];
        channel.sortOrder = i;
        channel.streamUrl = QStringLiteral("https://fixture.invalid/secret");
        channels.push_back(channel);
    }
    auto other = channels.first();
    other.profileId = otherProfile;
    other.categoryId = QStringLiteral("Other source");
    channels.push_back(other);
    database.upsertChannels(channels);
    const auto connectionName = QStringLiteral("group-metadata-fixture");
    {
        auto raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        raw.setDatabaseName(path);
        QVERIFY(raw.open());
        QSqlQuery query(raw);
        QVERIFY(query.exec(QStringLiteral("UPDATE channels SET stream_url='okiltv-secret:v99:unavailable'")));
    }
    QSqlDatabase::removeDatabase(connectionName);
    QVERIFY(database.loadChannels(profile).isEmpty());
    QCOMPARE(database.loadChannelGroupIds(profile),
        (QStringList { QStringLiteral("Sports"), ungroupedCategoryId(), QStringLiteral("News") }));
    QCOMPARE(database.loadChannelGroupIds(otherProfile), QStringList { QStringLiteral("Other source") });
    QVERIFY(database.loadChannelGroupIds(QUuid::createUuid()).isEmpty());
}

void CoreTests::settingsReloadCommitsExistingFile()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const auto path = tempDir.filePath(QStringLiteral("settings.json"));
    {
        SettingsManager settings(path);
        settings.load();
        settings.current().playerVolume = 37.5;
        settings.save();
        QVERIFY2(settings.lastSaveError().isEmpty(), qPrintable(settings.lastSaveError()));
    }
    // Every startup commits the migration state, including an already migrated
    // file. Windows rejects this if load() still owns an open read handle.
    for (int startup = 0; startup < 2; ++startup) {
        SettingsManager settings(path);
        settings.load();
        QVERIFY2(settings.lastSaveError().isEmpty(), qPrintable(settings.lastSaveError()));
        QCOMPARE(settings.current().playerVolume, 37.5);
    }
}

void CoreTests::settingsSaveReportsErrorAndCreatesParentDirectory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const auto nestedSettingsPath =
        tempDir.filePath(QStringLiteral("nested/path/settings.json"));
    SettingsManager nestedSettings(nestedSettingsPath);
    nestedSettings.load();
    nestedSettings.current().theme = QStringLiteral("Dark");
    nestedSettings.save();
    QVERIFY(nestedSettings.lastSaveError().isEmpty());
    QVERIFY(QFile::exists(nestedSettingsPath));

    const auto badPath = tempDir.filePath(QStringLiteral("as-directory"));
    QVERIFY(QDir().mkpath(badPath));
    SettingsManager failingSettings(badPath);
    QVERIFY_EXCEPTION_THROWN(failingSettings.load(), std::runtime_error);
    failingSettings.save();
    QVERIFY(!failingSettings.lastSaveError().isEmpty());
}

void CoreTests::catchupUrlResolverBuildsIndependentWindows()
{
    Channel channel;
    channel.id = 99;
    channel.source = ChannelSource::Xtream;
    channel.streamUrl = QStringLiteral("https://provider/live/99.ts");
    channel.catchupSupported = true;
    channel.catchupWindowHours = 72;
    ServerProfile profile;
    profile.xtreamBaseUrl = QStringLiteral("https://provider");
    profile.xtreamUsername = QStringLiteral("user");
    profile.xtreamPassword = QStringLiteral("pass");
    profile.xtreamServerTimezone = QStringLiteral("Europe/Warsaw");
    CatchupUrlResolver resolver(profile);
    const auto start = QDateTime::fromString(QStringLiteral("2026-09-18T10:00:17Z"), Qt::ISODate);
    const auto end = start.addSecs(7200);
    auto window = resolver.resolveWindow(channel, start, end);
    QVERIFY(window);
    QCOMPARE(window->programStartUtc, start.addSecs(-17));
    QCOMPARE(window->programStopUtc, end.addSecs(-17));
    QCOMPARE(window->durationSeconds, 7200);
    QCOMPARE(window->url, QStringLiteral("https://provider/timeshift/user/pass/120/2026-09-18:12-00/99.ts"));
    channel.source = ChannelSource::M3U;
    channel.catchupMode = QStringLiteral("append");
    channel.catchupSourceTemplate = QStringLiteral("utc=%7Butc%7D&lutc={lutc}&duration={duration}&units={duration:1000}");
    window = resolver.resolveWindow(channel, start, end);
    QVERIFY(window);
    QCOMPARE(window->programStartUtc, start);
    QCOMPARE(window->programStopUtc, end);
    QCOMPARE(window->url, QStringLiteral("https://provider/live/99.ts?utc=%1&lutc=%2&duration=7200&units=7200000")
        .arg(start.toSecsSinceEpoch()).arg(end.toSecsSinceEpoch()));
    channel.catchupMode = QStringLiteral("default");
    channel.catchupSourceTemplate.prepend(QStringLiteral("https://archive/channel.ts?"));
    window = resolver.resolveWindow(channel, start.addSecs(3600), end);
    QVERIFY(window);
    QVERIFY(window->url.startsWith(QStringLiteral("https://archive/channel.ts?")));
    QVERIFY(window->url.contains(QStringLiteral("duration=3600&units=3600000")));
    QVERIFY(!resolver.resolveWindow(channel, end, start));
}

void CoreTests::catchupUrlResolverBuildsXtreamAndM3uTargets()
{
    QCOMPARE(serverProfileFromJson(QJsonObject {}).catchupSafetyMinutes, 3);
    ServerProfile safetyProfile;
    for (const int minutes : {0, 1, 2, 3, 30}) {
        safetyProfile.catchupSafetyMinutes = minutes;
        QCOMPARE(serverProfileFromJson(toJson(safetyProfile)).catchupSafetyMinutes, minutes);
    }
    safetyProfile.catchupSafetyMinutes = -1;
    QCOMPARE(serverProfileFromJson(toJson(safetyProfile)).catchupSafetyMinutes, 0);
    QCOMPARE(serverProfileFromJson(QJsonObject {{ QStringLiteral("catchupSafetyMinutes"), -1 }}).catchupSafetyMinutes, 0);
    safetyProfile.catchupSafetyMinutes = 100;
    QCOMPARE(serverProfileFromJson(toJson(safetyProfile)).catchupSafetyMinutes, 30);
    Channel xtreamChannel;
    xtreamChannel.id = 99;
    xtreamChannel.streamUrl = QStringLiteral("https://xtream.example/live/bob/pw/99.m3u8");
    xtreamChannel.source = ChannelSource::Xtream;
    xtreamChannel.catchupSupported = true;
    xtreamChannel.catchupWindowHours = 168;

    ServerProfile xtreamProfile;
    xtreamProfile.xtreamBaseUrl = QStringLiteral("https://xtream.example");
    xtreamProfile.xtreamUsername = QStringLiteral("bob");
    xtreamProfile.xtreamPassword = QStringLiteral("pw");
    xtreamProfile.xtreamServerTimezone = QStringLiteral("Europe/Warsaw");

    EpgEntry program;
    program.start = QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate);
    program.stop = QDateTime::fromString(QStringLiteral("2026-03-17T18:42:00Z"), Qt::ISODate);

    CatchupUrlResolver xtreamResolver(xtreamProfile);
    const auto xtreamTarget = xtreamResolver.resolve(xtreamChannel, program);
    QVERIFY(xtreamTarget.has_value());
    QCOMPARE(
        xtreamTarget->url,
        QStringLiteral("https://xtream.example/timeshift/bob/pw/43/2026-03-17:19-00/99.m3u8"));

    Channel xtreamFallbackChannel = xtreamChannel;
    xtreamFallbackChannel.streamUrl = QStringLiteral("https://xtream.example/live/bob/pw/99");
    const auto xtreamFallbackTarget = xtreamResolver.resolve(xtreamFallbackChannel, program);
    QVERIFY(xtreamFallbackTarget.has_value());
    QCOMPARE(
        xtreamFallbackTarget->url,
        QStringLiteral("https://xtream.example/timeshift/bob/pw/43/2026-03-17:19-00/99.ts"));

    ServerProfile xtreamInvalidTimezoneProfile = xtreamProfile;
    xtreamInvalidTimezoneProfile.xtreamServerTimezone = QStringLiteral("Not/AZone");
    CatchupUrlResolver xtreamInvalidTimezoneResolver(xtreamInvalidTimezoneProfile);
    const auto xtreamInvalidTimezoneTarget = xtreamInvalidTimezoneResolver.resolve(xtreamChannel, program);
    QVERIFY(xtreamInvalidTimezoneTarget.has_value());
    QCOMPARE(
        xtreamInvalidTimezoneTarget->url,
        QStringLiteral("https://xtream.example/timeshift/bob/pw/43/2026-03-17:18-00/99.m3u8"));

    EpgEntry liveProgram;
    liveProgram.start = QDateTime::currentDateTimeUtc().addSecs(-60 * 60);
    liveProgram.stop = QDateTime::currentDateTimeUtc().addSecs(40 * 60);
    const auto beforeResolve = QDateTime::currentDateTimeUtc();
    const auto liveTarget = xtreamResolver.resolve(xtreamChannel, liveProgram);
    const auto afterResolve = QDateTime::currentDateTimeUtc();
    QVERIFY(liveTarget.has_value());
    const QRegularExpression durationPattern(
        QStringLiteral(R"(/timeshift/[^/]+/[^/]+/(\d+)/\d{4}-\d{2}-\d{2}:\d{2}-\d{2}/)"));
    const auto match = durationPattern.match(liveTarget->url);
    QVERIFY(match.hasMatch());
    bool parsedDurationOk = false;
    const auto observedDurationMinutes = match.captured(1).toLongLong(&parsedDurationOk);
    QVERIFY(parsedDurationOk);
    const auto startMinuteEpoch = static_cast<qint64>(std::floor(static_cast<double>(liveProgram.start.toUTC().toSecsSinceEpoch()) / 60.0));
    const auto expectedLowerBound = std::max<qint64>(
        1,
        static_cast<qint64>(std::floor(static_cast<double>(beforeResolve.addSecs(-180).toSecsSinceEpoch()) / 60.0))
            - startMinuteEpoch);
    const auto expectedUpperBound = std::max<qint64>(
        1,
        static_cast<qint64>(std::floor(static_cast<double>(afterResolve.addSecs(-180).toSecsSinceEpoch()) / 60.0))
            - startMinuteEpoch);
    QVERIFY(observedDurationMinutes >= expectedLowerBound);
    QVERIFY(observedDurationMinutes <= expectedUpperBound);

    Channel m3uChannel;
    m3uChannel.source = ChannelSource::M3U;
    m3uChannel.streamUrl = QStringLiteral("http://archive.example/live.m3u8");
    m3uChannel.catchupSupported = true;
    m3uChannel.catchupWindowHours = 72;
    m3uChannel.catchupMode = QStringLiteral("append");
    m3uChannel.catchupSourceTemplate = QStringLiteral("?utc={utc}&dur={duration:2}&date={Y}-{m}-{d}");

    CatchupUrlResolver m3uResolver;
    const auto m3uTarget = m3uResolver.resolve(m3uChannel, program);
    QVERIFY(m3uTarget.has_value());
    QCOMPARE(
        m3uTarget->url,
        QStringLiteral("http://archive.example/live.m3u8?utc=1773770400&dur=5040&date=2026-03-17"));

    Channel synthesizedM3uChannel;
    synthesizedM3uChannel.source = ChannelSource::M3U;
    synthesizedM3uChannel.streamUrl = QStringLiteral("http://archive.example/live.m3u8");
    synthesizedM3uChannel.catchupSupported = true;
    synthesizedM3uChannel.catchupWindowHours = 72;
    synthesizedM3uChannel.catchupMode = QStringLiteral("append");
    synthesizedM3uChannel.catchupSourceTemplate = QStringLiteral("utc={utc}&lutc={lutc}");

    const auto synthesizedTarget = m3uResolver.resolve(synthesizedM3uChannel, program);
    QVERIFY(synthesizedTarget.has_value());
    QCOMPARE(
        synthesizedTarget->url,
        QStringLiteral("http://archive.example/live.m3u8?utc=1773770400&lutc=1773772920"));

    Channel existingQueryChannel = synthesizedM3uChannel;
    existingQueryChannel.streamUrl = QStringLiteral("http://archive.example/live.m3u8?existing=1");

    const auto existingQueryTarget = m3uResolver.resolve(existingQueryChannel, program);
    QVERIFY(existingQueryTarget.has_value());
    QCOMPARE(
        existingQueryTarget->url,
        QStringLiteral("http://archive.example/live.m3u8?existing=1&utc=1773770400&lutc=1773772920"));

    Channel encodedPlaceholderChannel = synthesizedM3uChannel;
    encodedPlaceholderChannel.streamUrl = QStringLiteral("http://archive.example/live.m3u8?existing=1");
    encodedPlaceholderChannel.catchupSourceTemplate = QStringLiteral("utc=%7Butc%7D&lutc=%7Blutc%7D");

    const auto encodedPlaceholderTarget = m3uResolver.resolve(encodedPlaceholderChannel, program);
    QVERIFY(encodedPlaceholderTarget.has_value());
    QCOMPARE(
        encodedPlaceholderTarget->url,
        QStringLiteral("http://archive.example/live.m3u8?existing=1&utc=1773770400&lutc=1773772920"));

    Channel doubleEncodedPlaceholderChannel = synthesizedM3uChannel;
    doubleEncodedPlaceholderChannel.streamUrl = QStringLiteral("http://archive.example/live.m3u8?existing=1");
    doubleEncodedPlaceholderChannel.catchupSourceTemplate = QStringLiteral("utc=%257Butc%257D&lutc=%257Blutc%257D");

    const auto doubleEncodedPlaceholderTarget = m3uResolver.resolve(doubleEncodedPlaceholderChannel, program);
    QVERIFY(doubleEncodedPlaceholderTarget.has_value());
    QCOMPARE(
        doubleEncodedPlaceholderTarget->url,
        QStringLiteral("http://archive.example/live.m3u8?existing=1&utc=1773770400&lutc=1773772920"));

    Channel explicitQueryAppendChannel = synthesizedM3uChannel;
    explicitQueryAppendChannel.streamUrl = QStringLiteral("http://archive.example/live.m3u8?existing=1#frag");
    explicitQueryAppendChannel.catchupSourceTemplate = QStringLiteral("?utc={utc}&dur={duration}");

    const auto explicitQueryAppendTarget = m3uResolver.resolve(explicitQueryAppendChannel, program);
    QVERIFY(explicitQueryAppendTarget.has_value());
    QCOMPARE(
        explicitQueryAppendTarget->url,
        QStringLiteral("http://archive.example/live.m3u8?existing=1&utc=1773770400&dur=2520#frag"));

    Channel rawAppendChannel = synthesizedM3uChannel;
    rawAppendChannel.catchupSourceTemplate = QStringLiteral("/timeshift/{utc}");

    const auto rawAppendTarget = m3uResolver.resolve(rawAppendChannel, program);
    QVERIFY(rawAppendTarget.has_value());
    QCOMPARE(
        rawAppendTarget->url,
        QStringLiteral("http://archive.example/live.m3u8/timeshift/1773770400"));
}

void CoreTests::catchupUrlResolverRejectsUnavailableTargets()
{
    Channel channel;
    channel.source = ChannelSource::M3U;
    channel.catchupSupported = true;
    channel.catchupWindowHours = 0;

    EpgEntry program;
    program.start = QDateTime::fromString(QStringLiteral("2026-03-17T19:00:00Z"), Qt::ISODate);
    program.stop = QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate);

    CatchupUrlResolver resolver;
    QVERIFY(!resolver.resolve(channel, program).has_value());

    Channel unresolvedPlaceholderChannel;
    unresolvedPlaceholderChannel.source = ChannelSource::M3U;
    unresolvedPlaceholderChannel.streamUrl = QStringLiteral("http://archive.example/live.m3u8");
    unresolvedPlaceholderChannel.catchupSupported = true;
    unresolvedPlaceholderChannel.catchupWindowHours = 72;
    unresolvedPlaceholderChannel.catchupMode = QStringLiteral("append");
    unresolvedPlaceholderChannel.catchupSourceTemplate = QStringLiteral("utc={utc}&token={unknown}");

    EpgEntry validProgram;
    validProgram.start = QDateTime::fromString(QStringLiteral("2026-03-17T18:00:00Z"), Qt::ISODate);
    validProgram.stop = QDateTime::fromString(QStringLiteral("2026-03-17T18:42:00Z"), Qt::ISODate);
    QString failureReason;
    const auto unresolvedTarget = resolver.resolve(unresolvedPlaceholderChannel, validProgram, &failureReason);
    QVERIFY(!unresolvedTarget.has_value());
    QVERIFY(failureReason.contains(QStringLiteral("unresolved placeholders")));
}

QTEST_MAIN(CoreTests)

#include "tst_core.moc"
