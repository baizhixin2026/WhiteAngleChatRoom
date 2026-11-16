#include "mysqluserstore.h"
#include "protocoltypes.h"

#include <QRandomGenerator>
#include <QPasswordDigestor>
#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

// 密码工具函数
namespace {
    // 生成16字节随机salt(盐: 给密码加随机附加值), 让相同的密码产生不同的哈希结果，防止彩虹表攻击和批量破解。
    QString generateSalt() {
        QByteArray salt(16, 0);
        for(int i = 0; i < salt.size(); i++)
            salt[i] = char(QRandomGenerator::global()->bounded(256));
        return QString::fromLatin1(salt.toBase64());    // toBase64 把任意二进制安全地表示成"安全字符"
    }

    // 客户端哈希（SHA256 的十六进制字符串）→ PBKDF2 派生密钥, 迭代轮数 100000, 输出长度：32字节 * 8 = 256位
    QString deriveHash(const QString &clientHash, const QString &saltBase64) {
        const QByteArray salt = QByteArray::fromBase64(saltBase64.toLatin1());
        const QByteArray derived = QPasswordDigestor::deriveKeyPbkdf2(
                    QCryptographicHash::Sha256, clientHash.toUtf8(), salt, 100000, 32);
        return QString::fromLatin1(derived.toBase64());
    }
}

namespace {
    inline qint64 setAccountNo()   // 8位对外账号: [10000000, 99999999]
    {
        return Proto::kAccountNoMin + qint64(QRandomGenerator::global()->bounded(90000000));
    }
}

MysqlUserStore::MysqlUserStore(const QString &connectionName)
    : m_connectionName(connectionName)
{

}

MysqlUserStore::Result MysqlUserStore::verifyLogin(const QString &user, const QString &clientHash)
{
    Result r;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if(!db.isOpen()) {
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
        emit logMessage(QStringLiteral("user:%1 verifyLogin: %2").arg(user, db.lastError().text()));
        return r;
    }
    QSqlQuery q(db);
    q.prepare("SELECT id, account_no, salt, password_hash, status, nickname , avatar_file_id "
                                    "FROM users WHERE username = :u");
    q.bindValue(":u", user);
    if(!q.exec()) {
        r.text    = QStringLiteral("服务暂时不可用，请稍后重试");
        emit logMessage(QStringLiteral("user:%1 verifyLogin: %2").arg(user, db.lastError().text()));
        return r;
    }
    if(!q.next()) {
        r.text = QStringLiteral("登录账号不存在");
        return r;
    }

    const qint64 uid = q.value("id").toLongLong();
    const qint64 accountNo = q.value("account_no").toLongLong();
    const QString salt = q.value("salt").toString();
    const QString storedHash = q.value("password_hash").toString();
    const int status = q.value("status").toInt();
    const QString nickname = q.value("nickname").toString();
    const QString avatarFileId = q.value("avatar_file_id").toString();
    if(status != 0) {
        r.text = QStringLiteral("账号已被封禁");
        emit logMessage(QStringLiteral("user:%1 账号已被封禁, 尝试再次登录").arg(user));
        return r;
    }
    const QString hash = deriveHash(clientHash, salt);
    if (hash != storedHash) {
        r.text = QStringLiteral("登录密码错误");
        return r;
    }
    r.ok = true;
    r.accountNo = accountNo;
    r.uid = uid;
    r.nickname = nickname.isEmpty() ? user : nickname;
    r.avatarFileId = avatarFileId;
    return r;
}

MysqlUserStore::Result MysqlUserStore::registerUser(const QString &user, const QString &clientHash)
{
    Result r;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("user:%1 registerUser: %2").arg(user, db.lastError().text()));
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
        return r;
    }
    const QString salt = generateSalt();
    const QString hash = deriveHash(clientHash, salt);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int kMaxRetry = 5;    // 账号碰撞重试上限

    QSqlQuery q(db);
    q.prepare("INSERT INTO users (account_no, username, salt, password_hash, nickname, created_at) "
              "VALUES (:a, :u, :s, :h, :n, :t)");
    for(int i = 0; i < kMaxRetry; ++i) {
        qint64 accountNo = setAccountNo();
        q.bindValue(":a", accountNo);
        q.bindValue(":u", user);
        q.bindValue(":s", salt);
        q.bindValue(":h", hash);
        q.bindValue(":n", user);
        q.bindValue(":t", now);
        if(q.exec()) {
            r.ok = true;
            r.accountNo = accountNo;
            return r;
        }
        const QSqlError err = q.lastError();
        if (err.nativeErrorCode().toInt() == 1062) {
            // 1062 有两种可能: username 碰撞 或 account_no 账号碰撞
            QSqlQuery checkUserName(db);
            checkUserName.prepare("SELECT 1 FROM users WHERE username = :username");
            checkUserName.bindValue(":username", user);
            if(checkUserName.exec() && checkUserName.next()) {
                emit logMessage(QStringLiteral("user:%1 注册失败，用户名已存在").arg(user));
                r.text = QStringLiteral("注册失败，用户名已存在");
                return r;
            }
            continue;   // account_no 账号碰撞
        }
        emit logMessage(QStringLiteral("user:%1 用户名注册失败 %2: %3").arg(user, err.nativeErrorCode(), err.text()));
        r.text = QStringLiteral("注册失败, 请稍后重试");
        return r;
    }
    emit logMessage(QStringLiteral("user:%1 用户注册失败，服务端随机生成的账号，连续5次碰撞！！").arg(user));
    r.text = QStringLiteral("注册失败, 请稍后重试");
    return r;
}

bool MysqlUserStore::changePassword(qint64 uid, const QString &oldPassWordHash,
                            const QString &newPassWordHash, int *errcode)
{
    if (errcode)
        *errcode = Err::Unknown;

    if (uid <= 0 || oldPassWordHash.isEmpty() || newPassWordHash.isEmpty()) {
        if (errcode)
            *errcode = Err::InvalidParam;
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("uid:%1 changePassword 数据库未连接: %2").arg(uid).arg(db.lastError().text()));
        if (errcode) *errcode = Err::ServiceUnavailable;
        return false;
    }

    QSqlQuery q(db);
    q.prepare("SELECT salt, password_hash, status FROM users WHERE id = :id");
    q.bindValue(":id", uid);
    if (!q.exec()) {
        emit logMessage(QStringLiteral("uid:%1 changePassword 查询失败: %2").arg(uid).arg(q.lastError().text()));
        if (errcode)
            *errcode = Err::DbError;
        return false;
    }
    if (!q.next()) {
        emit logMessage(QStringLiteral("uid:%1 changePassword 账号不存在").arg(uid));
        if (errcode)
            *errcode = Err::NotAuthenticated;
        return false;
    }

    const QString oldSalt    = q.value("salt").toString();
    const QString storedHash = q.value("password_hash").toString();
    if (q.value("status").toInt() != 0) {
        emit logMessage(QStringLiteral("uid:%1 changePassword 账号已被封禁").arg(uid));
        if (errcode)
            *errcode = Err::AccountBanned;
        return false;
    }

    if (deriveHash(oldPassWordHash, oldSalt) != storedHash) {
        emit logMessage(QStringLiteral("uid:%1 changePassword 旧密码输入错误").arg(uid));
        if (errcode)
            *errcode = Err::PasswordIncorrect;
        return false;
    }

    const QString newSalt = generateSalt();
    const QString newHash = deriveHash(newPassWordHash, newSalt);

    q.prepare("UPDATE users SET salt = :s, password_hash = :h WHERE id = :id AND password_hash = :old");
    q.bindValue(":s", newSalt);
    q.bindValue(":h", newHash);
    q.bindValue(":id", uid);
    q.bindValue(":old", storedHash);
    if (!q.exec()) {
        emit logMessage(QStringLiteral("uid:%1 changePassword 更新失败: %2").arg(uid).arg(q.lastError().text()));
        if (errcode)
            *errcode = Err::DbError;
        return false;
    }
    if (q.numRowsAffected() != 1) {
        // 读到旧值之后、写回之前，密码已经被另一条路径改掉了
        emit logMessage(QStringLiteral("uid:%1 changePassword 条件更新未命中，密码已被并发修改").arg(uid));
        if (errcode)
            *errcode = Err::PasswordIncorrect;
        return false;
    }

    if (errcode)
        *errcode = Err::Ok;
    return true;
}

void MysqlUserStore::updateLoginActiveTime(qint64 uid)
{
    if(uid <= 0)
        return ;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if(!db.isOpen()) {
        emit logMessage(QStringLiteral("uid:%1 touchLastLogin: %2").arg(uid).arg( db.lastError().text()));
        return;
    }
    QSqlQuery q(db);
    q.prepare("UPDATE users SET last_login_at = :t, last_active_at = :t WHERE id = :id");
    q.bindValue(":t", QDateTime::currentMSecsSinceEpoch());
    q.bindValue(":id", uid);
    if (!q.exec()) {
        emit logMessage(QStringLiteral("uid:%1 touchLastLogin: %2").arg(uid).arg( db.lastError().text()));
    }
}

void MysqlUserStore::updateLastActiveTime(qint64 uid, qint64 ts)
{
    if(uid <= 0 || ts <= 0)
        return;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if(!db.isOpen()) {
        emit logMessage(QStringLiteral("uid:%1 updateLastActiveTime: %2").arg(uid).arg(db.lastError().text()));
        return;
    }

    QSqlQuery q(db);
    q.prepare("UPDATE users SET last_active_at = :t WHERE id = :id AND last_active_at < :t2");
    q.bindValue(":t",  ts);
    q.bindValue(":t2", ts);
    q.bindValue(":id", uid);
    if(!q.exec())
        emit logMessage(QStringLiteral("uid:%1 updateLastActiveTime fail: %2") .arg(uid).arg(q.lastError().text()));
}

MysqlUserStore::Result MysqlUserStore::searchUserByAccountNo(qint64 accountNo)
{
    Result r;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
        emit logMessage(QStringLiteral("searchUserByAccountNo: 数据库未连接"));
        return r;
    }

    QSqlQuery q(db);
    q.prepare("SELECT id, nickname, avatar_file_id, status FROM users WHERE account_no = :a");
    q.bindValue(":a", accountNo);
    if (!q.exec()) {
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
        emit logMessage(QStringLiteral("searchUserByAccountNo 查询失败: %1").arg(q.lastError().text()));
        return r;
    }
    if (!q.next()) {
        r.text = QStringLiteral("账号不存在");
        return r;
    }
    if (q.value("status").toInt() != 0) {
        r.text = QStringLiteral("此账号异常");
        return r;
    }

    r.ok       = true;
    r.uid      = q.value("id").toLongLong();
    r.nickname = q.value("nickname").toString();
    r.avatarFileId = q.value("avatar_file_id").toString();
    return r;
}

MysqlUserStore::Result MysqlUserStore::sendAddFriendRequest(qint64 fromUid, qint64 toAccountNo, const QString &text)
{
    Result r;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
        emit logMessage(QStringLiteral("sendFriendRequest 数据库未连接"));
        return r;
    }

    // account_no → to_uid
    qint64 toUid = 0;
    {
        QSqlQuery q(db);
        q.prepare("SELECT id FROM users WHERE account_no = :a AND status = 0");
        q.bindValue(":a", toAccountNo);
        if (!q.exec()) {
            r.text = QStringLiteral("服务暂时不可用，请稍后重试");
            emit logMessage(QStringLiteral("sendFriendRequest 查询账号失败: %1").arg(q.lastError().text()));
            return r;
        }
        if (!q.next()) {
            r.text = QStringLiteral("账号不存在");
            return r;
        }
        toUid = q.value("id").toLongLong();
    }

    if (toUid == fromUid) {
        r.text = QStringLiteral("不能添加自己为好友");
        return r;
    }

    // 已经是好友
    {
        QSqlQuery q(db);
        q.prepare("SELECT 1 FROM friends WHERE uid = :u AND peer_uid = :p");
        q.bindValue(":u", fromUid);
        q.bindValue(":p", toUid);
        if (!q.exec()) {
            r.text = QStringLiteral("服务暂时不可用，请稍后重试");
            emit logMessage(QStringLiteral("sendFriendRequest 查好友失败: %1").arg(q.lastError().text()));
            return r;
        }
        if (q.next()) {
            r.text = QStringLiteral("你们已经是好友");
            return r;
        }
    }

    //  双向 pending 两端各一行， 不允许重复发送申请， status 0 待处理
    {
        QSqlQuery q(db);
        q.prepare("SELECT from_uid FROM friend_requests "
                  "WHERE status = 0 AND "
                  "((from_uid = :a AND to_uid = :b) OR (from_uid = :b AND to_uid = :a))");
        q.bindValue(":a", fromUid);
        q.bindValue(":b", toUid);
        if (!q.exec()) {
            r.text = QStringLiteral("服务暂时不可用，请稍后重试");
            emit logMessage(QStringLiteral("sendFriendRequest 查申请失败: %1").arg(q.lastError().text()));
            return r;
        }
        if (q.next()) {
            const qint64 pendFrom = q.value("from_uid").toLongLong();
            r.text = (pendFrom == fromUid)
                     ? QStringLiteral("已发送过申请，等待对方处理")
                     : QStringLiteral("对方已向你发起申请，请在好友申请列表中处理");
            return r;
        }
    }

    // INSERT 申请；撞 uk_pair 说明历史行存在（曾拒绝/曾撤回/曾通过后被删），
    // 复用旧行并重置 created_at / handled_at —— 这是"没有自动通过分支"的前提
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare("INSERT INTO friend_requests (from_uid, to_uid, req_text, status, created_at) "
              "VALUES (:f, :t, :x, 0, :ts)");
    q.bindValue(":f", fromUid);
    q.bindValue(":t", toUid);
    q.bindValue(":x", text);
    q.bindValue(":ts", now);
    if (q.exec()) {
        r.ok = true;
        r.toUid = toUid;
        r.text = QStringLiteral("申请已发送，等待对方处理");
        return r;
    }

    const QSqlError err = q.lastError();
    if (err.nativeErrorCode().toInt() != 1062) {
        emit logMessage(QStringLiteral("sendFriendRequest INSERT 失败: %1").arg(err.text()));
        r.text = QStringLiteral("申请发送失败，请稍后重试");
        return r;
    }

    // 1062: uk_pair 冲突, status != 0 -> status = 0, 复用旧行，重置为一条全新的待处理申请
    QSqlQuery q2(db);
    q2.prepare("UPDATE friend_requests SET status = 0, req_text = :x, created_at = :ts, handled_at = 0 "
               "WHERE from_uid = :f AND to_uid = :t");
    q2.bindValue(":x", text);
    q2.bindValue(":ts", now);
    q2.bindValue(":f", fromUid);
    q2.bindValue(":t", toUid);
    if (!q2.exec()) {
        emit logMessage(QStringLiteral("sendFriendRequest fromUid:%1 重复申请失败: %2")
                                                .arg(fromUid).arg(q2.lastError().text()));
        r.text = QStringLiteral("申请发送失败，请稍后重试");
        return r;
    }
    r.ok = true;
    r.toUid = toUid;
    r.text = QStringLiteral("申请已发送，等待对方处理");
    return r;
}

// 所有待处理请求
QVector<FriendRequestInfo> MysqlUserStore::listPendingRequests(qint64 toUid)
{
    QVector<FriendRequestInfo> friendRequest;
    if(toUid <= 0)
        return friendRequest;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("listPendingRequests: 数据库未连接"));
        return friendRequest;
    }

    // 同时查两张表, 查询所有“发给我的、状态为待处理”的好友请求, 最新的请求排前面DESC 取前150条
    QSqlQuery q(db);
    q.prepare( "SELECT fr.id, fr.from_uid, fr.req_text, fr.created_at, u.nickname "
               "FROM friend_requests fr "
               "JOIN users u ON u.id = fr.from_uid "
               "WHERE fr.to_uid = :me AND fr.status = 0 "
               "ORDER BY fr.created_at DESC LIMIT 150");
    q.bindValue(":me", toUid);
    if (!q.exec()) {
        emit logMessage(QStringLiteral("listPendingRequests 同时查两张表失败: %1").arg(q.lastError().text()));
        return friendRequest;
    }
    while (q.next()) {
        FriendRequestInfo info;
        info.reqId      = q.value("id").toLongLong();
        info.fromUid    = q.value("from_uid").toLongLong();
        info.fromNick   = q.value("nickname").toString();
        info.text       = q.value("req_text").toString();
        info.createdAt  = q.value("created_at").toLongLong();
        friendRequest.append(info);
    }
    return friendRequest;
}

// 好友列表 LIMIT 500
QVector<SearchResult> MysqlUserStore::listFriends(qint64 uid)
{
    QVector<SearchResult> friends;
    if (uid <= 0)
        return friends;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("listFriends: 数据库未连接"));
        return friends;
    }
    QSqlQuery q(db);
    q.prepare(
            "SELECT f.peer_uid, u.account_no, u.nickname, u.avatar_file_id "
            "FROM friends f JOIN users u ON u.id = f.peer_uid "
            "WHERE f.uid = :me ORDER BY f.created_at ASC LIMIT 500");
    q.bindValue(":me", uid);
    if (!q.exec()) {
        emit logMessage(QStringLiteral("listFriends 查询失败: %1").arg(q.lastError().text()));
        return friends;
    }
    while (q.next()) {
        SearchResult f;
        f.uid = q.value("peer_uid").toLongLong();
        f.accountNo = q.value("account_no").toLongLong();
        f.nick = q.value("nickname").toString();
        f.avatarFileId = q.value("avatar_file_id").toString();
        f.online = false;
        friends.append(f);
    }
    return friends;
}

MysqlUserStore::HandleFriendResult MysqlUserStore::handleFriendRequest(qint64 reqId, qint64 selfUid, bool accept)
{
    HandleFriendResult r;
    r.reqId = reqId;
    r.selfUid = selfUid;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
        emit logMessage(QStringLiteral("handleFriendRequest: 数据库未连接"));
        return r;
    }

    if (!db.transaction()) {
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
        emit logMessage(QStringLiteral("handleFriendRequest: transaction 失败"));
        return r;
    }

    // 锁定申请行（FOR UPDATE），校验归属+状态
    qint64 fromUid = 0;
    int status = -1;
    {
        QSqlQuery q(db);
        q.prepare("SELECT from_uid, status FROM friend_requests WHERE id = :id AND to_uid = :me FOR UPDATE");
        q.bindValue(":id", reqId);
        q.bindValue(":me", selfUid);
        if (!q.exec()) {
            db.rollback();
            r.text = QStringLiteral("服务暂时不可用，请稍后重试");
            emit logMessage(QStringLiteral("handleFriendRequest SELECT 失败: %1").arg(q.lastError().text()));
            return r;
        }
        if (!q.next()) {
            db.rollback();
            r.text = QStringLiteral("该申请已失效，请刷新后重试");
            return r;
        }
        fromUid = q.value("from_uid").toLongLong();
        status  = q.value("status").toInt();
    }

    if(status != 0) {
        db.rollback();
        r.text = (status == 1) ? QStringLiteral("您已同意该申请") : QStringLiteral("该申请您已经处理过了");
    }

    r.fromUid = fromUid;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 拒绝
    if(!accept) {
        QSqlQuery q(db);
        q.prepare("UPDATE friend_requests SET status = 2, handled_at = :ts WHERE id = :id");
        q.bindValue(":ts", now);
        q.bindValue(":id", reqId);
        if(!q.exec()) {
            db.rollback();
            r.text = QStringLiteral("服务暂时不可用，请稍后重试");
            emit logMessage(QStringLiteral("handleFriendRequest 拒绝失败: %1").arg(q.lastError().text()));
            return r;
        }
        if (!db.commit()) {
            r.text = QStringLiteral("服务暂时不可用，请稍后重试");
            return r;
        }
        r.ok = true;
        r.accepted = false;
        r.text = QStringLiteral("已拒绝");
        return r;
    }

    // 同意
    int selfCount = 0, fromCount = 0;
    {
        QSqlQuery q(db);
        q.prepare("SELECT (SELECT COUNT(*) FROM friends WHERE uid = :me)  AS c_self, "
                  "(SELECT COUNT(*) FROM friends WHERE uid = :you) AS c_from");
        q.bindValue(":me", selfUid);
        q.bindValue(":you", fromUid);
        if (!q.exec() || !q.next()) {
            db.rollback();
            r.text = QStringLiteral("服务暂时不可用，请稍后重试");
            return r;
        }
        selfCount = q.value("c_self").toInt();
        fromCount = q.value("c_from").toInt();
    }
    if(selfCount >= Proto::kMaxFriends) {
        db.rollback();
        r.text = QStringLiteral("你的好友数,已达上限 %1").arg(Proto::kMaxFriends);
        return r;
    }
    if (fromCount >= Proto::kMaxFriends) {
        db.rollback();
        r.text = QStringLiteral("对方好友数,已达上限 %1，无法申请添加").arg(Proto::kMaxFriends);
        return r;
    }

    // 更新好友申请表
    {
        QSqlQuery q(db);
        q.prepare("UPDATE friend_requests SET status = 1, handled_at = :ts "
                  "WHERE status = 0 AND ((from_uid = :a AND to_uid = :b) OR  (from_uid = :b AND to_uid = :a))");
        q.bindValue(":ts", now);
        q.bindValue(":a", selfUid);
        q.bindValue(":b", fromUid);
        if (!q.exec()) {
           db.rollback();
           r.text = QStringLiteral("服务暂时不可用，请稍后重试");
           emit logMessage(QStringLiteral("handleFriendRequest UPDATE status fail: %1").arg(q.lastError().text()));
           return r;
       }
    }

    // 插入好友列表, 插入2行, AB BA 同时生效
    int inserted = 0;
    {
        QSqlQuery q(db);
        q.prepare("INSERT IGNORE INTO friends (uid, peer_uid, created_at) VALUES (:u, :p, :t)");
        q.bindValue(":t", now);
        q.bindValue(":u", selfUid);
        q.bindValue(":p", fromUid);
        if (!q.exec()) {
            db.rollback();
            r.text = QStringLiteral("服务暂时不可用，请稍后重试");
            emit logMessage(QStringLiteral("handleFriendRequest INSERT friends fail: %1").arg(q.lastError().text()));
            return r;
        }
        inserted += q.numRowsAffected(); // 判重复, 首次添加+1, 重复+0

        q.bindValue(":u", fromUid);
        q.bindValue(":p", selfUid);
        if (!q.exec()) {
            db.rollback();
            r.text = QStringLiteral("服务暂时不可用，请稍后重试");
            emit logMessage(QStringLiteral("handleFriendRequest INSERT friends fail: %1").arg(q.lastError().text()));
            return r;
        }
        inserted += q.numRowsAffected();
    }
    r.firstTime = (inserted > 0);

    // 同时查两个人 id
    {
        QSqlQuery q(db);
        q.prepare("SELECT id, account_no, nickname, avatar_file_id FROM users WHERE id IN (:a, :b)");
        q.bindValue(":a", selfUid);
        q.bindValue(":b", fromUid);
        if (q.exec()) {
            while (q.next()) {
                const qint64 uid = q.value("id").toLongLong();
                const qint64 accountNo = q.value("account_no").toLongLong();
                QString nick = q.value("nickname").toString();
                const QString avatar = q.value("avatar_file_id").toString();
                if (uid == selfUid) {
                    r.selfAccountNo = accountNo;
                    r.selfNick = nick;
                    r.selfAvatarFileId = avatar;
                }
                else if (uid == fromUid) {
                    r.fromAccountNo = accountNo;
                    r.fromNick = nick;
                    r.fromAvatarFileId = avatar;
                }
            }
        }
    }
    if (!db.commit()) {
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
        emit logMessage(QStringLiteral("handleFriendRequest commit 失败"));
        return r;
    }
    r.ok = true;
    r.accepted = true;
    r.text = QStringLiteral("已同意");
    return r;
}

MysqlUserStore::RemoveFriendResult MysqlUserStore::removeFriend(qint64 uid, qint64 removeUid)
{
    RemoveFriendResult r;
    r.removeUid = removeUid;
    if(uid <= 0 || removeUid <= 0 || uid ==removeUid) {
        r.text = QStringLiteral("好友删除失败，请重试");
        return r;
    }
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        r.text = QStringLiteral("服务暂时不可用，请稍后重试");
        return r;
    }
    // 确认是好友关系
    {
        QSqlQuery q(db);
        q.prepare("SELECT 1 FROM friends WHERE uid = :u AND peer_uid = :p");
        q.bindValue(":u", uid);
        q.bindValue(":p", removeUid);
        if(!q.exec()) {
            r.text = QStringLiteral("服务暂时不可用，请稍后重试");
            emit logMessage(QStringLiteral("removeFriend 查好友关系失败: %1").arg(q.lastError().text()));
            return r;
        }
        if(!q.next()) {
            r.text = QStringLiteral("你们已经不是好友，不需要申请删除");
            return r;
        }
    }
    // 查对方昵称（推 friend_removed 时需要）
    {
        QSqlQuery q(db);
        q.prepare("SELECT nickname FROM users WHERE id = :p");
        q.bindValue(":p", removeUid);
        if (q.exec() && q.next()) {
            r.removeNick = q.value("nickname").toString();
        }
    }
    // 双向删两行
    QSqlQuery q(db);
    q.prepare("DELETE FROM friends WHERE (uid = :u AND peer_uid = :p) OR (uid = :p AND peer_uid = :u)");
    q.bindValue(":u", uid);
    q.bindValue(":p", removeUid);
    if (!q.exec()) {
        r.text = QStringLiteral("好友删除失败，请稍后重试");
        emit logMessage(QStringLiteral("removeFriend 好友删除失败: %1").arg(q.lastError().text()));
        return r;
    }

    {
        QSqlQuery q(db);
        q.prepare("DELETE FROM recentChat WHERE (uid = :u AND peer_uid = :p) OR (uid = :p AND peer_uid = :u)");
        q.bindValue(":u", uid);
        q.bindValue(":p", removeUid);
        if (!q.exec()) {
            emit logMessage(QStringLiteral("removeFriend 清理 recentChat 失败: %1").arg(q.lastError().text()));
        }
    }

    r.ok = true;
    r.text = QStringLiteral("删除好友成功");
    return r;
}

// 分页加载，查询时按时间倒序取，返回时按时间正序给客户端，  beforeMsgId 分页游标
HistoryPage MysqlUserStore::loadHistoryMsg(qint64 me, qint64 peerUid, const QString &beforeMsgId, int limit)
{
    HistoryPage r;
    if (me <= 0 || peerUid <= 0 || me == peerUid) {
        r.text = QStringLiteral("加载历史消息失败，请稍后重试");
        return r;
    }
    r.convId = peerUid;

    if (limit <= 0 || limit > 100)
            limit = Proto::kHistoryPageSize;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        r.text = QStringLiteral("加载历史消息失败，请稍后重试");
        emit logMessage(QStringLiteral("loadHistory: 数据库未连接"));
        return r;
    }

    bool hasAnchor = !beforeMsgId.isEmpty();
    qint64  anchorTs = 0;
    qint64  anchorId = 0;
    if (hasAnchor) {
        QSqlQuery aq(db);
        aq.prepare("SELECT ts, id FROM messages WHERE msg_id = :mid");
        aq.bindValue(":mid", beforeMsgId); // 分页锚点
        if (!aq.exec()) {
            r.text = QStringLiteral("加载历史消息失败，请稍后重试");
            emit logMessage(QStringLiteral("loadHistory 查锚点失败: %1").arg(aq.lastError().text()));
            return r;
        }
        if (!aq.next()) { // 锚点丢失, 返回空页 + hasMore=false，让客户端停手。
            r.ok = true;
            r.hasMore = false;
            r.text = QStringLiteral("");
            emit logMessage(QStringLiteral("loadHistory 锚点失效 msg_id=%1").arg(beforeMsgId));
            return r;
        }
        anchorTs = aq.value("ts").toLongLong(); // 分页锚点
        anchorId = aq.value("id").toLongLong();
    }

    struct Row { qint64 dbId; ChatMessage m; };
    auto runOneDirection = [&](const QString &colFrom, const QString &colTo, QList<Row> &out) -> bool
    {
        QString sql = QStringLiteral("SELECT id, msg_id, client_msg_id, sender_uid, receiver_uid, body, ts, "
                           " msg_type, file_id, file_name, file_size "
                           "FROM messages WHERE %1 = :me AND %2 = :peer").arg(colFrom, colTo);
        if (hasAnchor) {
            sql += QStringLiteral(" AND (ts, id) < (:ats, :aid)");
        }
        sql += QStringLiteral(" ORDER BY ts DESC, id DESC LIMIT :lim");

        QSqlQuery q(db);
        q.prepare(sql);
        q.bindValue(":me", me);
        q.bindValue(":peer", peerUid);
        if (hasAnchor) {
            q.bindValue(":ats", anchorTs);
            q.bindValue(":aid", anchorId);
        }
        q.bindValue(":lim", limit + 1);  // +1 用于探测 has_more
        if (!q.exec()) {
            emit logMessage(QStringLiteral("loadHistory %1→%2 查询历史消息失败: %3").arg(colFrom, colTo,
                                                                        q.lastError().text()));
            return false;
        }
        while (q.next()) {
            Row row;
            row.dbId = q.value("id").toLongLong();
            row.m.msgId       = q.value("msg_id").toString();
            row.m.clientMsgId = q.value("client_msg_id").toString();
            row.m.senderUid   = q.value("sender_uid").toLongLong();
            row.m.convId = q.value("receiver_uid").toLongLong();
            row.m.text        = q.value("body").toString();
            row.m.ts          = q.value("ts").toLongLong();
            row.m.msgType     = quint8(q.value("msg_type").toInt());
            row.m.fileId      = q.value("file_id").toString();
            row.m.fileName    = q.value("file_name").toString();
            row.m.fileSize    = q.value("file_size").toLongLong();
            out.append(row);
        }
        return true;
    };

    QList<Row> fwd, rev;   // 我发他 / 他发我
    if (!runOneDirection("sender_uid", "receiver_uid", fwd)) {
        r.text = QStringLiteral("加载历史消息失败，请稍后重试");
        return r;
    }
    if (!runOneDirection("receiver_uid", "sender_uid", rev)) {
        r.text = QStringLiteral("加载历史消息失败，请稍后重试");
        return r;
    }

    QList<Row> all = fwd + rev;
    std::sort(all.begin(), all.end(), [](const Row &a, const Row &b) {
        if(a.m.ts != b.m.ts)
            return a.m.ts > b.m.ts;
        return a.dbId > b.dbId;
    });

    r.hasMore = all.size() > limit;
    while(all.size() > limit)
        all.removeLast();
    r.messages.reserve(all.size());
    for(int i = all.size() -1; i >= 0; --i) // 转成时间正序返回给客户端（旧→新）
        r.messages.append(all[i].m);

    r.ok = true;
    return r;
}

bool MysqlUserStore::upsertRecentChat(qint64 ownerUid, qint64 peerUid, const QString &msgId, qint64 ts,
                            const QString &MsgText, int insertUnread, int updateUnread)
{
    if (ownerUid == peerUid)
        return false;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("upsertRecentChat: 数据库未连接"));
        return false;
    }

    const QString text = MsgText.left(100); // 预览截断到 100 字符（表字段是 255，留余量）
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    QSqlQuery q(db);
    q.prepare("INSERT INTO recentChat "
              "  (uid, peer_uid, last_msg_id, last_msg_ts, last_msg_text, unread_count, updated_at) "
              "VALUES (:owner, :peer, :msgId, :ts, :text, :insert_unread, :now) AS new_row "
              "ON DUPLICATE KEY UPDATE "
              "  last_msg_id   = new_row.last_msg_id,"
              "  last_msg_ts   = new_row.last_msg_ts,"
              "  last_msg_text = new_row.last_msg_text,"
              "  unread_count  = recentChat.unread_count + :updateUnread,"
              "  updated_at    = new_row.updated_at");
    q.bindValue(":owner", ownerUid);
    q.bindValue(":peer",  peerUid);
    q.bindValue(":msgId", msgId);
    q.bindValue(":ts",    ts);
    q.bindValue(":text",  text);
    q.bindValue(":insert_unread", insertUnread);    // 会话行不存在，INSERT
    q.bindValue(":updateUnread", updateUnread);     // 会话行已存在，UPDATE
    q.bindValue(":now",   now);
    if (!q.exec()) {
        emit logMessage(QStringLiteral("upsertRecentChat 失败 uid=%1 peer=%2: %3")
                        .arg(ownerUid).arg(peerUid).arg(q.lastError().text()));
        return false;
    }
    return true;
}

RecentChatResult MysqlUserStore::listRecentChat(qint64 uid)
{
    RecentChatResult r;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("listRecentChat: 数据库未连接"));
        return r;
    }
    QSqlQuery q(db);
    q.prepare("SELECT rc.peer_uid, rc.last_msg_id, rc.last_msg_ts, rc.last_msg_text, rc.unread_count, u.nickname "
              "FROM recentChat rc "
              "LEFT JOIN users u ON u.id = rc.peer_uid "
              "WHERE rc.uid = :me "
              "ORDER BY rc.last_msg_ts DESC "
              "LIMIT :limit");
    q.bindValue(":me", uid);
    q.bindValue(":limit", Proto::kRecentChatLimit);
    if(!q.exec()) {
        emit logMessage(QStringLiteral("listRecentChat 查询失败: %1").arg(q.lastError().text()));
        return r;
    }
    while(q.next()) {
        RecentChatItem item;
        item.convId = q.value("peer_uid").toLongLong();
        if(item.convId <= 0)
            continue;
        item.peerNick    = q.value("nickname").toString();
        item.lastMsgId   = q.value("last_msg_id").toString();
        item.lastMsgTs   = q.value("last_msg_ts").toLongLong();
        item.lastMsgText = q.value("last_msg_text").toString();
        item.unreadCount = q.value("unread_count").toInt();
        r.items.append(item);
    }
    r.ok = true;
    return r;
}

bool MysqlUserStore::clearUnread(qint64 uid, qint64 peerUid)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("MysqlUserStore::clearUnread: 数据库未连接"));
        return false;
    }

    QSqlQuery q(db);
    q.prepare("UPDATE recentChat SET unread_count = 0, updated_at = :now "
              "WHERE uid = :me AND peer_uid = :peer AND unread_count > 0");
    q.bindValue(":now", QDateTime::currentMSecsSinceEpoch());
    q.bindValue(":me", uid);
    q.bindValue(":peer", peerUid);
    if (!q.exec()) {
        emit logMessage(QStringLiteral("MysqlUserStore::clearUnread 失败 uid=%1 peer=%2: %3")
                        .arg(uid).arg(peerUid).arg(q.lastError().text()));
        return false;
    }
    return true;
}

// 把某个已就绪的头像文件挂到账号上, 只认「自己的 + biz=Avatar + status=1」，防止拿别人的 file_id 顶到自己头像上。
bool MysqlUserStore::setAvatar(qint64 uid, const QString &fileId, int *errcode)
{
    if (errcode)
        *errcode = Err::Unknown;
    if (uid <= 0 || fileId.isEmpty()) {
        if (errcode)
            *errcode = Err::InvalidParam;
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        if (errcode)
            *errcode = Err::ServiceUnavailable;
        emit logMessage(QStringLiteral("setAvatar: 数据库未连接"));
        return false;
    }

    {
        QSqlQuery q(db);
        q.prepare("SELECT 1 FROM files "
                  "WHERE file_id = :fid AND owner_uid = :uid AND biz = :biz AND status = 1");
        q.bindValue(QStringLiteral(":fid"), fileId);
        q.bindValue(QStringLiteral(":uid"), uid);
        q.bindValue(QStringLiteral(":biz"), int(Proto::BizAvatar));
        if (!q.exec()) {
            if (errcode)
                *errcode = Err::DbError;
            emit logMessage(QStringLiteral("setAvatar 查文件失败: %1").arg(q.lastError().text()));
            return false;
        }
        if (!q.next()) {
            if (errcode)
                *errcode = Err::FileNotFound;
            emit logMessage(QStringLiteral("setAvatar uid=%1 引用了不可用的头像文件 %2").arg(uid).arg(fileId));
            return false;
        }
    }

    QSqlQuery q(db);
    q.prepare("UPDATE users SET avatar_file_id = :fid WHERE id = :uid");
    q.bindValue(QStringLiteral(":fid"), fileId);
    q.bindValue(QStringLiteral(":uid"), uid);
    if (!q.exec()) {
        if (errcode)
            *errcode = Err::DbError;
        emit logMessage(QStringLiteral("setAvatar 更新头像失败: %1").arg(q.lastError().text()));
        return false;
    }

    if (errcode)
        *errcode = Err::Ok;
    return true;
}

QString MysqlUserStore::avatarFileIdOf(qint64 uid)
{
    if (uid <= 0)
        return QString();

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen())
        return QString();

    QSqlQuery q(db);
    q.prepare("SELECT avatar_file_id FROM users WHERE id = :uid");
    q.bindValue(QStringLiteral(":uid"), uid);
    if (!q.exec() || !q.next())
        return QString();
    return q.value(QStringLiteral("avatar_file_id")).toString();
}

