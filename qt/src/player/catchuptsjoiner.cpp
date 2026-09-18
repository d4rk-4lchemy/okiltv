#include "catchuptsjoiner.h"

#include <algorithm>
#include <utility>

namespace OKILTV::Player {
namespace {
constexpr qsizetype kPacketBytes = 188;
constexpr qsizetype kSignatureBytes = 32 * kPacketBytes;
constexpr qsizetype kSyncPackets = 5;
constexpr qint64 kClockWrap = 1LL << 33;
unsigned byteAt(const QByteArray &data, const qsizetype offset)
{
    return static_cast<unsigned char>(data.at(offset));
}
qint64 pesClock(const QByteArray &packet, const qsizetype at)
{
    return static_cast<qint64>(((static_cast<quint64>(byteAt(packet, at) >> 1) & 7U) << 30)
        | (static_cast<quint64>(byteAt(packet, at + 1)) << 22)
        | (static_cast<quint64>(byteAt(packet, at + 2) >> 1) << 15)
        | (static_cast<quint64>(byteAt(packet, at + 3)) << 7) | (byteAt(packet, at + 4) >> 1));
}
qint64 clockDelta(const qint64 next, const qint64 previous)
{
    return ((next - previous + kClockWrap / 2 + kClockWrap) % kClockWrap) - kClockWrap / 2;
}
qsizetype videoClockOffset(const QByteArray &packet)
{
    if (packet.size() != kPacketBytes || byteAt(packet, 0) != 0x47U
        || (byteAt(packet, 1) & 0xc0U) != 0x40U || (byteAt(packet, 3) & 0xd0U) != 0x10U) {
        return -1;
    }
    const qsizetype payload = 4 + ((byteAt(packet, 3) & 32U) != 0 ? 1 + byteAt(packet, 4) : 0);
    if (payload + 19 > kPacketBytes || packet.mid(payload, 3) != QByteArray::fromHex("000001")
        || (byteAt(packet, payload + 3) & 0xf0U) != 0xe0U
        || (byteAt(packet, payload + 7) & 0xc0U) != 0xc0U
        || byteAt(packet, payload + 8) < 10) {
        return -1;
    }
    for (const auto at : {payload + 9, payload + 14}) {
        if ((byteAt(packet, at) & 1U) == 0 || (byteAt(packet, at + 2) & 1U) == 0
            || (byteAt(packet, at + 4) & 1U) == 0) { return -1; }
    }
    return payload + 14; // Require DTS: do not interpolate reordered PTS alone.
}
std::optional<qint64> packetPcr(const QByteArray &packet)
{
    if ((byteAt(packet, 3) & 32U) == 0 || byteAt(packet, 4) < 7
        || byteAt(packet, 4) > 183 || (byteAt(packet, 5) & 16U) == 0) { return {}; }
    return static_cast<qint64>((static_cast<quint64>(byteAt(packet, 6)) << 25)
        | (static_cast<quint64>(byteAt(packet, 7)) << 17)
        | (static_cast<quint64>(byteAt(packet, 8)) << 9)
        | (static_cast<quint64>(byteAt(packet, 9)) << 1) | (byteAt(packet, 10) >> 7));
}
void shiftPesClock(QByteArray &packet, const qsizetype at, const qint64 shift)
{
    const auto value = static_cast<quint64>((pesClock(packet, at) + shift + kClockWrap) % kClockWrap);
    packet[at] = static_cast<char>((byteAt(packet, at) & 0xf1U) | (((value >> 30) & 7U) << 1));
    packet[at + 1] = static_cast<char>(value >> 22);
    packet[at + 2] = static_cast<char>(1U | (((value >> 15) & 127U) << 1));
    packet[at + 3] = static_cast<char>(value >> 7);
    packet[at + 4] = static_cast<char>(1U | ((value & 127U) << 1));
}
} // namespace

QByteArray CatchupTsJoiner::push(const QByteArray &data)
{
    if (!m_valid || periodPending()) {
        return {};
    }
    auto output = data;
    if (!m_synchronized) {
        // The first HTTP response can begin inside a packet. Verify a run of
        // sync bytes before dropping only that initial partial packet. Never
        // resynchronize later: that would hide corruption or break overlap joins.
        const auto taken = std::min(data.size(), (kSyncPackets + 1) * kPacketBytes - m_initialPackets.size());
        m_initialPackets.append(data.left(taken));
        for (qsizetype offset = 0; offset < kPacketBytes; ++offset) {
            if (offset + kSyncPackets * kPacketBytes > m_initialPackets.size()) {
                break;
            }
            bool aligned = true;
            for (qsizetype packet = 0; packet < kSyncPackets; ++packet) {
                if (byteAt(m_initialPackets, offset + packet * kPacketBytes) != 0x47U) {
                    aligned = false;
                    break;
                }
            }
            if (aligned) {
                output = m_initialPackets.mid(offset) + data.mid(taken);
                m_initialPackets.clear();
                m_synchronized = true;
                break;
            }
        }
        if (!m_synchronized) {
            if (m_initialPackets.size() >= (kSyncPackets + 1) * kPacketBytes) {
                m_valid = false;
                m_error = QStringLiteral("initial TS packet synchronization failed");
                m_initialPackets.clear();
            }
            return {};
        }
    }
    if (matching()) {
        m_search.append(data);
        const auto offset = m_search.indexOf(m_signature);
        if (offset < 0) {
            m_search = m_search.right(m_signature.size() - 1);
            return {};
        }
        output = m_search.mid(offset + m_signature.size());
        m_search.clear();
        m_signature.clear();
    }
    if (!output.isEmpty()) {
        output = inspectPackets(output);
    }
    return output;
}

bool CatchupTsJoiner::beginContinuation()
{
    if (!m_valid || !m_firstPts.has_value() || m_tail.size() < kSignatureBytes) {
        return false;
    }
    // Unpublished partial packets/PSI will be replayed after the overlap.
    m_inputPackets.clear();
    m_probe.clear();
    m_section.clear();
    m_sectionPid = -1;
    m_signature = m_tail;
    m_search.clear();
    return true;
}

double CatchupTsJoiner::durationSeconds() const
{
    return m_firstPts.has_value() ? static_cast<double>(m_maxPts - m_firstPts.value()) / 90000.0 : 0.0;
}

QByteArray CatchupTsJoiner::takeNextPeriod()
{
    return std::exchange(m_nextPeriod, {});
}

double CatchupTsJoiner::periodDurationSeconds() const
{
    if (m_forwardPeriodStartSeconds) { return *m_forwardPeriodStartSeconds; }
    return durationSeconds() + static_cast<double>(m_frameStep) / 90000.0;
}

QByteArray CatchupTsJoiner::finish()
{
    auto result = std::exchange(m_probe, {});
    result += std::exchange(m_inputPackets, {});
    observe(result);
    return result;
}

void CatchupTsJoiner::inspectSection(const QByteArray &section)
{
    // Only complete, current, CRC-validated single-section tables establish a
    // replacement. Other layouts retain conservative clock-error recovery.
    if (section.size() < 12 || (byteAt(section, 5) & 1U) == 0
        || byteAt(section, 6) != 0 || byteAt(section, 7) != 0) {
        m_probeComplete = true;
        return;
    }
    quint32 crc = 0xffffffffU;
    for (const auto value : section) {
        crc ^= static_cast<quint32>(static_cast<unsigned char>(value)) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) != 0 ? (crc << 1) ^ 0x04c11db7U : crc << 1;
        }
    }
    if (crc != 0) {
        m_probeComplete = true;
        return;
    }
    if (m_sectionPid == 0 && byteAt(section, 0) == 0) {
        Configuration candidate;
        int programmes = 0;
        for (qsizetype i = 8; i + 4 <= section.size() - 4; i += 4) {
            const auto program = static_cast<int>((byteAt(section, i) << 8) | byteAt(section, i + 1));
            if (program != 0) {
                ++programmes;
                candidate.program = program;
                candidate.pmtPid = static_cast<int>(((byteAt(section, i + 2) & 31U) << 8) | byteAt(section, i + 3));
            }
        }
        if (programmes != 1) {
            m_probeComplete = true;
            return;
        }
        m_lastPat = m_sectionPackets;
        m_lastPatSection = section;
        m_candidate = candidate;
        if (m_configuration && m_configuration->program == candidate.program
            && m_configuration->pmtPid == candidate.pmtPid) {
            m_probeComplete = true;
        }
        return;
    }
    if (m_sectionPid != m_candidate.pmtPid || byteAt(section, 0) != 2 || section.size() < 16) {
        m_probeComplete = true;
        return;
    }
    m_probeComplete = true;
    const auto program = static_cast<int>((byteAt(section, 3) << 8) | byteAt(section, 4));
    if (program != m_candidate.program) {
        return;
    }
    m_candidate.pcrPid = static_cast<int>(((byteAt(section, 8) & 31U) << 8) | byteAt(section, 9));
    m_candidate.streams.clear();
    qsizetype i = 12 + static_cast<qsizetype>(((byteAt(section, 10) & 15U) << 8) | byteAt(section, 11));
    bool hasVideo = false;
    while (i + 5 <= section.size() - 4) {
        const auto type = static_cast<int>(byteAt(section, i));
        const auto pid = static_cast<int>(((byteAt(section, i + 1) & 31U) << 8) | byteAt(section, i + 2));
        const auto length = static_cast<qsizetype>(((byteAt(section, i + 3) & 15U) << 8) | byteAt(section, i + 4));
        if (i + 5 + length > section.size() - 4) {
            return;
        }
        m_candidate.streams.insert(pid, type);
        hasVideo = hasVideo || type == 0x1b || type == 0x24 || type == 0x02;
        i += 5 + length;
    }
    if (i != section.size() - 4 || !hasVideo) {
        return;
    }
    m_lastPmtSection = section;
    m_lastPmt = m_sectionPackets;
    if (m_configuration && *m_configuration != m_candidate && m_firstPts.has_value()) {
        m_configurationChanged = true;
        m_periodDescription = QStringLiteral("program=%1->%2 pmt=%3->%4 pcr=%5->%6 oldStreams=%7 newStreams=%8")
            .arg(m_configuration->program).arg(m_candidate.program)
            .arg(m_configuration->pmtPid).arg(m_candidate.pmtPid)
            .arg(m_configuration->pcrPid).arg(m_candidate.pcrPid)
            .arg(m_configuration->streams.size()).arg(m_candidate.streams.size());
    } else {
        m_configuration = m_candidate;
    }
}

void CatchupTsJoiner::rememberRaw(const QByteArray &data)
{
    m_tail.append(data);
    if (m_tail.size() > kSignatureBytes) { m_tail = m_tail.right(kSignatureBytes); }
}

bool CatchupTsJoiner::repairIsolatedClock(const qsizetype offset)
{
    const auto packet = m_inputPackets.mid(offset, kPacketBytes);
    const auto at = videoClockOffset(packet);
    if (at < 0) { return true; }
    const auto pid = static_cast<int>(((byteAt(packet, 1) & 31U) << 8) | byteAt(packet, 2));
    const auto clock = pesClock(packet, at);
    const auto delta = m_lastDecodeClock ? clockDelta(clock, *m_lastDecodeClock) : 0;
    if (!m_configuration || pid != m_lastDecodePid || !m_lastDecodeClock
        || (delta >= -10 * 90000LL && delta <= 60 * 90000LL)) {
        m_lastDecodePid = pid;
        m_lastDecodeClock = clock;
        return true;
    }
    const auto pcr = packetPcr(packet);
    if (!pcr || std::abs(clockDelta(clock, *pcr)) > 90000 || !m_probe.isEmpty()
        || std::abs(clockDelta(pesClock(packet, at - 5), clock)) > 90000) { return true; }
    // Quarantine one PES only. The following video DTS must return to the old
    // clock within one second. A sustained reset remains a continuity failure.
    constexpr qsizetype limit = 1024LL * 1024;
    for (auto next = offset + kPacketBytes; next + kPacketBytes <= m_inputPackets.size(); next += kPacketBytes) {
        if (next - offset >= limit) { return true; }
        const auto candidate = m_inputPackets.mid(next, kPacketBytes);
        const auto nextPid = static_cast<int>(((byteAt(candidate, 1) & 31U) << 8) | byteAt(candidate, 2));
        if (byteAt(candidate, 0) != 0x47U) { return true; }
        if (nextPid == 0 || nextPid == m_configuration->pmtPid) {
            // Permit only byte-identical, previously validated single-packet
            // PSI sections inside the lookahead, never a configuration change.
            const qsizetype payload = 4 + ((byteAt(candidate, 3) & 32U) != 0 ? 1 + byteAt(candidate, 4) : 0);
            if ((byteAt(candidate, 1) & 0xc0U) != 0x40U || (byteAt(candidate, 3) & 0xd0U) != 0x10U
                || payload >= kPacketBytes) { return true; }
            const auto sectionAt = payload + 1 + static_cast<qsizetype>(byteAt(candidate, payload));
            const auto &known = nextPid == 0 ? m_lastPatSection : m_lastPmtSection;
            if (known.isEmpty() || sectionAt + known.size() > kPacketBytes
                || candidate.mid(sectionAt, known.size()) != known) { return true; }
            continue;
        }
        if (nextPid != pid || (byteAt(candidate, 1) & 64U) == 0) { continue; }
        const auto nextAt = videoClockOffset(candidate);
        if (nextAt < 0) { return true; }
        const auto nextClock = pesClock(candidate, nextAt);
        const auto gap = clockDelta(nextClock, *m_lastDecodeClock);
        const auto nextPcr = packetPcr(candidate);
        // PCR need not occur on every video PES. Keep looking within the
        // bounded quarantine while DTS stays in the new clock domain; only a
        // later DTS/PCR pair can confirm a sustained archive gap.
        if (delta > 60 * 90000LL && !nextPcr
            && clockDelta(nextClock, clock) > 0 && clockDelta(nextClock, clock) <= 90000) {
            continue;
        }
        // A sustained forward jump is missing archive time, not an isolated
        // outlier. Require two consistent video DTS/PCR samples and unchanged
        // validated tables. The first occurrence still takes normal recovery.
        if (delta > 60 * 90000LL && nextPcr
            && clockDelta(nextClock, clock) > 0 && clockDelta(nextClock, clock) <= 90000
            && std::abs(clockDelta(nextClock, *nextPcr)) <= 90000
            && m_firstPts && !m_lastPat.isEmpty() && !m_lastPmt.isEmpty()) {
            const ForwardGap forwardGap {pid, *m_lastDecodeClock, clock};
            if (m_allowedForwardGap == forwardGap) {
                m_forwardPeriodStartSeconds = static_cast<double>(
                    m_maxPts - *m_firstPts + clockDelta(pesClock(packet, at - 5), m_maxPts % kClockWrap)) / 90000.0;
                m_periodDescription = QStringLiteral("retried archive gap: pid=%1 jump=%2s; skipping missing media")
                    .arg(pid).arg(static_cast<double>(delta) / 90000.0, 0, 'f', 3);
                m_nextPeriod = m_lastPat + m_lastPmt + m_inputPackets.mid(offset);
                m_allowedForwardGap.reset();
                return false;
            }
            m_failedForwardGap = forwardGap;
        }
        if (gap <= 0 || gap > 90000 || !nextPcr || std::abs(clockDelta(nextClock, *nextPcr)) > 90000) { return true; }
        const auto restored = (*m_lastDecodeClock + gap / 2) % kClockWrap;
        const auto shift = clockDelta(restored, clock);
        auto repaired = m_inputPackets.mid(offset, next - offset);
        for (auto current = offset; current < next; current += kPacketBytes) {
            auto fixed = m_inputPackets.mid(current, kPacketBytes);
            const auto currentPid = static_cast<int>(((byteAt(fixed, 1) & 31U) << 8) | byteAt(fixed, 2));
            if (currentPid != pid) { continue; }
            if ((byteAt(fixed, 1) & 128U) != 0 || (byteAt(fixed, 3) & 192U) != 0
                || ((byteAt(fixed, 3) & 32U) != 0 && byteAt(fixed, 4) > 0 && (byteAt(fixed, 5) & 8U) != 0)) { return true; }
            if (current == offset) {
                shiftPesClock(fixed, at - 5, shift);
                shiftPesClock(fixed, at, shift);
            }
            // PCR is in the same erroneous clock domain as this PES. Keep its
            // offset to DTS and its extension bits unchanged; leave audio alone.
            if (const auto original = packetPcr(fixed)) {
                if (std::abs(clockDelta(*original, *pcr)) > 90000) { return true; }
                const auto value = static_cast<quint64>((*original + shift + kClockWrap) % kClockWrap);
                fixed[6] = static_cast<char>(value >> 25);
                fixed[7] = static_cast<char>(value >> 17);
                fixed[8] = static_cast<char>(value >> 9);
                fixed[9] = static_cast<char>(value >> 1);
                fixed[10] = static_cast<char>((byteAt(fixed, 10) & 127U) | ((value & 1U) << 7));
            }
            repaired.replace(current - offset, kPacketBytes, fixed);
        }
        m_inputPackets.replace(offset, repaired.size(), repaired);
        m_clockRepairDescription = QStringLiteral("pid=%1 previousDts=%2 invalidDts=%3 nextDts=%4 restoredDts=%5 bufferedBytes=%6")
            .arg(pid).arg(*m_lastDecodeClock).arg(clock).arg(nextClock).arg(restored).arg(repaired.size());
        m_lastDecodeClock = restored;
        ++m_repairedClockCount;
        return true;
    }
    return m_inputPackets.size() - offset >= limit;
}

QByteArray CatchupTsJoiner::inspectPackets(const QByteArray &data)
{
    m_inputPackets += data;
    const auto rawInput = m_inputPackets; // Overlap signatures always use provider bytes.
    QByteArray output;
    qsizetype offset = 0;
    while (offset + kPacketBytes <= m_inputPackets.size()) {
        if (!repairIsolatedClock(offset)) {
            if (periodPending()) {
                m_inputPackets.clear();
                return output;
            }
            break;
        }
        const auto packet = m_inputPackets.mid(offset, kPacketBytes);
        if (byteAt(packet, 0) != 0x47U) {
            m_valid = false;
            m_error = QStringLiteral("TS packet alignment lost after synchronization");
            break;
        }
        const auto pid = static_cast<int>(((byteAt(packet, 1) & 31U) << 8) | byteAt(packet, 2));
        const auto flags = byteAt(packet, 3);
        const bool start = (byteAt(packet, 1) & 64U) != 0;
        const qsizetype payload = 4 + ((flags & 32U) != 0 ? 1 + static_cast<qsizetype>(byteAt(packet, 4)) : 0);
        const bool psi = pid == 0 || pid == m_candidate.pmtPid;
        if (m_probe.isEmpty() && psi && start && (flags & 16U) != 0 && payload < kPacketBytes) {
            m_probeIsPat = pid == 0;
            m_probeComplete = false;
            m_configurationChanged = false;
        }
        const bool probing = !m_probe.isEmpty() || (psi && start && (flags & 16U) != 0 && payload < kPacketBytes);
        if (probing) {
            m_probe += packet;
            if (psi && (flags & 16U) != 0 && payload < kPacketBytes) {
                auto bytes = packet.mid(payload);
                const auto counter = static_cast<int>(flags & 15U);
                if (start) {
                    const auto pointer = static_cast<qsizetype>(byteAt(bytes, 0));
                    m_section = bytes.mid(std::min(bytes.size(), 1 + pointer));
                    m_sectionPackets = packet;
                    m_sectionPid = pid;
                } else if (pid == m_sectionPid && counter == ((m_sectionCounter + 1) & 15)) {
                    m_section += bytes;
                    m_sectionPackets += packet;
                } else {
                    m_section.clear();
                }
                m_sectionCounter = counter;
                if ((byteAt(packet, 1) & 128U) != 0 || (flags & 192U) != 0) {
                    m_section.clear();
                    m_probeComplete = true;
                }
                if (m_section.size() >= 3) {
                    const auto size = 3 + static_cast<qsizetype>(((byteAt(m_section, 1) & 15U) << 8) | byteAt(m_section, 2));
                    if (size > 1024) {
                        m_probeComplete = true;
                    } else if (m_section.size() >= size) {
                        inspectSection(m_section.left(size));
                        m_section.clear();
                    }
                }
            }
            if (m_configurationChanged) {
                m_nextPeriod = (m_probeIsPat ? QByteArray {} : m_lastPat) + m_probe + m_inputPackets.mid(offset + kPacketBytes);
                m_probe.clear();
                m_inputPackets.clear();
                return output;
            }
            if (m_probeComplete || m_probe.size() >= 1024LL * 1024) {
                observe(m_probe);
                output += m_probe;
                rememberRaw(m_probe);
                m_probe.clear();
            }
        } else {
            observe(packet);
            output += packet;
            rememberRaw(rawInput.mid(offset, kPacketBytes));
        }
        offset += kPacketBytes;
        if (!m_valid) {
            break;
        }
    }
    m_inputPackets.remove(0, offset);
    return output;
}

void CatchupTsJoiner::observe(const QByteArray &data)
{
    m_packets.append(data);
    qsizetype offset = 0;
    while (offset + kPacketBytes <= m_packets.size()) {
        if (byteAt(m_packets, offset) != 0x47U) {
            m_valid = false;
            m_error = QStringLiteral("TS packet alignment lost after synchronization");
            m_packets.clear();
            return;
        }
        ++m_packetCount;
        const auto pid = static_cast<int>(((byteAt(m_packets, offset + 1) & 0x1fU) << 8)
            | byteAt(m_packets, offset + 2));
        const auto flags = byteAt(m_packets, offset + 3);
        const auto adaptationLength = (flags & 0x20U) != 0 ? byteAt(m_packets, offset + 4) : 0U;
        const auto adaptationFlags = adaptationLength > 0 ? byteAt(m_packets, offset + 5) : 0U;
        const auto start = (byteAt(m_packets, offset + 1) & 0x40U) != 0;
        qsizetype payload = offset + 4;
        if ((flags & 0x20U) != 0) {
            payload += 1 + static_cast<qsizetype>(byteAt(m_packets, payload));
        }
        if (start && (flags & 0x10U) != 0 && payload + 14 <= offset + kPacketBytes
            && byteAt(m_packets, payload) == 0 && byteAt(m_packets, payload + 1) == 0
            && byteAt(m_packets, payload + 2) == 1
            && (byteAt(m_packets, payload + 3) & 0xf0U) == 0xe0U
            && (byteAt(m_packets, payload + 7) & 0x80U) != 0) {
            const auto p = payload + 9;
            const auto pts = static_cast<qint64>(
                (static_cast<quint64>((byteAt(m_packets, p) >> 1) & 7U) << 30)
                | (static_cast<quint64>(byteAt(m_packets, p + 1)) << 22)
                | (static_cast<quint64>(byteAt(m_packets, p + 2) >> 1) << 15)
                | (static_cast<quint64>(byteAt(m_packets, p + 3)) << 7)
                | (byteAt(m_packets, p + 4) >> 1));
            const auto previousPts = m_lastPts;
            const auto previousEpoch = m_clockEpoch;
            if (m_lastPts.has_value()) {
                const auto delta = pts - m_lastPts.value();
                if (delta < -kClockWrap / 2) {
                    m_clockEpoch += kClockWrap;
                } else if (delta > kClockWrap / 2) {
                    m_clockEpoch -= kClockWrap;
                }
            }
            m_lastPts = pts;
            const auto unwrapped = pts + m_clockEpoch;
            if (!m_firstPts.has_value()) {
                m_firstPts = unwrapped;
                m_maxPts = unwrapped;
            }
            if (unwrapped < m_maxPts - 10 * 90000LL || unwrapped > m_maxPts + 60 * 90000LL) {
                m_valid = false;
                m_error = QStringLiteral("video clock discontinuity: delta=%1s pid=%2 previousPid=%3 rawPts=%4 previousRawPts=%5 unwrappedPts=%6 maxPts=%7 epoch=%8 previousEpoch=%9 packet=%10 cc=%11 discontinuity=%12 randomAccess=%13 transportError=%14 adaptationLength=%15 verifiedDuration=%16s")
                    .arg(static_cast<double>(unwrapped - m_maxPts) / 90000.0, 0, 'f', 3)
                    .arg(pid).arg(m_lastVideoPid).arg(pts).arg(previousPts.value_or(-1))
                    .arg(unwrapped).arg(m_maxPts).arg(m_clockEpoch).arg(previousEpoch)
                    .arg(m_packetCount).arg(flags & 0x0fU)
                    .arg((adaptationFlags & 0x80U) != 0).arg((adaptationFlags & 0x40U) != 0)
                    .arg((byteAt(m_packets, offset + 1) & 0x80U) != 0)
                    .arg(adaptationLength).arg(durationSeconds(), 0, 'f', 3);
                return;
            }
            if (unwrapped > m_maxPts && unwrapped - m_maxPts <= 9000) {
                m_frameStep = unwrapped - m_maxPts;
            }
            m_lastVideoPid = pid;
            m_maxPts = std::max(m_maxPts, unwrapped);
        }
        offset += kPacketBytes;
    }
    m_packets.remove(0, offset);
}

} // namespace OKILTV::Player
