#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "protocolcodec.h"
#include "protocoltypes.h"
#include "userinfo.h"

#include <QMainWindow>
#include <QLabel>
#include <QList>
#include <QComboBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QFile>
#include <QJsonArray>

class UserTableModel;
class OnlineTableModel;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_startListenBtn_clicked();
    void on_stopListenBtn_clicked();
//    void on_sendMsg_clicked();
//    void on_sendMsgAll_clicked();

    void on_searchBtn_clicked();
    void onUsersReady(const QVector<UserInfo> &users);

private:
    void appendLog(const QString &msg);

private:
    Ui::MainWindow *ui;

    QLabel *m_statusPixmap = nullptr;
    QLabel *m_statusText = nullptr;
    UserTableModel *m_usersModel = nullptr;
    OnlineTableModel *m_onlineModel = nullptr;
    bool m_dbOk = false;

};
#endif // MAINWINDOW_H
