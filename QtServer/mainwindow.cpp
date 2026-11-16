#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "connectionmanager.h"
#include "usertablemodel.h"
#include "onlinetablemodel.h"
#include <QDateTime>
#include <QIcon>
#include <QStringList>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    setWindowTitle(" ");
    ui->portEdit->setText("8899");
    ui->portEdit->setPlaceholderText("输入端口");
    ui->stopListenBtn->setEnabled(false);
    ui->logMsg->setReadOnly(true);
    ui->logMsg->setMaximumBlockCount(5000);
    ui->stackedWidget->setCurrentWidget(ui->logPage);  // 默认启动显示, 避免UI里误触

    // 状态栏
    m_statusPixmap = new QLabel;
    m_statusText = new QLabel("连接状态: ");
    m_statusPixmap->setPixmap(QPixmap(":/tcp_disconnect"));
    ui->statusbar->addWidget(m_statusText);
    ui->statusbar->addWidget(m_statusPixmap);

    // 导航栏按钮
    auto *logBtn = ui->log_toolButton;
    logBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);    // 图标左、文字右
    logBtn->setIconSize(QSize(24, 24));
    logBtn->setIcon(QIcon(":/image/button/userManagement-1.png"));
    logBtn->setText(QStringLiteral("日志信息"));
    // 导航栏按钮
    auto *usersBtn = ui->users_toolButton;
    usersBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    usersBtn->setIconSize(QSize(24, 24));
    usersBtn->setIcon(QIcon(":/image/button/userManagement-1.png"));
    usersBtn->setText(QStringLiteral("所有用户"));
    // 导航栏按钮
    auto *onlineBtn = ui->online_toolButton;
    onlineBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    onlineBtn->setIconSize(QSize(24, 24));
    onlineBtn->setIcon(QIcon(":/image/button/dataQuery-1.png"));
    onlineBtn->setText(QStringLiteral("在线用户"));
    // 导航栏按钮
    auto *manageBtn = ui->manage_toolButton;
    manageBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    manageBtn->setIconSize(QSize(24, 24));
    manageBtn->setIcon(QIcon(":/image/button/userManagement-1.png"));
    manageBtn->setText(QStringLiteral("用户管理"));
    // 导航栏按钮
    auto *filesBtn = ui->files_toolButton;
    filesBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    filesBtn->setIconSize(QSize(24, 24));
    filesBtn->setIcon(QIcon(":/image/button/userManagement-1.png"));
    filesBtn->setText(QStringLiteral("文件管理"));

    ui->sqlStateBtn->setIcon(QIcon(QPixmap(":/image/button/sqlState-2.png")));
    ui->sqlStateBtn->setIconSize(QSize(24, 24));
    ui->sqlStateBtn->setText(QStringLiteral("  数据库"));
    ui->sqlStateBtn->setStyleSheet("color: white;");

    // 模型绑定, 初始化 QTableView 的外观和交互行为
    m_usersModel = new UserTableModel(this);
    ui->usersTableView->setModel(m_usersModel);
    ui->usersTableView->setSelectionBehavior(QAbstractItemView::SelectRows); // 点击某个单元格时，选中整行
    ui->usersTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->usersTableView->horizontalHeader()->setStretchLastSection(true); // 最后一列自动拉伸
    ui->usersTableView->verticalHeader()->setVisible(false);    // 左侧不显示行号
    ui->usersTableView->setAlternatingRowColors(true);          // 开启交替行颜色

    m_onlineModel = new OnlineTableModel(this);
    ui->onlineTableView->setModel(m_onlineModel);
    ui->onlineTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->onlineTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->onlineTableView->horizontalHeader()->setStretchLastSection(true);
    ui->onlineTableView->verticalHeader()->setVisible(false);
    ui->onlineTableView->setAlternatingRowColors(true);


    auto& conn = ConnectionManager::instance();
    connect(&conn, &ConnectionManager::serverListenOk, this, [=](QString msg){
        appendLog(msg);
        ui->startListenBtn->setEnabled(false);
        ui->stopListenBtn->setEnabled(true);
        m_statusPixmap->setPixmap(QPixmap(":/tcp_connect"));
    });
    connect(&conn, &ConnectionManager::serverListenFail, this, [=](QString msg){
        appendLog(msg);
        ui->startListenBtn->setEnabled(true);
        ui->stopListenBtn->setEnabled(false);
    });
    connect(&conn, &ConnectionManager::serverListenStopped, this, [=](bool ok){
        ui->startListenBtn->setEnabled(true);
        ui->stopListenBtn->setEnabled(false);
        if(ok)
            m_statusPixmap->setPixmap(QPixmap(":/tcp_disconnect"));
    });
    connect(&conn, &ConnectionManager::logMessage, this, [=](QString msg){
        appendLog(msg);
    });

    // 服务器的数据库, 需要优化，手动停止数据库后，需要重启程序才能初始化数据库，导致 ui->logMsg->appendPlainText 丢失
    connect(&conn, &ConnectionManager::usersReady, this, &MainWindow::onUsersReady);
    connect(&conn, &ConnectionManager::dbReady, this, [=](bool ok){
        if(ok) {
            m_dbOk = true;
            ui->sqlStateBtn->setIcon(QIcon(QPixmap(":/image/button/sqlState-1.png")));
        }
        else {
            m_dbOk = false;
            ui->sqlStateBtn->setIcon(QIcon(QPixmap(":/image/button/sqlState-2.png")));
        }
    });


    // 侧边栏 导航工具按钮: 切换页面
    connect(ui->log_toolButton, &QToolButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->logPage);
    });
    connect(ui->users_toolButton, &QToolButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->usersPage);
        ConnectionManager::instance().requestUsers();   // 查询服务器的数据库
    });
    connect(ui->online_toolButton, &QToolButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->onlinePage);
        ConnectionManager::instance().requestUsers();
    });
    connect(ui->manage_toolButton, &QToolButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->managePage);
    });
    connect(ui->files_toolButton, &QToolButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->filesPage);
    });
}

MainWindow::~MainWindow()
{
    ConnectionManager::shutdown();
    delete ui;
}

void MainWindow::appendLog(const QString &msg)
{
    static quint64 logCount = 0;
    ui->logMsg->appendPlainText(QStringLiteral("%1: [%2]  %3").arg(QString::number(logCount++),
                    QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"), msg));
}

void MainWindow::on_startListenBtn_clicked()
{
    if(!m_dbOk) {
        appendLog("数据库未连接");
        return;
    }
    bool ok;
    unsigned short port = ui->portEdit->text().trimmed().toUShort(&ok, 10);
    if(!ok || port == 0) {
        appendLog("端口号无效，请输入 1024-65535");
        return;
    }
    ConnectionManager::instance().startServerListen(port);
}


void MainWindow::on_stopListenBtn_clicked()
{
    if(!m_dbOk) {
        appendLog("数据库未连接");
        return;
    }
    ConnectionManager::instance().stopServerListen();
}

void MainWindow::onUsersReady(const QVector<UserInfo> &users)
{
    m_usersModel->setUsers(users);
    m_onlineModel->setUsers(users);
}


// managePage
void MainWindow::on_searchBtn_clicked()
{
//    QString name = ui->nameEdit->text().trimmed();
//    qint64 id = ui->idEdit->text().toLongLong();
    // .......测试
}


// 服务器私发
//void MainWindow::on_sendMsg_clicked()
//{
//    if (m_clients.isEmpty() || ui->clientCombox->currentIndex() < 0) {
//        appendLog("没有客户端连接，无法发送");
//        return;
//    }

//    // 从下拉框获取选中的 socket
//    QTcpSocket *selected = reinterpret_cast<QTcpSocket*>(
//        ui->clientCombox->currentData().value<quintptr>());

//    if (!selected || selected->state() != QAbstractSocket::ConnectedState) {
//        appendLog("选中的客户端已断开");
//        return;
//    }

//    QString msg = ui->msg->toPlainText();
//    if (msg.isEmpty()) {
//        appendLog("空消息，无法发送");
//        return;
//    }

//    QByteArray message = ("服务器: " + msg.toUtf8()).toBase64();
//    selected->write("SERVERMSG:" + message + "\n");
//    appendLog("服务器 → " + ui->clientCombox->currentText() + ": " + msg);
//}

// 服务器群发
//void MainWindow::on_sendMsgAll_clicked()
//{
//    if (m_clients.isEmpty()) {
//        appendLog("没有客户端连接，无法发送");
//        return;
//    }

//    QString msg = ui->msg->toPlainText();
//    if (msg.isEmpty()) {
//        appendLog("空消息，无法发送");
//        return;
//    }

//    QByteArray message = ("服务器群发: " + msg.toUtf8()).toBase64();
//    for (QTcpSocket *client : m_clients) {
//        client->write("SERVERMSG:" + message + "\n");
//    }
//    appendLog("服务器群发: " + msg);
//}





