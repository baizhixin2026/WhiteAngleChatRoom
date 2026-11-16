#include "mysqlfilestore.h"
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace {
    constexpr int kMaxMimeLen       = 128;
    constexpr int kMaxFileNameLen   = 255;

    QString clamp(const QString &s, int maxChars)
    {
        return s.size() > maxChars ? s.left(maxChars) : s;
    }

    void fillMeta(const QSqlQuery &q, FileMeta *m)
    {
        m->fileId    = q.value(QStringLiteral("file_id")).toString();
        m->sha256    = q.value(QStringLiteral("sha256")).toString();
        m->size      = q.value(QStringLiteral("size")).toLongLong();
        m->mime      = q.value(QStringLiteral("mime")).toString();
        m->fileName  = q.value(QStringLiteral("file_name")).toString();
        m->ownerUid  = q.value(QStringLiteral("owner_uid")).toLongLong();
        m->biz       = quint8(q.value(QStringLiteral("biz")).toInt());
        m->status    = quint8(q.value(QStringLiteral("status")).toInt());
        m->createdAt = q.value(QStringLiteral("created_at")).toLongLong();
    }
}

MysqlFileStore::MysqlFileStore(const QString &connectionName)
    : m_connectionName(connectionName)
{

}

bool MysqlFileStore::commitFile(const FileMeta &meta, int *errcode)
{
    if (errcode)
        *errcode = Err::Unknown;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        if (errcode)
            *errcode = Err::ServiceUnavailable;
        emit logMessage(QStringLiteral("commitFile: 数据库未连接"));
        return false;
    }
    if (!db.transaction()) {
        if (errcode)
            *errcode = Err::DbError;
        emit logMessage(QStringLiteral("commitFile 开启事务失败: %1").arg(db.lastError().text()));
        return false;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    {
        QSqlQuery q(db);
        q.prepare("INSERT INTO files "
                  "(file_id, sha256, size, mime, file_name, owner_uid, biz, status, created_at, ready_at) "
                  "VALUES (:fid, :sha, :size, :mime, :name, :owner, :biz, 1, :now, :now)");
        q.bindValue(QStringLiteral(":fid"),   meta.fileId);
        q.bindValue(QStringLiteral(":sha"),   meta.sha256);
        q.bindValue(QStringLiteral(":size"),  meta.size);
        q.bindValue(QStringLiteral(":mime"),  clamp(meta.mime, kMaxMimeLen));
        q.bindValue(QStringLiteral(":name"),  clamp(meta.fileName, kMaxFileNameLen));
        q.bindValue(QStringLiteral(":owner"), meta.ownerUid);
        q.bindValue(QStringLiteral(":biz"),   int(meta.biz));
        q.bindValue(QStringLiteral(":now"),   now);
        if (!q.exec()) {
            db.rollback();
            if (errcode)
                *errcode = Err::DbError;
            emit logMessage(QStringLiteral("commitFile 插入 files 失败: %1").arg(q.lastError().text()));
            return false;
        }
    }

    // 读权限：公开业务给 public 一行，上传者自己恒有一行
    auto insertAcl = [&](quint8 granteeType, qint64 granteeId) -> bool {
        QSqlQuery q(db);
        q.prepare("INSERT IGNORE INTO file_acl "
                  "(file_id, grantee_type, grantee_id, granted_by, created_at) "
                  "VALUES (:fid, :gt, :gid, :by, :now)");
        q.bindValue(QStringLiteral(":fid"), meta.fileId);
        q.bindValue(QStringLiteral(":gt"),  int(granteeType));
        q.bindValue(QStringLiteral(":gid"), granteeId);
        q.bindValue(QStringLiteral(":by"),  meta.ownerUid);
        q.bindValue(QStringLiteral(":now"), now);
        if (!q.exec()) {
            emit logMessage(QStringLiteral("commitFile 写 ACL 失败: %1").arg(q.lastError().text()));
            return false;
        }
        return true;
    };

    if ((Proto::isPublicReadBiz(meta.biz) && !insertAcl(Proto::GranteePublic, 0)) ||
                                !insertAcl(Proto::GranteeUid, meta.ownerUid)) {
        db.rollback();
        if (errcode)
            *errcode = Err::DbError;
        return false;
    }

    if (!db.commit()) {
        db.rollback();
        if (errcode)
            *errcode = Err::DbError;
        emit logMessage(QStringLiteral("commitFile 提交失败: %1").arg(db.lastError().text()));
        return false;
    }
    if (errcode)
        *errcode = Err::Ok;
    return true;
}

// 下载前的元数据 + 权限校验
bool MysqlFileStore::loadForRead(qint64 uid, const QString &fileId, FileMeta *meta, int *errcode)
{
    if (errcode)
        *errcode = Err::Unknown;
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        if (errcode)
            *errcode = Err::ServiceUnavailable;
        emit logMessage(QStringLiteral("loadForRead: 数据库未连接"));
        return false;
    }

    QSqlQuery q(db);
    q.prepare(
        "SELECT f.file_id, f.sha256, f.size, f.mime, f.file_name, f.owner_uid, f.biz, f.status, f.created_at "
        "FROM files f "
        "WHERE f.file_id = :fid AND f.status = 1  AND ( f.owner_uid = :uid "
        " OR EXISTS (SELECT 1 FROM file_acl a WHERE a.file_id = f.file_id AND a.grantee_type = 0) "
        " OR EXISTS (SELECT 1 FROM file_acl a WHERE a.file_id = f.file_id AND a.grantee_type = 1 AND a.grantee_id = :uid))");
    // 群聊：加 grantee_type = 2 且 grantee_id IN (用户所在群)
    q.bindValue(QStringLiteral(":fid"), fileId);
    q.bindValue(QStringLiteral(":uid"), uid);
    if (!q.exec()) {
        if (errcode)
            *errcode = Err::DbError;
        emit logMessage(QStringLiteral("loadForRead 失败: %1").arg(q.lastError().text()));
        return false;
    }
    if (!q.next()) {
        // 一条 SQL 无法区分 不存在与无权限，对客户端而言都一样，看不到就是没有
        if (errcode)
            *errcode = Err::FileNotFound;
        return false;
    }

    if (meta)
        fillMeta(q, meta);
    if (errcode)
        *errcode = Err::Ok;
    return true;
}

// 删除（一期只标记；仅 owner 可删）
bool MysqlFileStore::markDeleted(qint64 uid, const QString &fileId, int *errcode)
{
    if (errcode)
        *errcode = Err::Unknown;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        if (errcode)
            *errcode = Err::ServiceUnavailable;
        return false;
    }

    QSqlQuery q(db);
    q.prepare("UPDATE files SET status = 2 WHERE file_id = :fid AND owner_uid = :uid AND status = 1");
    q.bindValue(QStringLiteral(":fid"), fileId);
    q.bindValue(QStringLiteral(":uid"), uid);
    if (!q.exec()) {
        if (errcode) *errcode = Err::DbError;
        emit logMessage(QStringLiteral("markDeleted 失败: %1").arg(q.lastError().text()));
        return false;
    }
    if (q.numRowsAffected() <= 0) {
        if (errcode) *errcode = Err::PermissionDenied;
        return false;
    }
    if (errcode)
        *errcode = Err::Ok;
    return true;
}

// 文件留存到期：只做标记 status=2 且 created_at < cutoff
// 删盘/删行交给已有的 listReclaimableObjects + dropReclaimedRows
int MysqlFileStore::markFileExpired(qint64 cutoffMs, int limit)
{
    if (cutoffMs <= 0 || limit <= 0)
        return 0;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("markFileExpired: 数据库未连接"));
        return -1;
    }

    // 状态是待处理(status = 1)、创建时间早于 cutoff、且 biz 不等于头像业务的文件才被标记为过期
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE files SET status = 2 "
                             "WHERE status = 1 AND created_at < :cutoff AND biz <> :avatar "
                             "LIMIT :lim"));
    q.bindValue(QStringLiteral(":cutoff"), cutoffMs);
    q.bindValue(QStringLiteral(":avatar"), int(Proto::BizAvatar));
    q.bindValue(QStringLiteral(":lim"), limit);
    if (!q.exec()) {
        emit logMessage(QStringLiteral("markExpiredForRetention 失败: %1").arg(q.lastError().text()));
        return -1;
    }
    return q.numRowsAffected(); // 返回标记过期的文件个数
}


// 授权：granteeType = Proto::GranteePublic / GranteeUid / GranteeGroup
bool MysqlFileStore::grant(const QString &fileId, quint8 granteeType, qint64 granteeId, qint64 grantedBy)
{
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("grant: 数据库未连接"));
        return false;
    }

    QSqlQuery q(db);
    q.prepare("INSERT IGNORE INTO file_acl "
              "(file_id, grantee_type, grantee_id, granted_by, created_at) "
              "VALUES (:fid, :gt, :gid, :by, :now)");
    q.bindValue(QStringLiteral(":fid"), fileId);
    q.bindValue(QStringLiteral(":gt"),  int(granteeType));
    q.bindValue(QStringLiteral(":gid"), granteeId);
    q.bindValue(QStringLiteral(":by"),  grantedBy);
    q.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        emit logMessage(QStringLiteral("grant 失败: %1").arg(q.lastError().text()));
        return false;
    }
    return true;
}

// 聊天附件：把读权限授给接收方
bool MysqlFileStore::grantForMessage(qint64 senderUid, qint64 receiverUid, const QString &fileId, int *errcode)
{
    if (errcode)
        *errcode = Err::Unknown;
    if (senderUid <= 0 || receiverUid <= 0 || fileId.isEmpty()) {
        if (errcode)
            *errcode = Err::InvalidParam;
        return false;
    }

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        if (errcode)
            *errcode = Err::ServiceUnavailable;
        return false;
    }

    // 归属 + 状态 + biz 校验
    {
        QSqlQuery q(db);
        q.prepare("SELECT 1 FROM files "
                  "WHERE file_id = :fid AND owner_uid = :owner AND status = 1 "
                  "  AND biz BETWEEN :bizMin AND :bizMax");
        q.bindValue(QStringLiteral(":fid"), fileId);
        q.bindValue(QStringLiteral(":owner"), senderUid);
        q.bindValue(QStringLiteral(":bizMin"), int(Proto::BizChatImage));   // 1
        q.bindValue(QStringLiteral(":bizMax"), int(Proto::BizVideo));       // 4
        if (!q.exec()) {
            if (errcode)
                *errcode = Err::DbError;
            emit logMessage(QStringLiteral("grantForMessage 查文件失败: %1").arg(q.lastError().text()));
            return false;
        }
        if (!q.next()) {
            if (errcode)
                *errcode = Err::PermissionDenied;
            emit logMessage(QStringLiteral("grantForMessage uid=%1 引用了不可授权的文件 %2").arg(senderUid).arg(fileId));
            return false;
        }
    }

    // 文件授权
    QSqlQuery q(db);
    q.prepare("INSERT IGNORE INTO file_acl "
              "(file_id, grantee_type, grantee_id, granted_by, created_at) "
              "VALUES (:fid, :type, :gid, :by, :now)");
    q.bindValue(QStringLiteral(":fid"), fileId);
    q.bindValue(QStringLiteral(":type"), int(Proto::GranteeUid));
    q.bindValue(QStringLiteral(":gid"), receiverUid);
    q.bindValue(QStringLiteral(":by"), senderUid);
    q.bindValue(QStringLiteral(":now"), QDateTime::currentMSecsSinceEpoch());
    if (!q.exec()) {
        if (errcode)
            *errcode = Err::DbError;
        emit logMessage(QStringLiteral("grantForMessage 写 ACL 失败: %1").arg(q.lastError().text()));
        return false;
    }

    if (errcode)
        *errcode = Err::Ok;
    return true;
}

// ============================ 存储对象回收 ============================

QVector<ReclaimableObject> MysqlFileStore::listReclaimableObjects(int limit)
{
    QVector<ReclaimableObject> out;
    if (limit <= 0)
        return out;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        emit logMessage(QStringLiteral("listReclaimableObjects: 数据库未连接"));
        return out;
    }

    // 只有"没有任何 status=1 的引用、且确实存在已标删行"的组才可以回收。 后半个条件防止把 status=0（上传中尚未落库）的组误判成垃圾。
    QSqlQuery q(db);
    q.prepare("SELECT biz, sha256 FROM files GROUP BY biz, sha256 "
              "HAVING SUM(status = 1) = 0 AND SUM(status = 2) > 0 "
              "LIMIT :lim");
    q.bindValue(QStringLiteral(":lim"), limit);
    if (!q.exec()) {
        emit logMessage(QStringLiteral("listReclaimableObjects 失败: %1").arg(q.lastError().text()));
        return out;
    }
    while (q.next()) {
        ReclaimableObject o;
        o.biz    = quint8(q.value(QStringLiteral("biz")).toInt());
        o.sha256 = q.value(QStringLiteral("sha256")).toString();
        if (!o.sha256.isEmpty())
            out.append(o);
    }
    return out;
}

bool MysqlFileStore::dropReclaimedRows(quint8 biz, const QString &sha256, int *errcode)
{
    if (errcode)
        *errcode = Err::Unknown;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        if (errcode)
            *errcode = Err::ServiceUnavailable;
        return false;
    }
    if (!db.transaction()) {
        if (errcode)
            *errcode = Err::DbError;
        return false;
    }

    // 顺序：先删 ACL（靠 files 关联出 file_id），再删 files 行。 绝不能反过来， files 行一旦先没，ACL 就成了永远查不到主人的垃圾。
    {
        QSqlQuery q(db);
        q.prepare("DELETE a FROM file_acl a JOIN files f ON a.file_id = f.file_id "
                  "WHERE f.biz = :biz AND f.sha256 = :sha AND f.status = 2");
        q.bindValue(QStringLiteral(":biz"), int(biz));
        q.bindValue(QStringLiteral(":sha"), sha256);
        if (!q.exec()) {
            db.rollback();
            if (errcode)
                *errcode = Err::DbError;
            emit logMessage(QStringLiteral("dropReclaimedRows 删 ACL 失败: %1").arg(q.lastError().text()));
            return false;
        }
    }

    {
        QSqlQuery q(db);
        q.prepare("DELETE FROM files WHERE biz = :biz AND sha256 = :sha AND status = 2");
        q.bindValue(QStringLiteral(":biz"), int(biz));
        q.bindValue(QStringLiteral(":sha"), sha256);
        if (!q.exec()) {
            db.rollback();
            if (errcode)
                *errcode = Err::DbError;
            emit logMessage(QStringLiteral("dropReclaimedRows 删 files 失败: %1")
                                .arg(q.lastError().text()));
            return false;
        }
    }

    if (!db.commit()) {
        db.rollback();
        if (errcode)
            *errcode = Err::DbError;
        emit logMessage(QStringLiteral("dropReclaimedRows 提交失败: %1").arg(db.lastError().text()));
        return false;
    }
    if (errcode)
        *errcode = Err::Ok;
    return true;
}


bool MysqlFileStore::listLiveObjects(QSet<QString> *out, QString *errText)
{
    if (out)
        out->clear();
    if (errText)
        errText->clear();

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) {
        if (errText)
            *errText = QStringLiteral("数据库未连接");
        return false;
    }

    // 只取 status = 1 (status = 2 的行已经"标删"，它们的对象允许被回收)
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT biz, sha256 FROM files WHERE status = 1"))) {
        if (errText)
            *errText = q.lastError().text();
        emit logMessage(QStringLiteral("listLiveObjects 失败: %1").arg(q.lastError().text()));
        return false;
    }

    while (q.next()) {
        if (out) { // <biz>:<sha256>
            out->insert(storageObjectKey(quint8(q.value(0).toInt()), q.value(1).toString()));
        }
    }
    return true;
}

