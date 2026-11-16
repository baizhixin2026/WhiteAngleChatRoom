#include "connectionmanager.h"
#include "passworddialog.h"
#include "ui_passworddialog.h"

#include <qcryptographichash.h>
#include <QMessageBox>
#include <QShowEvent>
#include <QTimer>

namespace {
    constexpr int kServerReplyTimeoutMs = 8000;
}

PassWordDialog::PassWordDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::PassWordDialog)
{
    ui->setupUi(this);
    setFixedSize(300, 180);
    setModal(false);
    setWindowTitle("修改密码");

    ui->newPassWord->setMaxLength(Proto::kPasswordMaxLen);
    ui->confirmPassWord->setMaxLength(Proto::kPasswordMaxLen);

    ui->oldPassWord->setPlaceholderText(QStringLiteral("请输入旧密码"));
    ui->newPassWord->setPlaceholderText(QStringLiteral("6-20 位，且与旧密码不同"));
    ui->confirmPassWord->setPlaceholderText(QStringLiteral("请再次输入新密码"));
    ui->commitBtn->setEnabled(false);

    auto& conn = ConnectionManager::instance();
    connect(this, &PassWordDialog::changePasswordRequested, &conn, &ConnectionManager::changePassword);
    connect(&conn, &ConnectionManager::passwordChangeResult, this, &PassWordDialog::onPasswordChangeResult);

    // 三个输入框任一变化，都重新判断提交按钮能不能点
    connect(ui->oldPassWord,     &QLineEdit::textChanged, this, &PassWordDialog::refreshCommitState);
    connect(ui->newPassWord,     &QLineEdit::textChanged, this, &PassWordDialog::refreshCommitState);
    connect(ui->confirmPassWord, &QLineEdit::textChanged, this, &PassWordDialog::refreshCommitState);

    m_replyTimer = new QTimer(this);
    m_replyTimer->setSingleShot(true);
    m_replyTimer->setInterval(kServerReplyTimeoutMs);
    connect(m_replyTimer, &QTimer::timeout, this, &PassWordDialog::onServerReplyTimeout);
}

PassWordDialog::~PassWordDialog()
{
    delete ui;
}

// 每次打开都从干净状态开始
void PassWordDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);

    m_replyTimer->stop();
    ui->oldPassWord->clear();
    ui->newPassWord->clear();
    ui->confirmPassWord->clear();
    setBusy(false);
    ui->oldPassWord->setFocus();
}

void PassWordDialog::refreshCommitState()
{
    const bool filled = !ui->oldPassWord->text().isEmpty() && !ui->newPassWord->text().isEmpty()
                        && !ui->confirmPassWord->text().isEmpty();
    ui->commitBtn->setEnabled(filled && !m_busy);
}

void PassWordDialog::setBusy(bool busy)
{
    m_busy = busy;
    ui->commitBtn->setText(busy ? QStringLiteral("提交中…") : QStringLiteral("提交"));
    ui->oldPassWord->setEnabled(!busy);
    ui->newPassWord->setEnabled(!busy);
    ui->confirmPassWord->setEnabled(!busy);
    refreshCommitState();
}

void PassWordDialog::on_commitBtn_clicked()
{
    if (m_busy)
        return;

    const QString oldPassword = ui->oldPassWord->text();
    const QString newPassword = ui->newPassWord->text();

    if (oldPassword.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral(" "), QStringLiteral("请输入旧密码"));
        return;
    }
    if (!Proto::isValidPasswordLength(newPassword.size())) {
        QMessageBox::warning(this, QStringLiteral(" "), Err::friendlyText(Err::PasswordTooWeak));
        return;
    }
    if (newPassword != ui->confirmPassWord->text()) {
        QMessageBox::warning(this, QStringLiteral(" "), QStringLiteral("两次输入的新密码不一致"));
        return;
    }
    if (newPassword == oldPassword) {
        QMessageBox::warning(this, QStringLiteral(" "), Err::friendlyText(Err::PasswordSameAsOld));
        return;
    }

    const QByteArray oldHash = QCryptographicHash::hash(oldPassword.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QByteArray newHash = QCryptographicHash::hash(newPassword.toUtf8(), QCryptographicHash::Sha256).toHex();

    setBusy(true);
    m_replyTimer->start();
    emit changePasswordRequested(QString::fromLatin1(oldHash), QString::fromLatin1(newHash));
}

void PassWordDialog::onPasswordChangeResult(const PasswordChangeResult &msg)
{
    if (!m_busy)
        return;     // 不是本次提交的回执

    m_replyTimer->stop();
    setBusy(false);

    if (msg.ok) {
        // 改密成功后保持在线，不断连接
        QMessageBox::information(this, QStringLiteral(" "), QStringLiteral("密码已修改，下次登录请使用新密码"));
        accept();
        return;
    }

    QMessageBox::warning(this, QStringLiteral(" "),
                         msg.text.isEmpty() ? Err::friendlyText(Err::InvalidParam) : msg.text);

    if (msg.errcode == Err::PasswordIncorrect) {
        ui->oldPassWord->clear();       // 只是旧密码错了，新密码保留，省得重输
        ui->oldPassWord->setFocus();
    } else {
        ui->oldPassWord->clear();
        ui->newPassWord->clear();
        ui->confirmPassWord->clear();
    }
    refreshCommitState();
}

void PassWordDialog::onServerReplyTimeout()
{
    if (!m_busy)
        return;
    setBusy(false);
    QMessageBox::warning(this, QStringLiteral(" "), QStringLiteral("服务器没有响应，请稍后重试"));
}


