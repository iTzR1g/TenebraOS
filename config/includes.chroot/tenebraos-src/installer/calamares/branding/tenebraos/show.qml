import QtQuick 2.0
import calamares.slideshow 1.0

Presentation
{
    id: presentation

    Timer {
        interval: 5000
        running: true
        repeat: true
        onTriggered: presentation.goToNextSlide()
    }

    Slide {
        Text {
            anchors.centerIn: parent
            text: "Welcome to TenebraOS"
            font.pixelSize: 32
            color: "#ffffff"
        }
    }

    Slide {
        Text {
            anchors.centerIn: parent
            text: "A modern Linux distribution"
            font.pixelSize: 28
            color: "#cccccc"
        }
    }
}
