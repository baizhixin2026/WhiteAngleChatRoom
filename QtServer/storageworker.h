#ifndef STORAGEWORKER_H
#define STORAGEWORKER_H

/*
 * 目录布局（纯内容寻址，无时间维度）：
 *   <root>/<biz>/<sha[0:2]>/<sha[2:4]>/<sha256>        最终对象
 *   <root>/<biz>/<sha[0:2]>/<sha[2:4]>/<sha256>.part   上传中
 */

/*
 *  当数据库的存储介质是 NVMe 时， N=2 是安全的默认值。磁盘延迟在微秒级，网卡是唯一瓶颈。
 *  当数据库的存储介质是 HDD（机械硬盘）：瓶颈瞬间转移到磁盘。这时 N=2 的线程会立刻被磁盘的延迟拖垮，必须提升N，用更多并发来掩盖单个线程的磁盘等待时间。
 *  现在方案的线程数设计:（N默认2，HDD可调4-8），服务端的第一个瓶颈，几乎总是网卡带宽，而不是 NVMe 磁盘。
 *  换存储介质，改 N 即可， 通过调整 N 这个参数来适配不同的存储介质。
 *
 *  换 MinIO/S3 时只需新增实现类；若对象存储是异步 API，则 StorageWorker 那层改异步，这两件事不要混在一起做。
 */

/*
数据库的存储介质的选型, 完全取决于业务场景对“性能”和“容量成本”的权衡。
维度          HDD                             NVMe SSD
出货量份额     下降中（约占45%）                 快速上升（约占55%）
数据容量份额	 绝对主导（约占80%）                占比较小但快速增长
核心优势	     每TB成本极低，容量大               极致性能，微秒级延迟
典型场景      冷/温数据、备份、归档、视频监控      数据库、AI、实时分析、高性能计算
*/

#include "protocoltypes.h"
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>

class StorageWorker : public QObject
{
    Q_OBJECT
public:
    StorageWorker(int shardIndex, int shardCount, const QString &rootDir, QObject *parent = nullptr);
    ~StorageWorker() override;

public slots:
    void init();
    void cleanup();

    // 上传（写分片）
    void probeUpload(const TransferCtx &ctx);
    void writeChunk(quint32 transferId, quint64 offset, const QByteArray &data);
    void finishUpload(quint32 transferId);
    void cancelUploadAndDeletePart(quint32 transferId);   // 用户取消
    void cancelUploadAndKeepPart(quint32 transferId);     // 断线/关停

    // 下载（读分片）
    void openDownload(const TransferCtx &ctx);
    void readChunk(quint32 transferId, qint64 offset, int maxLen);
    void closeDownload(quint32 transferId);

    // 回收：删除最终对象（幂等）
    void deleteObject(quint8 biz, const QString &sha256);

    // 磁盘对账：删掉"库里没有任何活引用"的正式对象，以及超期的 .part
    // liveObjects 是 storageObjectKey() 生成的键集合；空集合是合法输入（新库）
    void scanStorage(const QSet<QString> &liveObjects);

signals:
    void uploadProbed(const ChunkResult &r);
    void uploadFinished(const ChunkResult &r, const QString &sha256);
    void chunkWritten(quint32 transferId, qint64 bytes);   // 单块落盘完成：网络层据此解除接收反压
    void downloadOpened(const ChunkResult &r, qint64 totalSize);
    void chunkRead(const FileChunk &c, bool eof);
    void uploadFailed(const ChunkResult &r);
    void downloadFailed(const ChunkResult &r);
    void objectDeleted(quint8 biz, const QString &sha256);
    void objectDeleteFailed(quint8 biz, const QString &sha256, int errcode);
    void logMessage(const QString &msg);

    void storageScanFinished(int removedObjects, int removedParts, int keptTooNew);

private slots:
    void processStep();
    void uploadsTimeout();
    void downloadsTimeout();

private:
    struct UploadJob {
        TransferCtx ctx;
        QFile              file;
        QCryptographicHash sha{QCryptographicHash::Sha256};
        QQueue<QByteArray> pending;   // 已从网络收到、还没写进磁盘的块队列（内存缓冲）
        qint64 nextOffset = 0;        // 已落盘字节数
        qint64 queuedEnd  = 0;        // 已入队字节数（= nextOffset + pending 之和）
        qint64 lastActiveAt = 0;      // 空闲超时判定的心跳时间戳
        bool   uploadFinish = false;

        bool    openPart(const QString &partPath, int *errcode);
        bool    appendData(const QByteArray &data, int *errcode);
        bool    commitTo(const QString &finalPath, int *errcode);
        void    abortAndRemove();
        QString digestHex() const;

        private:
            bool seedHash(int *errcode);     // 续传时把既有前缀补算进哈希
    };

    struct DownloadJob {
        TransferCtx ctx;
        QFile      file;
        qint64     size = 0;
        qint64     lastActiveAt = 0;

        bool openFile(const QString &path, int *errcode);
        bool readAt(qint64 offset, int maxLen, QByteArray *out, int *errcode);
    };

    // 路径布局
    QString partPath(quint8 biz, qint64 uid, const QString &sha256) const; // 上传：.part 绝对路径
    QString finalPath(quint8 biz, const QString &sha256) const;// 上传：最终路径
    QString dirFor(quint8 biz, const QString &sha256) const;
    qint64  availableBytes() const;
    static QString bizDirName(quint8 biz);
    static bool    isValidSha256(const QString &sha);

    // 内部流程
    void enqueueWrite(quint32 transferId);
    void destroyUploadJob(UploadJob *job, bool removePart);
    void destroyUploadJobAndDeletePart(UploadJob *job);   // 数据坏了，连 .part 一起删
    void destroyUploadJobAndKeepPart(UploadJob *job);     // 数据是好的，只关句柄
    void finalizeUpload(UploadJob *job);
    void closeDownloadJob(DownloadJob *job);

    int m_shardIndex = 0;
    int m_shardCount = 1;
    QString m_rootDir; // 存储根目录

    QHash<quint32, UploadJob *> m_uploadJobs;      // 上传会话表
    QHash<quint32, DownloadJob *> m_downloadJobs;  // 下载会话表

    QQueue<quint32> m_writePendingQueue;        // 公平轮转: 队列管顺序（FIFO 轮转），等待落盘的 transferId
    QSet<quint32>   m_writePendingSet;          // 集合管 tid 去重

    QHash<QString, quint32> m_activeParts;   // "<uid>-<sha256>" -> transferId , 互斥表

    QTimer *m_sweep = nullptr;               // 空闲清扫定时器, 僵尸 transfer 收掉
    bool m_stopping = false;
};

#endif // STORAGEWORKER_H
