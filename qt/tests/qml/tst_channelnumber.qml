import QtQuick
import QtTest
import "../../qml/components/ChannelNumber.js" as NumberEntry

TestCase {
    name: "ChannelNumber"
    function test_regions_data() {
        return [ {tag: "Polish", locale: "pl_PL", separator: ","},
                 {tag: "English", locale: "en_US", separator: "."},
                 {tag: "Arabic", locale: "ar_EG", separator: "٫"} ]
    }
    function test_regions(data) {
        compare(Qt.locale(data.locale).decimalPoint, data.separator)
        verify(NumberEntry.isSeparator(Qt.Key_Period, ".", data.separator))
        verify(NumberEntry.isSeparator(0, data.separator, data.separator))
        const input = NumberEntry.appendSeparator("2", data.separator)
        compare(input, "2" + data.separator)
        compare(NumberEntry.appendSeparator(input, data.separator), input)
        compare(NumberEntry.canonicalInput(input, data.separator), "")
        compare(NumberEntry.canonicalInput(input + "5", data.separator), "2.5")
        compare(NumberEntry.localize("2.5", data.separator), "2" + data.separator + "5")
        compare(NumberEntry.badge("2.5", data.separator), "002" + data.separator + "5")
        compare(NumberEntry.badge("2", data.separator), "002.")
        compare(NumberEntry.isSeparator(Qt.Key_Comma, ",", data.separator), data.separator === ",")
    }
    function test_invalid() {
        for (const input of ["2,", "2,,5", "2.3.4", "-2", "+2", "1e2", "2 000"])
            compare(NumberEntry.canonicalInput(input, ","), "")
        compare(NumberEntry.canonicalInput("2,0000000000000000001", ","), "2.0000000000000000001")
    }
}
