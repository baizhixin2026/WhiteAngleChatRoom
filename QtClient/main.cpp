#include "mainwindow.h"

#include <QApplication>
#include <QFile>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 群成员 选中高亮颜色 ( 在创建任何窗口之前设置样式 )
    QApplication::setStyle(QStyleFactory::create("Fusion"));

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
