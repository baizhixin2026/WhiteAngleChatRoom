#ifndef FILESERVICE_H
#define FILESERVICE_H

#include "protocoltypes.h"
#include "mysqlfilestore.h"
#include <QObject>
#include <QHash>
#include <QSet>
#include <QTimer>

class FileService : public QObject
{
    Q_OBJECT
public:
    FileService(int shardCount, QObject *parent = nullptr);
    ~FileService() override;

public slots:
    void init();
    void cleanup();

    // 来自网络线程
    void beginUpload(quint64 sessionId, qint64 uid, const FileMessage &req);
    void finishUpload(quint64 sessionId, quint32 transferId, const QString &finalSha256);
    void beginDownload(quint64 sessionId, qint64 uid, const FileMessage &req);
    void abortUpload(quint64 sessionId, quint32 transferId);     // 客户端取消上传
    void abortDownload(quint64 sessionId, quint32 transferId);   // 客户端取消下载
    void deleteFile(quint64 sessionId, qint64 uid, const QString &fileId);
    void closeSession(quint64 sessionId);


    // 来自存储线程
    void onUploadProbed(const ChunkResult &r);
    void onUploadFinished(const ChunkResult &r, const QString &sha256);
    void onDownloadOpened(const ChunkResult &r, qint64 totalSize);
    void onUploadFailed(const ChunkResult &r);
    void onDownloadFailed(const ChunkResult &r);
    void onDownloadSentComplete(quint32 transferId);

    // 来自 DBWorker
    void onCommitResult(quint32 transferId, bool ok, int errcode);
    void onReadableResult(quint32 transferId, bool ok, int errcode, const FileMeta &meta);
    void onDeleteResult(const QString &fileId, bool ok, int errcode);

    // 来自 DBWorker（回收）
    void onReclaimableObjects(const QVector<ReclaimableObject> &list);
    void onReclaimedRowsDropped(quint8 biz, const QString &sha256, bool ok, int errcode);
    // 来自存储线程（回收）
    void onObjectDeleted(quint8 biz, const QString &sha256);
    void onObjectDeleteFailed(quint8 biz, const QString &sha256, int errcode);

    void onLiveObjects(bool ok, const QSet<QString> &live);
    void onStorageScanFinished(int removedObjects, int removedParts, int keptTooNew);

signals:
    // 到网络线程
    void uploadBeginOk(quint64 sessionId, quint32 transferId, int shard, const QString &fileId,
                       qint64 existingOffset, qint64 totalSize); // existingOffset 断点续传
    void uploadEndOk(quint64 sessionId, quint32 transferId, const FileMeta &meta);
    void downloadBeginOk(quint64 sessionId, quint32 transferId, int shard, const FileMeta &meta,
                         qint64 startOffset, qint64 length);
    void uploadBeginFailed(quint64 sessionId, quint32 transferId, int errcode); // 客户端还在等 begin_ok
    void uploadEndFailed(quint64 sessionId, quint32 transferId, int errcode);  // 客户端已经拿到 begin_ok
    void downloadBeginFailed(quint64 sessionId, quint32 transferId, int errcode); // 客户端还在等 begin_ok
    void downloadFailed(quint64 sessionId, quint32 transferId, int errcode);   // 传输进行中
    void fileDeleted(quint64 sessionId, const QString &fileId, int errcode);

    // 回收（到 DBWorker）
    void reqQueryReclaimableObjects(int limit);
    void reqDropReclaimedRows(quint8 biz, const QString &sha256);
    // 回收（到存储线程，按 shard 过滤）
    void reqDeleteObject(int shard, quint8 biz, const QString &sha256);

    // 到存储线程（按 shard 参数由接线端过滤）
    void reqProbeUpload(const TransferCtx &ctx);
    void reqDeleteUploadPart(int shard, quint32 transferId);   // 用户取消 → 删 .part
    void reqKeepUploadPart(int shard, quint32 transferId);     // 断线/关停 → 只关句柄，.part 留着
    void reqFinishUpload(int shard, quint32 transferId);
    void reqOpenDownload(const TransferCtx &ctx);
    void reqCloseDownload(int shard, quint32 transferId);

    void reqQueryLiveObjects();
    void reqScanStorage(int shard, const QSet<QString> &live);

    // 到 DBWorker
    void reqCommitFile(quint32 transferId, const FileMeta &meta);   // 收尾一次性 INSERT + 写 ACL
    void reqLoadFileForRead(quint32 transferId, qint64 uid, const QString &fileId);
    void reqMarkFileDeleted(qint64 uid, const QString &fileId);

    void logMessage(const QString &msg);

private slots:
    void expireCommitWaits();        // 收尾 INSERT 超时
    void expireFileReadWaits();      // 读取权限查询超时
    void expireMarkDeletedWaits();   // 标记删除超时
    void sweepReclaimableObjects();  // 回收巡检
    void sweepStorage();
private:
    struct Live {
        TransferCtx ctx;
        quint64   sessionId = 0;
        QString   fileName;
        QString   mime;
        FileMeta  meta;
    };

    struct CommitWait {             // 等 files 行落库（收尾一次性 INSERT）
        quint64  sessionId = 0;
        FileMeta meta;              // status 已置 FileReady，INSERT 直接用
        qint64   enqueuedAt = 0;
    };

    struct FileReadWait {           // 等"这个用户能不能读这个文件"
        quint64     sessionId = 0;
        qint64      uid = 0;
        quint32     transferId = 0;
        FileMessage req;            // 里面的 offset / length 是下载区间
        qint64      enqueuedAt = 0;
    };

    struct MarkDeletedWait {        // 等文件被标记为已删除
        quint64 sessionId = 0;
        QString fileId;
        qint64  enqueuedAt = 0;
    };

    int  validateUpload(qint64 uid, const FileMessage &req) const;
    int  validateDownload(qint64 uid, const FileMessage &req) const;
    bool hasCapacity(quint64 sessionId) const;
    void addUploadRecord(const Live &live);
    void addDownloadRecord(const Live &live);

    // 只删记录，不通知存储（存储侧已经处理完时用）
    void removeUploadRecord(quint32 transferId);
    void removeDownloadRecord(quint32 transferId);

    // 用户主动取消：上传删 .part，下载关句柄
    void cancelUpload(quint32 transferId);
    void closeDownload(quint32 transferId);

    // 断线/关停：上传保留 .part 供续传，下载关句柄
    void suspendUpload(quint32 transferId);

    // 从「会话 -> 传输集合」索引里摘掉一个 id（上传/下载共用）
    void removeTransferIdFromSession(quint64 sessionId, quint32 transferId);


    int m_shardCount = 1;
    quint32 m_nextTransferId = 1;

    // 上传/下载的并发管理
    QHash<quint32, Live>          m_transfers;  // 正在进行的收发传输
    QHash<quint64, QSet<quint32>> m_transferIdsBySession;  // sessionId 查全部 transferId 集合

    QHash<quint32, CommitWait>      m_commitWaits;      // 键 = transferId
    QHash<quint32, FileReadWait>    m_fileReadWaits;    // 键 = transferId
    QHash<QString, MarkDeletedWait> m_markDeletedWaits; // 键 = fileId

    QTimer *m_sweep = nullptr;

    // 存储对象回收
    static QString reclaimKey(quint8 biz, const QString &sha256);
    QTimer *m_reclaimTimer = nullptr;
    QSet<QString> m_reclaimInFlight;        // "biz:sha" 正在删盘或正在删记录
    QHash<QString, int> m_reclaimFailures;  // "biz:sha" -> 连续失败次数

    QTimer *m_storageSweepTimer = nullptr;
    bool    m_storageSweepPending = false;

};

#endif // FILESERVICE_H
