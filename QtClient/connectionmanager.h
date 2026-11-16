#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include "protocoltypes.h"
#include "filetransfertypes.h"
#include <QHash>
#include <QObject>
#include <QThread>
#include <QTimer>

class NetworkWorker;    // 前置声明
class FileIoWorker;
class FileTransferManager;

class ConnectionManager : public QObject
{
    Q_OBJECT
public:
    // 单例模式获取: 同一个对象，共享同一个 Socket，状态完全统一
    static ConnectionManager &instance();

    static void shutdown();

    enum AuthMode { ModeLogin, ModeRegister };

    // 网络 API
    void connectToServer(const QString &ip, quint16 port, const QString &user, const QString &hash,AuthMode mode);
    void clientLogin(const QString &ip, quint16 port, const QString &userName,const QString &pwdHash);
    void clientRegister(const QString &ip, quint16 port, const QString &userName, const QString &pwdHash);
    void changePassword(const QString &oldPassWordHash, const QString &newPassWordHash);
    void disconnectFromServer();

    QString sendChatMessage(const QString &receiver, const QString &text, const QString &clientMsgId);
    bool sendAttachmentMessage(const ChatMessage &msg);
    void changeNickname(const QString &newNick);
    QString nicknameOfUid(const QString &uid) const;

    void changeAvatar(const QString &fileId);
    QString currentAvatarFileId() const { return m_myAvatarFileId; }

    void searchUser(qint64 accountNo);
    void sendFriendRequest(qint64 accountNo, const QString &text);
    void onHandleFriendRequest(qint64 reqId, bool accept);
    QVector<FriendRequestInfo> pendingFriendRequests() const;
    void removePendingRequestsFromUid(qint64 uid);
    void removeFriend(qint64 removeUid);

    void requestHistoryMsg(const QString &peerUid, const QString &beforeMsgId, int limit);
    void markRecentChatRead(qint64 peerUid);

    quint32 startFileUpload(const QString &localPath, const QString &fileName, const QString &mime, quint8 biz);
    quint32 downloadFile(const QString &fileId, const QString &savePath, TransferPriority priority = TransferPriority::User);

    // 获取当前状态
    bool isLoggedIn() const { return m_loggedIn;}
    QString currentUser() const {return m_userName;}
    qint64 currentUid() const { return m_myUid; }
    qint64 currentAccountNo() const { return m_myAccountNo; }


signals:
    // NetworkWorker UI 信号
    void registerSuccess(qint64 accountNo, const QString &name);
    void registerFailed(const QString &reason);
    void loginSuccess();
    void loginFailed(const QString &reason);
    void loginTimeOut(const QString &reason);
    void connectionLost();
    void connecting();
    void errorOccurred(const QString &error);
    // 消息
    void serverMessage(const QString &msg);     // 服务器主动推送的消息（公告、系统广播)
    void clientMessage(const ChatMessage &msg); // 其他客户端发来的聊天消息
    void systemMessage(const QString &msg);     // 本地系统提示（客户端自己产生的通知）
    void userListUpdated(const QList<UserEntry> &users, const QVariantMap &nickMap);

    void chatMessageAcked(const ChatAck &ack);

    // 昵称
    void nicknameChanged(const QString &uid, const QString &nick);
    void nicknameChangeResult(const NicknameChangeResult &msg);
    // 密码
    void passwordChangeResult(const PasswordChangeResult &msg);
    // 好友
    void userSearchResult(qint64 accountNo, const QString &nick, bool ok, const QString &text);
    void friendRequestResult(bool ok, const QString &text);
    void friendRequestReceived(const FriendRequestInfo &req);
    void friendRequestHandled(qint64 reqId, bool ok, const QString &text);
    void friendsFirstUpdate(const QList<SearchResult> &friends);
    void friendAdded(qint64 uid, qint64 accountNo, const QString &nick, bool online, QString avatarFileId);
    void friendRemoved(qint64 uid);
    void friendsOnlineUpdate(qint64 uid, bool online);
    void friendRemoveResult(bool ok, const QString &text, qint64 removeUid);
    void friendAvatarChanged(qint64 uid, const QString &avatarFileId);

    void historyMsgLoaded(const HistoryPage &page);
    void recentChatReceived(const RecentChatResult &msg);

    // 头像设置结果
    void avatarUpdateResult(bool ok, const QString &text, const QString &avatarFileId);
    // 文件传输
    void fileMessageReceived(const FileMessage &msg);
    void fileChunkReceived(const FileChunk &chunk);
    // 文件传输结果
    void uploadFinished(quint32 taskId, const QString &fileId, const FileMeta &meta);
    void uploadFailed(quint32 taskId, int errcode, const QString &text);
    void uploadProgress(quint32 taskId, qint64 sentBytes, qint64 totalBytes, qint64 elapsedMs);
    void downloadProgress(quint32 taskId, qint64 receivedBytes, qint64 totalBytes, qint64 elapsedMs);
    void downloadFinished(quint32 taskId, const QString &localPath, qint64 size);
    void downloadFailed(quint32 taskId, int errcode, const QString &text);

    // 信号在 Qt 中始终是 public 公开的, 信号无法设为私有，约定命名前缀 req 表示内部信号，用于区分。
    // 内部控制信号, 调用 NetworkWorker
    void reqConnectToServer(const QString &ip, quint16 port);
    void reqDisconnectFromServer();
    void reqSendJson(const QJsonObject &obj);
    void reqSendFileMessage(const FileMessage &msg);

private slots:
    // 可以被 Qt信号系统调用, 只能用于槽函数(接收信号), 不能用于信号(向外通知), 同时保持外部不可直接调用
    // NetworkWorker
    void onConnected();
    void onDisconnected();
    void onNetworkError(const QString &error);
    void onConnectionTimeout();

    void onChatMessageReceived(const ChatMessage &msg);
    void onSysMessageReceived(const SysMessage &msg);
    void onHistoryMsgReceived(const HistoryPage &msg);
    void onRecentChatReceived(const RecentChatResult &msg);
    // 心跳
    void onHeartbeatTick();


private:
    // 心跳
    void startHeartbeat();
    void stopHeartbeat();
    void sendHeartbeatPing();

    void resetSession(bool notifyLost);
    void teardownThreads();

private:
    explicit ConnectionManager(QObject *parent = nullptr);  // 把构造函数藏起来
    ~ConnectionManager();
    ConnectionManager(const ConnectionManager&) = delete;   // 禁止复制
    ConnectionManager& operator=(const ConnectionManager&) = delete;    // 禁止赋值

private:
    // NetworkWorker
    QThread *m_netWorkThread = nullptr;
    NetworkWorker *m_netWorker = nullptr;
    QTimer *m_loginTimer = nullptr;

    AuthMode m_authMode = ModeLogin;

    QString m_serverIP;
    quint16 m_serverPort;
    QString m_userName;
    QString m_pwdHash;
    bool m_loggedIn;
    bool m_connecting;  // TCP 连接进行中, 不是"已连接"

    qint64 m_myUid = 0;      // 当前登录用户的 uid
    qint64 m_myAccountNo = 0;      // 当前登录用户的 8位账号
    QVariantMap m_nickMap;   // uid -> nickname 昵称
    QString m_myAvatarFileId;

    QHash<qint64, FriendRequestInfo> m_pendingFriendReqs;

    bool m_shutdownDone = false;

    //心跳
    QTimer *m_hbTimer = nullptr;    // 30s 探测节拍
    int     m_hbSeq = 0;            // ping 序号
    int     m_lastPingSeq = 0;
    bool    m_pingInFlight = false; // 是否有 ping 在飞
    int     m_missedPongCount = 0;  // 连续未应答次数
    qint64  m_pingSentAtMs = 0;     // 临时观测：ping 发出时刻，用来算 RTT

private:
    // 文件传输：语义状态机在 GUI 线程，磁盘 IO 单独一个线程
    FileTransferManager *m_fileTransfer = nullptr;
    QThread             *m_fileIoThread = nullptr;
    FileIoWorker        *m_fileIoWorker = nullptr;

};

#endif // CONNECTIONMANAGER_H
