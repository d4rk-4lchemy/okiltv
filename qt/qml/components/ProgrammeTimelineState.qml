import QtQml

QtObject {
    id: timeline
    property real startMs: 0
    property real endMs: 0
    property real nowMs: 0
    property real archiveEdgeMs: 0
    property real positionMs: 0
    readonly property bool valid: Number.isFinite(startMs) && Number.isFinite(endMs) && endMs > startMs
    readonly property real nowFraction: fractionAt(nowMs)
    readonly property real archiveFraction: Math.min(nowFraction, fractionAt(archiveEdgeMs))
    readonly property real positionFraction: Math.min(nowFraction, fractionAt(positionMs))
    readonly property bool hasFuture: valid && endMs > nowMs

    function fractionAt(epochMs) {
        return valid && Number.isFinite(epochMs)
            ? Math.max(0, Math.min(1, (epochMs - startMs) / (endMs - startMs))) : 0
    }

    function timeAt(fraction) {
        return valid && Number.isFinite(fraction)
            ? startMs + Math.max(0, Math.min(1, fraction)) * (endMs - startMs) : NaN
    }

    function isFuture(fraction, clockMs) {
        return timeAt(fraction) > clockMs
    }
}
