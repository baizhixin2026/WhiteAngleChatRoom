#ifndef USERINFO_H
#define USERINFO_H

#include <QMetaType>
#include <QString>

struct UserInfo {
    qint64  id = 0;
    qint64  accountNo = 0;
    QString username;
    QString nickname;
    qint64  createdAt    = 0;    // 毫秒
    qint64  lastLoginAt  = 0;
    qint64  lastActiveAt = 0;
    bool    online       = false;
};

Q_DECLARE_METATYPE(UserInfo)
Q_DECLARE_METATYPE(QVector<UserInfo>)

#endif // USERINFO_H
