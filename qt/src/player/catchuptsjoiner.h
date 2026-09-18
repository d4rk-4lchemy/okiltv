#pragma once

#include <QByteArray>
#include <QString>
#include <QMap>
#include <optional>

namespace OKILTV::Player {

// Joins byte-identical overlaps and splits validated PAT/PMT configuration
// replacements into media periods. A single DTS/PCR outlier is quarantined
// and corrected only when the next video PES confirms the original clock;
// A confirmed forward archive gap remains a failure on first encounter; its
// explicit retry may split a period across the missing time. Other sustained
// or unverifiable clock changes remain failures.
// All state is bounded and network-thread owned.
class CatchupTsJoiner
{
public:
    struct ForwardGap {
        int pid;
        qint64 previousDts;
        qint64 nextDts;
        bool operator==(const ForwardGap &) const = default;
    };
    // Only a gap observed by the failed transport may be skipped on its retry.
    void allowRetriedForwardGap(std::optional<ForwardGap> gap) { m_allowedForwardGap = gap; }
    std::optional<ForwardGap> failedForwardGap() const { return m_failedForwardGap; }
    QByteArray push(const QByteArray &data);
    bool beginContinuation();
    bool periodPending() const { return !m_nextPeriod.isEmpty(); }
    QByteArray takeNextPeriod();
    QByteArray finish();
    double periodDurationSeconds() const;
    QString periodDescription() const { return m_periodDescription; }
    bool matching() const { return !m_signature.isEmpty(); }
    bool valid() const { return m_valid; }
    QString errorString() const { return m_error; }
    double durationSeconds() const;
    quint64 repairedClockCount() const { return m_repairedClockCount; }
    QString clockRepairDescription() const { return m_clockRepairDescription; }

private:
    void observe(const QByteArray &data);
    bool repairIsolatedClock(qsizetype offset);
    void rememberRaw(const QByteArray &data);
    QByteArray inspectPackets(const QByteArray &data);
    void inspectSection(const QByteArray &section);
    struct Configuration {
        int program { -1 };
        int pmtPid { -1 };
        int pcrPid { -1 };
        QMap<int, int> streams;
        bool operator==(const Configuration &) const = default;
    };
    std::optional<Configuration> m_configuration;
    Configuration m_candidate;
    QByteArray m_inputPackets;
    QByteArray m_probe;
    QByteArray m_section;
    QByteArray m_sectionPackets;
    QByteArray m_lastPat;
    QByteArray m_lastPatSection;
    QByteArray m_lastPmtSection;
    QByteArray m_lastPmt;
    std::optional<ForwardGap> m_allowedForwardGap;
    std::optional<ForwardGap> m_failedForwardGap;
    std::optional<double> m_forwardPeriodStartSeconds;
    QByteArray m_nextPeriod;
    QString m_periodDescription;
    int m_sectionPid { -1 };
    int m_sectionCounter { -1 };
    bool m_probeIsPat { false };
    bool m_probeComplete { false };
    bool m_configurationChanged { false };
    qint64 m_frameStep { 0 };
    std::optional<qint64> m_lastDecodeClock;
    int m_lastDecodePid { -1 };
    quint64 m_repairedClockCount { 0 };
    QString m_clockRepairDescription;
    QByteArray m_tail;
    QByteArray m_signature;
    QByteArray m_search;
    QByteArray m_packets;
    QByteArray m_initialPackets;
    bool m_synchronized { false };
    std::optional<qint64> m_firstPts;
    std::optional<qint64> m_lastPts;
    int m_lastVideoPid { -1 };
    quint64 m_packetCount { 0 };
    qint64 m_maxPts { 0 };
    qint64 m_clockEpoch { 0 };
    bool m_valid { true };
    QString m_error;
};

} // namespace OKILTV::Player
