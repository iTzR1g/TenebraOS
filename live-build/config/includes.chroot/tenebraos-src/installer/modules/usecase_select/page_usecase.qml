// usecase_select/page_usecase.qml
// TenebraOS - Profile selection page

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import io.calamares.ui 1.0

Page {
    id: root

    // Called by Calamares to check if Next can be pressed
    function onActivate() {}
    function onLeave() {
        // Write selection to globalStorage so autoconfig_run can read it
        if (gamingCard.selected)
            Calamares.globalStorage.insert("usecase_profile", "gaming")
        else if (learningCard.selected)
            Calamares.globalStorage.insert("usecase_profile", "learning")
        else if (officeCard.selected)
            Calamares.globalStorage.insert("usecase_profile", "office")
    }

    background: Rectangle { color: "#0D0D1A" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 40
        spacing: 24

        Text {
            text: "Choose Your Profile"
            font.pixelSize: 28
            font.bold: true
            color: "#FFFFFF"
            Layout.alignment: Qt.AlignHCenter
        }

        Text {
            text: "TenebraOS will install the right tools and settings for you."
            font.pixelSize: 14
            color: "#C8C8E0"
            Layout.alignment: Qt.AlignHCenter
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 20

            // --- Gaming Card ---
            ProfileCard {
                id: gamingCard
                Layout.fillWidth: true
                title: "Gaming"
                icon: "🎮"
                description: "Steam, Lutris, Wine,\nMangoHud, GameMode,\nVulkan drivers"
                accentColor: "#7B5EA7"
                onClicked: {
                    gamingCard.selected = true
                    learningCard.selected = false
                    officeCard.selected = false
                }
            }

            // --- Learning Card ---
            ProfileCard {
                id: learningCard
                Layout.fillWidth: true
                title: "Learning"
                icon: "📚"
                description: "VS Code, Python, Node,\nJupyter, Anki,\nVirtualBox"
                accentColor: "#5EA77B"
                onClicked: {
                    gamingCard.selected = false
                    learningCard.selected = true
                    officeCard.selected = false
                }
            }

            // --- Office Card ---
            ProfileCard {
                id: officeCard
                Layout.fillWidth: true
                title: "Office"
                icon: "💼"
                description: "LibreOffice, Thunderbird,\nGIMP, VLC,\nTLP power saving"
                accentColor: "#A77B5E"
                onClicked: {
                    gamingCard.selected = false
                    learningCard.selected = false
                    officeCard.selected = true
                }
            }
        }

        Item { Layout.fillHeight: true }
    }

    // Profile card component
    component ProfileCard: Rectangle {
        id: card
        property string title: ""
        property string icon: ""
        property string description: ""
        property string accentColor: "#7B5EA7"
        property bool selected: false
        signal clicked()

        height: 220
        radius: 12
        color: selected ? Qt.rgba(
            parseInt(accentColor.slice(1,3), 16)/255,
            parseInt(accentColor.slice(3,5), 16)/255,
            parseInt(accentColor.slice(5,7), 16)/255,
            0.25
        ) : "#1A1A2E"
        border.color: selected ? accentColor : "#2A2A3E"
        border.width: selected ? 2 : 1

        MouseArea {
            anchors.fill: parent
            onClicked: card.clicked()
            cursorShape: Qt.PointingHandCursor
        }

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 12

            Text {
                text: card.icon
                font.pixelSize: 48
                Layout.alignment: Qt.AlignHCenter
            }

            Text {
                text: card.title
                font.pixelSize: 20
                font.bold: true
                color: card.selected ? card.accentColor : "#FFFFFF"
                Layout.alignment: Qt.AlignHCenter
            }

            Text {
                text: card.description
                font.pixelSize: 12
                color: "#C8C8E0"
                horizontalAlignment: Text.AlignHCenter
                Layout.alignment: Qt.AlignHCenter
            }
        }
    }
}
