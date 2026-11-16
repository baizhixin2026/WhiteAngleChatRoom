#ifndef FRIENDREQUESTCARD_H
#define FRIENDREQUESTCARD_H

#include "protocoltypes.h"
#include <QWidget>
class QLabel;
class QPushButton;

/*
好友申请列表的单行卡片
布局: [头像] [昵称 / 验证信息 / 时间 / 双向提示] [同意][拒绝]
*/
class FriendRequestCard : public QWidget
{
    Q_OBJECT

public:
    explicit FriendRequestCard(const FriendRequestInfo &info, QWidget *parent = nullptr);

    qint64 getReqId() const { return m_reqId; }
    void setBusy();
    void resetBusy();

signals:
    void acceptClicked(qint64 reqId);
    void rejectClicked(qint64 reqId);

private:
    qint64 m_reqId = 0;
    QPushButton *m_acceptBtn = nullptr;
    QPushButton *m_rejectBtn = nullptr;
};

#endif // FRIENDREQUESTCARD_H
