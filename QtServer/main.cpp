#include "mainwindow.h"

#include <QApplication>
#include <QFile>

/* 运行在本地
 * make distclean
 * qmake
 * make -j4
 * mkdir -p /home/leo/chatFiles
 * CHAT_DB_PASSWORD=2222 CHAT_STORAGE=/home/leo/chatFiles ./QtServer
*/

int main(int argc, char *argv[])
{
    QApplication a(argc, argv); // GUI 程序, 需要 QtWidgets（底层还要 QtGui）

    QFile qssFile(":/style.qss");
    if(qssFile.open(QFile::ReadOnly)) {
        // QSS 含中文时必须用 QString::fromUtf8
        QString styleSheet = QString::fromUtf8(qssFile.readAll());
        a.setStyleSheet(styleSheet);
        qssFile.close();
    }

    MainWindow w;
    w.show();
    return a.exec();
}
