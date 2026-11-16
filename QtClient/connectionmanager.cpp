#include "connectionmanager.h"
#include "networkworker.h"
#include "userinfodialog.h"
#include "filetransfermanager.h"
#include "fileioworker.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonObject>
#include <QPair>
#include <QDebug>

static constexpr bool kDebugRtt = false;   // 复测时临时改 true

namespace {
    constexpr int kThreadWaitMs = 3000;

    PasswordChangeResult makePasswordResult(int errcode)
    {
        PasswordChangeResult r;
        r.ok      = (errcode == Err::Ok);
        r.errcode = errcode;
        r.text    = r.ok ? QString() : Err::friendlyText(errcode);
        return r;
    }
}

ConnectionManager::ConnectionManager(QObject *parent)
    : QObject{parent}, m_loggedIn(false), m_connecting(false)
{
    // 跨线程注册
    qRegisterMetaType<ChatMessage>("ChatMessage");
    qRegisterMetaType<ChatAck>("ChatAck");
    qRegisterMetaType<FileMessage>("FileMessage");
    qRegisterMetaType<FileChunk>("FileChunk");
    qRegisterMetaType<SysMessage>("SysMessage");
    qRegisterMetaType<NicknameChangeResult>("NicknameChangeResult");
    qRegisterMetaType<PasswordChangeResult>("PasswordChangeResult");
    qRegisterMetaType<QVariantMap>("QVariantMap");
    qRegisterMetaType<UserEntry>("UserEntry");
    qRegisterMetaType<QList<UserEntry>>("QList<UserEntry>");
    qRegisterMetaType<FriendRequestInfo>("FriendRequestInfo");
    qRegisterMetaType<HistoryPage>("HistoryPage");
    qRegisterMetaType<QList<ChatMessage>>("QList<ChatMessage>");
    qRegisterMetaType<RecentChatResult>("RecentChatResult");


    // NetworkWorker 子线程
    m_netWorkThread = new QThread(this);
    m_netWorker = new NetworkWorker;
    m_netWorker->moveToThread(m_netWorkThread);
    connect(m_netWorkThread, &QThread::started, m_netWorker,&NetworkWorker::init);
    connect(m_netWorkThread, &QThread::finished, m_netWorker, &QObject::deleteLater);

    // 登录超时
    m_loginTimer = new QTimer(this);
    m_loginTimer->setSingleShot(true);    // 单次触发模式, 自动停止
    m_loginTimer->setInterval(Proto::kLOGIN_TIMEOUT_Ms);
    connect(m_loginTimer, &QTimer::timeout, this, &ConnectionManager::onConnectionTimeout);

    // 心跳
    m_hbTimer = new QTimer(this);
    m_hbTimer->setInterval(Proto::kHeartbeatIntervalMs); // 30s
    connect(m_hbTimer, &QTimer::timeout, this, &ConnectionManager::onHeartbeatTick);

    // NetworkWorker 信号监听
    connect(m_netWorker, &NetworkWorker::connected, this, &ConnectionManager::onConnected);
    connect(m_netWorker, &NetworkWorker::disconnected, this, &ConnectionManager::onDisconnected);
    connect(m_netWorker, &NetworkWorker::errorOccurred, this, &ConnectionManager::onNetworkError);

    connect(m_netWorker, &NetworkWorker::chatAckReceived, this, [this](const ChatAck &a){
        const QString uidStr = QString::number(a.convId);
        QString nickname = nicknameOfUid(uidStr);
        if (!a.ok)
            emit systemMessage(QString("[消息未送达·%1] %2").arg(nickname, a.text));
        else if(a.pending)
            emit systemMessage(QString("[%1] %2").arg(nickname, a.text));
        emit chatMessageAcked(a);
    });
    connect(m_netWorker, &NetworkWorker::chatMessageReceived, this, &ConnectionManager::onChatMessageReceived);
    connect(m_netWorker, &NetworkWorker::sysMessageReceived, this, &ConnectionManager::onSysMessageReceived);
    connect(m_netWorker, &NetworkWorker::historyMsgReceived, this, &ConnectionManager::onHistoryMsgReceived);
    connect(m_netWorker, &NetworkWorker::recentChatReceived, this, &ConnectionManager::onRecentChatReceived);

    connect(this, &ConnectionManager::reqSendJson, m_netWorker,&NetworkWorker::sendJson);
    // 用户修改昵称
    connect(m_netWorker, &NetworkWorker::nicknameChangeResult, this, [this](const NicknameChangeResult &msg) {
        emit nicknameChangeResult(msg);
    });

    // 修改密码回执
    connect(m_netWorker, &NetworkWorker::passwordChangeResult, this, [this](const PasswordChangeResult &msg) {
        if (msg.ok)
            m_pwdHash.clear();   // 旧凭据已作废，内存里不再留密码哈希
        emit passwordChangeResult(msg);
    });

    // 内部网络路由: 请求连接、断开连接
    connect(this, &ConnectionManager::reqConnectToServer, m_netWorker,&NetworkWorker::connectToServer);
    connect(this, &ConnectionManager::reqDisconnectFromServer, m_netWorker,&NetworkWorker::disconnectFromServer);


    // ---------- 文件传输：磁盘 IO 线程 + 语义状态机 ----------
    m_fileIoThread = new QThread(this);
    m_fileIoWorker = new FileIoWorker;
    m_fileIoWorker->moveToThread(m_fileIoThread);
    connect(m_fileIoThread, &QThread::finished, m_fileIoWorker, &QObject::deleteLater);

    m_fileTransfer = new FileTransferManager(this);
    m_fileTransfer->attach(m_fileIoWorker);

    connect(m_fileTransfer, &FileTransferManager::reqSendFileMessage, m_netWorker, &NetworkWorker::sendFileMessage);
    connect(m_fileTransfer, &FileTransferManager::reqSendFileChunk,   m_netWorker, &NetworkWorker::sendFileChunk);
    connect(m_netWorker,    &NetworkWorker::fileMessageReceived, m_fileTransfer, &FileTransferManager::onFileMessage);
    connect(m_fileTransfer, &FileTransferManager::uploadFinished, this, &ConnectionManager::uploadFinished);
    connect(m_fileTransfer, &FileTransferManager::uploadFailed,   this, &ConnectionManager::uploadFailed);

    connect(m_netWorker, &NetworkWorker::fileChunkReceived, m_fileTransfer, &FileTransferManager::onDownloadChunkReceived);
    connect(m_fileTransfer, &FileTransferManager::uploadProgress,   this, &ConnectionManager::uploadProgress);
    connect(m_fileTransfer, &FileTransferManager::downloadProgress, this, &ConnectionManager::downloadProgress);
    connect(m_fileTransfer, &FileTransferManager::downloadFinished, this, &ConnectionManager::downloadFinished);
    connect(m_fileTransfer, &FileTransferManager::downloadFailed,   this, &ConnectionManager::downloadFailed);

    m_fileIoThread->start();
    m_netWorkThread->start();
}

ConnectionManager::~ConnectionManager()
{
    teardownThreads();
}

static ConnectionManager *g_instance = nullptr;

ConnectionManager &ConnectionManager::instance()
{
    if (!g_instance)
        g_instance = new ConnectionManager;
    return *g_instance;
}

void ConnectionManager::shutdown()
{
    if (!g_instance)
        return;
    ConnectionManager *inst = g_instance;
    g_instance = nullptr;
    delete inst;
}

void ConnectionManager::teardownThreads()
{
    if (m_shutdownDone)
        return;

    m_shutdownDone = true;
    resetSession(false);

    if (m_netWorkThread && m_netWorkThread->isRunning()) {
        m_netWorkThread->quit();
        if (!m_netWorkThread->wait(kThreadWaitMs)) {
            qWarning() << "网络线程 3s 内未退出，强制终止（worker 可能泄漏）";
            m_netWorkThread->terminate();
            m_netWorkThread->wait();
        }
    }
    m_netWorker = nullptr;

    if (m_fileIoThread && m_fileIoThread->isRunning()) {
        m_fileIoThread->quit();
        if (!m_fileIoThread->wait(kThreadWaitMs)) {
            qWarning() << "文件 IO 线程 3s 内未退出，强制终止";
            m_fileIoThread->terminate();
            m_fileIoThread->wait();
        }
    }
    m_fileIoWorker = nullptr;
    m_fileTransfer = nullptr;
}

void ConnectionManager::resetSession(bool notifyLost)
{
    const bool wasLoggedIn = m_loggedIn;
    if (notifyLost && wasLoggedIn)
        emit connectionLost();

    stopHeartbeat();
    if (m_loginTimer)
        m_loginTimer->stop();
    if (m_hbTimer)
        m_hbTimer->stop();
    if (m_fileTransfer)
        m_fileTransfer->abortAll(QStringLiteral("与服务器的连接已断开"));

    m_loggedIn   = false;
    m_connecting = false;
    m_myUid      = 0;
    m_myAvatarFileId.clear();
    m_pendingFriendReqs.clear();

    // 凭据不跨会话保留在内存里
    m_userName.clear();
    m_pwdHash.clear();

}

void ConnectionManager::connectToServer(const QString &ip, quint16 port,
                                  const QString &user, const QString &hash, AuthMode mode)
{
    if (m_loggedIn || m_connecting)
        return;

    m_nickMap.clear(); // 丢弃上一次会话的缓存，在这里清除,是为了离线时好友修改后的nick不回退

    m_authMode  = mode;
    m_userName  = user;
    m_pwdHash   = hash;
    m_connecting = true;
    emit connecting();
    m_loginTimer->start();
    emit reqConnectToServer(ip, port);
}

void ConnectionManager::clientLogin(const QString &ip, quint16 port,
                                    const QString &userName,const QString &pwdHash)
{
    connectToServer(ip, port, userName, pwdHash, ModeLogin);
}

void ConnectionManager::clientRegister(const QString &ip, quint16 port,
                                    const QString &userName,const QString &pwdHash)
{
    connectToServer(ip, port, userName, pwdHash, ModeRegister);
}

void ConnectionManager::onConnected()
{
    m_loginTimer->stop();
    m_connecting = false;
    startHeartbeat();

    QJsonObject obj;
    QString type = (m_authMode == ModeLogin) ? Proto::kAuthLogin : Proto::kAuthRegister;
    obj.insert("type", type);
    obj.insert("username", m_userName);
    obj.insert("password_hash", m_pwdHash);
    emit reqSendJson(obj);
}

void ConnectionManager::disconnectFromServer()
{
    resetSession(true);
    emit reqDisconnectFromServer();
}

void ConnectionManager::onDisconnected()
{
    resetSession(true);
}

void ConnectionManager::onNetworkError(const QString &error)
{
    m_loginTimer->stop();
    if (m_loggedIn) {
        stopHeartbeat();
    } else {
        // 登录/注册阶段失败：没有会话可掉线，只清连接态，不发 connectionLost()
        resetSession(false);
    }
    emit errorOccurred(error);
}

void ConnectionManager::onConnectionTimeout()
{
    if(!m_loggedIn && m_connecting) {
        m_loginTimer->stop();
        m_connecting = false;
        emit reqDisconnectFromServer();
        emit loginTimeOut("服务器响应超时, 服务器可能已关闭");
    }
}

QString ConnectionManager::sendChatMessage(const QString &receiver, const QString &text,
                                           const QString &clientMsgId)
{
    if(!m_loggedIn || text.isEmpty() || receiver.isEmpty() || clientMsgId.isEmpty())
        return{};

    QJsonObject obj;
    obj.insert("type", Proto::kChatC2S);
    obj.insert("client_msg_id", clientMsgId);
    obj.insert("conv_type", Proto::ConvType::ConvPrivate);
    obj.insert("conv_id", receiver);
    obj.insert("text", text);
    obj.insert("ts", QDateTime::currentSecsSinceEpoch());
    emit reqSendJson(obj);
    return clientMsgId;
}

// 附件
bool ConnectionManager::sendAttachmentMessage(const ChatMessage &msg)
{
    if (!m_loggedIn || msg.convId <= 0 || msg.clientMsgId.isEmpty())
        return false;
    if (msg.msgType == Proto::MsgTypeText || msg.fileId.isEmpty())
        return false;

    QJsonObject obj;
    obj.insert("type", Proto::kChatC2S);
    obj.insert("client_msg_id", msg.clientMsgId);
    obj.insert("conv_type", Proto::ConvType::ConvPrivate);
    obj.insert("conv_id", QString::number(msg.convId));
    obj.insert("msg_type", int(msg.msgType));
    obj.insert("file_id", msg.fileId);
    obj.insert("file_name", msg.fileName);
    obj.insert("file_size", double(msg.fileSize));
    obj.insert("text", msg.text); // 空串也发
    obj.insert("ts", QDateTime::currentSecsSinceEpoch());
    emit reqSendJson(obj);
    return true;
}

void ConnectionManager::changePassword(const QString &oldPassWordHash, const QString &newPassWordHash)
{
    if (!m_loggedIn) {
        emit passwordChangeResult(makePasswordResult(Err::NotAuthenticated));
        return;
    }
    if (oldPassWordHash.size() != Proto::kSha256HexLen || newPassWordHash.size() != Proto::kSha256HexLen) {
        emit passwordChangeResult(makePasswordResult(Err::InvalidParam));
        return;
    }
    if (oldPassWordHash == newPassWordHash) {
        emit passwordChangeResult(makePasswordResult(Err::PasswordSameAsOld));
        return;
    }

    QJsonObject obj;
    obj.insert("type", Proto::kPasswordChange);
    obj.insert("old_password_hash", oldPassWordHash);
    obj.insert("new_password_hash", newPassWordHash);
    emit reqSendJson(obj);
}

void ConnectionManager::changeNickname(const QString &newNick)
{
    if (!m_loggedIn || newNick.isEmpty())
        return;
    QJsonObject obj;
    obj.insert("type", Proto::kNicknameChange);
    obj.insert("new_nick", newNick);
    emit reqSendJson(obj);
}

QString ConnectionManager::nicknameOfUid(const QString &uid) const
{
    return m_nickMap.value(uid).toString();
}

void ConnectionManager::changeAvatar(const QString &fileId)
{
    if (!m_loggedIn || fileId.isEmpty())
        return;
    QJsonObject obj;
    obj.insert("type", Proto::kAvatarUpdate);
    obj.insert("file_id", fileId);
    emit reqSendJson(obj);
}

void ConnectionManager::startHeartbeat()
{
    m_hbSeq = 0;
    m_missedPongCount = 0;
    m_pingInFlight = false;
    m_hbTimer->start();
}

void ConnectionManager::stopHeartbeat()
{
    m_hbTimer->stop();
    m_pingInFlight = false;
    m_missedPongCount = 0;
    m_lastPingSeq = 0;
    m_pingSentAtMs = 0;
}

void ConnectionManager::sendHeartbeatPing()
{
    QJsonObject obj;
    obj.insert("type", Proto::kSysPing);
    obj.insert("seq",  ++m_hbSeq);
    m_lastPingSeq = m_hbSeq;
    m_pingInFlight = true;
    m_pingSentAtMs = QDateTime::currentMSecsSinceEpoch();   // 临时观测
    emit reqSendJson(obj);
}

// 客户端发心跳包 间隔30秒
void ConnectionManager::onHeartbeatTick()
{
    if(!m_loggedIn)
        return;
    if(m_pingInFlight) {
        ++m_missedPongCount; // 有 ping 在飞：判单次超时，累计连续失败
        m_pingInFlight = false; // 复位
        if(m_missedPongCount >= Proto::kClientMaxMissedPong) {
            stopHeartbeat();
            emit systemMessage(QStringLiteral("与服务器的连接已断开, 请重新登录"));
            emit reqDisconnectFromServer();
            return;
        }
    }
    sendHeartbeatPing();
}

void ConnectionManager::searchUser(qint64 accountNo)
{
    if (!m_loggedIn || !Proto::isValidAccountNo(accountNo))
        return;
    QJsonObject obj;
    obj.insert("type", Proto::kUserSearch);
    obj.insert("account_no", QString::number(accountNo));
    emit reqSendJson(obj);
}

void ConnectionManager::sendFriendRequest(qint64 accountNo, const QString &text)
{
    if (!m_loggedIn)
        return;
    QJsonObject obj;
    obj.insert("type", Proto::kFriendReqAdd);
    obj.insert("account_no", QString::number(accountNo));
    obj.insert("text", text);
    emit reqSendJson(obj);
}

QVector<FriendRequestInfo> ConnectionManager::pendingFriendRequests() const
{
    // 按 createdAt 倒序，最新的在最上面
    QVector<FriendRequestInfo> v = m_pendingFriendReqs.values().toVector();
    std::sort(v.begin(), v.end(), [](const FriendRequestInfo &a, const FriendRequestInfo &b) {
        return a.createdAt > b.createdAt;
    });
    return v;
}

void ConnectionManager::onHandleFriendRequest(qint64 reqId, bool accept)
{
    if (!m_loggedIn || reqId <= 0)
        return;
    QJsonObject obj;
    obj.insert("type", Proto::kFriendHandle);
    obj.insert("req_id", QString::number(reqId));
    obj.insert("accept", accept);
    emit reqSendJson(obj);
}

void ConnectionManager::removePendingRequestsFromUid(qint64 uid)
{
    QList<qint64> toRemove;
    for (auto it = m_pendingFriendReqs.constBegin(); it != m_pendingFriendReqs.constEnd(); ++it) {
        if (it.value().fromUid == uid)
            toRemove.append(it.key());
    }
    for (qint64 k : toRemove)
        m_pendingFriendReqs.remove(k);
}

void ConnectionManager::removeFriend(qint64 removeUid)
{
    if (!m_loggedIn || removeUid <= 0)
        return;
    QJsonObject obj;
    obj.insert("type", Proto::kFriendRemove);
    obj.insert("remove_uid", QString::number(removeUid));
    emit reqSendJson(obj);
}

void ConnectionManager::requestHistoryMsg(const QString &peerUid, const QString &beforeMsgId, int limit)
{
    if(!m_loggedIn || peerUid.isEmpty())
        return;
    QJsonObject obj;
    obj.insert("type", Proto::kHistoryRequest);
    obj.insert("conv_id", peerUid);
    if(!beforeMsgId.isEmpty())
        obj.insert("before_msg_id", beforeMsgId);
    obj.insert("limit", limit > 0 ? limit : Proto::kHistoryPageSize);
    emit reqSendJson(obj);
}

void ConnectionManager::markRecentChatRead(qint64 peerUid)
{
    if (!m_loggedIn || peerUid <= 0)
        return;
    QJsonObject obj;
    obj.insert("type", Proto::kRecentChatMarkRead);
    obj.insert("conv_id", QString::number(peerUid));
    emit reqSendJson(obj);
}

quint32 ConnectionManager::startFileUpload(const QString &localPath, const QString &fileName,
                                           const QString &mime, quint8 biz)
{
    if (!m_loggedIn) {
        emit systemMessage(QStringLiteral("未登录，无法发送文件"));
        return 0;
    }
    if (!m_fileTransfer)
        return 0;
    return m_fileTransfer->startUpload(localPath, fileName, mime, biz);
}

quint32 ConnectionManager::downloadFile(const QString &fileId, const QString &savePath, TransferPriority priority)
{
    if (!m_loggedIn) {
        emit systemMessage(QStringLiteral("未登录，无法下载文件"));
        return 0;
    }
    if (!m_fileTransfer)
        return 0;
    return m_fileTransfer->startDownload(fileId, savePath, priority);
}

void ConnectionManager::onChatMessageReceived(const ChatMessage &msg)
{
    emit clientMessage(msg);
}

void ConnectionManager::onSysMessageReceived(const SysMessage &msg)
{
    if(msg.type == Proto::kSysPong) {
        if(m_pingInFlight && msg.seq == m_lastPingSeq) {
            m_pingInFlight = false;
            m_missedPongCount = 0;

            // 临时观测：下载/上传期间这条 RTT 就是出口队列的排队延迟
            if (kDebugRtt && m_pingSentAtMs > 0){
                qInfo() << "[RTT] ping-pong =" << (QDateTime::currentMSecsSinceEpoch() - m_pingSentAtMs) << "ms";
                m_pingSentAtMs = 0;
            }
        }
    }
    else if(msg.type == Proto::kAuthRegisterOk) {
        m_connecting = false;
        emit registerSuccess(msg.accountNo ,msg.name);
        disconnectFromServer();
    }
    else if (msg.type == Proto::kAuthRegisterFail) {
        m_connecting = false;
        emit registerFailed(msg.text);
        disconnectFromServer();
    }
    else if (msg.type == Proto::kAuthLoginOk) {
        m_loggedIn = true;
        m_connecting = false;
        m_myUid = msg.uid;
        m_myAccountNo = msg.accountNo;
        m_myAvatarFileId = msg.avatarFileId;
        m_nickMap[QString::number(m_myUid)] = msg.nick;
        emit loginSuccess();
    }
    else if(msg.type == Proto::kAuthLoginFail) {
        m_connecting = false;
        emit loginFailed(msg.text);
        disconnectFromServer();
    }
    else if(msg.type == Proto::kNicknameChanged) {
        if (msg.uid != 0) {
            m_nickMap[QString::number(msg.uid)] = msg.nick;
            emit nicknameChanged(QString::number(msg.uid), msg.nick);
        }
    }
    else if (msg.type == Proto::kUserSearchResult) {
        qint64 accountNo = msg.accountNo;
        QString nick;
        if(msg.ok && !msg.searchUsers.isEmpty()) {
            nick = msg.searchUsers.first().nick;
        }
        emit userSearchResult(accountNo, nick, msg.ok, msg.text);
    }
    else if(msg.type == Proto::kFriendReqAddResult) {
        emit friendRequestResult(msg.ok, msg.text);
    }
    else if (msg.type == Proto::kFriendPendingRequest) {
        FriendRequestInfo info;
        info.reqId      = msg.reqId;
        info.fromUid    = msg.fromUid;
        info.fromNick   = msg.fromNick;
        info.text       = msg.text;
        info.createdAt  = msg.ts;
        if(info.reqId > 0 && !m_pendingFriendReqs.contains(info.reqId)) {
            m_pendingFriendReqs.insert(info.reqId, info);
            emit friendRequestReceived(info);
        }
    }
    else if(msg.type == Proto::kFriendHandleResult) {
        if (msg.ok)
            m_pendingFriendReqs.remove(msg.handledReqId);
        emit friendRequestHandled(msg.handledReqId, msg.ok, msg.text);
    }
    else if (msg.type == Proto::kSysFriends) {
        for (const SearchResult &f : msg.friends)
            m_nickMap[QString::number(f.uid)] = f.nick;
        emit friendsFirstUpdate(msg.friends);
    }
    else if (msg.type == Proto::kFriendAdded) {
        if (msg.uid > 0)
            m_nickMap[QString::number(msg.uid)] = msg.nick;
        emit friendAdded(msg.uid, msg.accountNo, msg.nick, msg.online, msg.avatarFileId);
    }
    else if (msg.type == Proto::kFriendRemoveResult) {
        emit friendRemoveResult(msg.ok, msg.text, msg.uid);
    }
    else if (msg.type == Proto::kFriendRemoved) {
        emit friendRemoved(msg.uid);
    }
    else if (msg.type == Proto::kSysFriendsOnlineUpdate) {
        emit friendsOnlineUpdate(msg.uid, msg.online);
    }
    else if (msg.type == Proto::kAvatarUpdateResult) {
        emit avatarUpdateResult(msg.ok, msg.text, msg.avatarFileId);
    }
    else if (msg.type == Proto::kFriendAvatarChanged) {
        emit friendAvatarChanged(msg.uid, msg.avatarFileId);
    }
    else if(msg.type == Proto::kSysAnnounce)
        emit serverMessage(msg.text);
}

void ConnectionManager::onHistoryMsgReceived(const HistoryPage &msg)
{
    if(!msg.ok)
        emit systemMessage(msg.text.isEmpty() ? QStringLiteral("历史消息加载失败") : msg.text);
    emit historyMsgLoaded(msg);
}

void ConnectionManager::onRecentChatReceived(const RecentChatResult &msg)
{
    emit recentChatReceived(msg);
}
