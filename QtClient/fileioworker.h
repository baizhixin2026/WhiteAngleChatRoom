#ifndef FILEIOWORKER_H
#define FILEIOWORKER_H

#include "filetransfertypes.h"
#include "protocoltypes.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QString>
#include <qcryptographichash.h>

/*
 * FileIoWorker —— 客户端唯一的磁盘 IO 线程
 *
 * 线程纪律：
 *   1. QFile 只在本线程创建 / 使用 / 关闭，绝不跨线程移动
 *   2. 一次槽调用只做一小步（读/写一块 256 KiB），做完立刻回事件循环
 *   3. 不懂业务语义：只按 taskId 读写字节
 */
class FileIoWorker : public QObject
{
    Q_OBJECT
public:
    explicit FileIoWorker(QObject *parent = nullptr);
    ~FileIoWorker() override;

public slots:
    // ---- 上传 ----
    void openUploadSource(quint32 taskId, const QString &path, qint64 startOffset);
    void readUploadBlock(quint32 taskId);

    // ---- 下载 ----
    // finalPath 是最终文件名；实际写的是 finalPath + ".part"，commit 时再改名
    void openDownloadSink(quint32 taskId, const QString &finalPath, qint64 startOffset);
    void writeDownloadBlock(quint32 taskId, quint64 offset, const QByteArray &data);
    void closeDownloadSink(quint32 taskId, bool commit);

    // 任务收尾：两种句柄一起清
    void closeTask(quint32 taskId);

    void hashFile(quint32 taskId, const QString &path);

signals:
    void uploadSourceOpened(quint32 taskId, qint64 size);
    void uploadSourceOpenFailed(quint32 taskId, const QString &reason);
    void uploadBlockRead(quint32 taskId, quint64 offset, const QByteArray &data);
    void uploadBlockReadFailed(quint32 taskId, const QString &reason);

    void downloadSinkOpened(quint32 taskId);
    void downloadSinkOpenFailed(quint32 taskId, const QString &reason);
    void downloadBlockWritten(quint32 taskId, qint64 writtenBytes);
    void downloadBlockWriteFailed(quint32 taskId, const QString &reason);
    void downloadSinkClosed(quint32 taskId, bool ok, const QString &reason);

    void fileHashProgress(quint32 taskId, qint64 hashedBytes, qint64 totalBytes);
    void fileHashed(quint32 taskId, const QString &sha256, qint64 size);
    void fileHashFailed(quint32 taskId, const QString &reason);

private:
    struct UploadSource {
        QFile  file;
        qint64 readOffset = 0;
        qint64 size       = 0;
    };

    struct DownloadSink {
        QFile   file;
        QString partPath;
        QString finalPath;
        qint64  writeOffset = 0;
    };

    struct HashJob {
        QFile  file;
        qint64 hashed = 0;
        qint64 total  = 0;
        QCryptographicHash sha{QCryptographicHash::Sha256};
    };

    void closeUploadSource(quint32 taskId);
    void destroyDownloadSink(quint32 taskId, bool commit);

    void stepHash(quint32 taskId);
    void destroyHashJob(quint32 taskId);

    QHash<quint32, UploadSource *> m_uploadSources;
    QHash<quint32, DownloadSink *> m_downloadSinks;
    QHash<quint32, HashJob *> m_hashJobs;
};

#endif // FILEIOWORKER_H
