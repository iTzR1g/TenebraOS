import QtQuick 2.0

Item {
    id: slide
    property alias content: contentLayer.children

    Item {
        id: contentLayer
        anchors.fill: parent
    }
}
