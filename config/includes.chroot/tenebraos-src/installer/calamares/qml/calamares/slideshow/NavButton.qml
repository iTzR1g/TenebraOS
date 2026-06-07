import QtQuick 2.0

Item {
    id: navButton
    property string iconSource: ""
    signal clicked()

    width: 48
    height: 48

    MouseArea {
        anchors.fill: parent
        onClicked: navButton.clicked()
    }
}
