#pragma once

#include <QByteArray>
#include <optional>

namespace OKILTV::Player {

// Keeps the initial MPEG-TS clock away from the 33-bit wrap boundary. A single
// modular offset is applied to every PES PTS/DTS and PCR/OPCR, preserving A/V
// offsets and PCR extensions. No elementary stream data is decoded or changed.
class MpegTsTimestampNormalizer
{
public:
    QByteArray push(const QByteArray &data);
    QByteArray finish();
    bool active() const { return m_transportStream && m_offset.has_value(); }

private:
    QByteArray drain(bool finished);
    bool processPacket(qsizetype packetOffset, bool finished);
    quint64 shifted(quint64 clock);

    QByteArray m_pending;
    std::optional<quint64> m_offset;
    bool m_detected { false };
    bool m_transportStream { false };
};

} // namespace OKILTV::Player
