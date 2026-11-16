#include "dbworker.h"
#include "protocoltypes.h"
#include "mysqlfilestore.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QDebug>
#include <QUuid>
#include <QSqlQuery>
#include <QDateTime>
#include <QTimer>

/*
 * DBWorker 线程纪律（破坏即引入乱序 bug）：
 * 1. 所有 DB 槽在本单一线程串行执行——这是消息保序的根基
 * 2. 槽内禁止开线程池、禁止 QtConcurrent、禁止任何异步查询
 * 3. 每个槽末尾同步 emit 结果信号，顺序 = 收到请求的顺序
 * 4. 磁盘 IO 绝不进网络线程和 DB 线程
 */

namespace {
    // 最近会话的预览文案由服务端统一生成，客户端不自己拼
    QString previewText(const ChatMessage &m)
    {
        switch (m.msgType) {
            case Proto::MsgTypeImage: return QStringLiteral("[图片]");
            case Proto::MsgTypeFile:  return QStringLiteral("[文件] ") + m.fileName;
            case Proto::MsgTypeVoice: return QStringLiteral("[语音]");
            case Proto::MsgTypeVideo: return QStringLiteral("[视频]");
            default:                  return m.text;
        }
    }

    // 文件/会话消息 到期默认值，可用环境变量覆盖测试
    constexpr int    kDefaultFileRetentionDays    = 60;   // 60
    constexpr int    kDefaultMessageRetentionDays = 90;   // 90
    constexpr qint64 kMsPerDay                    = 24LL * 60 * 60 * 1000;

    constexpr int    kRetentionBatch           = 500;                 // 每批行数：太小跑得久，太大阻塞 DB 线程
    constexpr int    kRetentionMaxRowsPerSweep = 50000;               // 单轮上限，防止积压太多一次占住 DB 线程
    constexpr int    kRetentionFirstDelayMs    = 5 * 60 * 1000;       // 启动后 5 分钟先跑一次
    constexpr qint64 kRetentionIntervalMs      = 24LL * 60 * 60  * 1000;  // 每 24 小时一次
}

DBWorker::DBWorker(QObject *parent)
    : QObject(parent), m_connectionName(QUuid::createUuid().toString(QUuid::WithoutBraces))
{

}

void DBWorker::closeDatabase()
{
    if(m_userStore) {
        delete m_userStore;
        m_userStore = nullptr;
    }
    if(m_fileStore) {
        delete m_fileStore;
        m_fileStore = nullptr;
    }

    if (!QSqlDatabase::contains(m_connectionName))
        return;

    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isOpen())
            db.close();
    }
    QSqlDatabase::removeDatabase(m_connectionName);
}

DBWorker::~DBWorker()
{
    closeDatabase();
}

// 当需要增加新功能或者重构时， 数据库的数据可以直接删除清空，不用做备份和倒入，因为都是测试数据。
void DBWorker::init()
{
    m_dbOk = false;
    if(!QSqlDatabase::isDriverAvailable("QMYSQL")) {
        emit logMessage(QStringLiteral("QMYSQL 数据库驱动不可用。请确认已安装 Qt MySQL 驱动插件"));
        emit dbReady(false);
        return;
    }

    if (QSqlDatabase::contains(m_connectionName)) {
        closeDatabase();
    }

    QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", m_connectionName);
    if(!db.isValid()) {
        emit logMessage(QStringLiteral("DBWorker init addDatabase fail，数据库驱动名错误或驱动未加载"));
        emit dbReady(false);
        return;
    }

    // 读环境变量 （ 云服务器公网IP 62.234.74.239 ）
    db.setHostName(qEnvironmentVariable("CHAT_DB_HOST", QStringLiteral("127.0.0.1")));
    const int dbPort = qEnvironmentVariableIntValue("CHAT_DB_PORT");
    db.setPort(quint16(dbPort > 0 ? dbPort : 3306));
    db.setDatabaseName(qEnvironmentVariable("CHAT_DB_NAME", QStringLiteral("chat")));
    db.setUserName(qEnvironmentVariable("CHAT_DB_USER", QStringLiteral("chat_user")));
    db.setPassword(qEnvironmentVariable("CHAT_DB_PASSWORD", "2222"));

    db.setConnectOptions(QStringLiteral("MYSQL_OPT_CONNECT_TIMEOUT=5")); // 连接超时（5秒）
    if(!db.open()) {
        emit logMessage(QStringLiteral("数据库连接失败: %1").arg(db.lastError().text()));
        emit dbReady(false);
        return;
    }

    static const char *kCreateUsers =
            "CREATE TABLE IF NOT EXISTS users ("
            "  id             BIGINT AUTO_INCREMENT PRIMARY KEY,"
            "  account_no     BIGINT UNIQUE NOT NULL,"
            "  username       VARCHAR(16) UNIQUE NOT NULL,"
            "  salt           VARCHAR(64) NOT NULL,"
            "  password_hash  VARCHAR(128) NOT NULL,"
            "  status         TINYINT NOT NULL DEFAULT 0,"
            "  nickname       VARCHAR(32) NOT NULL DEFAULT '',"
            "  avatar_file_id VARCHAR(36) NOT NULL DEFAULT '',"
            "  created_at     BIGINT NOT NULL,"
            "  last_login_at  BIGINT NOT NULL DEFAULT 0,"
            "  last_active_at BIGINT NOT NULL DEFAULT 0"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    // account_no  对外账号，8位随机 , 10000000 ~ 99999999
    // UNIQUE 让 username 有唯一约束, 注册时 用户名不能重复
    // last_active_at 客户端最后交互时间 或者 客户端发last心跳包时间（ 心跳包30s发一次 ）
    // InnoDB 支持事务、行级锁、外键、崩溃恢复，现代 MySQL 默认引擎
    // utf8mb4 是 UTF-8 的完整版，能存下所有 Unicode 字符，包括 emoji 中文

    QSqlQuery q(db);
    if (!q.exec(kCreateUsers)) {
        emit logMessage(QStringLiteral("数据库建表失败 users: %1").arg(q.lastError().text()));
        emit dbReady(false);
        return;
    }

    static const char *kCreateMessages =
            "CREATE TABLE IF NOT EXISTS messages ("
            " id            BIGINT AUTO_INCREMENT PRIMARY KEY,"
            " msg_id        VARCHAR(64)  NOT NULL UNIQUE,"
            " client_msg_id VARCHAR(64)  NOT NULL,"
            " sender_uid    BIGINT       NOT NULL,"
            " receiver_uid  BIGINT       NOT NULL,"
            " body          TEXT         NOT NULL,"
            " ts            BIGINT       NOT NULL,"
            " status        TINYINT      NOT NULL DEFAULT 0,"
            " msg_type      TINYINT      NOT NULL DEFAULT 0,"
            " file_id       VARCHAR(36)  NOT NULL DEFAULT '',"
            " file_name     VARCHAR(255) NOT NULL DEFAULT '',"
            " file_size     BIGINT       NOT NULL DEFAULT 0,"
            " UNIQUE KEY uk_dedup (sender_uid, client_msg_id),"
            " KEY idx_pull    (receiver_uid, status, id),"
            " KEY idx_conv_fwd (sender_uid, receiver_uid, ts, id),"
            " KEY idx_conv_rev (receiver_uid, sender_uid, ts, id),"
            " KEY idx_ts (ts)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    // status: 0=等待送达, 1=送达
    // uk_dedup: 消息去重
    // idx_pull: 拉取离线消息
    // idx_conv_fwd idx_conv_rev: 拉取历史消息
    if (!q.exec(kCreateMessages)) {
        emit logMessage(QStringLiteral("数据库建表失败 messages: %1").arg(q.lastError().text()));
        emit dbReady(false);
        return;
    }

    static const char *kCreateFriendRequests =
            "CREATE TABLE IF NOT EXISTS friend_requests ("
            "  id          BIGINT AUTO_INCREMENT PRIMARY KEY,"
            "  from_uid    BIGINT       NOT NULL,"
            "  to_uid      BIGINT       NOT NULL,"
            "  req_text    VARCHAR(64)  NOT NULL DEFAULT '',"
            "  status      TINYINT      NOT NULL DEFAULT 0,"
            "  created_at  BIGINT       NOT NULL,"
            "  handled_at  BIGINT       NOT NULL DEFAULT 0,"
            "  UNIQUE KEY uk_pair  (from_uid, to_uid),"
            "  KEY        idx_inbox (to_uid, status)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    // status: 0=待处理, 1=已通过, 2=已拒绝, 3=已撤回
    // uk_pair 允许 A→B 和 B→A , 同时 pending（两行，互不冲突）
    // 重复申请时，由 UPDATE 回 0 复用旧行
    // idx_inbox 待处理申请: WHERE to_uid=? AND status=0
    if (!q.exec(kCreateFriendRequests)) {
        emit logMessage(QStringLiteral("数据库建表失败 friend_requests: %1").arg(q.lastError().text()));
        emit dbReady(false);
        return;
    }


    static const char *kCreateFriends =
            "CREATE TABLE IF NOT EXISTS friends ("
            "  uid         BIGINT NOT NULL,"
            "  peer_uid    BIGINT NOT NULL,"
            "  created_at  BIGINT NOT NULL,"
            "  PRIMARY KEY (uid, peer_uid),"
            "  KEY idx_peer (peer_uid)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    // 好友关系存双向两行: A 和 B 是好友 => (A,B) + (B,A)
    // 查A的所有好友:     WHERE uid = A（走主键前缀）
    // 查A是否为B的好友:   uid=A, peer_uid=B 主键 O(1)
    if (!q.exec(kCreateFriends)) {
        emit logMessage(QStringLiteral("数据库建表失败 friends: %1").arg(q.lastError().text()));
        emit dbReady(false);
        return;
    }


    static const char *kCreateRecentChat =
            "CREATE TABLE IF NOT EXISTS recentChat ("
            "  uid           BIGINT       NOT NULL,"
            "  peer_uid      BIGINT       NOT NULL,"
            "  last_msg_id   VARCHAR(64)  NOT NULL,"
            "  last_msg_ts   BIGINT       NOT NULL,"
            "  last_msg_text VARCHAR(255) NOT NULL,"
            "  unread_count  INT          NOT NULL DEFAULT 0,"
            "  updated_at    BIGINT       NOT NULL,"
            "  PRIMARY KEY (uid, peer_uid),"
            "  KEY idx_recent (uid, last_msg_ts DESC)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    // last_msg_text 只存预览（服务端插入前截断到 100 字符）
    if (!q.exec(kCreateRecentChat)) {
        emit logMessage(QStringLiteral("数据库建表失败 recentChat: %1").arg(q.lastError().text()));
        emit dbReady(false);
        return;
    }

    static const char *kCreateFiles =
            "CREATE TABLE IF NOT EXISTS files ("
            "  id          BIGINT AUTO_INCREMENT PRIMARY KEY,"
            "  file_id     VARCHAR(36)  NOT NULL UNIQUE,"
            "  sha256      CHAR(64)     NOT NULL,"
            "  size        BIGINT       NOT NULL DEFAULT 0,"
            "  mime        VARCHAR(128) NOT NULL DEFAULT '',"
            "  file_name   VARCHAR(255) NOT NULL DEFAULT '',"
            "  owner_uid   BIGINT       NOT NULL,"
            "  biz         TINYINT      NOT NULL,"
            "  status      TINYINT      NOT NULL DEFAULT 0,"
            "  created_at  BIGINT       NOT NULL,"
            "  ready_at    BIGINT       NOT NULL DEFAULT 0,"
            "  KEY idx_owner (owner_uid, status),"
            "  KEY idx_expire (status, created_at)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    // 1=可用 2=已删除
    // sha256 用于完整性校验记录
    if (!q.exec(kCreateFiles)) {
        emit logMessage(QStringLiteral("数据库建表失败 files: %1").arg(q.lastError().text()));
        emit dbReady(false);
        return;
    }

    static const char *kCreateFileAcl =
            "CREATE TABLE IF NOT EXISTS file_acl ("
            "  file_id       VARCHAR(36) NOT NULL,"
            "  grantee_type  TINYINT     NOT NULL,"
            "  grantee_id    BIGINT      NOT NULL DEFAULT 0,"
            "  granted_by    BIGINT      NOT NULL DEFAULT 0,"
            "  created_at    BIGINT      NOT NULL,"
            "  PRIMARY KEY (file_id, grantee_type, grantee_id),"
            "  KEY idx_grantee (grantee_type, grantee_id)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";
    // 读权限的唯一来源，不从消息表反推：群聊上线只需加 grantee_type=2
    if (!q.exec(kCreateFileAcl)) {
        emit logMessage(QStringLiteral("数据库建表失败 file_acl: %1").arg(q.lastError().text()));
        emit dbReady(false);
        return;
    }


    if (!m_userStore) {
        m_userStore = new MysqlUserStore(m_connectionName);
        connect(m_userStore, &MysqlUserStore::logMessage, this, &DBWorker::logMessage);
    }

    if (!m_fileStore) {
        m_fileStore = new MysqlFileStore(m_connectionName);
        connect(m_fileStore, &MysqlFileStore::logMessage, this, &DBWorker::logMessage);
    }

    m_dbOk = true;
    emit logMessage(QStringLiteral("数据库连接成功"));
    emit dbReady(true);

    // 留存清理：启动 5 分钟后跑一次，之后每 24 小时一次
    if (!m_retentionTimer) {
        m_retentionTimer = new QTimer(this);
        m_retentionTimer->setInterval(int(kRetentionIntervalMs));
        connect(m_retentionTimer, &QTimer::timeout, this, &DBWorker::sweepRetention);
        m_retentionTimer->start();
    }
    QTimer::singleShot(kRetentionFirstDelayMs, this, &DBWorker::sweepRetention);
}

void DBWorker::cleanup()
{
    if (m_retentionTimer)
        m_retentionTimer->stop();
    closeDatabase();
    m_dbOk = false;
    emit logMessage(QStringLiteral("数据库连接已关闭"));
}

// ============================ 留存清理 ============================
// 文件 60 天：只做"标记 status=2", 删盘与删行交给已有的 5 分钟回收巡检
// 消息 90 天：直接分批 DELETE。
// 全程在 DB 线程串行执行，批之间用 singleShot(0) 让出事件循环, 否则一次大 DELETE 会把登录、消息落库全部堵住。

void DBWorker::sweepRetention()
{
    if (m_retentionRunning)
        return;     // 上一轮还没跑完：跳过。每天一次，丢一轮不影响正确性

    if (!m_dbOk || !m_fileStore) {
        emit logMessage(QStringLiteral("数据库未就绪, 留存清理跳过"));
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int fileDays = qEnvironmentVariableIntValue("CHAT_FILE_RETENTION_DAYS");
    int msgDays  = qEnvironmentVariableIntValue("CHAT_MESSAGE_RETENTION_DAYS");
    if (fileDays <= 0)
        fileDays = kDefaultFileRetentionDays;
    if (msgDays  <= 0)
        msgDays  = kDefaultMessageRetentionDays;

    m_retentionFileCutoff    = now - qint64(fileDays) * kMsPerDay;
    m_retentionMessageCutoff = now - qint64(msgDays)  * kMsPerDay;
    m_retentionFileRows      = 0;
    m_retentionMessageRows   = 0;
    m_retentionRunning       = true;

    emit logMessage(QStringLiteral("留存清理开始：\n文件保留 %1 天（早于 %2 的文件将标记过期）\n会话消息保留 %3 天（早于 %4 的消息将删除）")
                    .arg(fileDays).arg(QDateTime::fromMSecsSinceEpoch(m_retentionFileCutoff).
                                       toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                    .arg(msgDays).arg(QDateTime::fromMSecsSinceEpoch(m_retentionMessageCutoff).
                                      toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));

    markFileAndDeleteChatMessages();
}

void DBWorker::markFileAndDeleteChatMessages()
{
    const int rows = m_fileStore->markFileExpired(m_retentionFileCutoff, kRetentionBatch);
    if (rows < 0) {
        emit logMessage(QStringLiteral("markFileAndDeleteChatMessages 文件标记过期失败，终止本轮"));
        retentionFinish();
        return;
    }

    m_retentionFileRows += rows;

    // 还有积压就再来一批：让出一轮事件循环，别把 DB 线程一个人占住
    if (rows >= kRetentionBatch && m_retentionFileRows < kRetentionMaxRowsPerSweep) {
        QTimer::singleShot(0, this, [this]() { markFileAndDeleteChatMessages(); });
        return;
    }

    deleteExpiredMessages(); // 文件阶段结束，进入消息阶段
}

void DBWorker::deleteExpiredMessages()
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("deleteExpiredMessages 数据库未连接，终止本轮"));
        retentionFinish();
        return;
    }

    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM messages WHERE ts < :cutoff LIMIT :lim"));
    q.bindValue(QStringLiteral(":cutoff"), m_retentionMessageCutoff);
    q.bindValue(QStringLiteral(":lim"), kRetentionBatch);
    if (!q.exec()) {
        emit logMessage(QStringLiteral("deleteExpiredMessages 删除消息失败 %1").arg(q.lastError().text()));
        retentionFinish();
        return;
    }

    const int rows = q.numRowsAffected();
    m_retentionMessageRows += rows;

    if (rows >= kRetentionBatch && m_retentionMessageRows < kRetentionMaxRowsPerSweep) {
        QTimer::singleShot(0, this, [this]() { deleteExpiredMessages(); });
        return;
    }

    retentionFinish();
}

void DBWorker::retentionFinish()
{
    m_retentionRunning = false;
    emit logMessage(QStringLiteral("到期清理：%1 个文件被标记到期，%2 个会话消息被删除")
                    .arg(m_retentionFileRows).arg(m_retentionMessageRows));
}

void DBWorker::authLogin(quint64 sessionId, const QString &user, const QString &clientHash)
{
    if (!m_dbOk || !m_userStore) {
        emit authResult(sessionId, 0, user, false, QStringLiteral("服务暂时不可用，请稍后重试"), 0, 0, {},{});
        return;
    }
    const MysqlUserStore::Result r = m_userStore->verifyLogin(user, clientHash);
    if (!r.ok) {
        emit authResult(sessionId, 0, user, false, r.text, 0, 0, {}, {});
        return;
    }

    m_userStore->updateLoginActiveTime(r.uid);
    emit authResult(sessionId, 0, user, true, {}, r.uid, r.accountNo, r.nickname, r.avatarFileId);
}

void DBWorker::authRegister(quint64 sessionId, const QString &user, const QString &clientHash)
{
    if (!m_dbOk || !m_userStore) {
        emit authResult(sessionId, 1, user, false, QStringLiteral("服务暂时不可用，请稍后重试"), 0, 0, {}, {});
        return;
    }
    const MysqlUserStore::Result r = m_userStore->registerUser(user, clientHash);
    emit authResult(sessionId, 1, user, r.ok, r.text, 0, r.accountNo, {}, {});
}

// 查询数据库所有已注册用户
void DBWorker::queryUsers()
{
    QVector<UserInfo> allUsers;
    if (!m_dbOk || !m_userStore) {
        emit usersResult(allUsers);
        return;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if(!db.isOpen()) {
        emit logMessage(QStringLiteral("数据库未连接, 查询数据库失败 queryUsers "));
        emit usersResult(allUsers);
        return;
    }

    QSqlQuery q(db);    // LIMIT 1000
    if(!q.exec("SELECT id, account_no, username, nickname, created_at, last_login_at, last_active_at "
                    "FROM users ORDER BY id LIMIT 1500")) {
        emit logMessage(QStringLiteral("查询数据库失败 queryUsers : %1").arg(q.lastError().text()));
        emit usersResult(allUsers);
        return;
    }
    while(q.next()) {
        UserInfo u;
        u.id            = q.value("id").toLongLong();
        u.accountNo     = q.value("account_no").toLongLong();
        u.username      = q.value("username").toString();
        u.nickname      = q.value("nickname").toString();
        u.createdAt     = q.value("created_at").toLongLong();
        u.lastLoginAt   = q.value("last_login_at").toLongLong();
        u.lastActiveAt  = q.value("last_active_at").toLongLong();
        u.online        = false;
        allUsers.append(u);
    }
    emit usersResult(allUsers);
}

void DBWorker::saveMessage(quint64 sessionId, const ChatMessage &msg)
{
    if(!m_dbOk || !m_userStore) {
        emit messageSaved(sessionId, msg, false, QStringLiteral("消息发送失败，请稍后重试"));
        return;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if(!db.isOpen()) {
        emit messageSaved(sessionId, msg, false, QStringLiteral("消息发送失败，请稍后重试"));
        emit logMessage(QStringLiteral("DBWorker::saveMessage 数据库未连接"));
        return;
    }

    if (msg.senderUid <= 0 || msg.convId <= 0) {
        emit messageSaved(sessionId, msg, false, QStringLiteral("消息发送失败，请重试"));
        emit logMessage(QStringLiteral("DBWorker::saveMessage uid 无效"));
        return;
    }

    // 附件消息：先给接收方授权，再落库。
    // 顺序不能反 —— 一旦消息落库成功、回了"发送成功"，而接收方没有读权限，
    // 对方点开只会得到"文件不存在"，比直接发送失败更让人困惑。
    if (msg.msgType != Proto::MsgTypeText && !msg.fileId.isEmpty()) {
        if (!m_fileStore) {
            emit messageSaved(sessionId, msg, false, QStringLiteral("消息发送失败，请稍后重试"));
            return;
        }
        int grantErr = Err::Unknown;
        if (!m_fileStore->grantForMessage(msg.senderUid, msg.convId, msg.fileId, &grantErr)) {
            emit logMessage(QStringLiteral("saveMessage 附件授权失败 uid=%1 file=%2 errcode=%3")
                                .arg(msg.senderUid).arg(msg.fileId).arg(grantErr));
            emit messageSaved(sessionId, msg, false, Err::friendlyText(grantErr));
            return;
        }
    }

    ChatMessage saved = msg;
    // 纯文本消息这些字段可能根本没赋值（null QString），直接 bind 会绑成 SQL NULL，撞上 NOT NULL 约束。统一归一成空串
    if (saved.fileId.isNull())
        saved.fileId = QStringLiteral("");
    if (saved.fileName.isNull())
        saved.fileName = QStringLiteral("");
    if (saved.fileSize < 0)
        saved.fileSize = 0;
    if (saved.clientMsgId.isNull())
        saved.clientMsgId = QStringLiteral("");
    if (saved.text.isNull())
        saved.text = QStringLiteral("");

    saved.msgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    saved.ts = QDateTime::currentMSecsSinceEpoch();

    QSqlQuery q(db);
    q.prepare("INSERT INTO messages (msg_id, client_msg_id, sender_uid, receiver_uid, body, ts, "
              " msg_type, file_id, file_name, file_size) "
              "VALUES (:smid, :cmid, :from, :to, :body, :time, :mtype, :fid, :fname, :fsize)");
    q.bindValue(":smid", saved.msgId);
    q.bindValue(":cmid", saved.clientMsgId);
    q.bindValue(":from", saved.senderUid);
    q.bindValue(":to",   saved.convId);
    q.bindValue(":body", saved.text);
    q.bindValue(":time", saved.ts);  // 服务器时间覆盖，客户端 message time 不落库
    q.bindValue(":mtype", int(saved.msgType));
    q.bindValue(":fid",   saved.fileId);
    q.bindValue(":fname", saved.fileName);
    q.bindValue(":fsize", saved.fileSize);

    if (!q.exec()) {
        const QSqlError err = q.lastError();
        if (err.nativeErrorCode().toInt() == 1062) { // msg_id 唯一键冲突
            // 消息去重, 重复消息只落库一条
            QSqlQuery q2(db);
            q2.prepare("SELECT msg_id, ts FROM messages "
                       "WHERE sender_uid = :from AND client_msg_id = :cmid");
            // MySQL 优化器会直接选择自定义的 uk_dedup索引 定位数据, 避免全表扫描
            q2.bindValue(":from", saved.senderUid);
            q2.bindValue(":cmid", saved.clientMsgId);
            if(q2.exec() && q2.next()) {
                saved.msgId = q2.value("msg_id").toString();
                saved.ts = q2.value("ts").toLongLong();
                emit messageSaved(sessionId, saved, true, {});
                return;
            }
            // 1062 但不是 uk_dedup 冲突（比如 msg_id 撞了），几乎不可能发生
            emit logMessage(QStringLiteral("saveMessage[%1] 1062 但未查到原记录: %2")
                                            .arg(saved.clientMsgId, err.text()));
            emit messageSaved(sessionId, saved, false, QStringLiteral("消息发送失败，请稍后重试"));
            return;
        }
        emit logMessage(QStringLiteral("saveMessage[%1] 失败: %2")
                                    .arg(saved.clientMsgId, err.text()));
        emit messageSaved(sessionId, saved, false, QStringLiteral("消息发送失败，请稍后重试"));
        return;
    }

    const QString preview = previewText(saved);
    m_userStore->upsertRecentChat(saved.senderUid, saved.convId, saved.msgId, saved.ts, preview, 0, 0);
    m_userStore->upsertRecentChat(saved.convId, saved.senderUid, saved.msgId, saved.ts, preview, 1, 1);
    emit messageSaved(sessionId, saved, true, {});
}

void DBWorker::loadofflineMsg(quint64 sessionId, qint64 uid)
{
    QVector<ChatMessage> msgs;
    if(!m_dbOk || !m_userStore || uid <= 0 ) {
        emit offlineMsgLoaded(sessionId, msgs);
        return;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if(!db.isOpen()) {
        emit logMessage(QStringLiteral("DBWorker::loadofflineMsg 数据库未连接"));
        emit offlineMsgLoaded(sessionId, msgs);
        return;
    }

    // 只推最近的未读（新→旧），超出上限的由历史拉取兜底
    QSqlQuery q(db);
    q.prepare("SELECT m.msg_id, m.client_msg_id, m.sender_uid, m.receiver_uid, m.body, m.ts, "
              " m.msg_type, m.file_id, m.file_name, m.file_size, "
              " u.nickname AS from_nick "
              "FROM messages m "
              "LEFT JOIN users u ON u.id = m.sender_uid "
              "WHERE m.receiver_uid = :me AND m.status = 0 "
              "ORDER BY m.id DESC LIMIT :limit");
    q.bindValue(":me" , uid);
    q.bindValue(":limit" , Proto::kOfflinePullLimit);
    if(!q.exec()) {
        emit logMessage(QStringLiteral("loadofflineMsg user:%1 拉取离线消息失败 %2").
                                        arg(uid).arg(q.lastError().text()));
        emit offlineMsgLoaded(sessionId, msgs);
        return;
    }
    while(q.next()) {
        ChatMessage m;
        m.msgId       = q.value("msg_id").toString();
        m.clientMsgId = q.value("client_msg_id").toString();
        m.senderUid   = q.value("sender_uid").toLongLong();
        m.convId = q.value("receiver_uid").toLongLong();
        m.fromNick    = q.value("from_nick").toString();
        m.text        = q.value("body").toString();
        m.ts          = q.value("ts").toLongLong();
        m.msgType     = quint8(q.value("msg_type").toInt());
        m.fileId      = q.value("file_id").toString();
        m.fileName    = q.value("file_name").toString();
        m.fileSize    = q.value("file_size").toLongLong();
        msgs.append(m);
    }
    if (msgs.size() == Proto::kOfflinePullLimit) {
        emit logMessage(QStringLiteral("uid=%1 未读离线消息达到 %2 条上限，可能存在更早的未拉取消息，由客户端拉取历史消息补齐")
                        .arg(uid).arg(Proto::kOfflinePullLimit));
    }
    emit offlineMsgLoaded(sessionId, msgs);
}

// 标记信息已拉取
void DBWorker::markDelivered(const QStringList &msgIds)
{
    if (msgIds.isEmpty() || !m_dbOk || !m_userStore)
            return;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if(!db.isOpen()) {
        emit logMessage(QStringLiteral("markDelivered: 数据库未连接"));
        return;
    }

    // 构造 IN 占位符：:msg0, :msg1, 用索引msg_id 更新 status
    QStringList p;
    p.reserve(msgIds.size());
    for(int i = 0; i < msgIds.size(); ++i)
        p << QStringLiteral(":msg%1").arg(i);
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE messages SET status = 1 WHERE msg_id IN (%1)").arg(p.join(",")));
    for(int i = 0; i < msgIds.size(); ++i)
        q.bindValue(p[i], msgIds[i]);
    if(!q.exec()) {
        emit logMessage(QStringLiteral("markDelivered 失败: ") + q.lastError().text());
        // 失败就下次登录时重新拉消息（status 保持 0）
    }
}

void DBWorker::loadHistoryMsg(quint64 sessionId, qint64 me, qint64 peerUid, const QString &beforeMsgId, int limit)
{
    HistoryPage r;
    if (!m_dbOk || !m_userStore) {
        r.convId = peerUid;
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
        emit historyMsgLoaded(sessionId, r);
        return;
    }
    r = m_userStore->loadHistoryMsg(me, peerUid, beforeMsgId, limit);
    emit historyMsgLoaded(sessionId, r);
}

// 修改昵称
void DBWorker::updateNickname(quint64 sessionId, const QString &username, const QString &newNick)
{
    if(!m_dbOk) {
        emit nicknameUpdateResult(sessionId, username, false, {}, QStringLiteral("服务暂时不可用，请稍后重试"));
        return;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit nicknameUpdateResult(sessionId, username, false, {}, QStringLiteral("服务暂时不可用，请稍后重试"));
        return;
    }
    QSqlQuery q(db);
    q.prepare("UPDATE users SET nickname = :n WHERE username = :u");
    q.bindValue(":n", newNick);
    q.bindValue(":u", username);
    if(!q.exec()) {
        emit logMessage(QStringLiteral("updateNickname 修改昵称失败: %1").arg(q.lastError().text()));
        emit nicknameUpdateResult(sessionId, username, false, {}, QStringLiteral("修改昵称失败，请稍后重试"));
        return;
    }
    emit nicknameUpdateResult(sessionId, username, true, newNick, {});
}

// 修改密码：校验、换盐、落库都在 userStore 里做，这里只负责线程切换和回执
void DBWorker::changePassword(quint64 sessionId, qint64 uid,
                              const QString &oldPassWordHash, const QString &newPassWordHash)
{
    if (!m_dbOk || !m_userStore) {
        emit passwordChangeResult(sessionId, false, Err::ServiceUnavailable);
        return;
    }

    int errcode = Err::Unknown;
    const bool ok = m_userStore->changePassword(uid, oldPassWordHash, newPassWordHash, &errcode);
    emit passwordChangeResult(sessionId, ok, ok ? Err::Ok : errcode);
}

void DBWorker::updateAvatar(quint64 sessionId, qint64 uid, const QString &fileId)
{
    if (!m_dbOk || !m_userStore) {
        emit avatarUpdateResult(sessionId, false, Err::ServiceUnavailable, {});
        return;
    }

    const QString oldFileId = m_userStore->avatarFileIdOf(uid);

    int errcode = Err::Unknown;
    const bool ok = m_userStore->setAvatar(uid, fileId, &errcode);
    if (!ok) {
        emit avatarUpdateResult(sessionId, false, errcode, {});
        return;
    }

    // 旧头像只做"标记删除"（files.status = 2），物理文件交给回收巡检。不在这里直接删盘，因为同一份内容可能被别的用户共享（内容寻址去重）。
    if (!oldFileId.isEmpty() && oldFileId != fileId && m_fileStore) {
        int ignored = Err::Ok;
        if (m_fileStore->markDeleted(uid, oldFileId, &ignored))
            emit logMessage(QStringLiteral("uid=%1 旧头像已标记删除 file_id=%2").arg(uid).arg(oldFileId));
        else
            emit logMessage(QStringLiteral("uid=%1 旧头像标记失败 file_id=%2 errcode=%3").arg(uid).arg(oldFileId).arg(ignored));
    }

    emit avatarUpdateResult(sessionId, true, Err::Ok, fileId);
}


void DBWorker::updateLastActiveTime(qint64 uid, qint64 ts)
{
    if(!m_dbOk || !m_userStore)
        return;
    m_userStore->updateLastActiveTime(uid, ts);
}

void DBWorker::searchUser(quint64 sessionId, qint64 accountNo)
{
    SearchResult user;
    user.accountNo = accountNo;
    if (!m_dbOk || !m_userStore) {
        emit userSearchResult(sessionId, false, QStringLiteral("服务暂时不可用，请稍后重试"), user);
        return;
    }
    const MysqlUserStore::Result r = m_userStore->searchUserByAccountNo(accountNo);
    if (r.ok) {
        user.uid    = r.uid;
        user.nick   = r.nickname;
        user.online = false;
    }
    emit userSearchResult(sessionId, r.ok, r.text, user);
}

void DBWorker::sendAddFriendRequest(quint64 sessionId, qint64 fromUid, qint64 toAccountNo, const QString &text)
{
    if (!m_dbOk || !m_userStore) {
        emit friendRequestResult(sessionId, false, QStringLiteral("服务暂时不可用，请稍后重试"), 0);
        return;
    }
    const MysqlUserStore::Result r = m_userStore->sendAddFriendRequest(fromUid, toAccountNo, text);
    emit friendRequestResult(sessionId, r.ok, r.text, r.toUid);
}

// 待处理申请
void DBWorker::listPendingRequests(quint64 sessionId, qint64 uid)
{
    QVector<FriendRequestInfo> friendRequest;
    if (m_dbOk && m_userStore && uid > 0)
        friendRequest = m_userStore->listPendingRequests(uid);
    emit pendingRequestsResult(sessionId, friendRequest);
}

// 好友列表
void DBWorker::listFriends(quint64 sessionId, qint64 uid)
{
    QVector<SearchResult> friends;
    if (m_dbOk && m_userStore && uid > 0)
        friends = m_userStore->listFriends(uid);
    emit listfriendsResult(sessionId, friends);
}

void DBWorker::handleFriendRequest(quint64 sessionId, qint64 reqId, qint64 selfUid, bool accept)
{
    MysqlUserStore::HandleFriendResult r;
    r.reqId = reqId;
    r.selfUid = selfUid;
    if (!m_dbOk || !m_userStore) {
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
    } else {
        r = m_userStore->handleFriendRequest(reqId, selfUid, accept);
    }
    emit friendHandleResult(sessionId, r);
}

void DBWorker::removeFriend(quint64 sessionId, qint64 uid, qint64 removeUid)
{
    MysqlUserStore::RemoveFriendResult r;
    if (!m_dbOk || !m_userStore) {
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
    } else {
        r = m_userStore->removeFriend(uid, removeUid);
    }
    emit removeFriendResult(sessionId, r);
}

void DBWorker::listRecentChat(quint64 sessionId, qint64 uid)
{
    RecentChatResult r;
    if (m_dbOk && m_userStore && uid > 0)
        r = m_userStore->listRecentChat(uid);
    emit recentChatLoaded(sessionId, r);
}

void DBWorker::markRecentChatRead(qint64 uid, qint64 peerUid)
{
    if (!m_dbOk || !m_userStore) {
        emit logMessage(QStringLiteral("markRecentChatRead 数据库未就绪 uid=%1 peer=%2").arg(uid).arg(peerUid));
        return;
    }
    if (!m_userStore->clearUnread(uid, peerUid)) {
        emit logMessage(QStringLiteral("markRecentChatRead 落库失败 uid=%1 peer=%2").arg(uid).arg(peerUid));
    }
}

void DBWorker::loadFileForRead(quint32 transferId, qint64 uid, const QString &fileId)
{
    if (!m_dbOk || !m_fileStore) {
        emit fileReadableResult(transferId, false, Err::ServiceUnavailable, FileMeta());
        return;
    }
    FileMeta meta;
    int errcode = Err::Unknown;
    const bool ok = m_fileStore->loadForRead(uid, fileId, &meta, &errcode);
    emit fileReadableResult(transferId, ok, ok ? Err::Ok : errcode, meta);
}

void DBWorker::markFileDeleted(qint64 uid, const QString &fileId)
{
    if (!m_dbOk || !m_fileStore) {
        emit fileDeleteResult(fileId, false, Err::ServiceUnavailable);
        return;
    }
    int errcode = Err::Unknown;
    const bool ok = m_fileStore->markDeleted(uid, fileId, &errcode);
    emit fileDeleteResult(fileId, ok, ok ? Err::Ok : errcode);
}

void DBWorker::commitFile(quint32 transferId, const FileMeta &meta)
{
    if (!m_dbOk || !m_fileStore) {
        emit commitFileResult(transferId, false, Err::ServiceUnavailable);
        return;
    }
    int errcode = Err::Unknown;
    const bool ok = m_fileStore->commitFile(meta, &errcode);
    emit commitFileResult(transferId, ok, ok ? Err::Ok : errcode);
}

void DBWorker::queryReclaimableObjects(int limit)
{
    QVector<ReclaimableObject> list;
    if (m_dbOk && m_fileStore)
        list = m_fileStore->listReclaimableObjects(limit);
    emit reclaimableObjectsResult(list);
}

void DBWorker::dropReclaimedRows(quint8 biz, const QString &sha256)
{
    if (!m_dbOk || !m_fileStore) {
        emit reclaimedRowsDropped(biz, sha256, false, Err::ServiceUnavailable);
        return;
    }
    int errcode = Err::Unknown;
    const bool ok = m_fileStore->dropReclaimedRows(biz, sha256, &errcode);
    emit reclaimedRowsDropped(biz, sha256, ok, ok ? Err::Ok : errcode);
}

void DBWorker::queryLiveObjects()
{
    QSet<QString> live;

    if (!m_dbOk || !m_fileStore) {
        emit liveObjectsResult(false, live);   // ← 失败就是失败，不能当"空集合"发出去
        return;
    }

    QString err;
    const bool ok = m_fileStore->listLiveObjects(&live, &err);
    if (!ok)
        emit logMessage(QStringLiteral("磁盘对账：查询活对象失败 %1").arg(err));
    emit liveObjectsResult(ok, live);
}

