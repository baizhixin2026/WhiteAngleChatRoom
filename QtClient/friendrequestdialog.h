#ifndef FRIENDREQUESTDIALOG_H
#define FRIENDREQUESTDIALOG_H

#include "protocoltypes.h"
#include <QDialog>
#include <QHash>

class FriendRequestCard;

namespace Ui {
class FriendRequestDialog;
}

class FriendRequestDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FriendRequestDialog(QWidget *parent = nullptr);
    ~FriendRequestDialog();

    // 全量替换（打开窗口时调用一次）
    void setRequests(const QVector<FriendRequestInfo> &reqs);
    // 增量：窗口已打开时收到新申请
    void addFriendRequest(const FriendRequestInfo &req);
    // 处理完一条：删除行
    void removeRequest(qint64 reqId);
    int count() const { return m_cards.size(); }

signals:
    void handleFriendRequest(qint64 reqId, bool accept);

private:
    void updateEmptyState();

protected:
//    void resizeEvent(QResizeEvent *event) override; // 调试窗口尺寸

private:
    Ui::FriendRequestDialog *ui;

    QHash<qint64, FriendRequestCard*> m_cards;
};

#endif // FRIENDREQUESTDIALOG_H
