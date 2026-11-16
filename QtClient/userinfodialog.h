#ifndef USERINFODIALOG_H
#define USERINFODIALOG_H

#include "protocoltypes.h"
#include <QDialog>
#include <QString>
#include <passworddialog.h>

class AvatarSetDialog;

namespace Ui {
class UserInfoDialog;
}

class UserInfoDialog : public QDialog
{
    Q_OBJECT

public:
    explicit UserInfoDialog(QWidget *parent = nullptr);
    ~UserInfoDialog();

    void setNicknameResult(const NicknameChangeResult &msg);

signals:
    void nicknameChangeRequested(const QString &newNick);

private slots:
    void on_nicknameSaveBtn_clicked();
    void onAvatarCropped(const QImage &cropped);
    void onAvatarUploadFinished(quint32 taskId, const QString &fileId, const FileMeta &meta);
    void onAvatarUploadFailed(quint32 taskId, int errcode, const QString &text);
    void onAvatarUpdateResult(bool ok, const QString &text, const QString &avatarFileId);

    void on_passwordBtn_clicked();

private:
    void refreshMyAvatarIcon();

private:
    Ui::UserInfoDialog *ui;

    QString m_username;
    QString m_nickname;
    qint64 m_accountNo;

    QString  m_pendingAvatarFileId;
    bool     m_avatarUpdateInFlight = false;

    AvatarSetDialog *m_avatarSetDialog = nullptr;
    QString  m_pendingAvatarPath;              // 临时文件，用户上传成功后删
    quint32  m_avatarUploadTaskId = 0;

    PassWordDialog *m_passWordDialog = nullptr;

};

#endif // USERINFODIALOG_H
