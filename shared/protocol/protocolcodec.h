#ifndef PROTOCOLCODEC_H
#define PROTOCOLCODEC_H

#include "protocoltypes.h"
#include <QByteArray>
#include <QJsonObject>

/*
 * ProtocolCodec 协议编解码
 * 帧格式 : [1B 通道][4B 大端载荷长度][载荷]
 * 0x01 通道: 载荷 = UTF-8 JSON（控制面: 登录/聊天/系统/文件控制）
 * 0x02 通道: 载荷 = [4B fileId][8B offset][4B dataLen][裸二进制]（数据面）
 *
 * 线程纪律: 只在 NetworkWorker 线程内调用，禁止跨线程。
*/

enum class FrameStatus {
    NeedMore = 0,   // 半包：数据不足，缓冲区保持不变，等后续字节
    Ok,             // 成功取出一帧
    BadChannel,     // 通道号非法：流已失去同步，必须断开连接
    FrameTooLarge   // 长度超限：流已不可信，必须断开连接
};

class ProtocolCodec
{
public:
    ProtocolCodec() = delete;

    static QByteArray encodeJson(const QJsonObject &obj);
    static QByteArray encodeFileChunk(const FileChunk &c);

    static QJsonObject toJson(const FileMessage &m);

    static FrameStatus takeFrame(QByteArray &buf, quint8 *channel, quint8 *version, QByteArray *payload);

    // 载荷解码
    static bool toJsonObject(const QByteArray &payload, QJsonObject *out);
    static bool toFileChunk(const QByteArray &payload, FileChunk *c);

    // JSON → 结构体（按 type 匹配，不匹配返回 false)
    static bool objectToStruct(const QJsonObject &obj, ChatMessage *m);
    static bool objectToStruct(const QJsonObject &obj, ChatAck *m);
    static bool objectToStruct(const QJsonObject &obj, SysMessage *m);
    static bool objectToStruct(const QJsonObject &obj, NicknameChangeResult *m);
    static bool objectToStruct(const QJsonObject &obj, HistoryPage *m);
    static bool objectToStruct(const QJsonObject &obj, RecentChatResult *m);
    static bool objectToStruct(const QJsonObject &obj, FileMessage *m);
    static bool objectToStruct(const QJsonObject &obj, PasswordChangeResult *m);

    static bool isFileControlType(const QString &type); // 服务端分流

private:
    static QByteArray wrapFrame(quint8 channel, const QByteArray &payload);
};

#endif // PROTOCOLCODEC_H
