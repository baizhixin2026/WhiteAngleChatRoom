#include "protocolcodec.h"

#include <QtEndian>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <QDebug>

// JSON 读写小工具
namespace {
    // uid 一律用字符串传,避免大整数在 JSON 里被当 double 截断
    qint64 jUid(const QJsonObject &o, const char *k)
    {
        return o.value(QLatin1String(k)).toString().toLongLong();
    }

    void putUid(QJsonObject &o, const char *k, qint64 v)
    {
        if (v > 0)
            o.insert(QLatin1String(k), QString::number(v));
    }

    // 长度 / 偏移 / 时间戳用数字传（都在 2^53 内，加/减法无精度问题）
    qint64 jNum(const QJsonObject &o, const char *k, qint64 def = 0)
    {
        const QJsonValue v = o.value(QLatin1String(k));
        if (v.isUndefined() || v.isNull())
            return def;
        return static_cast<qint64>(v.toDouble());
    }

    QJsonObject fileMetaToJson(const FileMeta &m)
    {
        QJsonObject o;
        if (!m.fileId.isEmpty())
            o.insert(QStringLiteral("file_id"), m.fileId);
        if (!m.sha256.isEmpty())
            o.insert(QStringLiteral("sha256"), m.sha256);
        if (!m.fileName.isEmpty())
            o.insert(QStringLiteral("file_name"), m.fileName);
        if (!m.mime.isEmpty())
            o.insert(QStringLiteral("mime"), m.mime);
        if (m.size > 0)
            o.insert(QStringLiteral("size"), double(m.size));
        putUid(o, "owner_uid", m.ownerUid);
        o.insert(QStringLiteral("biz"), int(m.biz));
        o.insert(QStringLiteral("status"), int(m.status));
        if (m.createdAt > 0)
            o.insert(QStringLiteral("created_at"), double(m.createdAt));
        return o;
    }

    void jsonToFileMeta(const QJsonObject &o, FileMeta *m)
    {
        if (!m)
            return;
        m->fileId    = o.value(QStringLiteral("file_id")).toString();
        m->sha256    = o.value(QStringLiteral("sha256")).toString().toLower();
        m->fileName  = o.value(QStringLiteral("file_name")).toString();
        m->mime      = o.value(QStringLiteral("mime")).toString();
        m->size      = jNum(o, "size");
        m->ownerUid  = jUid(o, "owner_uid");
        m->biz       = quint8(jNum(o, "biz"));
        m->status    = quint8(jNum(o, "status"));
        m->createdAt = jNum(o, "created_at");
    }

    const QSet<QString> &fileControlTypes()
    {
        static const QSet<QString> kTypes = {
            Proto::kFileUploadBegin,   Proto::kFileUploadBeginOk,
            Proto::kFileUploadEnd,     Proto::kFileUploadEndOk,
            Proto::kFileUploadAbort,   Proto::kFileUploadAbortOk,
            Proto::kFileDownloadBegin, Proto::kFileDownloadBeginOk,
            Proto::kFileDownloadAbort, Proto::kFileDownloadAbortOk,
            Proto::kFileMetaQuery,     Proto::kFileMetaResult,
            Proto::kFileDelete,        Proto::kFileDeleteOk,
        };
        return kTypes;
    }

    static const QSet<QString> &sysMessageTypes()
    {
        static const QSet<QString> kTypes = {
            Proto::kSysAnnounce,  Proto::kSysPing,  Proto::kSysPong,
            Proto::kAuthLoginOk,  Proto::kAuthLoginFail,
            Proto::kAuthRegisterOk, Proto::kAuthRegisterFail,
            Proto::kNicknameChanged, Proto::kAvatarUpdateResult,
            // 好友
            Proto::kFriendPendingRequest,  Proto::kFriendReqAddResult, Proto::kFriendAdded,
            Proto::kFriendRemoveResult,  Proto::kFriendRemoved,
            Proto::kUserSearchResult, Proto::kFriendHandleResult,
            Proto::kFriendAvatarChanged,
            Proto::kSysFriends,     Proto::kSysFriendsOnlineUpdate,
        };
        return kTypes;
    }

    void parseFriendList(const QJsonArray &arr, QList<SearchResult> *friends)
    {
        friends->clear();
        friends->reserve(arr.size());
        for (const QJsonValue &v : arr) {
            if (!v.isObject())
                continue;
            const QJsonObject o = v.toObject();
            SearchResult e;
            e.uid          = jUid(o, "uid");
            e.accountNo    = jUid(o, "account_no");
            e.nick         = o.value(QStringLiteral("nick")).toString();
            e.avatarFileId = o.value(QStringLiteral("avatar_file_id")).toString();
            e.online       = o.value(QStringLiteral("online")).toBool();
            if (e.uid <= 0)
                continue;
            friends->append(e);
        }
    }

    void parseChatMessageArray(const QJsonArray &arr, quint8 defConvType, qint64 defConvId, QList<ChatMessage> *out)
    {
        out->clear();
        out->reserve(arr.size());
        for(const QJsonValue &v : arr) {
            if(!v.isObject())
                continue;
            const QJsonObject o = v.toObject();
            ChatMessage c;
            c.convType    = quint8(jNum(o, "conv_type", defConvType));
            c.convId      = o.contains(QStringLiteral("conv_id")) ? jUid(o, "conv_id") : defConvId;
            c.msgType     = quint8(jNum(o, "msg_type", Proto::MsgTypeText));
            if(!Proto::isValidMsgType(c.msgType))
                c.msgType = Proto::MsgTypeText;
            c.msgId       = o.value(QStringLiteral("msg_id")).toString();
            c.clientMsgId = o.value(QStringLiteral("client_msg_id")).toString();
            c.senderUid   = jUid(o, "from_uid");
            c.text        = o.value(QStringLiteral("text")).toString();
            c.fileId      = o.value(QStringLiteral("file_id")).toString();
            c.fileName    = o.value(QStringLiteral("file_name")).toString();
            c.fileSize    = jNum(o, "file_size");
            c.ts          = jNum(o, "ts");
            c.fromNick    = o.value(QStringLiteral("from_nick")).toString();
            out->append(c);
        }
    }
}


// 打包帧(帧头+载荷): 帧头6字节(1字节通道号 + 1字节版本号 + 4字节长度) + 载荷
QByteArray ProtocolCodec::wrapFrame(quint8 channel, const QByteArray &payload)
{
    if(payload.size() > int(Proto::kMaxFrameSize))
        return QByteArray();
    QByteArray frame;
    frame.reserve(Proto::Frame::HeaderSize + payload.size());
    const quint32 len = qToBigEndian<quint32>(payload.size()); //转换为大端序（网络字节序）的32位无符号整数
    frame.append(char(channel));
    frame.append(Proto::kVersion);
    frame.append(reinterpret_cast<const char*>(&len),Proto::Frame::LengthSize); // 将len的4个字节按大端序追加到帧中
    frame.append(payload);
    return frame;
}

// （0x01 通道）编码: JSON 对象
QByteArray ProtocolCodec::encodeJson(const QJsonObject &obj)
{
    const QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);// 紧凑格式的JSON
    return wrapFrame(Proto::kChJson, body);
}

// （0x02 通道）编码：文件块
QByteArray ProtocolCodec::encodeFileChunk(const FileChunk &c)
{
    if(c.transferId == 0) {
        qWarning() << "encodeFileChunk: transferId 未分配";
        return QByteArray();
    }

    if(c.data.isEmpty() || c.data.size() > Proto::kFileChunkSize) {
        qWarning() << "encodeFileChunk: bad data size " << c.data.size();
        return QByteArray();
    }

    // 载荷 = [4B fileId][8B offset][4B dataLen][裸二进制]
    QByteArray payload;
    payload.reserve(Proto::Chunk::HeaderSize + c.data.size());
    const quint32 tid = qToBigEndian<quint32>(c.transferId);
    const quint64 off = qToBigEndian<quint64>(c.offset);
    const quint32 len = qToBigEndian<quint32>(quint32(c.data.size()));
    payload.append(reinterpret_cast<const char *>(&tid), Proto::Chunk::TransferIdSize);
    payload.append(reinterpret_cast<const char *>(&off), Proto::Chunk::OffsetSize);
    payload.append(reinterpret_cast<const char *>(&len), Proto::Chunk::LenSize);
    payload.append(c.data);   // 裸二进制
    return wrapFrame(Proto::kChFile, payload);
}

// FileMessage -> JSON
QJsonObject ProtocolCodec::toJson(const FileMessage &m)
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), m.type);
    if (m.transferId != 0)
        o.insert(QStringLiteral("transfer_id"), double(m.transferId));
    if (!m.fileId.isEmpty())
        o.insert(QStringLiteral("file_id"), m.fileId);
    o.insert(QStringLiteral("biz"), int(m.biz));

    if(!m.fileName.isEmpty())
        o.insert(QStringLiteral("file_name"), m.fileName);
    if(m.fileSize > 0)
        o.insert(QStringLiteral("file_size"), double(m.fileSize));
    if(!m.sha256.isEmpty())
        o.insert(QStringLiteral("sha256"), m.sha256);
    if(!m.mime.isEmpty())
        o.insert(QStringLiteral("mime"), m.mime);
    if(m.offset > 0)
        o.insert(QStringLiteral("offset"), double(m.offset));
    if(m.length >= 0)
        o.insert(QStringLiteral("length"), double(m.length));   // -1 = 到末尾
    if(m.chunkSize > 0)
        o.insert(QStringLiteral("chunk_size"), int(m.chunkSize));
    if(m.totalSize > 0)
        o.insert(QStringLiteral("total_size"), double(m.totalSize));
    if(m.expireAt > 0)
        o.insert(QStringLiteral("expire_at"), double(m.expireAt));
    if(!m.meta.fileId.isEmpty())
        o.insert(QStringLiteral("file_meta"), fileMetaToJson(m.meta));
    o.insert(QStringLiteral("ok"), m.ok);
    o.insert(QStringLiteral("errcode"), m.errcode);
    if (!m.text.isEmpty())
        o.insert(QStringLiteral("text"), m.text);
    return o;
}

// 分帧（处理粘包/半包）, 成功返回 true 并输出通道号+载荷, 数据不足返回 false
FrameStatus ProtocolCodec::takeFrame(QByteArray &buf, quint8 *channel, quint8 *version, QByteArray *payload)
{
    if(buf.size() < Proto::Frame::HeaderSize)
        return FrameStatus::NeedMore;

    const quint8 ch = quint8(buf.at(0));
    if(ch != Proto::kChJson && ch != Proto::kChFile) {
        qWarning() << "takeFrame: bad channel " << ch << ", buffer dropped";
        return FrameStatus::BadChannel;
    }

    const quint8 ver = quint8(buf[Proto::Frame::ChannelSize]);
    const quint32 len = qFromBigEndian<quint32>( reinterpret_cast<const uchar *>(buf.constData())
                                        + Proto::Frame::ChannelSize + Proto::Frame::VersionSize);
    if(len > Proto::kMaxFrameSize) {
        qWarning() << "takeFrame: frame too large " << len << ", buffer dropped";
        return FrameStatus::FrameTooLarge;
    }

    const int total = Proto::Frame::HeaderSize + int(len);
    if(buf.size() < total)
        return FrameStatus::NeedMore;   // 半包: 等后续字节

    if(channel)
        *channel = ch;
    if(version)
        *version = ver;
    if(payload)
        *payload = buf.mid(Proto::Frame::HeaderSize, int(len));
    buf.remove(0, total);    // 粘包: 循环调用逐帧取
    return FrameStatus::Ok;
}

// 载荷解码 通道0x01, JSON文本的字节流 解析成 QJsonObject 结构化对象(键值对结构) 可以用 obj["type"] 访问字段
bool ProtocolCodec::toJsonObject(const QByteArray &payload, QJsonObject *obj)
{
    if (!obj)
        return false;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if(!doc.isObject()) {
        qWarning() << "toJsonObject: parse failed at offset= " << err.offset << err.errorString();
        return false;
    }
    *obj = doc.object();
    return true;
}

// 载荷解码 通道0x02, [4B fileId][8B offset][4B dataLen][裸二进制]
bool ProtocolCodec::toFileChunk(const QByteArray &payload, FileChunk *c)
{
    if(!c)
        return false;
    if(payload.size() < Proto::Chunk::HeaderSize ) {
        qWarning() << "toFileChunk: payload too small " << payload.size();
        return false;
    }

    const uchar *p = reinterpret_cast<const uchar *>(payload.constData());
    const quint32 len = qFromBigEndian<quint32>(p + Proto::Chunk::TransferIdSize + Proto::Chunk::OffsetSize);
    if(len > Proto::kFileChunkSize || int(len) != (payload.size() - Proto::Chunk::HeaderSize)) {
        qWarning() << "toFileChunk: bad data length: " << len << " vs payload: " << payload.size();
        return false;
    }
    c->transferId = qFromBigEndian<quint32>(p);
    c->offset = qFromBigEndian<quint64>(p + Proto::Chunk::TransferIdSize);
    c->data = payload.mid(Proto::Chunk::HeaderSize, int(len));
    return true;
}

bool ProtocolCodec::objectToStruct(const QJsonObject &obj, ChatMessage *m)
{
    if (!m || obj.value(QStringLiteral("type")).toString() != Proto::kChatS2C)
        return false;
    m->msgId       = obj.value(QStringLiteral("msg_id")).toString();
    m->clientMsgId = obj.value(QStringLiteral("client_msg_id")).toString();
    m->senderUid   = jUid(obj, "from_uid");
    m->convType    = quint8(jNum(obj, "conv_type", Proto::ConvPrivate));
    m->convId      = jUid(obj, "conv_id");
    m->msgType     = quint8(jNum(obj, "msg_type", Proto::MsgTypeText));
    if (!Proto::isValidMsgType(m->msgType))
        m->msgType = Proto::MsgTypeText;
    m->text        = obj.value(QStringLiteral("text")).toString();
    m->fileId      = obj.value(QStringLiteral("file_id")).toString();
    m->fileName    = obj.value(QStringLiteral("file_name")).toString();
    m->fileSize    = jNum(obj, "file_size");
    m->ts          = jNum(obj, "ts");
    m->fromNick    = obj.value(QStringLiteral("from_nick")).toString();
    return true;
}

bool ProtocolCodec::objectToStruct(const QJsonObject &obj, ChatAck *m)
{
    if (!m || obj.value(QStringLiteral("type")).toString() != Proto::kChatAck)
            return false;
    m->convType    = quint8(jNum(obj, "conv_type", Proto::ConvPrivate));
    m->convId      = jUid(obj, "conv_id");
    m->msgId       = obj.value(QStringLiteral("msg_id")).toString();
    m->clientMsgId = obj.value(QStringLiteral("client_msg_id")).toString();
    m->serverTs    = jNum(obj, "server_ts");
    m->ok          = obj.value(QStringLiteral("ok")).toBool();
    m->pending     = obj.value(QStringLiteral("pending")).toBool();
    m->errcode     = int(jNum(obj, "errcode"));
    m->text        = obj.value(QStringLiteral("text")).toString();
    return true;
}

bool ProtocolCodec::objectToStruct(const QJsonObject &obj, SysMessage *s)
{
    if (!s)
        return false;

    s->type = obj.value(QStringLiteral("type")).toString();
    if (!sysMessageTypes().contains(s->type))
        return false;

    s->text = obj.value(QStringLiteral("text")).toString();
    s->seq  = int(jNum(obj, "seq"));

    if (s->type == Proto::kAuthLoginOk) {
        s->uid          = jUid(obj, "uid");
        s->accountNo    = jUid(obj, "account_no");
        s->nick         = obj.value(QStringLiteral("nick")).toString();
        s->avatarFileId = obj.value(QStringLiteral("avatar_file_id")).toString();
    }
    else if (s->type == Proto::kAuthRegisterOk) {
        s->accountNo = jUid(obj, "account_no");
        s->name      = obj.value(QStringLiteral("name")).toString();
    }
    else if(s->type == Proto::kNicknameChanged) {
        s->uid  = jUid(obj, "uid");
        s->nick = obj.value(QStringLiteral("nick")).toString();
        if (s->uid > 0)
            s->nickMap[QString::number(s->uid)] = s->nick;   // 昵称表同步更新
    }
    else if (s->type == Proto::kAvatarUpdateResult) {
        s->ok           = obj.value(QStringLiteral("ok")).toBool();
        s->avatarFileId = obj.value(QStringLiteral("avatar_file_id")).toString();
    }
    else if (s->type == Proto::kSysFriends) {   // 登录快照
        parseFriendList(obj.value(QStringLiteral("friends")).toArray(), &s->friends);
        for (const SearchResult &f : s->friends)
            s->nickMap[QString::number(f.uid)] = f.nick;
    }
    else if (s->type == Proto::kSysFriendsOnlineUpdate) {
        s->uid    = jUid(obj, "uid");
        s->online = obj.value(QStringLiteral("online")).toBool();
    }
    else if(s->type == Proto::kUserSearchResult) {  // 搜索结果
        // 搜索按 8 位账号精确命中，最多 1 条；找不到时 ok=false、users 为空
        s->ok = obj.value(QStringLiteral("ok")).toBool();
        s->accountNo = jUid(obj, "account_no");
        s->searchUsers.clear();
        if (s->ok) {
            SearchResult u;
            u.uid          = jUid(obj, "uid");
            u.accountNo    = s->accountNo;
            u.nick         = obj.value(QStringLiteral("nick")).toString();
            u.avatarFileId = obj.value(QStringLiteral("avatar_file_id")).toString();
            u.online       = false;   // 搜索结果永远看不到在线状态
            if (u.uid > 0)
                s->searchUsers.append(u);
        }
    }
    else if(s->type == Proto::kFriendReqAddResult) {   // 给申请人的回执
        s->ok   = obj.value(QStringLiteral("ok")).toBool();
        s->text = obj.value(QStringLiteral("text")).toString();
    }
    else if(s->type == Proto::kFriendPendingRequest) { // 待我处理的好友申请
        s->reqId    = jUid(obj, "req_id");
        s->fromUid  = jUid(obj, "from_uid");
        s->fromNick = obj.value(QStringLiteral("from_nick")).toString();
        s->text     = obj.value(QStringLiteral("text")).toString();
        s->ts       = jNum(obj, "ts");
    }
    else if(s->type == Proto::kFriendHandleResult) { // 接收方：处理结果
        s->handledReqId = jUid(obj, "req_id");
        s->ok           = obj.value(QStringLiteral("ok")).toBool();
        s->text         = obj.value(QStringLiteral("text")).toString();
    }
    else if(s->type == Proto::kFriendAdded) { // 增量
        s->uid          = jUid(obj, "uid");
        s->accountNo    = jUid(obj, "account_no");
        s->nick         = obj.value(QStringLiteral("nick")).toString();
        s->avatarFileId = obj.value(QStringLiteral("avatar_file_id")).toString();
        s->online       = obj.value(QStringLiteral("online")).toBool();
        if (s->uid > 0)
            s->nickMap[QString::number(s->uid)] = s->nick;
    }
    else if(s->type == Proto::kFriendRemoved) { // 增量
        s->uid = jUid(obj, "uid");
    }
    else if(s->type == Proto::kFriendRemoveResult) {
        s->ok   = obj.value(QStringLiteral("ok")).toBool();
        s->text = obj.value(QStringLiteral("text")).toString();
        s->uid  = jUid(obj, "remove_uid");
    }
    else if (s->type == Proto::kFriendAvatarChanged) {
        s->uid          = jUid(obj, "uid");
        s->avatarFileId = obj.value(QStringLiteral("avatar_file_id")).toString();
    }

    return true;
}

bool ProtocolCodec::objectToStruct(const QJsonObject &obj, NicknameChangeResult *s)
{
    if (!s || obj.value(QStringLiteral("type")).toString() != Proto::kNicknameChangeResult)
        return false;
    s->nick = obj.value(QStringLiteral("nick")).toString();
    s->text = obj.value(QStringLiteral("text")).toString();
    s->ok   = obj.value(QStringLiteral("ok")).toBool();
    return true;
}

bool ProtocolCodec::objectToStruct(const QJsonObject &obj, PasswordChangeResult *m)
{
    if (!m || obj.value(QStringLiteral("type")).toString() != Proto::kPasswordChangeResult)
        return false;
    m->ok      = obj.value(QStringLiteral("ok")).toBool();
    m->errcode = int(jNum(obj, "errcode"));
    m->text    = obj.value(QStringLiteral("text")).toString();
    return true;
}

bool ProtocolCodec::objectToStruct(const QJsonObject &obj, HistoryPage *m)
{
    if (!m || obj.value(QStringLiteral("type")).toString() != Proto::kHistoryResult)
        return false;

    m->ok       = obj.value(QStringLiteral("ok")).toBool();
    m->text     = obj.value(QStringLiteral("text")).toString();
    m->convType = quint8(jNum(obj, "conv_type", Proto::ConvPrivate));
    m->convId   = jUid(obj, "conv_id");
    m->hasMore  = obj.value(QStringLiteral("has_more")).toBool();
    parseChatMessageArray(obj.value(QStringLiteral("messages")).toArray(), m->convType, m->convId, &m->messages);
    return true;
}

bool ProtocolCodec::objectToStruct(const QJsonObject &obj, RecentChatResult *m)
{
    if (!m || obj.value(QStringLiteral("type")).toString() != Proto::kRecentChatResult)
        return false;

    m->ok   = obj.value(QStringLiteral("ok")).toBool();
    m->text = obj.value(QStringLiteral("text")).toString();
    m->items.clear();

    const QJsonArray arr = obj.value(QStringLiteral("items")).toArray();
    m->items.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        RecentChatItem item;
        item.convType     = quint8(jNum(o, "conv_type", Proto::ConvPrivate));
        item.convId       = jUid(o, "conv_id");
        if (item.convId <= 0)
            continue;
        item.peerNick     = o.value(QStringLiteral("peer_nick")).toString();
        item.lastMsgId    = o.value(QStringLiteral("last_msg_id")).toString();
        item.lastMsgTs    = jNum(o, "last_msg_ts");
        item.lastMsgText  = o.value(QStringLiteral("last_msg_text")).toString();
        item.lastMsgType  = quint8(jNum(o, "last_msg_type", Proto::MsgTypeText));
        if (!Proto::isValidMsgType(item.lastMsgType))
            item.lastMsgType = Proto::MsgTypeText;
        item.unreadCount  = int(jNum(o, "unread_count"));
        m->items.append(item);
    }
    return true;
}

bool ProtocolCodec::isFileControlType(const QString &type)
{
    return !type.isEmpty() && fileControlTypes().contains(type);
}

bool ProtocolCodec::objectToStruct(const QJsonObject &obj, FileMessage *m)
{
    if (!m)
        return false;

    const QString type = obj.value(QStringLiteral("type")).toString();
    if (!isFileControlType(type))
        return false;

    m->type       = type;
    m->transferId = quint32(jNum(obj, "transfer_id"));
    m->fileId     = obj.value(QStringLiteral("file_id")).toString();
    m->biz        = quint8(jNum(obj, "biz"));
    m->fileName   = obj.value(QStringLiteral("file_name")).toString();
    m->fileSize   = jNum(obj, "file_size");
    m->sha256     = obj.value(QStringLiteral("sha256")).toString().toLower();
    m->mime       = obj.value(QStringLiteral("mime")).toString();
    m->offset     = jNum(obj, "offset");
    m->length     = obj.contains(QStringLiteral("length")) ? jNum(obj, "length") : -1;
    m->chunkSize  = int(jNum(obj, "chunk_size"));
    m->totalSize  = jNum(obj, "total_size");
    m->expireAt   = jNum(obj, "expire_at");
    m->ok         = obj.value(QStringLiteral("ok")).toBool();
    m->errcode    = int(jNum(obj, "errcode"));
    m->text       = obj.value(QStringLiteral("text")).toString();
    if (obj.value(QStringLiteral("file_meta")).isObject())
        jsonToFileMeta(obj.value(QStringLiteral("file_meta")).toObject(), &m->meta);
    return true;
}



