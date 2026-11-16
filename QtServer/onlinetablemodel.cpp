#include "onlinetablemodel.h"


QVector<UserInfo> OnlineTableModel::filterUsers(const QVector<UserInfo> &allUser) const
{
    QVector<UserInfo> onlineUsers;
    onlineUsers.reserve(allUser.size());
    for(const UserInfo &u : allUser)
        if(u.online)
            onlineUsers.append(u);
    return onlineUsers;
}
