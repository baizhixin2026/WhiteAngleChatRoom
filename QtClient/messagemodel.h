#ifndef MESSAGEMODEL_H
#define MESSAGEMODEL_H

#include "messageItem.h"
#include <QAbstractListModel>
#include <QList>


class MessageModel : public QAbstractListModel
{
    Q_OBJECT

public:
    explicit MessageModel(QObject *parent = nullptr);

    enum MessageRole {
        BodyRole = Qt::UserRole + 1,    // 消息正文
        SenderRole,                     // 发送者
        SenderUidRole,                  // uid
        IsSelfRole,                     // 是否自己发送
        TimestampRole,                  // 时间戳
        AvatarRole,                     // 头像
        ShowNameRole,                   // 是否显示名字
        MsgTypeRole,
        FileIdRole,
        FileNameRole,
        FileSizeRole,
        ThumbRole
    };

    // Basic functionality:
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    // message
    void setMessages(const QList<MessageItem> &messages);
    QList<MessageItem> saveMessages();
    void appendMessage(const MessageItem &msg);
    void clear();
    void updateMsgIdByClientId(const QString &clientMsgId, const QString &msgId);
    void prependMessages(const QList<MessageItem> &msgs);
    void refreshAvatar(qint64 senderUid, const QPixmap &pixmap);
    void refreshThumb(const QString &fileId, const QPixmap &pixmap);

private:
    QList<MessageItem> m_messages;
};

#endif // MESSAGEMODEL_H
