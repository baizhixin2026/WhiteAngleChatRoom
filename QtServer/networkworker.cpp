#include "networkworker.h"
#include "protocolcodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QList>
#include <QTcpSocket>
#include <QUuid>
#include <algorithm>
#include <QDateTime>

namespace {

    // 出口数据队列上限（已编码、还没写进 socket 的帧）：上传/下载，每路传输 1 块
    // 只留 1 块会让第二路永远拿不到数据，60 秒后被判"下载空闲超时"（客户端看到 4002 传输已取消）。
    constexpr qint64 kOutDataQueueBytes = Proto::kFileChunkSize * Proto::kMaxTransfersPerSession;   // 会话最多 2 路 → 2 块

    // 下行水位：现在服务器带宽是3Mbps，链路上 48KB ≈ 0.13 秒排队。 根据服务器的出口带宽调整下行水位值
    constexpr qint64 kDownloadSendWaterBytes = 48 * 1024;

    // 队列硬上限：对端长期不收，主动断，避免服务端内存被慢客户端拖爆。
    constexpr qint64 kOutDataQueueAbortBytes = 4 * 1024 * 1024;

    // 接收侧两个水位。setReadBufferSize 是"总闸"：内部缓冲到量就不再从内核读，
    // TCP 窗口随之收缩，发送端自己减速 —— 这是唯一正确的反压手段。
    // 不设它（默认 0 = 不限），快客户端能在一次 readAll 里灌进来好几 MB，只能靠 abort 收场。
    constexpr int kSessionReadBuffer = 1024 * 1024;   // 单会话 socket 内部读缓冲上限
    constexpr int kReadGateBytes     = 512 * 1024;    // 接收缓冲还有这么多没消化，就先不读 socket

    // 应用层上传反压水位：已投给存储线程、但还没收到 chunkWritten 回执的字节 ≥ 2MB 时停止读 socket。
    // 每块 256KB，2MB = 8 块；配合"每轮最多消化 4 帧"，留给存储线程足够的时间落盘。
    constexpr qint64 kUploadPendingPauseBytes = 2 * 1024 * 1024;

    // 密码 64 位十六进制
    bool isSha256Hex(const QString &hash)
    {
        if (hash.size() != Proto::kSha256HexLen)
            return false;
        for (const QChar c : hash) {
            const ushort u = c.unicode();
            const bool digit = (u >= '0' && u <= '9');
            const bool lower = (u >= 'a' && u <= 'f');
            const bool upper = (u >= 'A' && u <= 'F');
            if (!digit && !lower && !upper)
                return false;
        }
        return true;
    }
}


NetworkWorker::NetworkWorker(QObject *parent)
    : QObject{parent}
{
    m_authTimer = new QTimer(this);
    m_authTimer->setInterval(Proto::kSessionReapSweepMs);
    m_authTimer->setSingleShot(false); // 周期性触发
    connect(m_authTimer, &QTimer::timeout, this, &NetworkWorker::onAuthTimeout);
    connect(m_authTimer, &QTimer::timeout, this, &NetworkWorker::onHeartbeatTimeout);
    m_authTimer->start();
}

NetworkWorker::~NetworkWorker()
{
    teardown();
}

// 资源回收总入口；幂等，cleanup() 与析构共用
void NetworkWorker::teardown()
{
    if(m_tcpServer)
        m_tcpServer->close();
    closeAllSessions();
    if(m_authTimer)
        m_authTimer->stop();
}

void NetworkWorker::closeAllSessions()
{
    if(m_sessions.isEmpty())
        return;
    const QList<ClientSession *> sessions = m_sessions.values();
    for(ClientSession *s : sessions) {
        if(s->uid > 0 && !s->userName.isEmpty())
            emit reqDbUpdateLastActiveTime(static_cast<qint64>(s->uid), s->lastReceiveTime);
        destroySession(s);
    }
}

void NetworkWorker::destroySession(ClientSession *s)
{
    if (!s)
        return;
    if(s->uid > 0)
        m_onlineUidToSession.remove(s->uid);
    m_sessions.remove(s->id);   // 任何重入路径,  都不可能再拿到这个指针， 避免野指针
    if(s->sock) {
        disconnect(s->sock, nullptr, this, nullptr);    // 先断信号
        s->sock->close();           // 立即断开，不等对端；未发完的数据被丢弃
        s->sock->deleteLater();     // 套接字生命周期交给事件循环（父对象是 m_tcpServer）
        s->sock = nullptr;
    }

    for (auto it = s->Netransfers.constBegin(); it != s->Netransfers.constEnd(); ++it)
        m_transferOwner.remove(it.key());
    s->Netransfers.clear();
    s->outCtrl.clear();
    s->outData.clear();
    s->outDataBytes = 0;
    delete s;
}

void NetworkWorker::startServerListen(unsigned short port)
{
    if(!m_tcpServer) {
        m_tcpServer = new QTcpServer(this);
        connect(m_tcpServer, &QTcpServer::newConnection, this, &NetworkWorker::onNewConnection);
    }
    if(m_tcpServer->isListening()) {
        emit serverListenFail(QStringLiteral("已在监听中，端口: %1").arg(QString::number(m_tcpServer->serverPort())));
        return;
    }
//    if(m_tcpServer->listen(QHostAddress::Any, port))
    if(m_tcpServer->listen(QHostAddress::AnyIPv4, port)) {
        emit serverListenOk(QStringLiteral("开始监听，端口: %1").arg(QString::number(m_tcpServer->serverPort())));
    }
    else {
        // 监听失败（端口被占用、权限不足等）
        emit serverListenFail(QStringLiteral("监听失败: %1").arg(m_tcpServer->errorString()));
    }
}

void NetworkWorker::stopServerListen()
{
    if(!m_tcpServer || !m_tcpServer->isListening()) {
        emit logMessage(QStringLiteral("当前未在监听，无需停止"));
        emit serverListenStopped(false);
        return;
    }
    m_tcpServer->close();

    // 断开 所有客户端
    const int closedCount = m_sessions.size();
    closeAllSessions();
    emit onlineUsersChanged({});    // 清空
    emit logMessage(QStringLiteral("已停止监听，断开 %1 个连接").arg(closedCount));
    emit serverListenStopped(true);
}

void NetworkWorker::cleanup()
{
    const int closedCount = m_sessions.size();
    teardown();
    emit logMessage(QStringLiteral("NetworkWorker::cleanup 清理断开 %1 个连接").arg(closedCount));
}

void NetworkWorker::onDbReady(bool ok)
{
    m_dbReady = ok;
}

void NetworkWorker::onNewConnection()
{
    QTcpSocket *sock = m_tcpServer ? m_tcpServer->nextPendingConnection() : nullptr;
    if(!sock)
        return;

    ClientSession *s = new ClientSession;
    s->id = m_nextSessionId++;
    s->sock = sock;
    s->lastReceiveTime = QDateTime::currentMSecsSinceEpoch();
    sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);   // 不合并小数据包, 立即发送, 心跳不延迟
    sock->setReadBufferSize(kSessionReadBuffer);                 // 接收侧反压总闸
    m_sessions.insert(s->id, s);
    emit logMessage(QStringLiteral("连接客户端: %1:%2 [sid=%3]")
                        .arg(sock->peerAddress().toString()).arg(sock->peerPort()).arg(s->id));
    connect(sock, &QTcpSocket::readyRead, this, [this, s](){ onReadyRead(s); });
    connect(sock, &QTcpSocket::disconnected, this, [this, s](){ onSessionDisconnected(s); });
    connect(sock, &QTcpSocket::bytesWritten, this, [this, s]() { flushOut(s); });
}

void NetworkWorker::onReadyRead(ClientSession *s)
{
    if(!s || !s->sock)
        return;

    // 先消化，再读取：下面两个条件任一成立都不碰 socket，让内核接收窗口把压力传回客户端。
    //   1. 应用层缓冲里还有没处理完的帧；
    //   2. 已投给存储线程的上传块积压到 2MB（等 chunkWritten 回执再放行）。
    auto uploadBackpressured = [s]() {
        for (auto it = s->Netransfers.constBegin(); it != s->Netransfers.constEnd(); ++it) {
            if (it->dir == Proto::TransferUpload && it->pendingWriteBytes >= kUploadPendingPauseBytes)
                return true;
        }
        return false;
    };

    if (!uploadBackpressured() && s->buffer.size() < kReadGateBytes)
        s->buffer.append(s->sock->readAll());

    if(s->buffer.size() > Proto::kMaxSessionBuffer ) {
        emit logMessage(QStringLiteral("会话 sid=%1 接收缓冲超限 %2 字节，断开连接").arg(s->id).arg(s->buffer.size()));
        s->sock->abort();
        return;
    }

    int dataFramesThisTurn = 0; // 本轮已投递的数据块数
    for (;;) {
        quint8 ch = 0, version = 0;
        QByteArray payload;
        const FrameStatus status = ProtocolCodec::takeFrame(s->buffer, &ch, &version, &payload);
        if (status == FrameStatus::NeedMore)
            break;
        if (status == FrameStatus::BadChannel || status == FrameStatus::FrameTooLarge) {
            // 流已失同步：唯一安全做法是重建连接，绝不能清缓冲接着解
            emit logMessage(QStringLiteral("会话 sid=%1 协议错误（通道/长度非法），断开连接").arg(s->id));
            s->sock->abort();
            return;
        }

        s->lastReceiveTime = QDateTime::currentMSecsSinceEpoch();  // 记录每个客户端的最后活跃时间

        if(version != Proto::kVersion) {
            emit logMessage(QStringLiteral("协议版本不兼容：客户端 %1:%2 version=%3 (需要 %4)，已断开连接")
                                        .arg(s->sock->peerAddress().toString())
                                        .arg(s->sock->peerPort()).arg(version).arg(Proto::kVersion));
            QJsonObject err;
            err.insert("type", "version_mismatch");
            err.insert("server_ver", Proto::kVersion);
            s->outCtrl.enqueue(ProtocolCodec::encodeJson(err));
            flushOut(s);
            s->sock->disconnectFromHost();
            return;
        }

        // 0x02 通道
        if(ch == Proto::kChFile) {
            FileChunk chunk;
            if(!ProtocolCodec::toFileChunk(payload, &chunk)) {
                emit logMessage(QStringLiteral("会话 sid=%1 收到非法块帧，丢弃").arg(s->id));
                continue;
            }
            handleUploadChunk(s, chunk);
            #if 1
            // 让出一轮事件循环，给存储线程写盘的机会；剩余帧留在 s->buffer 里
            if (++dataFramesThisTurn >= 4) {
                const quint64 sid = s->id;
                QMetaObject::invokeMethod(this, [this, sid]() {
                    if (ClientSession *ss = findSessionById(sid))
                        onReadyRead(ss);

                }, Qt::QueuedConnection);
                return;
            }
            #endif
            continue;
        }

        // 0x01 通道
        QJsonObject obj;
        if(!ProtocolCodec::toJsonObject(payload, &obj))
            continue;
        const QString type = obj.value(QStringLiteral("type")).toString();
        if (type == Proto::kAuthRegister) {
            handleRegister(s, obj);
            continue;
        }
        if (type == Proto::kAuthLogin) {
            handleLogin(s, obj);
            continue;
        }
        if (type == Proto::kSysPing) {
            sendPong(s, int(obj.value(QStringLiteral("seq")).toDouble()));
//            emit logMessage(QStringLiteral("心跳 uid:%1  seq:%2").arg(s->uid).arg(obj.value("seq").toInt(0)));
            continue;
        }

        // 文件传输控制
        FileMessage fileMsg;
        if (ProtocolCodec::isFileControlType(type)) {
            if (!requireAuth(s, "file"))
                return;
            if (!ProtocolCodec::objectToStruct(obj, &fileMsg))
                continue;
            handleFileMsg(s, fileMsg);
            continue;
        }

        // 业务消息，先认证
        if (!requireAuth(s, "business"))
            return;
        if (type == Proto::kChatC2S)
            handleChat(s, obj);
        else if (type == Proto::kHistoryRequest)
            handleHistoryMsgRequest(s, obj);
        else if (type == Proto::kRecentChatMarkRead)
            handleRecentChatMarkRead(s, obj);
        else if (type == Proto::kNicknameChange)
            handleNicknameChange(s, obj);
        else if (type == Proto::kUserSearch)
            handleUserSearch(s, obj);
        else if (type == Proto::kFriendReqAdd)
            handleFriendReqAdd(s, obj);
        else if (type == Proto::kFriendHandle)
            handleFriendHandle(s, obj);
        else if (type == Proto::kFriendRemove)
            handleFriendRemove(s, obj);
        else if (type == Proto::kAvatarUpdate)
            handleAvatarUpdate(s, obj);
        else if (type == Proto::kPasswordChange)
            handlePasswordChange(s, obj);
        else
            emit logMessage(QStringLiteral("会话 sid=%1 未知消息类型: %2").arg(s->id).arg(type));
    }

    // 入口可能因为水位条件没读 socket；这一轮把应用层缓冲消化掉后补一次读取，
    // 否则 Qt 不会仅因为"内部缓冲里还有数据"再发一次 readyRead。
    if (!uploadBackpressured() && s->buffer.size() < kReadGateBytes
        && s->sock->bytesAvailable() > 0) {
        const quint64 sid = s->id;
        QMetaObject::invokeMethod(this, [this, sid]() {
            if (ClientSession *ss = findSessionById(sid))
                onReadyRead(ss);
        }, Qt::QueuedConnection);
    }
}

bool NetworkWorker::sendPong(ClientSession *s, int seq)
{
    QJsonObject obj;
    obj.insert("type", Proto::kSysPong);
    obj.insert("seq",  seq);    // 原样回传
    return sendJsonTo(s, obj);
}

bool NetworkWorker::requireAuth(ClientSession *s, const char *what)
{
    if (s && s->uid > 0 && !s->userName.isEmpty())
        return true;
    emit logMessage(QStringLiteral("未认证会话发送 %1（sid=%2），断开").arg(QString::fromLatin1(what)).arg(s ? s->id : 0));
    if (s && s->sock)
        s->sock->abort();
    return false;
}


void NetworkWorker::onHeartbeatTimeout()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<ClientSession *> sessions = m_sessions.values();

    for(ClientSession *s : sessions) {
        if(s->lastReceiveTime <= 0)
            s->lastReceiveTime = now;
        if(now - s->lastReceiveTime <= Proto::kServerSessionIdleMs)
            continue;
        emit logMessage(QStringLiteral("心跳超时离线: %1 (uid=%2) 静默 %3 秒")
                                .arg(s->userName.isEmpty() ? QStringLiteral("<未认证>") : s->userName)
                                .arg(s->uid).arg((now - s->lastReceiveTime) / 1000));
        QTcpSocket *sock = s->sock;
        if(sock) {
            sock->abort(); // abort() 同步发出 disconnected
        }
    }
}

NetworkWorker::ClientSession *NetworkWorker::findSessionById(quint64 id) const
{
    return m_sessions.value(id, nullptr);
}

// 认证处理 + 账号持久化
void NetworkWorker::handleRegister(ClientSession *s, const QJsonObject &obj)
{
    if (!s->userName.isEmpty() || s->authOp >= 0)
        return;         // 已登录或正在认证中，忽略重复请求
    if(!m_dbReady) {
        QJsonObject resp;
        resp.insert("type", Proto::kAuthRegisterFail);
        resp.insert("text", QStringLiteral("服务未就绪，请稍后重试"));
        sendJsonTo(s, resp);
        return;
    }

    const QString name = obj.value("username").toString().trimmed();
    const QString clientHash = obj.value("password_hash").toString();
    if (name.isEmpty() || name.size() > Proto::kMaxUsernameLen) {
        QJsonObject resp;
        resp.insert("type", Proto::kAuthRegisterFail);
        resp.insert("text", QStringLiteral("用户名格式不正确"));
        sendJsonTo(s, resp);
        return;
    }
    if (clientHash.size() != Proto::kSha256HexLen) {
        QJsonObject resp;
        resp.insert("type", Proto::kAuthRegisterFail);
        resp.insert("text", QStringLiteral("密码格式不正确"));
        sendJsonTo(s, resp);
        return;
    }

    s->authOp = 1;
    s->authExpireAt = QDateTime::currentMSecsSinceEpoch() + 5000;  // 认证超时时间5s
    emit reqDbAuthRegister(s->id, name, clientHash);
}

void NetworkWorker::handleLogin(ClientSession *newC, const QJsonObject &obj)
{
    if (!newC->userName.isEmpty() || newC->authOp >= 0)
        return;         // 已登录或正在认证中，忽略重复请求
    if (!m_dbReady) {
        QJsonObject resp;
        resp.insert("type", Proto::kAuthLoginFail);
        resp.insert("text", QStringLiteral("服务未就绪，请稍后重试"));
        sendJsonTo(newC, resp);
        return;
    }

    const QString name = obj.value("username").toString().trimmed();
    const QString clientHash = obj.value("password_hash").toString();
    if (name.isEmpty() || name.size() > Proto::kMaxUsernameLen) {
        QJsonObject resp;
        resp.insert("type", Proto::kAuthLoginFail);
        resp.insert("text", QStringLiteral("用户名格式不正确"));
        sendJsonTo(newC, resp);
        return;
    }

    if (clientHash.size() != Proto::kSha256HexLen) {
        QJsonObject resp;
        resp.insert("type", Proto::kAuthLoginFail);
        resp.insert("text", QStringLiteral("密码格式不正确"));
        sendJsonTo(newC, resp);
        return;
    }

    newC->authOp = 0;
    newC->authExpireAt = QDateTime::currentMSecsSinceEpoch() + 5000;
    emit reqDbAuthLogin(newC->id, name, clientHash);
}

void NetworkWorker::onDbAuthResult(quint64 sessionId, int op, const QString &username, bool ok, const QString &text,
                         qint64 uid, qint64 accountNo,const QString &nickname, const QString &avatarFileId)
{
    ClientSession *s = findSessionById(sessionId);
    if(!s)
        return;     // 用户已断开
    if(s->authOp != op)
        return;     // authOp -1, 说明已超时

    // 复位状态机（在回包之前，避免回包过程中又被触发）
    s->authOp = -1;
    s->authExpireAt = 0;

    QJsonObject resp;
    if(op == 0) { // login
        if(!ok) {
            resp.insert("type", Proto::kAuthLoginFail);
            resp.insert("text", text);
            sendJsonTo(s, resp);
            emit logMessage(QStringLiteral("登录失败: %1 - %2").arg(username,text));
            return;
        }
        // 在线查重
        ClientSession *existing = m_onlineUidToSession.value(uid, nullptr);
        if(existing && existing != s) {
            resp.insert("type", Proto::kAuthLoginFail);
            resp.insert("text", QStringLiteral("该账号已在线"));
            sendJsonTo(s, resp);
//            emit logMessage(QStringLiteral("登录失败: %1 - 已在别处登录").arg(username));
            return;
        }
        s->uid = uid;
        s->accountNo = accountNo;
        s->userName = username;
        s->nickname = nickname;
        m_onlineUidToSession.insert(s->uid, s);   // 在线统计
        setServerOnlineUserList();    // 服务端UI在线列表

        resp.insert("type", Proto::kAuthLoginOk);
        resp.insert("server_ts", QDateTime::currentMSecsSinceEpoch());
        resp.insert("uid", QString::number(uid));
        resp.insert("account_no", QString::number(accountNo));
        resp.insert("nick", nickname);
        resp.insert("avatar_file_id", avatarFileId);
        sendJsonTo(s, resp);

        emit logMessage(QStringLiteral("上线: %1  (uid: %2)  (账号: %3)").arg(username).arg(uid).arg(accountNo));
        emit reqDbListFriends(s->id, s->uid);          // 数据库拉取：好友列表 + 通知好友我上线了
        emit reqDbListPendingRequests(s->id, s->uid);  // 数据库拉取：待处理好友申请
        emit reqDbLoadOfflineMsg(s->id, s->uid);       // 数据库拉取：未读消息
        emit reqDbListRecentChat(s->id, s->uid);       // 数据库拉取：最近会话
    }
    else {  // register
        if(ok) {
            resp.insert("type",Proto::kAuthRegisterOk);
            resp.insert("name", username);
            resp.insert("account_no",QString::number(accountNo));
            emit logMessage(QStringLiteral("注册成功: %1  (账号: %2)").arg(username).arg(accountNo));
        } else {
            resp.insert("type",Proto::kAuthRegisterFail);
            resp.insert("text", text);
        }
        sendJsonTo(s, resp);
    }
}

void NetworkWorker::handleNicknameChange(ClientSession *s, const QJsonObject &obj)
{
    if(s->userName.isEmpty())
        return;
    if(!m_dbReady) {
        QJsonObject resp;
        resp.insert("type", Proto::kNicknameChangeResult);
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("服务未就绪，请稍后重试"));
        sendJsonTo(s, resp);
        return;
    }

    const QString username = s->userName;
    const QString newNick = obj.value("new_nick").toString().trimmed();
    if (username.isEmpty() || newNick.isEmpty() || newNick.size() > Proto::kMaxNicknameLen) {
        QJsonObject resp;
        resp.insert("type", Proto::kNicknameChangeResult);
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("昵称格式不正确"));
        sendJsonTo(s, resp);
        return;
    }
    emit reqDbUpdateNickname(s->id, username, newNick);
}

// 修改密码：只收两次 SHA256 摘要，服务端用库里的盐验旧、换新盐存新
void NetworkWorker::handlePasswordChange(ClientSession *s, const QJsonObject &obj)
{
    if (!s || s->uid <= 0 || s->userName.isEmpty())
        return;

    const QString oldHash = obj.value("old_password_hash").toString().trimmed();
    const QString newHash = obj.value("new_password_hash").toString().trimmed();

    auto refuse = [this, s](int errcode) {
        QJsonObject resp;
        resp.insert("type", Proto::kPasswordChangeResult);
        resp.insert("ok", false);
        resp.insert("errcode", errcode);
        resp.insert("text", Err::friendlyText(errcode));
        sendJsonTo(s, resp);
    };

    if (!m_dbReady) {
        refuse(Err::ServiceUnavailable);
        return;
    }
    if (!isSha256Hex(oldHash) || !isSha256Hex(newHash)) {
        refuse(Err::InvalidParam);
        return;
    }
    if (oldHash == newHash) {
        refuse(Err::PasswordSameAsOld);
        return;
    }

    emit reqDbChangePassword(s->id, s->uid, oldHash, newHash);
}

void NetworkWorker::onDbPasswordChangeResult(quint64 sessionId, bool ok, int errcode)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s)
        return;  // 用户已断开

    QJsonObject resp;
    resp.insert("type", Proto::kPasswordChangeResult);
    resp.insert("ok", ok);
    resp.insert("errcode", ok ? int(Err::Ok) : errcode);
    resp.insert("text", ok ? QString() : Err::friendlyText(errcode));
    sendJsonTo(s, resp);

    // 改密成功后会话保持在线，不断连接
    if (ok)
        emit logMessage(QStringLiteral("密码修改成功: %1 (uid=%2)").arg(s->userName).arg(s->uid));
    else
        emit logMessage(QStringLiteral("密码修改失败: %1 (uid=%2) errcode=%3").arg(s->userName).arg(s->uid).arg(errcode));
}

void NetworkWorker::onNicknameUpdateResult(quint64 sessionId, const QString &username,
                                           bool ok, const QString &newNick, const QString &text)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s)
        return;
    QJsonObject resp;
    resp.insert("type", Proto::kNicknameChangeResult);
    resp.insert("ok", ok);
    if(ok) {
        s->nickname = newNick;
        resp.insert("nick", newNick);
    } else {
        resp.insert("text", text);
    }
    sendJsonTo(s, resp);

    if(ok) {
        QJsonObject nf;
        nf.insert("type", Proto::kNicknameChanged);
        nf.insert("uid", QString::number(s->uid));
        nf.insert("nick", newNick);
        broadcastJson(nf);
        emit logMessage(QStringLiteral("昵称变更: %1 (uid=%2) → %3").arg(username).arg(s->uid).arg(newNick));
    }
}

void NetworkWorker::onDbAvatarUpdateResult(quint64 sessionId, bool ok, int errcode, const QString &avatarFileId)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s)
        return;

    // 先回执给本人
    QJsonObject resp;
    resp.insert("type", Proto::kAvatarUpdateResult);
    resp.insert("ok", ok);
    if (ok)
        resp.insert("avatar_file_id", avatarFileId);
    else
        resp.insert("text", Err::friendlyText(errcode));
    sendJsonTo(s, resp);

    if (!ok) {
        emit logMessage(QStringLiteral("uid=%1 设置头像失败 errcode=%2").arg(s->uid).arg(errcode));
        return;
    }

    // 推给在线好友
    QJsonObject changed;
    changed.insert("type", Proto::kFriendAvatarChanged);
    changed.insert("uid", QString::number(s->uid));
    changed.insert("avatar_file_id", avatarFileId);
    for (qint64 fid : s->friendUids) {
        ClientSession *peer = m_onlineUidToSession.value(fid, nullptr);
        if (!peer || peer == s)
            continue;
        sendJsonTo(peer, changed);
    }
    emit logMessage(QStringLiteral("%1(uid=%2) 头像已更新，已通知在线好友").arg(s->userName).arg(s->uid));
}

void NetworkWorker::handleHistoryMsgRequest(ClientSession *s, const QJsonObject &obj)
{
    if(s->userName.isEmpty())
        return;
    if(!m_dbReady) {
        QJsonObject resp;
        resp.insert("type", Proto::kHistoryResult);
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("服务暂时不可用，请稍后重试"));
        resp.insert("conv_id", obj.value("conv_id").toString());
        resp.insert("has_more", false);
        resp.insert("messages", QJsonArray());
        sendJsonTo(s, resp);
        return;
    }

    bool ok = false;
    const qint64 peerUid = obj.value("conv_id").toString().toLongLong(&ok);
    if(!ok || peerUid <= 0 || peerUid == s->uid) {
        QJsonObject resp;
        resp.insert("type", Proto::kHistoryResult);
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("拉取历史消息失败"));
        resp.insert("conv_id", obj.value("conv_id").toString());
        resp.insert("has_more", false);
        resp.insert("messages", QJsonArray());
        sendJsonTo(s, resp);
        return;
    }

    const QString beforeMsgId = obj.value("before_msg_id").toString();
    int limit = obj.value("limit").toInt(Proto::kHistoryPageSize);
    if(limit <= 0 || limit > 100)
        limit = Proto::kHistoryPageSize;
    emit reqDbLoadHistoryMsg(s->id, s->uid, peerUid, beforeMsgId, limit);
}

void NetworkWorker::onDbHistoryMsgLoaded(quint64 sessionId, const HistoryPage &page)
{
    ClientSession *s = findSessionById(sessionId);
    if(!s)
        return;
    QJsonArray arr;
    for(const ChatMessage &m :page.messages) {
        QJsonObject o;
        o.insert("msg_id",        m.msgId);
        o.insert("client_msg_id", m.clientMsgId);
        o.insert("from_uid",      QString::number(m.senderUid));
        o.insert("conv_id",        QString::number(m.convId));
        o.insert("text",          m.text);
        o.insert("ts",            static_cast<double>(m.ts));
        o.insert("msg_type", int(m.msgType));
        if (!m.fileId.isEmpty())
            o.insert("file_id", m.fileId);
        if (!m.fileName.isEmpty())
            o.insert("file_name", m.fileName);
        if (m.fileSize > 0)
            o.insert("file_size", double(m.fileSize));
        arr.append(o);
    }

    QJsonObject resp;
    resp.insert("type", Proto::kHistoryResult);
    resp.insert("ok", page.ok);
    resp.insert("text", page.text);
    resp.insert("conv_id", QString::number(page.convId));
    resp.insert("has_more", page.hasMore);
    resp.insert("messages", arr);
    sendJsonTo(s, resp);
}

void NetworkWorker::onDbRecentChatLoaded(quint64 sessionId, const RecentChatResult &r)
{
    ClientSession *s = findSessionById(sessionId);
    if(!s)
        return;
    QJsonArray arr;
    for(const RecentChatItem &item : r.items) {
        QJsonObject o;
        o.insert("conv_type",     QString::number(Proto::ConvPrivate));
        o.insert("conv_id",       QString::number(item.convId));
        o.insert("peer_nick",     item.peerNick);
        o.insert("last_msg_id",   item.lastMsgId);
        o.insert("last_msg_ts",   static_cast<double>(item.lastMsgTs));
        o.insert("last_msg_text", item.lastMsgText);
        o.insert("unread_count",  item.unreadCount);
        arr.append(o);
    }

    QJsonObject resp;
    resp.insert("type", Proto::kRecentChatResult);
    resp.insert("ok", r.ok);
    resp.insert("items", arr);
    sendJsonTo(s, resp);
}

void NetworkWorker::handleRecentChatMarkRead(ClientSession *s, const QJsonObject &obj)
{
    if(s->userName.isEmpty() || !m_dbReady)
        return;
    bool ok = false;
    const qint64 peerUid = obj.value("conv_id").toString().toLongLong(&ok);
    if (!ok || peerUid <= 0)
        return;
    emit reqDbMarkRecentChatRead(s->uid, peerUid);
}

void NetworkWorker::onAuthTimeout()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (ClientSession *s : m_sessions) {
        if (s->authOp < 0 || s->authExpireAt >= now)
            continue;       // 未认证、未超时

        const int op = s->authOp;
        s->authOp = -1;         // 先复位，防止晚到结果被放行
        s->authExpireAt = 0;

        QJsonObject resp;
        resp.insert("type", op == 0 ? Proto::kAuthLoginFail : Proto::kAuthRegisterFail);
        resp.insert("text", QStringLiteral("服务器响应超时, 认证失败，请重试"));
        sendJsonTo(s, resp);
        if(op)
            emit logMessage(QStringLiteral("注册账号超时: %1").arg(s->userName));
        else
            emit logMessage(QStringLiteral("登录超时: %1").arg(s->userName));
    }
}

void NetworkWorker::onSessionDisconnected(ClientSession *s)
{
    if(!s || !m_sessions.contains(s->id))
        return;     // 已被其他路径拆除（心跳超时 / 停止监听 / 退出清理)

    const bool wasOnline = (s->uid != 0 && !s->userName.isEmpty());
    const qint64 offUid = static_cast<qint64>(s->uid);
    const qint64 offTime = s->lastReceiveTime;
    const QString username = s->userName;

    if(!username.isEmpty())
        emit logMessage(QStringLiteral("下线: %1  (ID: %2)").arg(username).arg(offUid));

    if(wasOnline) {
        QJsonObject o;
        o.insert("type", Proto::kSysFriendsOnlineUpdate);
        o.insert("uid", QString::number(offUid));
        o.insert("online", false);
        for(qint64 fid : s->friendUids) {   // 遍历好友 uid
            ClientSession *peer = m_onlineUidToSession.value(fid, nullptr);
            if(!peer || peer == s)
                continue;   // 只广播给在线的会话
            sendJsonTo(peer, o);
        }
        emit reqDbUpdateLastActiveTime(offUid, offTime);
    }
    emit reqCloseSession(s->id); // 让 FileService 回收本会话的传输：保留 .part（可续传），uploading 行标记删除
    destroySession(s);
    if(wasOnline)
        setServerOnlineUserList();
}


bool NetworkWorker::sendJsonTo(ClientSession *s, const QJsonObject &obj)
{
    if (!s || !s->sock)
        return false;
    if (s->sock->state() != QAbstractSocket::ConnectedState)
        return false;
    s->outCtrl.enqueue(ProtocolCodec::encodeJson(obj));
    flushOut(s);
    return true;
}

void NetworkWorker::setServerOnlineUserList()
{
    QSet<QString> onlineUsers;
    for(ClientSession *c : m_sessions) {
        if(!c->userName.isEmpty() && c->uid != 0) {
            onlineUsers.insert(c->userName);
        }
    }
    emit onlineUsersChanged(onlineUsers);
}

void NetworkWorker::broadcastJson(const QJsonObject &obj)
{
    for(ClientSession *c : m_sessions) {
        if(!c->userName.isEmpty())
            sendJsonTo(c, obj);
    }
}

void NetworkWorker::handleChat(ClientSession *s, const QJsonObject &obj)
{
    if (s->userName.isEmpty())
        return; // 未认证

    const QString clientMsgId = obj.value("client_msg_id").toString();
    const QString toUidStr = obj.value("conv_id").toString();
    const QString text = obj.value("text").toString();
    const quint8  msgType  = quint8(obj.value(QStringLiteral("msg_type")).toInt(int(Proto::MsgTypeText)));
    const QString fileId   = obj.value(QStringLiteral("file_id")).toString();
    const QString fileName = obj.value(QStringLiteral("file_name")).toString();
    const qint64  fileSize = qint64(obj.value(QStringLiteral("file_size")).toDouble());

    QJsonObject ack;
    ack.insert("type", Proto::kChatAck);
    ack.insert("client_msg_id", clientMsgId);
    ack.insert("conv_id", toUidStr);

    if(!m_dbReady) {
        ack.insert("ok", false);
        ack.insert("text", QStringLiteral("服务暂时不可用，请稍后重试"));
        sendJsonTo(s, ack);
        return;
    }

    if(toUidStr.isEmpty()) {
        ack.insert("ok", false);
        ack.insert("text", QStringLiteral("消息发送失败，请重试"));
        sendJsonTo(s, ack);
        return;
    }

    if (!Proto::isValidMsgType(msgType)) {
        ack.insert("ok", false);
        ack.insert("text", QStringLiteral("消息发送失败，请重试"));
        sendJsonTo(s, ack);
        return;
    }

    // 只有纯文本消息要求正文非空；附件消息的正文可以为空
    if (msgType == Proto::MsgTypeText && text.isEmpty()) {
        ack.insert("ok", false);
        ack.insert("text", QStringLiteral("消息不能为空，发送失败，请重试"));
        sendJsonTo(s, ack);
        return;
    }

    // 附件消息必须带完整的文件信息（归属校验在 DB 层做）
    if (msgType != Proto::MsgTypeText) {
        if (fileId.isEmpty() || fileId.size() > 36 || fileName.isEmpty()) {
            ack.insert("ok", false);
            ack.insert("text", QStringLiteral("文件信息不完整，请重新发送"));
            sendJsonTo(s, ack);
            return;
        }
    }

    bool ok = false;
    const qint64 toUid = toUidStr.toLongLong(&ok);
    if (!ok || toUid <= 0) {
        ack.insert("ok", false);
        ack.insert("text", QStringLiteral("消息发送失败，请重试"));
        sendJsonTo(s, ack);
        return;
    }
    // 不能给自己发消息
    if (toUid == s->uid) {
        return;
    }

    if(!s->friendListReady) {
        ack.insert("ok", false);
        ack.insert("text", QStringLiteral("好友列表加载中...请稍后重试"));
        sendJsonTo(s, ack);
        return;
    }

    if(!s->friendUids.contains(toUid)) {
        ack.insert("ok", false);
        ack.insert("text", QStringLiteral("仅好友之间可以发送消息"));
        sendJsonTo(s, ack);
        emit logMessage(QStringLiteral("消息拒收: %1(uid=%2) 发给非好友 uid=%3").arg(s->userName).arg(s->uid).arg(toUid));
        return;
    }

    ChatMessage msg;
    msg.clientMsgId = clientMsgId;
    msg.senderUid   = s->uid;
    msg.convId      = toUid;
    msg.msgType     = msgType;
    msg.text        = text;
    msg.fromNick    = s->nickname;
    if (msgType != Proto::MsgTypeText) {
        msg.fileId   = fileId;
        msg.fileName = fileName;
        msg.fileSize = fileSize;
    }
    emit reqDbSaveMessage(s->id, msg);
}


void NetworkWorker::handleUserSearch(ClientSession *s, const QJsonObject &obj)
{
    if (s->userName.isEmpty())
        return;

    QJsonObject resp;
    resp.insert("type", Proto::kUserSearchResult);

    if (!m_dbReady) {
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("服务暂时不可用，请稍后重试"));
        sendJsonTo(s, resp);
        return;
    }

    bool ok = false;
    const qint64 accountNo = obj.value("account_no").toString().toLongLong(&ok);
    if (!ok || !Proto::isValidAccountNo(accountNo)) {
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("账号无效"));
        sendJsonTo(s, resp);
        return;
    }
    if (accountNo == s->accountNo) {
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("不能添加自己"));
        sendJsonTo(s, resp);
        return;
    }

    emit reqDbSearchUser(s->id, accountNo);
}

void NetworkWorker::onDbUserSearchResult(quint64 sessionId, bool ok, const QString &text, const SearchResult &user)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s)
        return;

    QJsonObject resp;
    resp.insert("type", Proto::kUserSearchResult);
    resp.insert("account_no", QString::number(user.accountNo));
    resp.insert("ok", ok);
    if (ok) {
        resp.insert("uid", QString::number(user.uid));
        resp.insert("nick", user.nick);
        resp.insert("avatar_file_id", user.avatarFileId);
    } else {
        resp.insert("text", text);
    }
    sendJsonTo(s, resp);
}

void NetworkWorker::handleFriendReqAdd(ClientSession *s, const QJsonObject &obj)
{
    if (s->userName.isEmpty())
        return; // 未认证
    QJsonObject resp;
    resp.insert("type", Proto::kFriendReqAddResult);

    if(!m_dbReady) {
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("服务暂时不可用，请稍后重试"));
        sendJsonTo(s, resp);
        return;
    }
    bool ok = false;
    const qint64 toAccountNo = obj.value("account_no").toString().toLongLong(&ok);
    if (!ok || !Proto::isValidAccountNo(toAccountNo)) {
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("账号格式不正确"));
        sendJsonTo(s, resp);
        return;
    }
    const QString reqText = obj.value("text").toString().trimmed();
    if (reqText.size() > Proto::kMaxFriendReqTextLen) {
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("验证信息过长"));
        sendJsonTo(s, resp);
        return;
    }
    emit reqDbSendAddFriendRequest(s->id, s->uid, toAccountNo, reqText);
}

void NetworkWorker::onDbFriendRequestResult(quint64 sessionId, bool ok, const QString &text, qint64 toUid)
{
    ClientSession *s = findSessionById(sessionId);
    if (s) {
        QJsonObject resp;
        resp.insert("type", Proto::kFriendReqAddResult);
        resp.insert("ok", ok);
        resp.insert("text", text);
        sendJsonTo(s, resp);
    }
    if(!ok || toUid <= 0) {
        return;
    }
    ClientSession *target = m_onlineUidToSession.value(toUid);
    if(!target)
        return;
    emit reqDbListPendingRequests(target->id, target->uid);
}

// 接收 同意/拒绝
void NetworkWorker::handleFriendHandle(ClientSession *s, const QJsonObject &obj)
{
    if (s->userName.isEmpty())
        return;
    const qint64 reqId = obj.value("req_id").toString().toLongLong();
    QJsonObject resp;
    resp.insert("type", Proto::kFriendHandleResult);
    if (!m_dbReady) {
        resp.insert("req_id", QString::number(reqId));
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("服务暂时不可用，请稍后重试"));
        sendJsonTo(s, resp);
        return;
    }

    const bool accept = obj.value("accept").toBool();
    emit reqDbHandleFriendRequest(s->id, reqId, s->uid, accept);
}

// 待处理好友申请
void NetworkWorker::onPendingRequestsResult(quint64 sessionId, const QVector<FriendRequestInfo> &reqs)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s || reqs.isEmpty())
        return;
    for (const FriendRequestInfo &r : reqs) {
        QJsonObject obj;
        obj.insert("type", Proto::kFriendPendingRequest);
        obj.insert("req_id",   QString::number(r.reqId));
        obj.insert("from_uid", QString::number(r.fromUid));
        obj.insert("from_nick", r.fromNick);
        obj.insert("text", r.text);
        obj.insert("ts", static_cast<double>(r.createdAt));
        sendJsonTo(s, obj);
    }
    emit logMessage(QStringLiteral("推送待处理好友申请，给 %1：%2 条").arg(s->userName).arg(reqs.size()));
}

// 好友列表 + 通知好友我上线了
void NetworkWorker::onListFriendsResult(quint64 sessionId, const QVector<SearchResult> &friends)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s || s->uid <= 0)
        return;

    s->friendUids.clear();
    QJsonArray arr;
    for(const SearchResult &f : friends) {
        s->friendUids.insert(f.uid);

        QJsonObject o;
        o.insert("uid", QString::number(f.uid));
        o.insert("account_no", QString::number(f.accountNo));
        o.insert("nick", f.nick);
        o.insert("avatar_file_id", f.avatarFileId);
        o.insert("online", m_onlineUidToSession.contains(f.uid));
        arr.append(o);
    }
    s->friendCount = s->friendUids.size();
    s->friendListReady = true;  // 服务端的好友列表加载完成

    QJsonObject roster;
    roster.insert("type", Proto::kSysFriends);
    roster.insert("friends", arr);
    sendJsonTo(s, roster);
    emit logMessage(QStringLiteral("kSysRoster 推送给 %1：%2 位好友").arg(s->userName).arg(friends.size()));

    QJsonObject o;
    o.insert("type", Proto::kSysFriendsOnlineUpdate);
    o.insert("uid", QString::number(s->uid));
    o.insert("online", true);
    for(qint64 fid : s->friendUids) {   // 遍历好友 uid
        ClientSession *peer = m_onlineUidToSession.value(fid, nullptr);
        if(!peer || peer== s)
            continue;   // 好友离线 或 自己
        sendJsonTo(peer, o);
    }
}

void NetworkWorker::onDbFriendHandleResult(quint64 sessionId, const MysqlUserStore::HandleFriendResult &r)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s)
        return;
    {
        QJsonObject resp;
        resp.insert("type", Proto::kFriendHandleResult);
        resp.insert("req_id", QString::number(r.reqId));
        resp.insert("ok", r.ok);
        resp.insert("text", r.text);
        sendJsonTo(s, resp);
    }

    if (!r.ok || !r.accepted)
        return;
    s->friendUids.insert(r.fromUid); // 更新接收方 session 缓存
    s->friendCount = s->friendUids.size();

    if (!r.firstTime)
        return; // 重复同意申请时，不推，避免客户端重复插行

    {
        QJsonObject obj;
        obj.insert("type", Proto::kFriendAdded);
        obj.insert("uid", QString::number(r.fromUid));
        obj.insert("account_no", QString::number(r.fromAccountNo));
        obj.insert("nick", r.fromNick);
        obj.insert("avatar_file_id", r.fromAvatarFileId);
        obj.insert("online", m_onlineUidToSession.contains(r.fromUid));
        sendJsonTo(s, obj);
    }

    // from 申请人在线时
    ClientSession *from = m_onlineUidToSession.value(r.fromUid, nullptr);
    if(from) {
        from->friendUids.insert(r.selfUid);
        from->friendCount = from->friendUids.size();
        QJsonObject obj;
        obj.insert("type", Proto::kFriendAdded);
        obj.insert("uid", QString::number(r.selfUid));
        obj.insert("account_no", QString::number(r.selfAccountNo));
        obj.insert("nick", r.selfNick);
        obj.insert("avatar_file_id", r.selfAvatarFileId);
        obj.insert("online", true);
        sendJsonTo(from, obj);
    }
    emit logMessage(QStringLiteral("好友建立: %1(%2) ↔ %3(%4)").arg(s->userName).arg(r.selfUid)
                            .arg(from ? from->userName : QStringLiteral("<离线>")).arg(r.fromUid));
}

void NetworkWorker::handleFriendRemove(ClientSession *s, const QJsonObject &obj)
{
    if (s->userName.isEmpty())
        return;
    QJsonObject resp;
    resp.insert("type", Proto::kFriendRemoveResult);
    if(!m_dbReady) {
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("服务暂时不可用，请稍后重试"));
        sendJsonTo(s, resp);
        return;
    }
    bool ok = false;
    const qint64 removeUid = obj.value("remove_uid").toString().toLongLong(&ok);
    if(!ok || removeUid <= 0) {
        resp.insert("ok", false);
        resp.insert("text", QStringLiteral("好友删除失败，请重试"));
        sendJsonTo(s, resp);
        return;
    }
    emit reqDbRemoveFriend(s->id, s->uid, removeUid);
}

void NetworkWorker::handleAvatarUpdate(ClientSession *s, const QJsonObject &obj)
{
    if (s->userName.isEmpty())
        return;

    const QString fileId = obj.value("file_id").toString();

    QJsonObject resp;
    resp.insert("type", Proto::kAvatarUpdateResult);

    if (!m_dbReady) {
        resp.insert("ok", false);
        resp.insert("text", Err::friendlyText(Err::ServiceUnavailable));
        sendJsonTo(s, resp);
        return;
    }
    if (fileId.isEmpty() || fileId.size() > 36) {   // files.file_id VARCHAR(36)
        resp.insert("ok", false);
        resp.insert("text", Err::friendlyText(Err::InvalidParam));
        sendJsonTo(s, resp);
        return;
    }

    emit reqDbUpdateAvatar(s->id, s->uid, fileId);
}

void NetworkWorker::onDbRemoveFriendResult(quint64 sessionId, const MysqlUserStore::RemoveFriendResult &r)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s)
        return;
    {
        QJsonObject resp;
        resp.insert("type", Proto::kFriendRemoveResult);
        resp.insert("ok", r.ok);
        resp.insert("text", r.text);
        resp.insert("remove_uid", QString::number(r.removeUid));
        sendJsonTo(s, resp);
    }
    if(!r.ok) {
        emit logMessage(QStringLiteral("uid:%1 删除好友uid:%2 失败: %3").arg(s->uid).arg(r.removeUid).arg(r.text));
        return;
    }

    // 更新服务端缓存
    s->friendUids.remove(r.removeUid);
    s->friendCount = s->friendUids.size();

    ClientSession *peer = m_onlineUidToSession.value(r.removeUid, nullptr);
    if (peer) {
        peer->friendUids.remove(s->uid);
        peer->friendCount = peer->friendUids.size();
    }
    // 通知被删除方
    if(peer) {
        QJsonObject o;
        o.insert("type", Proto::kFriendRemoved);
        o.insert("uid",  QString::number(s->uid));
        sendJsonTo(peer, o);
    }
    else {
//        emit logMessage(QStringLiteral("%1(uid=%2) 好友删除: uid=%3 (已离线)")
//                            .arg(s->userName).arg(s->uid).arg(r.removeUid));
    }
}

void NetworkWorker::onMessageSaved(quint64 sessionId, const ChatMessage &msg, bool ok, const QString &reason)
{
    QJsonObject ack;
    ack.insert("type", Proto::kChatAck);
    ack.insert("client_msg_id", msg.clientMsgId);
    ack.insert("conv_id", QString::number(msg.convId));
    ClientSession *s = findSessionById(sessionId);

    // 消息落库成功
    if(ok) {
        ClientSession *target = m_onlineUidToSession.value(msg.convId, nullptr);
        const bool receiverOnline = (target != nullptr);
        bool delivered = false;
        if(target) {    // 转发消息
            QJsonObject relay;
            relay.insert("type", Proto::kChatS2C);
            relay.insert("msg_id", msg.msgId);
            relay.insert("client_msg_id", msg.clientMsgId);
            relay.insert("from_uid", QString::number(msg.senderUid));
            relay.insert("conv_id",   QString::number(msg.convId));
            relay.insert("from_nick", msg.fromNick);
            relay.insert("ts", msg.ts);
            relay.insert("text", msg.text);
            relay.insert("msg_type", int(msg.msgType));
            if (!msg.fileId.isEmpty())
                relay.insert("file_id", msg.fileId);
            if (!msg.fileName.isEmpty())
                relay.insert("file_name", msg.fileName);
            if (msg.fileSize > 0)
                relay.insert("file_size", double(msg.fileSize));
            delivered = sendJsonTo(target, relay);
        }
        if(delivered)
            emit reqDbMarkDelivered({msg.msgId}); // 标记 数据库中status=1

        if(!s)
            return; // 发送方已断开，ACK 无处可回
        ack.insert("ok", true);
        ack.insert("msg_id", msg.msgId);
        ack.insert("server_ts", msg.ts);
        ack.insert("pending", !receiverOnline);
        if(!receiverOnline) {
            ack.insert("text", QStringLiteral("对方离线，消息已保存"));
        }
        sendJsonTo(s, ack);
    }
    else {
        if (!s)
            return; // 发送方已断开，ACK 无处可回
        ack.insert("ok", false);
        ack.insert("pending", false);
        ack.insert("text", reason);
        sendJsonTo(s, ack);
        emit logMessage(QStringLiteral("消息落库失败 %1 -> %2: %3").arg(msg.senderUid).arg(msg.convId).arg(reason));
    }
}

void NetworkWorker::onOfflineMsgLoaded(quint64 sessionId, const QVector<ChatMessage> &msgs)
{
    if(msgs.isEmpty())
        return;
    ClientSession *s = findSessionById(sessionId);
    if(!s) {
        emit logMessage(QStringLiteral("离线消息投递时，客户端已断开，保留待下次拉取"));
        return;
    }

    // DB 按 id DESC 拉取（新→旧），投递按时间正序（旧→新）
    QVector<ChatMessage> ordered = msgs;
    std::reverse(ordered.begin(), ordered.end());

    QStringList successIds;
    successIds.reserve(msgs.size());
    for(const ChatMessage &m : ordered) {
        QJsonObject relay;
        relay.insert("type", Proto::kChatS2C);
        relay.insert("msg_id", m.msgId);
        relay.insert("client_msg_id", m.clientMsgId);
        relay.insert("from_uid", QString::number(m.senderUid));
        relay.insert("conv_id",   QString::number(m.convId));
        relay.insert("from_nick", m.fromNick);
        relay.insert("ts", m.ts);
        relay.insert("text", m.text);
        relay.insert("msg_type", int(m.msgType));
        if (!m.fileId.isEmpty())
            relay.insert("file_id", m.fileId);
        if (!m.fileName.isEmpty())
            relay.insert("file_name", m.fileName);
        if (m.fileSize > 0)
            relay.insert("file_size", double(m.fileSize));

        if(!sendJsonTo(s, relay))
            break;  // socket 挂了，后续也发不出去，停止
        successIds.append(m.msgId);
    }
    if(!successIds.isEmpty())
        emit reqDbMarkDelivered(successIds);
    emit logMessage(QStringLiteral("补推离线消息给 %1：%2/%3 条").arg(s->userName)
                                .arg(successIds.size()).arg(msgs.size()));
}

void NetworkWorker::handleUploadChunk(ClientSession *s, const FileChunk &chunk)
{
    auto netTransfer = s->Netransfers.find(chunk.transferId);
    if(netTransfer == s->Netransfers.end() || netTransfer->dir != Proto::TransferUpload) {
        emit logMessage(QStringLiteral("会话 %1 收到未登记的上传块 tid=%2").arg(s->id).arg(chunk.transferId));
        return;
    }
    // 记在"已投递未落盘"的账上：StorageWorker 每写完一块回 chunkWritten，欠账降下来才继续读 socket
    netTransfer->pendingWriteBytes += chunk.data.size();
    emit reqWriteChunk(netTransfer->shard, chunk.transferId, chunk.offset, chunk.data);
}

void NetworkWorker::onChunkWritten(quint32 transferId, qint64 bytes)
{
    const quint64 sid = m_transferOwner.value(transferId, 0);
    ClientSession *s = findSessionById(sid);
    if (!s)
        return;

    auto netTransfer = s->Netransfers.find(transferId);
    if (netTransfer == s->Netransfers.end() || netTransfer->dir != Proto::TransferUpload)
        return;

    netTransfer->pendingWriteBytes = qMax<qint64>(0, netTransfer->pendingWriteBytes - bytes);
    if (netTransfer->pendingWriteBytes >= kUploadPendingPauseBytes)
        return;     // 还没降到水位以下，继续压着 socket

    // 之前可能因为积压停止读 socket：现在有回执了，立刻把缓冲/内核里剩下的数据接着消化
    if (!s->buffer.isEmpty() || (s->sock && s->sock->bytesAvailable() > 0))
        onReadyRead(s);
}

void NetworkWorker::handleFileMsg(ClientSession *s, const FileMessage &f)
{
    if (f.type == Proto::kFileUploadBegin) {
        emit reqBeginUpload(s->id, s->uid, f);
    }
    else if (f.type == Proto::kFileUploadEnd) {
        emit reqFinishUpload(s->id, f.transferId, f.sha256);
    }
    else if (f.type == Proto::kFileUploadAbort) {
        emit reqAbortUpload(s->id, f.transferId);
    }
    else if (f.type == Proto::kFileDownloadBegin) {
        emit reqBeginDownload(s->id, s->uid, f);
    }
    else if (f.type == Proto::kFileDownloadAbort) {
        emit reqAbortDownload(s->id, f.transferId);
    }
    else if (f.type == Proto::kFileDelete) {
        emit reqDeleteFile(s->id, s->uid, f.fileId);
    }
    else {
        emit logMessage(QStringLiteral("会话 sid=%1 发送消息类型: %2，已忽略").arg(s->id).arg(f.type));
    }
}

void NetworkWorker::flushOut(ClientSession *s)
{
    if (!s || !s->sock)
        return;
    if (s->sock->state() != QAbstractSocket::ConnectedState)
        return;

    // 控制帧：永远优先，不看水位。一个聊天/回执包就几百字节，塞不爆。
    while (!s->outCtrl.isEmpty())
        s->sock->write(s->outCtrl.dequeue());

    // 数据帧：socket 待发字节低于水位才补一块。
    // 出口最多积压 75KB + 256KB ≈ 330KB；服务器带宽3Mbps 下 ≈ 0.9 秒
    while (!s->outData.isEmpty() && s->sock->bytesToWrite() < kDownloadSendWaterBytes) {
        const OutFrame f = s->outData.dequeue();
        s->outDataBytes -= f.frame.size();
        s->sock->write(f.frame);
        maybeRequestNext(s, f.transferId);   // 写一块就推进一次下载泵，bytesWritten → flushOut 这条链闭合
    }
}

void NetworkWorker::sendChunkTo(ClientSession *s, quint32 transferId, const FileChunk &c)
{
    if (!s || !s->sock)
        return;
    const QByteArray frame = ProtocolCodec::encodeFileChunk(c);
    if (frame.isEmpty())
        return;
    OutFrame f;
    f.transferId = transferId;
    f.frame = frame;
    s->outDataBytes += frame.size();
    s->outData.enqueue(f);
    // 队列超过硬上限：说明对端长期不收，主动断，避免服务端内存被慢客户端拖爆
    if(s->outDataBytes > kOutDataQueueAbortBytes) {
        emit logMessage(QStringLiteral("会话 sid=%1 出口积压 %2 字节，断开连接").arg(s->id).arg(s->outDataBytes));
        s->sock->abort();
        return;
    }
    flushOut(s);
    return;
}

// 下载专用函数: 服务端主动"请求"存储线程读一块 → 存储线程返回块 → 网络线程写到 socket→ socket 把数据刷给内核（bytesWritten）→ 再请求下一块
void NetworkWorker::maybeRequestNext(ClientSession *s, quint32 transferId)
{
    if (!s || transferId == 0)
        return;
    auto netTransfer = s->Netransfers.find(transferId);
    if (netTransfer == s->Netransfers.end() || netTransfer->dir != Proto::TransferDownload)
        return;
    if(netTransfer->eof || netTransfer->inFlight > 0)
        return;     // 同一 transfer 只允许一块在途
    if(s->outDataBytes >= kOutDataQueueBytes)
        return;
    const qint64 readLen = netTransfer->readEnd - netTransfer->nextReadOffset;
    if(readLen <= 0) {
        netTransfer->eof = true;
        return;
    }
    const int maxLen = int(qMin<qint64>(Proto::kFileChunkSize, readLen));
    netTransfer->inFlight++;
    emit reqReadChunk(netTransfer->shard, transferId, netTransfer->nextReadOffset, maxLen);
}

void NetworkWorker::registerTransfer(ClientSession *s, const NetTransfer &t)
{
    if(!s)
        return;
    s->Netransfers.insert(t.transferId, t);
    m_transferOwner.insert(t.transferId, s->id);
}

void NetworkWorker::releaseTransfer(ClientSession *s, quint32 transferId)
{
    if(s)
        s->Netransfers.remove(transferId);
    m_transferOwner.remove(transferId);
}

void NetworkWorker::onUploadBeginOk(quint64 sessionId, quint32 transferId, int shard,
                                    const QString &fileId, qint64 existingOffset, qint64 totalSize)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s)
        return;
    NetTransfer t;
    t.transferId = transferId;
    t.dir        = Proto::TransferUpload;
    t.shard      = shard;
    registerTransfer(s, t);

    FileMessage m;
    m.type = Proto::kFileUploadBeginOk;
    m.transferId = transferId;
    m.fileId     = fileId;
    m.chunkSize  = Proto::kFileChunkSize;
    m.offset     = existingOffset;     // 服务端已收字节数（0 = 全新上传，>0 = 该用户自己的断点续传）
    m.totalSize  = totalSize;
    m.ok         = true;
    sendJsonTo(s, ProtocolCodec::toJson(m));
}

void NetworkWorker::onUploadEndOk(quint64 sessionId, quint32 transferId, const FileMeta &meta)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s)
        return;

    FileMessage m;
    m.type = Proto::kFileUploadEndOk;
    m.transferId = transferId;
    m.fileId = meta.fileId;
    m.ok = true;
    m.meta = meta;
    sendJsonTo(s, ProtocolCodec::toJson(m));

    // 传输结束：从会话与全局归属表里摘掉
    releaseTransfer(s, transferId);
}

void NetworkWorker::onDownloadBeginOk(quint64 sessionId, quint32 transferId, int shard,
                                      const FileMeta &meta, qint64 startOffset, qint64 length)
{
    ClientSession *s = findSessionById(sessionId);
    if (!s)
        return;

    NetTransfer t;
    t.transferId     = transferId;
    t.dir            = Proto::TransferDownload;
    t.shard          = shard;
    t.nextReadOffset = startOffset;
    t.readEnd        = startOffset + qMax<qint64>(0, length > 0 ? length : meta.size - startOffset);
    t.eof            = (t.readEnd <= startOffset);
    registerTransfer(s, t);

    FileMessage m;
    m.type       = Proto::kFileDownloadBeginOk;
    m.transferId = transferId;
    m.fileId     = meta.fileId;
    m.fileName   = meta.fileName;
    m.mime       = meta.mime;
    m.totalSize  = meta.size;
    m.chunkSize  = Proto::kFileChunkSize;
    m.offset     = startOffset;
    m.length     = length;
    m.biz        = meta.biz;
    m.sha256     = meta.sha256;
    m.ok         = true;
    sendJsonTo(s, ProtocolCodec::toJson(m));

    // 开始拉第一块
    maybeRequestNext(s, transferId); // nextReadOffset readEnd 已赋值
}

void NetworkWorker::onUploadBeginFailed(quint64 sessionId, quint32 transferId, int errcode)
{
    ClientSession *session = findSessionById(sessionId);
    if (!session)
        return;

    FileMessage reply;
    reply.type    = Proto::kFileUploadBeginOk;
    reply.transferId = transferId;
    reply.ok      = false;
    reply.errcode = errcode;
    reply.text    = Err::friendlyText(errcode);
    sendJsonTo(session, ProtocolCodec::toJson(reply));
}

// 上传中途/收尾失败：客户端已经登记过这个 transferId，摘掉它
void NetworkWorker::onUploadEndFailed(quint64 sessionId, quint32 transferId, int errcode)
{
    ClientSession *session = findSessionById(sessionId);
    if (!session)
        return;

    FileMessage reply;
    reply.type       = Proto::kFileUploadEndOk;
    reply.transferId = transferId;
    reply.ok         = false;
    reply.errcode    = errcode;
    reply.text       = Err::friendlyText(errcode);
    sendJsonTo(session, ProtocolCodec::toJson(reply));

    releaseTransfer(session, transferId);
}

// 下载开始被拒：还没登记传输，不需要 release
void NetworkWorker::onDownloadBeginFailed(quint64 sessionId, quint32 transferId, int errcode)
{
    ClientSession *session = findSessionById(sessionId);
    if (!session)
        return;

    FileMessage reply;
    reply.type    = Proto::kFileDownloadBeginOk;
    reply.transferId = transferId;
    reply.ok      = false;
    reply.errcode = errcode;
    reply.text    = Err::friendlyText(errcode);
    sendJsonTo(session, ProtocolCodec::toJson(reply));
}

// 下载中途失败：清掉传输记录，客户端重新 begin 即可
void NetworkWorker::onDownloadFailed(quint64 sessionId, quint32 transferId, int errcode)
{
    ClientSession *session = findSessionById(sessionId);
    if (!session)
        return;

    FileMessage reply;
    reply.type       = Proto::kFileDownloadBeginOk;
    reply.transferId = transferId;
    reply.ok         = false;
    reply.errcode    = errcode;
    reply.text       = Err::friendlyText(errcode);
    sendJsonTo(session, ProtocolCodec::toJson(reply));

    releaseTransfer(session, transferId);
}

void NetworkWorker::onFileDeleted(quint64 sessionId, const QString &fileId, int errcode)
{
    ClientSession *s = findSessionById(sessionId);
        if (!s)
            return;

    FileMessage m;
    m.type   = Proto::kFileDeleteOk;
    m.fileId = fileId;
    m.ok     = (errcode == Err::Ok);
    m.errcode = errcode;
    m.text   = (errcode == Err::Ok) ? QString() : Err::friendlyText(errcode);
    sendJsonTo(s, ProtocolCodec::toJson(m));
}

void NetworkWorker::onChunkRead(const FileChunk &chunk, bool eof)
{
    const quint64 sid = m_transferOwner.value(chunk.transferId, 0);
    ClientSession *s = findSessionById(sid);
    if (!s)
        return;
    auto netTransfer_1 = s->Netransfers.find(chunk.transferId);
    if (netTransfer_1 == s->Netransfers.end())
        return;

    if (netTransfer_1->inFlight > 0)
        --netTransfer_1->inFlight;

    netTransfer_1->nextReadOffset = qint64(chunk.offset) + chunk.data.size();
    if (eof || netTransfer_1->nextReadOffset >= netTransfer_1->readEnd)
        netTransfer_1->eof = true;

    sendChunkTo(s, chunk.transferId, chunk);

    s = findSessionById(sid);  // 重新取
    if (!s)
        return;
    auto netTransfer_2 = s->Netransfers.find(chunk.transferId);
    if (netTransfer_2 == s->Netransfers.end())
        return;

    // 若这一块因为水位被压在队列里，下一块的请求由后续 flushOut 推进
    if (s->outData.isEmpty())
        maybeRequestNext(s, chunk.transferId);

    if (netTransfer_2->eof && netTransfer_2->inFlight <= 0) {
        releaseTransfer(s, chunk.transferId);
        emit reqDownloadSentComplete(chunk.transferId);
    }
}





