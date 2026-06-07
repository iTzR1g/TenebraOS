import QtQuick 2.0

Text {
    property int slideIndex: 0
    property int slideCount: 1
    text: (slideIndex + 1) + " / " + slideCount
}
