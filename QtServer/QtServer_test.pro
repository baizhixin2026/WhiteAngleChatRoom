# 双 main 切换

# ─────────────────────────────────────────────────────────
# 默认版本（不指定 CONFIG）：GUI 调试版
# 服务器无头版： qmake CONFIG+=headless
# ─────────────────────────────────────────────────────────

CONFIG += c++17
CONFIG += warn_on

SHARED_DIR = $$PWD/../shared/protocol
INCLUDEPATH += $$PWD/../shared/protocol

# ─────────────────────────────────────────────────────────
# 共用模块：GUI 版和无头版都编译
#   这些文件只依赖 QtCore / QtNetwork / QtSql，不碰任何 QWidget
# ─────────────────────────────────────────────────────────

QT += core network sql

SOURCES += \
    connectionmanager.cpp \
    dbworker.cpp \
    fileservice.cpp \
    mysqlfilestore.cpp \
    mysqluserstore.cpp \
    networkworker.cpp \
    storageworker.cpp \
    $$SHARED_DIR/protocolcodec.cpp

HEADERS += \
    connectionmanager.h \
    dbworker.h \
    fileservice.h \
    mysqlfilestore.h \
    mysqluserstore.h \
    networkworker.h \
    shardroute.h \
    storageremovekey.h \
    storageworker.h \
    userinfo.h \
    sql/0002_file_meta.sql \
    $$SHARED_DIR/protocoltypes.h \
    $$SHARED_DIR/protocolcodec.h

# ─────────────────────────────────────────────────────────
# 版本分支
# ─────────────────────────────────────────────────────────

contains(CONFIG, headless) {
    # 无头服务端：不链接 QtGui / QtWidgets
    QT -= gui
    CONFIG += console
    TARGET = QtServer-headless
    SOURCES += main_headless.cpp
} else {
    # 本地调试 GUI 版（默认）
    QT += gui widgets
    TARGET = QtServer
    SOURCES += main.cpp mainwindow.cpp usertablemodel.cpp onlinetablemodel.cpp
    HEADERS += mainwindow.h usertablemodel.h onlinetablemodel.h
    FORMS += mainwindow.ui
    RESOURCES += res.qrc
    DISTFILES += style.qss
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target


# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0
