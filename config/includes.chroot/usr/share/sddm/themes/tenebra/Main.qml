import QtQuick 2.0
import SddmComponents 2.0

Rectangle {
    id: root

    width: 1920
    height: 1080

    property string welcomeMessage: "TenebraOS"
    property color textColor: "white"
    property color accentColor: "#7a5cff"
    property color panelColor: "#1a1030"
    property color panelBorder: "#3a2a6a"

    TextConstants { id: textConstants }

    Connections {
        target: sddm

        function onLoginSucceeded() { }

        function onLoginFailed() {
            errorMessage.text = textConstants.loginFailed
            password.text = ""
            password.forceActiveFocus()
        }
    }

    Background {
        anchors.fill: parent
        source: (config.background && config.background !== "") ? config.background : "/usr/share/wallpapers/tenebra/contents/images/1920x1080.png"
        fillMode: Image.PreserveAspectCrop
    }

    // Slight dark scrim so the form stays legible on any wallpaper.
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.35
    }

    Rectangle {
        id: form
        anchors.centerIn: parent
        width: 420
        height: childrenRect.height
        radius: 10
        color: panelColor
        border.color: panelBorder
        border.width: 1
        opacity: 0.92

        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 36
            spacing: 16

            Image {
                id: logo
                anchors.horizontalCenter: parent.horizontalCenter
                source: "logo.png"
                sourceSize.height: 88
                fillMode: Image.PreserveAspectFit
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.welcomeMessage
                color: root.textColor
                font.pixelSize: 26
                font.bold: true
            }

            Column {
                width: 360
                spacing: 12

                Text {
                    id: lblName
                    text: textConstants.userName
                    color: root.textColor
                    font.pixelSize: 14
                }

                TextBox {
                    id: name
                    width: parent.width
                    height: 40
                    text: userModel.lastUser
                    font.pixelSize: 16
                    color: "#1e1440"
                    textColor: root.textColor
                    borderColor: panelBorder
                    focusColor: accentColor
                    hoverColor: accentColor
                    radius: 5
                }

                Text {
                    id: lblPassword
                    text: textConstants.password
                    color: root.textColor
                    font.pixelSize: 14
                }

                PasswordBox {
                    id: password
                    width: parent.width
                    height: 40
                    font.pixelSize: 16
                    color: "#1e1440"
                    textColor: root.textColor
                    borderColor: panelBorder
                    focusColor: accentColor
                    hoverColor: accentColor
                    radius: 5
                    focus: true

                    Keys.onPressed: {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            startLogin()
                            event.accepted = true
                        }
                    }
                }

                Text {
                    id: errorMessage
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: "#ff6b6b"
                    font.pixelSize: 13
                    text: ""
                }

                ComboBox {
                    id: session
                    width: parent.width
                    height: 40
                    font.pixelSize: 14
                    color: "#1e1440"
                    menuColor: "#241a50"
                    borderColor: panelBorder
                    focusColor: accentColor
                    hoverColor: accentColor
                    textColor: root.textColor
                    model: sessionModel
                    index: sessionModel.lastIndex
                    arrowIcon: "chevron-down.png"
                }

                Button {
                    id: loginButton
                    width: parent.width
                    height: 42
                    text: textConstants.login
                    color: accentColor
                    activeColor: "#8f6aff"
                    pressedColor: "#5a3ee0"
                    borderColor: accentColor
                    font.pixelSize: 16
                    font.bold: true
                    textColor: "white"

                    onClicked: startLogin()

                    Keys.onPressed: {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                            startLogin()
                            event.accepted = true
                        }
                    }
                }
            }
        }
    }

    function startLogin() {
        errorMessage.text = ""
        sddm.login(name.text, password.text, session.index)
    }

    // Power actions, bottom-left corner.
    Row {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 20
        spacing: 10
        opacity: 0.9

        Button {
            id: rebootButton
            text: textConstants.reboot
            width: 120
            height: 40
            color: "#221640"
            activeColor: accentColor
            pressedColor: "#140c26"
            borderColor: panelBorder
            font.pixelSize: 13
            textColor: root.textColor
            visible: sddm.canReboot

            onClicked: sddm.reboot()
        }

        Button {
            id: shutdownButton
            text: textConstants.shutdown
            width: 120
            height: 40
            color: "#221640"
            activeColor: accentColor
            pressedColor: "#140c26"
            borderColor: panelBorder
            font.pixelSize: 13
            textColor: root.textColor
            visible: sddm.canPowerOff

            onClicked: sddm.powerOff()
        }
    }

    Component.onCompleted: {
        if (name.text === "") {
            name.forceActiveFocus()
        } else {
            password.forceActiveFocus()
        }
    }
}