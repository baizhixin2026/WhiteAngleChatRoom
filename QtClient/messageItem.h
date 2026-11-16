#ifndef MESSAGEITEM_H
#define MESSAGEITEM_H

#include "protocoltypes.h"
#include <QString>
#include <QPixmap>

struct MessageItem {
    QString msgId;  // 服务器为每条消息生成的全局唯一 ID，用于在服务器端 数据库持久化、索引和管理消息。
    QString clientMsgId ;  // 客户端生成的本地消息 ID，用于防重和确认
    qint64  senderUid = 0;
    quint8  msgType   = Proto::MsgTypeText;
    QString fileId;
    QString fileName;
    qint64  fileSize  = 0;
    QString sender;     // nickname
    QString receiver;   // uid
    QString body;
    qint64  timestamp;
    QPixmap avatar;         // 发送者头像
    QPixmap thumb;      // 图片消息的缩略图（文件消息为空）
    bool isSelf = false;
    bool showName = false;  // 是否显示名字（群聊显示，私聊不显示）
};


#endif // MESSAGEITEM_H
