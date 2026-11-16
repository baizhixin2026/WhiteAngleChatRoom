#include "logindialog.h"
#include "ui_logindialog.h"
#include "connectionmanager.h"
#include <QCryptographicHash>
#include <QHostAddress>
#include <QMessageBox>

LoginDialog::LoginDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::LoginDialog)
{
    ui->setupUi(this);
    setFixedSize(230, 230);

    setWindowTitle("登录");
    ui->ipEdit->setText("62.234.74.239"); // 62.234.74.239 / 192.168.64.200
    ui->portEdit->setText("8899");
    ui->ipEdit->setPlaceholderText("请输入服务器IP");
    ui->portEdit->setPlaceholderText("请输入服务器端口");
    ui->nameEdit->setPlaceholderText("请输入用户名");

    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);   // Window移除帮助按钮
    setModal(true); // 模态

    auto& conn = ConnectionManager::instance();
    connect(this, &LoginDialog::clientLogin, &conn, &ConnectionManager::clientLogin);
    connect(this, &LoginDialog::clientRegister, &conn, &ConnectionManager::clientRegister);

    connect(&conn,&ConnectionManager::registerSuccess, this, [=](qint64 accountNo, const QString &name){
        QMessageBox::information(this, "提示", QStringLiteral("注册成功\n用户名: %1\n账号: %2").arg(name).arg(accountNo));
    });

    connect(&conn,&ConnectionManager::registerFailed, this, [=](const QString r){
        QMessageBox::warning(this, "提示", r);
    });

    connect(&conn,&ConnectionManager::loginSuccess, this, [=](){
        accept();
    });

    connect(&conn,&ConnectionManager::loginTimeOut, this, [=](const QString &reason){
        ui->loginBtn->setEnabled(true);
        QMessageBox::warning(this, "提示", "登录超时: " + reason);
        return;
    });

    connect(&conn,&ConnectionManager::loginFailed, this, [=](const QString &reason){
        ui->loginBtn->setEnabled(true);
        QMessageBox::warning(this, "提示", reason);
        return;
    });

    connect(&conn,&ConnectionManager::errorOccurred, this, [=](const QString &err){
        ui->loginBtn->setEnabled(true);
        QMessageBox::warning(this, "提示", "服务器连接失败: " + err);
        return;
    });

    connect(&conn,&ConnectionManager::connectionLost, this, [=](){
        ui->loginBtn->setEnabled(true);
        ui->nameEdit->clear();
        ui->pwdEdit->clear();
        ui->confirmPwdEdit->clear();
        return;
    });
}

LoginDialog::~LoginDialog()
{
    delete ui;
}

void LoginDialog::on_loginBtn_clicked()
{
    ui->loginBtn->setEnabled(false);

    QString ip = ui->ipEdit->text().trimmed();
    QHostAddress addr(ip);
    if(addr.isNull() || addr.protocol() == QAbstractSocket::UnknownNetworkLayerProtocol) {
        QMessageBox::warning(this, "提示", "IP 地址无效");
        ui->loginBtn->setEnabled(true);
        return;
    }

    bool ok;
    unsigned short port = ui->portEdit->text().trimmed().toUShort(&ok, 10);
    if(!ok || port == 0) {
        QMessageBox::warning(this, "提示", "端口无效, 请输入: 1024-65535");
        ui->loginBtn->setEnabled(true);
        return;
    }

    QString name = ui->nameEdit->text().trimmed();
    if(name.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入用户名");
        ui->loginBtn->setEnabled(true);
        return;
    }

    QString pwd = ui->pwdEdit->text();
    if(pwd.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入密码");
        ui->loginBtn->setEnabled(true);
        return;
    }

    // 计算密码哈希
    QByteArray pwdHash = QCryptographicHash::hash(pwd.toUtf8(), QCryptographicHash::Sha256).toHex();
    emit clientLogin(ip, port, name, QString::fromLatin1(pwdHash));
}


void LoginDialog::on_registerBtn_clicked()
{
    QString ip = ui->ipEdit->text().trimmed();
    QHostAddress addr(ip);
    if(addr.isNull() || addr.protocol() == QAbstractSocket::UnknownNetworkLayerProtocol) {
        QMessageBox::warning(this, " ", "IP 地址无效");
        return;
    }

    bool ok;
    quint16 port = ui->portEdit->text().trimmed().toUShort(&ok, 10);
    if(!ok || port == 0) {
        QMessageBox::warning(this, " ", "端口无效, 请输入: 1 - 65535");
        return;
    }

    QString name = ui->nameEdit->text().trimmed();
    if(name.isEmpty()) {
        QMessageBox::warning(this, " ", "请输入用户名");
        return;
    }
    if (name.size() > 16) {
        QMessageBox::warning(this, " ", "用户名长度不能超过16个字符");
        return;
    }

    QString pwd = ui->pwdEdit->text();
    if(pwd.isEmpty()) {
        QMessageBox::warning(this, " ", "请输入密码");
        return;
    }

    QString confirmPwd= ui->confirmPwdEdit->text();
    if(confirmPwd.isEmpty()) {
        QMessageBox::warning(this, " ", "请确认密码");
        return;
    }

    if (!Proto::isValidPasswordLength(pwd.size())) {
        QMessageBox::warning(this, " ", "密码长度错误， 6-20位之间");
        return;
    }

    if(pwd != confirmPwd) {
        QMessageBox::warning(this, " ", "两次密码不一致,请重新输入密码");
        return;
    }

    // 计算密码哈希
    QByteArray pwdHash = QCryptographicHash::hash(confirmPwd.toUtf8(), QCryptographicHash::Sha256).toHex();
    emit clientRegister(ip, port, name, QString::fromLatin1(pwdHash));
}

