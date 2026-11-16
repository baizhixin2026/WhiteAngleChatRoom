#ifndef PASSWORDDIALOG_H
#define PASSWORDDIALOG_H

#include "protocoltypes.h"
#include <QDialog>
#include <QString>

namespace Ui {
class PassWordDialog;
}

class QTimer;

class PassWordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PassWordDialog(QWidget *parent = nullptr);
    ~PassWordDialog();
protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void on_commitBtn_clicked();
    void refreshCommitState();
    void onPasswordChangeResult(const PasswordChangeResult &msg);
    void onServerReplyTimeout();

signals:
    void changePasswordRequested(const QString &oldPassWordHash, const QString &newPassWordHash);

private:
    Ui::PassWordDialog *ui;

    void setBusy(bool busy);
    QTimer *m_replyTimer = nullptr;
    bool    m_busy = false;
};

#endif // PASSWORDDIALOG_H
