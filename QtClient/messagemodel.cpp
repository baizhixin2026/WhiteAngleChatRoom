#include "messagemodel.h"

MessageModel::MessageModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int MessageModel::rowCount(const QModelIndex &parent) const
{
    // For list models only the root node (an invalid parent) should return the list's size. For all
    // other (valid) parents, rowCount() should return 0 so that it does not become a tree model.
    if (parent.isValid())
        return 0;
    return m_messages.size();
}

QVariant MessageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_messages.size())
        return QVariant();

    const MessageItem &msg = m_messages.at(index.row());
    switch(role) {
        case BodyRole:     return msg.body;
        case SenderRole:   return msg.sender;
        case SenderUidRole:return msg.senderUid;
        case IsSelfRole:   return msg.isSelf;
        case TimestampRole:return msg.timestamp;
        case AvatarRole:   return msg.avatar;
        case ShowNameRole: return msg.showName;
        case MsgTypeRole:  return msg.msgType;
        case FileIdRole:   return msg.fileId;
        case FileNameRole: return msg.fileName;
        case FileSizeRole: return msg.fileSize;
        case ThumbRole:    return msg.thumb;
        default:
            return QVariant();
    }
}

// 切换会话
void MessageModel::setMessages(const QList<MessageItem> &messages)
{
    beginResetModel();
    m_messages = messages;
    endResetModel();
}

QList<MessageItem> MessageModel::saveMessages()
{
    return m_messages;
}

// 新消息: 每次追加一条消息
void MessageModel::appendMessage(const MessageItem &msg)
{
    const int row = m_messages.size();
    beginInsertRows(QModelIndex(), row, row);   // 插入1行
    m_messages.append(msg);
    endInsertRows();
}

void MessageModel::clear()
{
    beginResetModel();
    m_messages.clear();
    endResetModel();
}

void MessageModel::updateMsgIdByClientId(const QString &clientMsgId, const QString &msgId)
{
    if (clientMsgId.isEmpty() || msgId.isEmpty())
        return;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].clientMsgId == clientMsgId && m_messages[i].msgId.isEmpty()) {
            m_messages[i].msgId = msgId;
            return;
        }
    }
}

void MessageModel::prependMessages(const QList<MessageItem> &msgs)
{
    if(msgs.isEmpty())
        return;
    const int n = msgs.size();
    beginInsertRows(QModelIndex(), 0, n - 1);
    for(int i = n - 1; i >= 0; --i)
        m_messages.prepend(msgs[i]);  // 头部插入旧消息
    endInsertRows();
}

// 某个人的头像下载好了：把属于他的消息行全部换图并重绘
void MessageModel::refreshAvatar(qint64 senderUid, const QPixmap &pixmap)
{
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].senderUid != senderUid)
            continue;
        m_messages[i].avatar = pixmap;
        emit dataChanged(index(i), index(i), {AvatarRole});
    }
}

void MessageModel::refreshThumb(const QString &fileId, const QPixmap &pixmap)
{
    if (fileId.isEmpty())
        return;
    for (int i = 0; i < m_messages.size(); ++i) {
        if (m_messages[i].fileId != fileId)
            continue;
        m_messages[i].thumb = pixmap;
        emit dataChanged(index(i), index(i), {ThumbRole});
    }
}
