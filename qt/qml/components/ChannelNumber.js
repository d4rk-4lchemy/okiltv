.pragma library

function localize(number, separator) {
    return String(number).replace(".", separator)
}

function badge(number, separator) {
    const parts = String(number).split(".")
    return parts[0].padStart(3, "0")
        + (parts.length === 2 ? separator + parts[1] : ".")
}

function isSeparator(key, text, separator) {
    return key === Qt.Key_Period || text === "." || text === separator
        || (separator === "," && key === Qt.Key_Comma)
        || (separator.length === 1 && key === separator.charCodeAt(0))
}

// An empty return value means an incomplete/invalid number, never an integer fallback.
function canonicalInput(input, separator) {
    const number = input.split(separator).join(".")
    if (!/^[0-9]+(\.[0-9]+)?$/.test(number)) return ""
    return number
}

function appendSeparator(input, separator) {
    return input.indexOf(separator) >= 0 ? input : input + separator
}
