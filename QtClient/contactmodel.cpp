#include "contactmodel.h"
#include <QDebug>

ContactModel::ContactModel(QObject *parent)
    : QAbstractListModel(parent)
{

}

int ContactModel::rowCount(const QModelIndex &parent) const
{
    // For list models only the root node (an invalid parent) should return the list's size. For all
    // other (valid) parents, rowCount() should return 0 so that it does not become a tree model.
    if (parent.isValid())
        return 0;

    return m_contacts.size();   // ListView 行数
}

// index: 列的索引( QList 的列表模型，视图默认只有1列 ) , row: 行的索引
QVariant ContactModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_contacts.size())
        return QVariant();  // 空 QVariant 表示“查无此项”

    const ContactItem &item = m_contacts.at(index.row());
    switch(role) {
        case IdRole:
            return item.uid;
        case NameRole:
            return item.name;
        case AvatarRole:
            return item.avatar;
        case OnlineRole:
            return item.online;
        case UnreadRole:
            return item.unreadCount;
        default:
            return QVariant();
    }
}

// 整表重建，所有行重查数据
void ContactModel::setContacts(const QList<ContactItem> &contacts)
{
    beginResetModel(); // QAbstractListModel 提供的信号机制, 通知视图 QListView , 开始修改数据
    m_contacts = contacts;
    endResetModel(); // 告诉视图“数据改完了，请重新读取所有行数（rowCount）和内容（data）”, 列表立即刷新
}

// 只重绘一行
void ContactModel::updateOnlineStatus(const QString &uid, bool online)
{
    for(int i = 0; i < m_contacts.size(); i++) {
        if(m_contacts[i].uid == uid) {
            m_contacts[i].online = online;
            emit dataChanged(index(i), index(i), {OnlineRole});// 只有 OnlineRole 对应的数据变了
            return;
        }
    }
}

ContactItem ContactModel::getContact(const QString &uid) const
{
    for(const ContactItem &item : m_contacts) {
        if(item.uid == uid) {
            return item;
        }
    }
    return ContactItem();
}

void ContactModel::setUnreadCount(const QString &uid, int count)
{
    for (int i = 0; i < m_contacts.size(); ++i) {
        if (m_contacts[i].uid == uid) {
            if (m_contacts[i].unreadCount == count)
                return;
            m_contacts[i].unreadCount = count;
            emit dataChanged(index(i), index(i), {UnreadRole});
            return;
        }
    }
}

void ContactModel::updateNickname(const QString &uid, const QString &nick)
{
    for (int i = 0; i < m_contacts.size(); ++i) {
        if (m_contacts[i].uid == uid) {
            if (m_contacts[i].name == nick)
                return;
            m_contacts[i].name = nick;
            emit dataChanged(index(i), index(i), {NameRole});
            return;
        }
    }
}

void ContactModel::updateAvatar(const QString &uid, const QPixmap &pixmap)
{
    for (int i = 0; i < m_contacts.size(); ++i) {
        if (m_contacts[i].uid != uid)
            continue;
        m_contacts[i].avatar = pixmap;
        emit dataChanged(index(i), index(i), {AvatarRole});
        return;
    }
}


// 二分找插入位置
static int sortedInsertPos(const QList<ContactItem> &list, qint64 time)
{
    int begin = 0, end = list.size();
    while(begin < end) {
        const int mid = (begin + end) / 2;
        if(list[mid].sortTime >= time)
            begin = mid + 1;
        else
            end = mid;
    }
    return begin;
}

void ContactModel::upsertContact(const ContactItem &item)
{
    for(int i = 0; i < m_contacts.size(); ++i) {
        if(m_contacts[i].uid != item.uid)
            continue;
        int indexes = i;
        const bool dataChange = (m_contacts[i].name != item.name || m_contacts[i].online != item.online
                                    || m_contacts[i].unreadCount != item.unreadCount);
        const bool timeChange = m_autoSort & (item.sortTime > m_contacts[i].sortTime);
        if(timeChange)
            indexes = sortedInsertPos(m_contacts, item.sortTime);

        if(dataChange || timeChange) {
            m_contacts[i].name        = item.name;
            m_contacts[i].online      = item.online;
            m_contacts[i].unreadCount = item.unreadCount;
            m_contacts[i].avatar      = item.avatar;
            m_contacts[i].sortTime    = item.sortTime;
            emit dataChanged(index(i), index(i), {NameRole, OnlineRole, UnreadRole, AvatarRole});
        }
        if(timeChange && indexes < i) {
            if(!beginMoveRows(QModelIndex(), i, i, QModelIndex(), indexes)) {
                qWarning() << "upsertContact: unexpected invalid move" << i << indexes;
            } else {
                m_contacts.move(i, indexes);
                endMoveRows();
            }
        }
        return;
    }
    const int row = m_autoSort ? sortedInsertPos(m_contacts, item.sortTime) : m_contacts.size();
    beginInsertRows(QModelIndex(), row, row);
    m_contacts.insert(row, item);
    endInsertRows();
}

void ContactModel::removeContact(const QString &uid)
{
    for(int i = 0; i < m_contacts.size(); ++i) {
        if(m_contacts[i].uid == uid) {
            beginRemoveRows(QModelIndex(), i, i);
            m_contacts.removeAt(i);
            endRemoveRows();
            return;
        }
    }
}


