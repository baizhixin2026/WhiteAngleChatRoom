#include "userinfodialog.h"
#include "ui_userinfodialog.h"
#include "connectionmanager.h"
#include "avatarsetdialog.h"
#include "avatarcache.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QBuffer>
#include <QCryptographicHash>
#include <QPainter>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QDebug>
#include <QStandardPaths>

namespace {
    const QPixmap &defaultAvatar() // 默认头像
    {
        static const QPixmap defaultAvatar(QStringLiteral(":/image/user/user-9.png"));
        return defaultAvatar;
    }
}

UserInfoDialog::UserInfoDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::UserInfoDialog)
{
    ui->setupUi(this);
    setFixedSize(200, 300);

    setWindowTitle("我的账户");
    ui->avatarToolButton->setIcon(QIcon(defaultAvatar()));
    ui->avatarToolButton->setIconSize(QSize(32, 32));
    ui->avatarToolButton->setToolButtonStyle(Qt::ToolButtonIconOnly);


    ui->nicknameEdit->setPlaceholderText("输入新的昵称");
    ui->nicknameSaveBtn->setEnabled(false);
    ui->passwordBtn->setEnabled(false);
    setModal(false);

    // 换头像
    connect(ui->avatarToolButton, &QToolButton::clicked, this, [this]() {
        if (!ConnectionManager::instance().isLoggedIn()) {
            QMessageBox::information(this, "提示", "请先登录");
            return;
        }
        if (!m_avatarSetDialog) {
            m_avatarSetDialog = new AvatarSetDialog(this);
            connect(m_avatarSetDialog, &AvatarSetDialog::avatarReady, this, &UserInfoDialog::onAvatarCropped);
        }
        m_avatarSetDialog->show();
        m_avatarSetDialog->raise();          // 提到最前
        m_avatarSetDialog->activateWindow(); // 给焦点
    });

    auto& conn = ConnectionManager::instance();
    if(conn.isLoggedIn()) {
        ui->nicknameSaveBtn->setEnabled(true);
        ui->passwordBtn->setEnabled(true);
        m_username = conn.currentUser();
        m_accountNo = conn.currentAccountNo();
        const QString displayName = conn.nicknameOfUid(QString::number(conn.currentUid()));
        ui->accountNoLabel->setText(QString::number(m_accountNo));
        ui->userNameLabel->setText(m_username);
        ui->nickNameLabel->setText(displayName);

        refreshMyAvatarIcon();
    } else {
        ui->nicknameSaveBtn->setEnabled(false);
        ui->passwordBtn->setEnabled(false);
        ui->accountNoLabel->setText("未登录");
        ui->userNameLabel->setText(" ");
        ui->nickNameLabel->setText(" ");
    }

    connect(&conn, &ConnectionManager::loginSuccess, this, [=]() {
        ui->nicknameSaveBtn->setEnabled(true);
        ui->passwordBtn->setEnabled(true);
        m_accountNo = ConnectionManager::instance().currentAccountNo();
        m_username = ConnectionManager::instance().currentUser();
        const QString displayName = ConnectionManager::instance().
                    nicknameOfUid(QString::number(ConnectionManager::instance().currentUid()));
        ui->accountNoLabel->setText(QString::number(m_accountNo));
        ui->nickNameLabel->setText(displayName);
        ui->userNameLabel->setText(m_username);
        refreshMyAvatarIcon();
    });

    connect(&conn, &ConnectionManager::connectionLost, this, [=]() {
        ui->nicknameSaveBtn->setEnabled(false);
        ui->passwordBtn->setEnabled(false);
        ui->accountNoLabel->setText("未登录");
        ui->userNameLabel->setText(" ");
        ui->nickNameLabel->setText(" ");
        refreshMyAvatarIcon();
    });

    connect(this, &UserInfoDialog::nicknameChangeRequested, &conn, &ConnectionManager::changeNickname);
    connect(&conn, &ConnectionManager::nicknameChangeResult, this, &UserInfoDialog::setNicknameResult);

    connect(&conn, &ConnectionManager::uploadFinished, this, &UserInfoDialog::onAvatarUploadFinished);
    connect(&conn, &ConnectionManager::uploadFailed,   this, &UserInfoDialog::onAvatarUploadFailed);
    connect(&conn, &ConnectionManager::avatarUpdateResult, this, &UserInfoDialog::onAvatarUpdateResult);

    // 头像缓存就绪后刷新自己的头像
    connect(&AvatarCache::instance(), &AvatarCache::avatarLoaded, this, [this](qint64 uid, const QString &fileId) {
        Q_UNUSED(fileId);
        if (uid == ConnectionManager::instance().currentUid())
            refreshMyAvatarIcon();
    });
}

UserInfoDialog::~UserInfoDialog()
{
    delete ui;
}

void UserInfoDialog::on_nicknameSaveBtn_clicked()
{
    const QString nickname = ui->nicknameEdit->text().trimmed();
    if(nickname.isEmpty()) {
        QMessageBox::information(this, "提示", "请输入新昵称");
        return;
    }
    if(nickname.size() > Proto::kMaxNicknameLen) {
        QMessageBox::information(this, "提示", "昵称不能超过 32 个字符");
        return;
    }
    ui->nicknameSaveBtn->setEnabled(false);
    emit nicknameChangeRequested(nickname);
}

void UserInfoDialog::setNicknameResult(const NicknameChangeResult &msg)
{
    ui->nicknameSaveBtn->setEnabled(true);
    if(msg.ok) {
        m_nickname = msg.nick;
        ui->nickNameLabel->setText(m_nickname);
        ui->nicknameEdit->clear();
        QMessageBox::information(this, "提示", "昵称修改成功");
    } else {
        QMessageBox::warning(this, "提示", "昵称修改失败 " + msg.text);
    }
}

void UserInfoDialog::onAvatarCropped(const QImage &cropped)
{
    QByteArray avatarBytes;
    QBuffer buf(&avatarBytes); // 把一块 QByteArray 包装成 QIODevice 的适配器 ( QImage::save 的两个重载只接受 QString 文件名或 QIODevice* )
    buf.open(QIODevice::WriteOnly);
    cropped.save(&buf, "PNG"); // 把 QImage 编码成指定格式: 编码成 PNG 格式的字节数据, 编码 + 写入buf
    buf.close();

    if (avatarBytes.isEmpty() || avatarBytes.size() > Proto::kMaxAvatarFileSize) {
        QMessageBox::warning(this, "提示", "头像超过大小限制，请换一张");
        return;
    }

    const QString ext  = QStringLiteral("png");
    const QString mime = QStringLiteral("image/png");

    // 预览一律用"最终要上传的那份字节"解码，保证用户看到的和传上去的是同一张图（降级成 JPEG 后透明区域会变白）
    const QPixmap preview = QPixmap::fromImage(QImage::fromData(avatarBytes));
    if (!preview.isNull())
        ui->avatarToolButton->setIcon(QIcon(preview));

    // 落临时文件：引擎统一吃"本地路径"
    if (!m_pendingAvatarPath.isEmpty())
        QFile::remove(m_pendingAvatarPath);

    //  /tmp/avatar_time.png
    m_pendingAvatarPath = QDir(QDir::tempPath()).filePath(QStringLiteral("avatar_%1.%2")
                        .arg(QString::number(QDateTime::currentMSecsSinceEpoch()), ext));

    //  avatarBytes 写进 out
    QFile out(m_pendingAvatarPath);
    if (!out.open(QIODevice::WriteOnly) || out.write(avatarBytes) != avatarBytes.size()) {
        QMessageBox::warning(this, "提示", "头像临时文件写入失败");
        out.close();
        QFile::remove(m_pendingAvatarPath);
        m_pendingAvatarPath.clear();
        return;
    }
    out.close();

    m_avatarUploadTaskId = ConnectionManager::instance().startFileUpload( m_pendingAvatarPath,
                    QStringLiteral("avatar.") + ext, mime, Proto::BizAvatar);

    if (m_avatarUploadTaskId == 0)
        QMessageBox::warning(this, "提示", "头像上传失败，请重试");
}

void UserInfoDialog::onAvatarUploadFinished(quint32 taskId, const QString &fileId, const FileMeta &meta)
{
    if (taskId != m_avatarUploadTaskId || taskId == 0)
        return;   // 不是本窗口发起的那一路

    qInfo() << "[头像] 上传完成 fileId=" << fileId  << "size=" << meta.size << "mime=" << meta.mime << "fileName=" << meta.fileName;
    m_avatarUploadTaskId = 0;

    if (!m_pendingAvatarPath.isEmpty()) {
        QFile::remove(m_pendingAvatarPath);   // 服务端已有副本，本地不留
        m_pendingAvatarPath.clear();
    }

    m_pendingAvatarFileId = fileId;
    m_avatarUpdateInFlight = true;
    ConnectionManager::instance().changeAvatar(fileId);
}

void UserInfoDialog::onAvatarUploadFailed(quint32 taskId, int errcode, const QString &text)
{
    if (taskId != m_avatarUploadTaskId || taskId == 0)
        return;

    m_avatarUploadTaskId = 0;
    QMessageBox::warning(this, QStringLiteral("提示"),
                         text.isEmpty() ? QStringLiteral("头像保存失败，请重试") : text);
    Q_UNUSED(errcode);

    // 失败时保留临时文件，用户点"确定"重来时无需重新裁剪（本步先不接重试入口）
}

void UserInfoDialog::onAvatarUpdateResult(bool ok, const QString &text, const QString &avatarFileId)
{
    if (!m_avatarUpdateInFlight)
        return;
    m_avatarUpdateInFlight = false;

    if (ok) {
        qInfo() << "[头像] 设置成功 avatar_file_id=" << avatarFileId;
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("头像已更新"));
    } else {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             text.isEmpty() ? QStringLiteral("头像设置失败，请重试") : text);
    }
}

void UserInfoDialog::refreshMyAvatarIcon()
{
    const qint64 uid = ConnectionManager::instance().currentUid();
    const QPixmap pixmap = (uid > 0) ? AvatarCache::instance().pixmapForUid(uid) : QPixmap();

    ui->avatarToolButton->setIcon(QIcon(pixmap.isNull() ? defaultAvatar() : pixmap));
    ui->avatarToolButton->setIconSize(QSize(32, 32));
}

void UserInfoDialog::on_passwordBtn_clicked()
{
    if (!ConnectionManager::instance().isLoggedIn()) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先登录"));
        return;
    }
    if (!m_passWordDialog)
        m_passWordDialog = new PassWordDialog(this);
    m_passWordDialog->show();
    m_passWordDialog->raise();          // 提到最前
    m_passWordDialog->activateWindow(); // 给焦点
}
