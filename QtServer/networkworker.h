#ifndef NETWORKWORKER_H
#define NETWORKWORKER_H

#include "mysqluserstore.h"
#include "protocoltypes.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>
#include <QQueue>

// 落盘交给存储分片，落库交给 DBWorker

class IUserStore;   // 前置声明

class NetworkWorker : public QObject
{
    Q_OBJECT

public:
    explicit NetworkWorker(QObject *parent = nullptr);
    ~NetworkWorker();

public slots:
    void startServerListen(unsigned short port);
    void stopServerListen();
    void cleanup();
    void onDbReady(bool ok);
    void onDbAuthResult(quint64 sessionId, int op, const QString &username, bool ok, const QString &text,
                    qint64 uid, qint64 accountNo, const QString &nickname, const QString &avatarFileId);
    void onNicknameUpdateResult(quint64 sessionId, const QString &username,
                                    bool ok, const QString &newNick, const QString &text);
    void onDbAvatarUpdateResult(quint64 sessionId, bool ok, int errcode, const QString &avatarFileId);
    void onDbPasswordChangeResult(quint64 sessionId, bool ok, int errcode);

    void onMessageSaved(quint64 sessionId, const ChatMessage &msg,bool ok, const QString &reason);
    void onOfflineMsgLoaded(quint64 sessionId, const QVector<ChatMessage> &msgs);
    void onDbHistoryMsgLoaded(quint64 sessionId, const HistoryPage &page);

    void onDbUserSearchResult(quint64 sessionId, bool ok, const QString &text, const SearchResult &user);
    void onDbFriendRequestResult(quint64 sessionId, bool ok, const QString &text, qint64 toUid);
    void onPendingRequestsResult(quint64 sessionId, const QVector<FriendRequestInfo> &reqs);
    void onListFriendsResult(quint64 sessionId, const QVector<SearchResult> &friends);
    void onDbFriendHandleResult(quint64 sessionId, const MysqlUserStore::HandleFriendResult &r);
    void onDbRemoveFriendResult(quint64 sessionId, const MysqlUserStore::RemoveFriendResult &r);

    void onDbRecentChatLoaded(quint64 sessionId, const RecentChatResult &r);

    // FileService / 存储分片 -> 网络线程
    void onUploadBeginOk(quint64 sessionId, quint32 transferId, int shard,
                             const QString &fileId, qint64 existingOffset, qint64 totalSize);
    void onUploadEndOk(quint64 sessionId, quint32 transferId, const FileMeta &meta);
    void onDownloadBeginOk(quint64 sessionId, quint32 transferId, int shard,
                           const FileMeta &meta, qint64 startOffset, qint64 length);
    void onUploadBeginFailed(quint64 sessionId, quint32 transferId, int errcode);
    void onUploadEndFailed(quint64 sessionId, quint32 transferId, int errcode);
    void onDownloadBeginFailed(quint64 sessionId, quint32 transferId, int errcode);
    void onDownloadFailed(quint64 sessionId, quint32 transferId, int errcode);
    void onFileDeleted(quint64 sessionId, const QString &fileId, int errcode);
    void onChunkRead(const FileChunk &chunk, bool eof);
    void onChunkWritten(quint32 transferId, qint64 bytes);   // 存储线程写完一块：解除接收反压

private slots:
    void onAuthTimeout();
    void onHeartbeatTimeout();

signals:
    void serverListenOk(QString msg);
    void serverListenFail(QString msg);
    void logMessage(QString msg);
    void serverListenStopped(bool);

    // Worker 直连 DBWorker
    void reqDbAuthLogin(quint64 sessionId, const QString &user, const QString &clientHash);
    void reqDbAuthRegister(quint64 sessionId, const QString &user, const QString &clientHash);
    void reqDbSaveMessage(quint64 sessionId, const ChatMessage &msg);
    void reqDbLoadOfflineMsg(quint64 sessionId, qint64 uid);
    void reqDbMarkDelivered(const QStringList &msgIds);
    void reqDbLoadHistoryMsg(quint64 sessionId, qint64 me, qint64 peerUid, const QString &beforeMsgId, int limit);

    void reqDbUpdateNickname(quint64 sessionId, const QString &username, const QString &newNick);
    void reqDbChangePassword(quint64 sessionId, qint64 uid, const QString &oldPassWordHash, const QString &newPassWordHash);
    void reqDbUpdateAvatar(quint64 sessionId, qint64 uid, const QString &fileId);
    void reqDbUpdateLastActiveTime(qint64 uid, qint64 ts);

    void reqDbSearchUser(quint64 sessionId, qint64 accountNo);
    void reqDbSendAddFriendRequest(quint64 sessionId, qint64 fromUid, qint64 toAccountNo, const QString &text);
    void reqDbListPendingRequests(quint64 sessionId, qint64 uid);
    void reqDbListFriends(quint64 sessionId, qint64 uid);
    void reqDbHandleFriendRequest(quint64 sessionId, qint64 reqId, qint64 selfUid, bool accept);
    void reqDbRemoveFriend(quint64 sessionId, qint64 uid, qint64 removeUid);

    void reqDbListRecentChat(quint64 sessionId, qint64 uid);
    void reqDbMarkRecentChatRead(qint64 uid, qint64 peerUid);

    // 在线用户集合变化，供 ConnectionManager 缓存
    void onlineUsersChanged(const QSet<QString> &users);

    // 网络线程 -> FileService
    void reqBeginUpload(quint64 sessionId, qint64 uid, const FileMessage &req);
    void reqFinishUpload(quint64 sessionId, quint32 transferId, const QString &finalSha256);
    void reqAbortUpload(quint64 sessionId, quint32 transferId);
    void reqAbortDownload(quint64 sessionId, quint32 transferId);
    void reqBeginDownload(quint64 sessionId, qint64 uid, const FileMessage &req);
    void reqDownloadSentComplete(quint32 transferId);
    void reqDeleteFile(quint64 sessionId, qint64 uid, const QString &fileId);
    void reqCloseSession(quint64 sessionId);

    // 网络线程 -> 存储分片
    void reqWriteChunk(int shard, quint32 transferId, quint64 offset, const QByteArray &data);
    void reqReadChunk(int shard, quint32 transferId, qint64 offset, int maxLen);

private:
    struct OutFrame {
        quint32    transferId = 0;   // 0 = 非数据帧
        QByteArray frame;
    };

    struct NetTransfer {
        quint32 transferId     = 0;
        quint8  dir            = Proto::TransferUpload;
        int     shard          = 0;
        qint64  nextReadOffset = 0;
        qint64  readEnd        = 0;
        bool    eof            = false;
        int     inFlight       = 0;   // 下载：已请求未写出的块数（≤1）
        qint64  pendingWriteBytes = 0;  // 上传：已投给存储线程、还没收到 chunkWritten 的字节数
    };

    struct ClientSession {
        quint64     id = 0;          // 唯一 id
        QTcpSocket  *sock = nullptr;
        QByteArray  buffer;
        qint64      uid = 0;         // 用户 uid
        qint64      accountNo = 0;   // 用户 8位账号
        QString     userName;        // 登录名空: 未认证
        QString     nickname;        // 用户的昵称
        int         authOp = -1;     // 哨兵值: -1 无认证请求在飞, 0 登录中, 1 注册中
        qint64      authExpireAt = 0;// 到期时间
        qint64      lastReceiveTime = 0;  // 最后一次收到完整帧的毫秒时间

        QSet<qint64> friendUids;
        int          friendCount = 0;
        bool         friendListReady = false;

        QHash<quint32, NetTransfer> Netransfers; // 会话内记录
        QQueue<QByteArray> outCtrl;      // 控制帧：永远优先
        QQueue<OutFrame>   outData;      // 数据帧：低水位放行
        qint64             outDataBytes = 0;
    };



    void onNewConnection();
    void onReadyRead(ClientSession *s);
    void onSessionDisconnected(ClientSession *s);

    bool requireAuth(ClientSession *s, const char *what);

    void handleRegister(ClientSession *s, const QJsonObject &obj);
    void handleLogin(ClientSession *newC, const QJsonObject &obj);
    void handleChat(ClientSession *s, const QJsonObject &obj);
    void handleNicknameChange(ClientSession *s, const QJsonObject &obj);
    void handlePasswordChange(ClientSession *s, const QJsonObject &obj);
    void handleHistoryMsgRequest(ClientSession *s, const QJsonObject &obj);

    void handleUserSearch(ClientSession *s, const QJsonObject &obj);
    void handleFriendReqAdd(ClientSession *s, const QJsonObject &obj);
    void handleFriendHandle(ClientSession *s, const QJsonObject &obj);
    void handleFriendRemove(ClientSession *s, const QJsonObject &obj);
    void handleAvatarUpdate(ClientSession *s, const QJsonObject &obj);

    void handleRecentChatMarkRead(ClientSession *s, const QJsonObject &obj);

    // 文件
    void handleFileMsg(ClientSession *s, const FileMessage &f);
    void handleUploadChunk(ClientSession *s, const FileChunk &chunk);

    void flushOut(ClientSession *s);
    void sendChunkTo(ClientSession *s, quint32 transferId, const FileChunk &c);
    void maybeRequestNext(ClientSession *s, quint32 transferId);
    void registerTransfer(ClientSession *s, const NetTransfer &t);
    void releaseTransfer(ClientSession *s, quint32 transferId);

    bool sendJsonTo(ClientSession *s, const QJsonObject &obj);
    void setServerOnlineUserList();
    void broadcastJson(const QJsonObject &obj);

    bool sendPong(ClientSession *s, int seq);

    ClientSession *findSessionById(quint64 id) const;

    void destroySession(ClientSession *s);
    void closeAllSessions();
    void teardown();

private:
    QTcpServer *m_tcpServer = nullptr;
    quint64 m_nextSessionId = 1;
    QHash<quint64, ClientSession*>m_sessions;
    QHash<qint64, ClientSession*> m_onlineUidToSession; // m_onlineUidToSession 和 m_sessions 严格同步

    bool m_dbReady = false;
    QTimer *m_authTimer = nullptr;

    // transferId -> sessionId；下载块回到网络线程时用来找会话
    QHash<quint32, quint64> m_transferOwner;    // 全局归属索引
};

#endif // NETWORKWORKER_H
