#!/usr/bin/env python3
# usecase_select/main.py
# PyQt5 viewmodule for Gaming/Learning/Office selection

import libcalamares

import sys
from PyQt5.QtWidgets import QWidget, QVBoxLayout, QLabel, QButtonGroup, QRadioButton

class UsecaseSelectView(QWidget):
    def __init__(self):
        super().__init__()
        self.setObjectName("usecase_select")
        layout = QVBoxLayout()
        title = QLabel("What will you use this system for?")
        title.setStyleSheet("font-size: 18px; font-weight: bold; margin-bottom: 20px;")
        layout.addWidget(title)

        self.group = QButtonGroup()
        options = [
            ("gaming", "Gaming"),
            ("learning", "Learning & Development"),
            ("office", "Daily Use & Office"),
        ]
        for value, label in options:
            btn = QRadioButton(label)
            btn.setProperty("usecase", value)
            self.group.addButton(btn)
            layout.addWidget(btn)

        self.group.buttons()[0].setChecked(True)
        self.setLayout(layout)

    def onActivate(self):
        pass

    def onLeave(self):
        selected = None
        for btn in self.group.buttons():
            if btn.isChecked():
                selected = btn.property("usecase")
                break
        libcalamares.globalstorage.insert("usecase", selected or "gaming")

def get_view():
    return UsecaseSelectView()
