import QtQuick 2.0
import calamares.slideshow 1.0

Presentation {
    id: presentation

    Timer {
        interval: 6000
        running: presentation.activatedInCalamares
        repeat: true
        onTriggered: presentation.goToNextSlide()
    }

    function onActivate() {
        presentation.currentSlide = 0;
    }

    function onLeave() {
    }

    // Monochrome, site-matched: black canvas, white headings, gray body.
    property color fg: "#ffffff"
    property color sub: "#b0b0b0"
    property color dim: "#888888"

    Slide {
        Rectangle { anchors.fill: parent; color: "#000000" }
        Column {
            anchors.centerIn: parent
            spacing: 16
            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                source: "logo.png"
                width: 120
                height: 120
                fillMode: Image.PreserveAspectFit
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Welcome to TenebraOS"
                font.pixelSize: 34
                font.bold: true
                color: presentation.fg
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Stable Linux, built on Devuan"
                font.pixelSize: 20
                color: presentation.sub
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "systemd-free · runit · KDE Plasma"
                font.pixelSize: 15
                color: presentation.dim
            }
        }
    }

    Slide {
        Rectangle { anchors.fill: parent; color: "#000000" }
        Column {
            anchors.centerIn: parent
            spacing: 14
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Configured around you"
                font.pixelSize: 30
                font.bold: true
                color: presentation.fg
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Pick your use case — gaming, development, or office —"
                font.pixelSize: 18
                color: presentation.sub
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "and the installer sets up the drivers and apps for it."
                font.pixelSize: 18
                color: presentation.sub
            }
        }
    }

    Slide {
        Rectangle { anchors.fill: parent; color: "#000000" }
        Column {
            anchors.centerIn: parent
            spacing: 14
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Hardware-aware"
                font.pixelSize: 30
                font.bold: true
                color: presentation.fg
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "GPU and Apple T2 hardware are detected for you"
                font.pixelSize: 18
                color: presentation.sub
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "— and confirmed before any driver is applied."
                font.pixelSize: 18
                color: presentation.sub
            }
        }
    }

    Slide {
        Rectangle { anchors.fill: parent; color: "#000000" }
        Column {
            anchors.centerIn: parent
            spacing: 14
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Thank you for choosing TenebraOS"
                font.pixelSize: 30
                font.bold: true
                color: presentation.fg
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Reboot when the installation is finished."
                font.pixelSize: 18
                color: presentation.sub
            }
        }
    }
}
