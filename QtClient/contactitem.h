#ifndef CONTACTITEM_H
#define CONTACTITEM_H

#include <QString>
#include <QPixmap>

struct ContactItem {
    QString uid;
    QString name;
    QPixmap avatar;         // 头像
    bool online = false;
    int unreadCount = 0;    // 未读好友消息
    qint64 sortTime = 0;
};

#endif
