#pragma once

#include <QDateTime>
#include <QVariantList>
#include <QVariantMap>

namespace OKILTV::App {

// Recompute only clock-dependent presentation; retain identity and hydrated details.
inline void updateEpgProgramTime(QVariantMap &program, const QDateTime &nowUtc)
{
    if (program.isEmpty()) return;
    const auto start = QDateTime::fromString(program.value(QStringLiteral("start")).toString(), Qt::ISODateWithMs);
    const auto stop = QDateTime::fromString(program.value(QStringLiteral("stop")).toString(), Qt::ISODateWithMs);
    const bool isNow = start.isValid() && stop.isValid() && start <= nowUtc && nowUtc < stop;
    const auto durationMs = start.msecsTo(stop);
    program.insert(QStringLiteral("isNow"), isNow);
    program.insert(QStringLiteral("progressPercent"), isNow && durationMs > 0
        ? static_cast<double>(start.msecsTo(nowUtc)) / static_cast<double>(durationMs) * 100.0 : 0.0);
}

inline void updateEpgProgramTimes(QVariantList &programs, const QDateTime &nowUtc)
{
    for (auto &value : programs) {
        auto program = value.toMap();
        updateEpgProgramTime(program, nowUtc);
        value = program;
    }
}

} // namespace OKILTV::App
