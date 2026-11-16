#ifndef MYSQLFILESTORE_H
#define MYSQLFILESTORE_H

/*
 * 线程纪律：只在 DBWorker 线程内调用。
 */

#include "protocoltypes.h"
#include "storageremovekey.h"
#include <QObject>
#include <QString>
#include <QVector>
#include <QMetaType>
#include <QSet>

/*
 * 一个"所有引用都已标删、可以物理回收"的存储对象。
 * 注意键是 (biz, sha256) 而不是 sha256：
 * finalPath 把 biz 编进了目录，同一份字节用不同 biz 上传会落成两个物理文件。
 */
struct ReclaimableObject {
    quint8  biz    = 0;
    QString sha256;
};
Q_DECLARE_METATYPE(ReclaimableObject)
Q_DECLARE_METATYPE(QVector<ReclaimableObject>)


class MysqlFileStore : public QObject
{
    Q_OBJECT
public:
    explicit MysqlFileStore(const QString &connectionName);
    ~MysqlFileStore() override = default;

    bool commitFile(const FileMeta &meta, int *errcode);

    bool loadForRead(qint64 uid, const QString &fileId, FileMeta *meta, int *errcode);

    bool markDeleted(qint64 uid, const QString &fileId, int *errcode);

    int markFileExpired(qint64 cutoffMs, int limit);

    bool grant(const QString &fileId, quint8 granteeType, qint64 granteeId, qint64 grantedBy);

    // 聊天附件：把读权限授给接收方。
    // 必须先确认这个 file_id 是发送者自己的、已就绪、且属于"附件类" biz, 否则客户端只要猜到一个 file_id，就能把任意文件授权给任意人。
    bool grantForMessage(qint64 senderUid, qint64 receiverUid, const QString &fileId, int *errcode);

    // 回收：找出没有任何活引用的对象
    QVector<ReclaimableObject> listReclaimableObjects(int limit);

    // 回收：盘上对象已删除，清掉对应的 files / file_acl 行
    bool dropReclaimedRows(quint8 biz, const QString &sha256, int *errcode);

    // 回收对账：把所有"活"对象（status = 1）的 key 收集起来。
    // 返回 false 表示查询失败 —— 调用方必须放弃本轮对账，绝不能在空集合上删盘。
    bool listLiveObjects(QSet<QString> *out, QString *errText);

signals:
    void logMessage(const QString &msg);

private:
    QString m_connectionName;
};

#endif // MYSQLFILESTORE_H














