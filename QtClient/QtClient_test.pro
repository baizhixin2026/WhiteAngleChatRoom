QT       += core gui network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SHARED_DIR = $$PWD/../shared/protocol

INCLUDEPATH += $$PWD/../shared/protocol

SOURCES += \
    avatarcache.cpp \
    avatarsetdialog.cpp \
    connectionmanager.cpp \
    contactdelegate.cpp \
    contactmodel.cpp \
    emojipanel.cpp \
    fileioworker.cpp \
    filetransfermanager.cpp \
    friendrequestcard.cpp \
    friendrequestdialog.cpp \
    logindialog.cpp \
    main.cpp \
    mainwindow.cpp \
    messagedelegate.cpp \
    messagemodel.cpp \
    networkworker.cpp \
    passworddialog.cpp \
    userinfodialog.cpp \
    $$SHARED_DIR/protocolcodec.cpp

HEADERS += \
    avatarcache.h \
    avatardisplay.h \
    avatarsetdialog.h \
    chatinputedit.h \
    connectionmanager.h \
    contactdelegate.h \
    contactitem.h \
    contactmodel.h \
    emojipanel.h \
    emojitext.h \
    fileioworker.h \
    filetransfermanager.h \
    filetransfertypes.h \
    friendrequestcard.h \
    friendrequestdialog.h \
    logindialog.h \
    mainwindow.h \
    messageItem.h \
    messagedelegate.h \
    messagemodel.h \
    networkworker.h \
    passworddialog.h \
    userinfodialog.h \
    $$SHARED_DIR/protocoltypes.h \
    $$SHARED_DIR/protocolcodec.h

FORMS += \
    avatarsetdialog.ui \
    emojipanel.ui \
    friendrequestdialog.ui \
    logindialog.ui \
    mainwindow.ui \
    passworddialog.ui \
    userinfodialog.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    res.qrc

DISTFILES += \
    style.qss
