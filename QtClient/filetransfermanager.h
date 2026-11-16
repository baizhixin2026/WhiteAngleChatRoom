#ifndef FILETRANSFERMANAGER_H
#define FILETRANSFERMANAGER_H

#include "filetransfertypes.h"
#include "protocoltypes.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>

class FileIoWorker;

/*
 * FileTransferManager 文件传输的唯一语义状态机，跑在 GUI 线程
 *
 * 只做：任务表、状态机、控制面消息组装、把块交给网络线程
 * 不做：socket 读写（NetworkWorker）、磁盘读写（FileIoWorker）
 */
class FileTransferManager : public QObject
{
    Q_OBJECT
public:
    explicit FileTransferManager(QObject *parent = nullptr);
    ~FileTransferManager() override;

    void checkStalledTransfers();
    void attach(FileIoWorker *ioWorker);    // 由 ConnectionManager 在启动时接线

    // 从本地路径发起上传：先让磁盘线程算 sha256，再走 begin 流程
    quint32 startUpload(const QString &localPath, const QString &fileName, const QString &mime, quint8 biz);
    quint32 startDownload(const QString &fileId, const QString &savePath, TransferPriority priority = TransferPriority::User);

signals:
    // 到 NetworkWorker（ConnectionManager 直接转发）
    void reqSendFileMessage(const FileMessage &msg);
    void reqSendFileChunk(const FileChunk &chunk);
    // 到 FileIoWorker
    void reqIoHashFile(quint32 taskId, const QString &path);
    void reqIoOpenUploadSource(quint32 taskId, const QString &path, qint64 startOffset);
    void reqIoReadUploadBlock(quint32 taskId);
    void reqIoCloseTask(quint32 taskId);
    // 到 UI
    void uploadFinished(quint32 taskId, const QString &fileId, const FileMeta &meta);
    void uploadFailed(quint32 taskId, int errcode, const QString &text);

    // 到 FileIoWorker
    void reqIoOpenDownloadSink(quint32 taskId, const QString &finalPath, qint64 startOffset);
    void reqIoWriteDownloadBlock(quint32 taskId, quint64 offset, const QByteArray &data);
    void reqIoCloseDownloadSink(quint32 taskId, bool commit);
    // 到 UI
    void uploadProgress(quint32 taskId, qint64 sentBytes, qint64 totalBytes, qint64 elapsedMs);
    void downloadProgress(quint32 taskId, qint64 receivedBytes, qint64 totalBytes, qint64 elapsedMs);
    void downloadFinished(quint32 taskId, const QString &localPath, qint64 size);
    void downloadFailed(quint32 taskId, int errcode, const QString &text);

public slots:
    void abortAll(const QString &reason);

    // 来自 NetworkWorker
    void onFileMessage(const FileMessage &msg);
    // 来自 FileIoWorker
    void onFileHashed(quint32 taskId, const QString &sha256, qint64 size);
    void onFileHashFailed(quint32 taskId, const QString &reason);

    void onUploadSourceOpened(quint32 taskId, qint64 size);
    void onUploadSourceOpenFailed(quint32 taskId, const QString &reason);
    void onUploadBlockRead(quint32 taskId, quint64 offset, const QByteArray &data);
    void onUploadBlockReadFailed(quint32 taskId, const QString &reason);

    void onDownloadChunkReceived(const FileChunk &chunk);
    void onDownloadSinkOpened(quint32 taskId);
    void onDownloadSinkOpenFailed(quint32 taskId, const QString &reason);
    void onDownloadBlockWritten(quint32 taskId, qint64 writtenBytes);
    void onDownloadBlockWriteFailed(quint32 taskId, const QString &reason);
    void onDownloadSinkClosed(quint32 taskId, bool ok, const QString &reason);

private:
    struct Task {
        FileTransferTask t;
        TransferState    state = TransferState::Idle;
        TransferPriority priority = TransferPriority::User;
        int              errcode = 0;
        QString          text;
        qint64           expectedLength = 0;   // 下载：本次应收字节数
        qint64           lastActivityAt = 0;
        QElapsedTimer    streamClock;          // 数据面计时：进入 Streaming 时 start()，专供进度算 elapsedMs
    };

    Task *taskOf(quint32 taskId) const;
    Task *taskByTransferId(quint32 transferId) const;
    Task *singleTaskInState(TransferState st) const;    // 只在"同时仅一路传输"的约束下用到

    void handleUploadBeginOk(const FileMessage &msg);
    void handleUploadEndOk(const FileMessage &msg);
    void uploadNextStep(quint32 taskId);
    bool hasFreeSlot(TransferPriority p) const;
    void tryStartNextTask();
    void upAndDownloadbegin(Task *task);

    void failTask(quint32 taskId, int errcode, const QString &text);
    void finishTask(quint32 taskId);

    void handleDownloadBeginOk(const FileMessage &msg);
    void sendDownloadAbort(quint32 transferId);

    FileIoWorker *m_ioWorker = nullptr;
    QTimer *m_stallSweep = nullptr;
    quint32 m_nextTaskId = 1;
    QHash<quint32, Task *> m_tasks;                  // taskId -> 任务
    QHash<quint32, quint32> m_taskIdByTransferId;    // 服务端 transferId -> taskId
    QList<quint32> m_pendingQueue;   // 排队中的 taskId，按到达顺序
};

#endif // FILETRANSFERMANAGER_H
