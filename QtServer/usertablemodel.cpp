#include "usertablemodel.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>

namespace {
    QString fmtMs(qint64 ms)
    {
        return QDateTime::fromMSecsSinceEpoch(ms).toString("yyyy-MM-dd HH:mm");
    }
}

UserTableModel::UserTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

QVariant UserTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();
    switch(section) {
        case ColId:         return QStringLiteral("ID");
        case ColAccountNo:  return QStringLiteral("账号");
        case ColUsername:   return QStringLiteral("用户名");
        case ColNickname:   return QStringLiteral("昵称");
        case ColCreated:    return QStringLiteral("注册时间");
        case ColLastLogin:  return QStringLiteral("最后登录");
        case ColLastActive: return QStringLiteral("最后活跃");
        case ColOnline:     return QStringLiteral("状态");
    }
    return QVariant();
}

int UserTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;

    return m_users.size();
}

int UserTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;

    return ColCount;
}

QVariant UserTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_users.size() )
        return QVariant();

    const UserInfo &user = m_users.at(index.row());
    if(role == Qt::DisplayRole) {
        switch(index.column()) {
            case ColId:             return user.id;
            case ColAccountNo:      return user.accountNo;
            case ColUsername:       return user.username;
            case ColNickname:       return user.nickname;
            case ColCreated:        return fmtMs(user.createdAt);
            case ColLastLogin:      return user.lastLoginAt <= 0 ? QStringLiteral("从未登录")
                                                                 : fmtMs(user.lastLoginAt);
            case ColLastActive:     return user.lastActiveAt <= 0 ? QStringLiteral("-")
                                                                  : fmtMs(user.lastActiveAt);
            case ColOnline:         return user.online ? QStringLiteral("在线")
                                                       : QStringLiteral("离线");
        }
    }

    // online列着色 (ForegroundRole 文字颜色, BackgroundRole 背景颜色， FontRole 字体)
    if(role == Qt::ForegroundRole && index.column() == ColOnline) {
        return QBrush(user.online ? QColor(0x00, 0x99, 0xFF) : QColor(0x99, 0x99, 0x99));
    }
    // ID 左对齐
//    if(role == Qt::TextAlignmentRole && index.column() == ColId)
//        return int(Qt::AlignLeft | Qt::AlignVCenter);
    return QVariant();
}

QVector<UserInfo> UserTableModel::filterUsers(const QVector<UserInfo> &allUser) const
{
    return allUser;
}

void UserTableModel::setUsers(const QVector<UserInfo> &users)
{
    beginResetModel();
    m_users = filterUsers(users);
    endResetModel();
}

