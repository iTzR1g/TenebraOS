import QtQuick 2.0
import calamares.slideshow 1.0

Slide {
    Text {
        anchors.centerIn: parent
        text: "Welcome to TenebraOS"
        font.pixelSize: 32
        color: "#ffffff"
    }
    Text {
        anchors {
            top: parent.verticalCenter
            topMargin: 50
            horizontalCenter: parent.horizontalCenter
        }
        text: "A dark, modern Linux distribution"
        font.pixelSize: 18
        color: "#e94560"
    }
}
