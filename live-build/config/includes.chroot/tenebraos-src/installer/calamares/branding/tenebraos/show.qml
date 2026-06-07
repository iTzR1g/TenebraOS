import QtQuick 2.0

Rectangle {
    color: "#0D0D1A"
    width: 800
    height: 500

    Image {
        anchors.fill: parent
        source: "background.jpg"
        fillMode: Image.PreserveAspectCrop
        opacity: 0.3
    }

    Image {
        anchors.centerIn: parent
        source: "splash.png"
        width: 128
        height: 128
        fillMode: Image.PreserveAspectFit
    }

    Text {
        anchors {
            top: parent.top
            topMargin: 30
            horizontalCenter: parent.horizontalCenter
        }
        text: "TenebraOS"
        color: "#C8C8E0"
        font.pixelSize: 36
        font.bold: true
    }

    Text {
        anchors {
            bottom: parent.bottom
            bottomMargin: 40
            horizontalCenter: parent.horizontalCenter
        }
        text: "Installing...  Welcome to the darkness."
        color: "#7B5EA7"
        font.pixelSize: 16
    }
}
