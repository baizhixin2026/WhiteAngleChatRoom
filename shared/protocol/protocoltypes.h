#ifndef PROTOCOLTYPES_H
#define PROTOCOLTYPES_H

#include "errcodes.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QMetaType>
#include <QVariantMap>
#include <QVector>

// 服务端要求 MySQL 8.0+ （使用了 8.0.14+ 的行值比较语法）

namespace Proto {
    constexpr int kVersion          = 4;
    constexpr quint32 kMaxFrameSize = 1 * 1024 * 1024;  // 单帧上限（含载荷，不含帧头）
    constexpr int kFileChunkSize    = 256 * 1024;       // 文件块大小
    constexpr int kChunkPerFrame    = 1;                // 预留：一块一帧

    // 通道
    constexpr quint8 kChJson = 0x01;// 载荷 = UTF-8 JSON（控制面: 登录/聊天/系统/文件控制）
    constexpr quint8 kChFile = 0x02;// 载荷 = [4B fileId][8B offset][4B dataLen][裸二进制]（数据面）

    // 通用长度限制
    constexpr int kMaxUsernameLen = 16;
    constexpr int kMaxNicknameLen = 32;
    constexpr int kSha256HexLen   = 64; // SHA256 摘要是 256 bit = 32 字节，hex 编码后每个字节 2 个字符 → 固定 64 个字符
    constexpr int kOfflinePullLimit = 500;
    constexpr int kLOGIN_TIMEOUT_Ms = 5000;

    // 账号注册登录
    inline const QString kAuthRegister     = QStringLiteral("auth_register");
    inline const QString kAuthRegisterOk   = QStringLiteral("auth_register_ok");
    inline const QString kAuthRegisterFail = QStringLiteral("auth_register_fail");
    inline const QString kAuthLogout       = QStringLiteral("auth_logout");
    inline const QString kAuthLogin         = QStringLiteral("auth_login");
    inline const QString kAuthLoginOk       = QStringLiteral("auth_login_ok");
    inline const QString kAuthLoginFail     = QStringLiteral("auth_login_fail");

    // 账号验证
    constexpr qint64 kAccountNoMin = 10000000;
    constexpr qint64 kAccountNoMax = 99999999;
    inline bool isValidAccountNo(qint64 no)
    {
        return no >= kAccountNoMin && no <= kAccountNoMax;
    }

    // 昵称
    inline const QString kNicknameChange        = QStringLiteral("nickname_change");
    inline const QString kNicknameChangeResult  = QStringLiteral("nickname_change_result");
    inline const QString kNicknameChanged       = QStringLiteral("nickname_changed");

    // 修改密码
    inline const QString kPasswordChange       = QStringLiteral("password_change");
    inline const QString kPasswordChangeResult = QStringLiteral("password_change_result");
    // 密码策略：两端共用。服务端只收到摘要、看不到明文，所以长度只能由客户端把关
    constexpr int kPasswordMinLen = 6;
    constexpr int kPasswordMaxLen = 20;
    inline bool isValidPasswordLength(int len)
    {
        return len >= kPasswordMinLen && len <= kPasswordMaxLen;
    }

    // 聊天消息
    inline const QString kChatC2S           = QStringLiteral("chat_c2s");
    inline const QString kChatS2C           = QStringLiteral("chat_s2c");
    inline const QString kChatAck           = QStringLiteral("chat_ack");

    // 心跳保活
    inline const QString kSysPing           = QStringLiteral("sys_ping");
    inline const QString kSysPong           = QStringLiteral("sys_pong");
    constexpr int kHeartbeatIntervalMs = 30 * 1000;   // 空闲超过它才发 ping
    constexpr int kServerSessionIdleMs = 90 * 1000;   // 服务端判死（3 × 30s）
    constexpr int kClientMaxMissedPong = 3;           // 连续丢失这么多才判死
    constexpr int kSessionReapSweepMs  = 1000;        // 服务端扫描周期

    // 杂项
    constexpr int kMsgSize = 14;
    inline const QString kSysAnnounce       = QStringLiteral("sys_announce");   // 服务器广播


    // 好友
    constexpr int kMaxFriendReqTextLen = 32;
    constexpr int kMaxFriends          = 500;   // 好友上限 两端共用同一个值
    inline const QString kUserSearch       = QStringLiteral("user_search");               // C→S 按 8 位账号找人
    inline const QString kUserSearchResult = QStringLiteral("user_search_result");        // S→C 搜索结果
    inline const QString kFriendReqAdd     = QStringLiteral("friend_req_add");            // C→S 发起好友申请
    inline const QString kFriendReqAddResult  = QStringLiteral("friend_reqadd_result");   // S→C 给申请人的回执
    inline const QString kFriendPendingRequest= QStringLiteral("friend_pending_request"); // S→C 待我处理的申请
    inline const QString kFriendHandle     = QStringLiteral("friend_handle");             // C→S 通过/拒绝
    inline const QString kFriendHandleResult= QStringLiteral("friend_handle_result");     // S→C 给接收方：处理结果
    inline const QString kFriendAdded      = QStringLiteral("friend_added");              // S→C roster 增量
    inline const QString kFriendRemove      = QStringLiteral("friend_remove");            // C→S 删除好友
    inline const QString kFriendRemoveResult= QStringLiteral("friend_remove_result");     // S→C 删除结果
    inline const QString kFriendRemoved    = QStringLiteral("friend_removed");            // S→C roster 增量
    inline const QString kFriendAvatarChanged   = QStringLiteral("friend_avatar_changed");
    inline const QString kSysFriends       = QStringLiteral("sys_friends");               // S→C 登录快照
    inline const QString kSysFriendsOnlineUpdate = QStringLiteral("sys_presence");        // S→C 在线状态增量

    // 历史消息拉取
    inline const QString kHistoryRequest    = QStringLiteral("history_request");    // C→S {peer_uid, before_msg_id?, limit}
    inline const QString kHistoryResult     = QStringLiteral("history_result");     // S→C {peer_uid, ok, messages, has_more}
    constexpr int kHistoryPageSize = 30;    // 单页默认 加载历史信息条数

    // 最近会话
    inline const QString kRecentChatResult = QStringLiteral("recent_chat_result");    // S→C
    inline const QString kRecentChatMarkRead = QStringLiteral("recent_chat_mark_read");  // C→S
    constexpr int kRecentChatLimit = 60;

    // 表情包
    constexpr int kEmojiUiInputSize = 18;
    constexpr int kEmojiUiShowSize = 24;
    inline const QChar kEmojiBegin = QChar(0xE000);
    inline const QChar kEmojiEnd   = QChar(0xE001);
    inline const QString kEmojiFace = QStringLiteral("face");
    inline const QString kEmojiHand = QStringLiteral("hand");
    inline const QString kEmojiOther = QStringLiteral("other");

    // 头像：先走文件上传拿到 file_id（biz = BizAvatar），再发 avatar_update 挂到账号上
    inline const QString kAvatarUpdate       = QStringLiteral("avatar_update");         // C→S {file_id}
    inline const QString kAvatarUpdateResult = QStringLiteral("avatar_update_result");  // S→C {ok, text, avatar_file_id}

    // 文件上传：begin → *_begin_ok → [0x02 块...] → end → *_end_ok（成功后再发业务消息）
    inline const QString kFileUploadBegin   = QStringLiteral("file_upload_begin");
    inline const QString kFileUploadBeginOk = QStringLiteral("file_upload_begin_ok");
    inline const QString kFileUploadEnd     = QStringLiteral("file_upload_end");
    inline const QString kFileUploadEndOk   = QStringLiteral("file_upload_end_ok");
    inline const QString kFileUploadAbort   = QStringLiteral("file_upload_abort");
    inline const QString kFileUploadAbortOk = QStringLiteral("file_upload_abort_ok");
    // 文件下载：begin → *_begin_ok → [0x02 块...] → 收到 size 字节即完成
    inline const QString kFileDownloadBegin   = QStringLiteral("file_download_begin");
    inline const QString kFileDownloadBeginOk = QStringLiteral("file_download_begin_ok");
    inline const QString kFileDownloadAbort   = QStringLiteral("file_download_abort");
    inline const QString kFileDownloadAbortOk = QStringLiteral("file_download_abort_ok");
    // 元数据 / 删除引用
    inline const QString kFileMetaQuery  = QStringLiteral("file_meta_query");
    inline const QString kFileMetaResult = QStringLiteral("file_meta_result");
    inline const QString kFileDelete     = QStringLiteral("file_delete");
    inline const QString kFileDeleteOk   = QStringLiteral("file_delete_ok");

    constexpr qint64 kMaxFileSize            = 2LL * 1024 * 1024 * 1024;  // 2 GiB
    constexpr qint64 kMaxAvatarFileSize      = 512 * 1024;                // 512 KiB
    constexpr int    kMaxFileNameLen         = 200;                       // 字符
    constexpr int    kMaxTransfersPerSession = 2;                         // 超了回 TooManyTransfers
    constexpr int    kMaxConcurrentTransfers = 200;                       // 服务端全局
    constexpr qint64 kUploadIdleTimeoutMs    = 60 * 1000;
    constexpr qint64 kDownloadIdleTimeoutMs  = 5 * 60 * 1000;             // 弱网排空 4MB 出口队列可能要几分钟
    constexpr int    kMaxSessionBuffer       = 2 * 1024 * 1024;           // 会话接收缓冲硬上限


    // 群聊，预留


    // 会话类型：群聊上线时只需新增取值，协议不再断版
    enum ConvType : quint8 {
        ConvPrivate = 0,
        ConvGroup   = 1
    };

    // 消息类型：MsgText 之外全部携带 file_id
    enum MsgType : quint8 {
        MsgTypeText  = 0,
        MsgTypeImage = 1,
        MsgTypeFile  = 2,
        MsgTypeVoice = 3,
        MsgTypeVideo = 4
    };
    inline bool isValidMsgType(quint8 t) {return t <= MsgTypeVideo;}

    enum FileBizType : quint8 { // 只允许追加，禁止改已有值的语义（BizAvatar = 0 永远不能变成别的，否则老数据全乱）
        BizAvatar      = 0,   // 头像
        BizChatImage   = 1,   // 聊天图片
        BizChatFile    = 2,   // 聊天附件
        BizVoice       = 3,   // 语音
        BizVideo       = 4,   // 聊天视频
        BizPublicVideo = 5    // 公共视频（多端播放)
    };
    inline bool isValidBizType(quint8 b) { return b <= BizPublicVideo; }

    // 文件状态：0=上传中 1=可用 2=已删除（只标记，一期不物理删除）
    enum FileStatus : quint8 {
        FileUploading = 0,
        FileReady     = 1,
        FileDeleted   = 2
    };

    // 权限受让方类型：权限不从中推导消息，独立 ACL
    enum FileGranteeType : quint8 {
        GranteePublic = 0,  // 所有登录用户可读
        GranteeUid    = 1,  // 指定某个用户可读
        GranteeGroup  = 2   // 指定群可读
    };

    // 传输方向
    enum TransferDirection : quint8 {
        TransferUpload   = 0,
        TransferDownload = 1
    };

    inline bool isPublicReadBiz(quint8 biz)
    {
        return biz == BizAvatar || biz == BizPublicVideo;
    }

    // 帧 / 块头部尺寸
    namespace Frame {
        inline constexpr int ChannelSize = 1;
        inline constexpr int VersionSize = 1;
        inline constexpr int LengthSize  = 4;
        inline constexpr int HeaderSize  = ChannelSize + VersionSize + LengthSize;  // 6
    }

    namespace Chunk {
        inline constexpr int TransferIdSize = 4;
        inline constexpr int OffsetSize     = 8;
        inline constexpr int LenSize        = 4;
        inline constexpr int HeaderSize     = TransferIdSize + OffsetSize + LenSize;  // = 16
    }

    // 单帧上限必须放得下一整块 + 块头
    static_assert(int(kMaxFrameSize) >= Proto::kFileChunkSize + Chunk::HeaderSize,
                                        "kMaxFrameSize must hold one full file chunk");
}


struct UserEntry {
    qint64  uid = 0;  // 系统内部身份
    QString name;     // username 登录名
    QString nick;     // nickname 显示名
    QString avatarFileId;
    bool    online = false;
};

// 聊天消息：conv_type + conv_id 标识，附件由 msg_type + file_id 标识
struct ChatMessage {
    quint8  convType  = Proto::ConvPrivate;
    qint64  convId    = 0;      // 接收方: 私聊 = 对端uid, 群聊 = 群 id
    quint8  msgType   = Proto::MsgTypeText;
    QString msgId;          // 服务器为每条消息生成的全局唯一 ID，用于在服务器端 数据库持久化、索引和管理消息。
    QString clientMsgId;    // 客户端生成，用于去重 / ACK 配对
    qint64  senderUid = 0;
    QString text;               // msgType==MsgTypeText 时的正文；附件消息可为空
    QString fileId;             // msgType!=MsgTypeText 时有效
    QString fileName;           // 附件显示名（服务端清洗后）
    qint64  fileSize  = 0;      // 附件大小，用于气泡直接显示，避免额外查询
    qint64  ts        = 0;
    QString fromNick;
    bool isPrivate() const { return convType == Proto::ConvPrivate; }
};

// 好友 / 搜索结果的最小用户信息
struct SearchResult {
    qint64  uid    = 0;
    qint64  accountNo = 0;
    QString nick;
    QString avatarFileId;
    bool    online = false;
};

struct SysMessage {
    QString type;
    QString text;
    QList<UserEntry> users;
    QVariantMap nickMap;    // uid -> nickname
    qint64  uid = 0;
    qint64  accountNo = 0;
    QString nick;           // 注册默认 = username，改昵称禁止空
    QString name;
    QString avatarFileId;
    int     seq  = 0;       // sys_ping / sys_pong 心跳配对序号

    // 好友
    QList<SearchResult> friends;   // 登录快照
    QList<SearchResult> searchUsers;
    qint64  reqId      = 0;
    qint64  fromUid    = 0;
    QString fromNick;
    qint64  handledReqId = 0;
    qint64  ts = 0;
    bool    online     = false;
    bool    ok         = false;
};

struct ChatAck {
    quint8  convType    = Proto::ConvPrivate;
    qint64  convId      = 0;
    QString msgId;
    QString clientMsgId;
    qint64  serverTs   = 0;
    bool    ok          = false;
    bool    pending     = false;
    int     errcode     = 0;
    QString text;
};


// 好友申请, 双向申请不会自动成为好友，仍要一方点"同意"。
struct FriendRequestInfo {
    qint64  reqId      = 0;     // friend_requests.id
    qint64  fromUid    = 0;     // 申请人 uid
    QString fromNick;           // 申请人昵称
    QString text;               // 验证信息（≤32字）
    qint64  createdAt  = 0;     // 服务端 friend_requests.created_at，毫秒
};


struct NicknameChangeResult {
    bool    ok = false;
    QString nick;
    QString text;
};

struct PasswordChangeResult {
    bool    ok = false;
    int     errcode = 0;
    QString text;
};

// 一页历史消息: 服务端返回时 messages 已按时间正序（旧→新），客户端 prepend 到会话头部
struct HistoryPage {
    bool ok = false;
    quint8  convType = Proto::ConvPrivate;
    qint64  convId   = 0;           // peerUid
    QList<ChatMessage> messages;    // 按 ts 正序（旧→新）
    bool    hasMore = false;        // 是否还有更早的一页
    QString text;
};

struct RecentChatItem {
    quint8  lastMsgType  = Proto::MsgTypeText;
    quint8  convType = Proto::ConvPrivate;
    qint64  convId   = 0;
    QString peerNick;
    QString lastMsgId;
    qint64  lastMsgTs = 0;
    QString lastMsgText;    // 预览文案由服务端统一生成（含 [图片]/[文件] xxx.pdf）
    int     unreadCount = 0;
};

struct RecentChatResult {
    bool    ok = false;
    QString text;
    QList<RecentChatItem> items;
};

struct FileNotice {
    QString type;
    QString fileId;
    QString sender, receiver;
    QString fileName;
    qint64  fileSize = 0;
    int     seq      = -1;
    QString reason;
    QString md5;
};


// 文件元数据：控制面所有 *_ok 里下发的只读快照
struct FileMeta {
    QString fileId;         // 服务端全局 UUID
    QString sha256;         // 内容寻址键（十六进制小写）
    QString fileName;       // 显示名（服务端已清洗）
    QString mime;           // 文件内容格式的标准标识
    qint64  size      = 0;
    qint64  ownerUid  = 0;
    quint8  biz       = 0;  // FileBizType 文件的"用途标签"
    quint8  status    = 0;  // FileStatus
    qint64  createdAt = 0;
};

// 文件控制面消息的统一载体（按 type 解释字段，字段语义见上方注释）
struct FileMessage {
    QString type;

    // 通用
    quint32 transferId = 0;   // 服务端下发；0 = 尚未分配
    QString fileId;

    // 请求侧字段（客户端 → 服务端）
    QString fileName;
    qint64  fileSize   = 0;   // upload_begin：声明大小（服务端仅作初筛，以实收为准）
    QString sha256;           // upload_begin：客户端预告值；upload_end：最终值
    QString mime;             // 文件内容格式的标准标识
    quint8  biz        = 0;   // FileBizType 文件的"用途标签", 头像 附件 视频...
    qint64  offset     = 0;   // begin_ok：服务端已收字节 / 下载起点
    qint64  length     = -1;  // 下载请求长度，-1 = 到文件末尾

    // 应答侧字段 （服务端 → 客户端）
    int     chunkSize  = 0;
    qint64  totalSize  = 0;   // download_begin_ok：文件总大小
    qint64  expireAt   = 0;   // 上传会话过期时间（毫秒时间戳） 暂时未用到

    // 结果字段（所有应答共用）
    bool    ok         = false;
    int     errcode    = 0;
    QString text;

    FileMeta meta;            // 服务端下发的文件权威元数据
};

// 0x02 数据面（线上格式：transferId + offset + dataLen + 裸二进制）
struct FileChunk {
    quint32    transferId = 0;
    quint64    offset     = 0;
    QByteArray data;
};

// 传输上下文：跨线程只传值拷贝，禁止跨线程持有指针
struct TransferCtx {
    quint32 transferId = 0;       // 由服务端在 *_begin_ok 时分配, 是 0x02 数据面唯一的身份牌
    quint8  dir        = Proto::TransferUpload;
    quint8  biz        = 0;
    qint64  uid        = 0;
    QString fileId;
    QString sha256;
    qint64  declaredSize = 0;     // 上传：声明大小
    qint64  totalSize    = 0;     // 下载：文件总大小
    qint64  received     = 0;     // 上传：已收字节 / 下载：已发字节
    qint64  readOffset   = 0;     // 下载：读游标
    qint64  readLength   = -1;    // 下载：本次请求长度
    int     shard        = 0;     // 固定分片线程处理：读按 transferId、写按 sha256
    qint64  createdAt    = 0;
    qint64  lastActiveAt = 0;
    bool    aborted      = false;
};

// 存储线程 → 网络线程 的处理回执（成败 + 进度）
struct ChunkResult {
    quint32 transferId = 0;
    int     shard  = 0;
    bool    ok     = true;
    int     errcode = 0;      // 0 = 成功；否则 Err::Code
    qint64  offset = 0;
    int     bytes  = 0;
    qint64  received = 0;     // 处理后的累计字节
};

// 进度（主线程节流后刷 UI，走信号不落协议）
struct TransferProgress {
    quint32 transferId = 0;
    QString fileId;
    qint64  done = 0;
    qint64  total = 0;
    qint64  bytesPerSec = 0;
    int     percent = 0;
};


// 让 Qt 认识自定义类型, 跨线程信号槽传递自定义类型必须使用它, 同时需要 qRegisterMetaType 运行时注册
Q_DECLARE_METATYPE(ChatMessage)
Q_DECLARE_METATYPE(ChatAck)
Q_DECLARE_METATYPE(SysMessage)
Q_DECLARE_METATYPE(NicknameChangeResult)
Q_DECLARE_METATYPE(PasswordChangeResult)

Q_DECLARE_METATYPE(UserEntry)
Q_DECLARE_METATYPE(QList<UserEntry>)
Q_DECLARE_METATYPE(SearchResult)
Q_DECLARE_METATYPE(QList<SearchResult>)
Q_DECLARE_METATYPE(FriendRequestInfo)
Q_DECLARE_METATYPE(QVector<FriendRequestInfo>)
Q_DECLARE_METATYPE(HistoryPage)
Q_DECLARE_METATYPE(QList<ChatMessage>)

Q_DECLARE_METATYPE(RecentChatItem)
Q_DECLARE_METATYPE(RecentChatResult)
Q_DECLARE_METATYPE(QList<RecentChatItem>)

Q_DECLARE_METATYPE(FileMeta)
Q_DECLARE_METATYPE(FileMessage)
Q_DECLARE_METATYPE(FileChunk)
Q_DECLARE_METATYPE(TransferCtx)
Q_DECLARE_METATYPE(ChunkResult)
Q_DECLARE_METATYPE(TransferProgress)


#endif // PROTOCOLTYPES_H
