#!/usr/bin/env bash

[[ ! -d /usr/include/x86_64-linux-gnu/qt5 ]] || sudo ln -s /usr/include/x86_64-linux-gnu/qt5 /usr/include/qt5
[[ ! -d /usr/include/x86_64-linux-gnu/qt5/QtGui ]] || sudo ln -s /usr/include/x86_64-linux-gnu/qt5/QtGui /usr/include/qt5/QtGui
[[ ! -d /usr/include/x86_64-linux-gnu/qt5/QtCore ]] || sudo ln -s /usr/include/x86_64-linux-gnu/qt5/QtCore /usr/include/qt5/QtCore
[[ ! -d /usr/include/x86_64-linux-gnu/qt5/QtWidgets ]] || sudo ln -s /usr/include/x86_64-linux-gnu/qt5/QtWidgets /usr/include/qt5/QtWidgets
