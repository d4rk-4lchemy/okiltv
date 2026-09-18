#include "mpegtstimestampnormalizer.h"

#include <QList>
#include <algorithm>
#include <array>

namespace OKILTV::Player {
namespace {
constexpr qsizetype kPacketSize = 188;
constexpr qsizetype kMaxHeaderLookahead = qsizetype { 64 } * 1024;
constexpr quint64 kClockMask = (quint64 { 1 } << 33) - 1;
// Put the first source clock halfway through the format's wrap range, leaving
// room on both sides for leading audio and reordered video. The actual offset
// is computed from this source's first clock, not from a captured timestamp.
constexpr quint64 kSafeStart = quint64 { 1 } << 32;

unsigned byteAt(const QByteArray &data, qsizetype offset)
{
    return static_cast<unsigned char>(data.at(offset));
}

qsizetype payloadOffset(const QByteArray &data, qsizetype packet)
{
    const auto flags = byteAt(data, packet + 3);
    if ((flags & 0x10U) == 0 || (flags & 0xc0U) != 0) {
        return packet + kPacketSize;
    }
    return std::min(packet + kPacketSize,
        packet + 4 + ((flags & 0x20U) != 0 ? 1 + byteAt(data, packet + 4) : 0));
}

unsigned packetPid(const QByteArray &data, qsizetype packet)
{
    return ((byteAt(data, packet + 1) & 0x1fU) << 8) | byteAt(data, packet + 2);
}

quint64 pesClock(const QByteArray &data, const QList<qsizetype> &positions, int offset)
{
    const auto at = [&](int index) { return static_cast<quint64>(byteAt(data, positions.at(offset + index))); };
    return ((at(0) & 0x0eU) << 29) | (at(1) << 22) | ((at(2) & 0xfeU) << 14)
        | (at(3) << 7) | (at(4) >> 1);
}

void writePesClock(QByteArray &data, const QList<qsizetype> &positions, int offset, quint64 clock)
{
    const std::array<unsigned, 5> bytes {
        (byteAt(data, positions.at(offset)) & 0xf1U) | static_cast<unsigned>(((clock >> 30) & 7U) << 1),
        static_cast<unsigned>((clock >> 22) & 0xffU),
        static_cast<unsigned>(((clock >> 15) & 0x7fU) << 1) | 1U,
        static_cast<unsigned>((clock >> 7) & 0xffU),
        static_cast<unsigned>((clock & 0x7fU) << 1) | 1U,
    };
    for (int index = 0; index < 5; ++index) {
        data[positions.at(offset + index)] = static_cast<char>(bytes.at(static_cast<size_t>(index)));
    }
}
} // namespace

QByteArray MpegTsTimestampNormalizer::push(const QByteArray &data)
{
    m_pending.append(data);
    return drain(false);
}

QByteArray MpegTsTimestampNormalizer::finish()
{
    return drain(true);
}

quint64 MpegTsTimestampNormalizer::shifted(quint64 clock)
{
    if (!m_offset.has_value()) {
        m_offset = (kSafeStart - clock) & kClockMask;
    }
    return (clock + *m_offset) & kClockMask;
}

QByteArray MpegTsTimestampNormalizer::drain(bool finished)
{
    if (!m_detected) {
        if (m_pending.size() < 3 * kPacketSize && !finished) {
            return {};
        }
        m_detected = true;
        m_transportStream = m_pending.size() >= 3 * kPacketSize
            && byteAt(m_pending, 0) == 0x47U
            && byteAt(m_pending, kPacketSize) == 0x47U
            && byteAt(m_pending, 2 * kPacketSize) == 0x47U;
    }
    if (!m_transportStream) {
        QByteArray result;
        result.swap(m_pending);
        return result;
    }
    qsizetype consumed = 0;
    while (consumed + kPacketSize <= m_pending.size()) {
        if (byteAt(m_pending, consumed) != 0x47U) {
            // Preserve malformed trailing data instead of guessing packet alignment.
            m_transportStream = false;
            consumed = m_pending.size();
            break;
        }
        if (!processPacket(consumed, finished)) {
            break;
        }
        consumed += kPacketSize;
    }
    if (finished) {
        consumed = m_pending.size();
    }
    auto result = m_pending.left(consumed);
    m_pending.remove(0, consumed);
    return result;
}

bool MpegTsTimestampNormalizer::processPacket(qsizetype packet, bool finished)
{
    QList<qsizetype> header;
    int timestampFlags = 0;
    if ((byteAt(m_pending, packet + 1) & 0x40U) != 0
        && payloadOffset(m_pending, packet) < packet + kPacketSize) {
        const auto pid = packetPid(m_pending, packet);
        int required = 9;
        bool interrupted = false;
        // PES headers can straddle TS packets, including across network reads.
        for (auto next = packet; next + kPacketSize <= m_pending.size()
             && next - packet < kMaxHeaderLookahead; next += kPacketSize) {
            if (byteAt(m_pending, next) != 0x47U) {
                interrupted = true;
                break;
            }
            if (packetPid(m_pending, next) != pid) {
                continue;
            }
            if (next != packet && (byteAt(m_pending, next + 1) & 0x40U) != 0) {
                interrupted = true;
                break;
            }
            for (auto offset = payloadOffset(m_pending, next);
                 offset < next + kPacketSize && header.size() < required; ++offset) {
                header.append(offset);
                if (header.size() == 3
                    && (byteAt(m_pending, header[0]) != 0 || byteAt(m_pending, header[1]) != 0
                        || byteAt(m_pending, header[2]) != 1)) {
                    required = 3;
                }
                if (header.size() == 9) {
                    const auto flags = byteAt(m_pending, header[7]) >> 6;
                    if ((byteAt(m_pending, header[6]) & 0xc0U) == 0x80U
                        && (flags == 2 || flags == 3)
                        && byteAt(m_pending, header[8]) >= (flags == 3 ? 10U : 5U)) {
                        timestampFlags = static_cast<int>(flags);
                        required = flags == 3 ? 19 : 14;
                    }
                }
            }
            if (header.size() >= required) {
                break;
            }
        }
        if (header.size() < required) {
            if (!finished && !interrupted && m_pending.size() - packet < kMaxHeaderLookahead) {
                return false;
            }
            timestampFlags = 0;
        }
    }

    // PCR supplies the decode-clock origin when available. Keep the 27 MHz
    // extension and reserved bits unchanged; only shift its 90 kHz base.
    if ((byteAt(m_pending, packet + 3) & 0x20U) != 0 && byteAt(m_pending, packet + 4) >= 7) {
        const auto adaptationEnd = std::min(packet + kPacketSize,
            packet + 5 + static_cast<qsizetype>(byteAt(m_pending, packet + 4)));
        const auto flags = byteAt(m_pending, packet + 5);
        auto offset = packet + 6;
        for (const auto flag : { 0x10U, 0x08U }) {
            if ((flags & flag) == 0) {
                continue;
            }
            if (offset + 6 > adaptationEnd) {
                break;
            }
            const quint64 clock = (static_cast<quint64>(byteAt(m_pending, offset)) << 25)
                | (static_cast<quint64>(byteAt(m_pending, offset + 1)) << 17)
                | (static_cast<quint64>(byteAt(m_pending, offset + 2)) << 9)
                | (static_cast<quint64>(byteAt(m_pending, offset + 3)) << 1)
                | (byteAt(m_pending, offset + 4) >> 7);
            const auto value = shifted(clock);
            m_pending[offset] = static_cast<char>(value >> 25);
            m_pending[offset + 1] = static_cast<char>(value >> 17);
            m_pending[offset + 2] = static_cast<char>(value >> 9);
            m_pending[offset + 3] = static_cast<char>(value >> 1);
            m_pending[offset + 4] = static_cast<char>((byteAt(m_pending, offset + 4) & 0x7fU) | ((value & 1U) << 7));
            offset += 6;
        }
    }
    if (timestampFlags != 0) {
        if (timestampFlags == 3) {
            writePesClock(m_pending, header, 14, shifted(pesClock(m_pending, header, 14)));
        }
        writePesClock(m_pending, header, 9, shifted(pesClock(m_pending, header, 9)));
    }
    return true;
}

} // namespace OKILTV::Player
