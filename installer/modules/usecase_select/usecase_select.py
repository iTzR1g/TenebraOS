# installer/modules/usecase_select/usecase_select.py
# TenebraOS - Use-case profile selection screen for Calamares
# Debian 13 (Trixie)

import libcalamares
from PyQt5.QtWidgets import (
    QWidget, QVBoxLayout, QLabel,
    QRadioButton, QButtonGroup, QGroupBox
)


class UseCase(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout()

        title = QLabel('<h2>How will you use TenebraOS?</h2>')
        layout.addWidget(title)

        subtitle = QLabel(
            'TenebraOS will install the right packages and settings for your needs.'
        )
        layout.addWidget(subtitle)

        self.group = QButtonGroup(self)

        options = [
            (
                'gaming',
                'Gaming',
                'Steam, Lutris, Heroic, MangoHud, Proton, GameMode\n'
                'Mesa/DXVK, low-latency kernel params, vm.swappiness=10\n'
                'Desktop: KDE Plasma'
            ),
            (
                'learning',
                'Learning & Development',
                'VS Code, Python, Jupyter, VirtualBox, Anki\n'
                'Git, Node.js, zram, developer shell tools\n'
                'Desktop: GNOME'
            ),
            (
                'office',
                'Daily Use & Office',
                'LibreOffice, Thunderbird, Firefox, GIMP\n'
                'Conservative power profile, clean GNOME desktop\n'
                'Desktop: GNOME'
            ),
        ]

        for i, (val, label, desc) in enumerate(options):
            box = QGroupBox()
            box_layout = QVBoxLayout()
            rb = QRadioButton(label)
            rb.setProperty('value', val)
            if i == 2:
                rb.setChecked(True)  # default = office
            self.group.addButton(rb)
            box_layout.addWidget(rb)
            box_layout.addWidget(QLabel(desc))
            box.setLayout(box_layout)
            layout.addWidget(box)

        self.setLayout(layout)

    def isNextEnabled(self):
        return True

    def onLeave(self):
        btn = self.group.checkedButton()
        if btn:
            usecase = btn.property('value')
            libcalamares.globalstorage.insert('usecase', usecase)
            with open('/tmp/calamares-usecase.conf', 'w') as f:
                f.write(usecase)


def run():
    return None
