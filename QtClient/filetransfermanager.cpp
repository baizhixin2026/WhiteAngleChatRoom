#include "filetransfermanager.h"
#include "fileioworker.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>

namespace {
    constexpr int    kStallSweepMs    = 1000;
    constexpr qint64 kTransferIdleTimeoutMs = 60 * 1000;   // 60 秒没有任何新字节就判死
    constexpr qint64 kBeginReplyTimeoutMs    = 10  * 1000;   // begin：10 秒没等到回执（局域网 RTT 是毫秒级）

    // 服务端每会话只有 2 个传输槽（Proto::kMaxTransfersPerSession = 2），客户端自己先把闸门关好，不要等服务端回 TooManyTransfers
    constexpr int kMaxActiveTransfers  = 2;

    constexpr int kMaxBackgroundActive = 1; // 后台预取（头像）最多只占 1 格，永远给用户留一格
    constexpr int kTaskLevel = 2; // 任务优先级
}

FileTransferManager::FileTransferManager(QObject *parent)
    : QObject(parent)
{
}

FileTransferManager::~FileTransferManager()
{
    const QList<quint32> taskIds = m_tasks.keys();
    for (quint32 taskId : taskIds)
        delete m_tasks.take(taskId);
}

void FileTransferManager::abortAll(const QString &reason)
{
    const QList<quint32> ids = m_tasks.keys();
    for (quint32 taskId : ids) {
        Task *task = taskOf(taskId);
        if (!task)
            continue;
        if (task->state == TransferState::Done || task->state == TransferState::Failed)
            continue;
        failTask(taskId, Err::NotAuthenticated, reason);
    }
}

void FileTransferManager::attach(FileIoWorker *ioWorker)
{
    if (!ioWorker)
        return;
    m_ioWorker = ioWorker;
    m_stallSweep = new QTimer(this);
    m_stallSweep->setInterval(kStallSweepMs);
    connect(m_stallSweep, &QTimer::timeout, this, &FileTransferManager::checkStalledTransfers);
    m_stallSweep->start();

    // Manager(GUI) -> IO(磁盘线程)：队列连接
    connect(this, &FileTransferManager::reqIoHashFile, ioWorker, &FileIoWorker::hashFile, Qt::QueuedConnection);
    connect(this, &FileTransferManager::reqIoOpenUploadSource, ioWorker,
                            &FileIoWorker::openUploadSource, Qt::QueuedConnection);
    connect(this, &FileTransferManager::reqIoReadUploadBlock, ioWorker,
                            &FileIoWorker::readUploadBlock, Qt::QueuedConnection);
    connect(this, &FileTransferManager::reqIoCloseTask, ioWorker,
                            &FileIoWorker::closeTask, Qt::QueuedConnection);

    // IO(磁盘线程) -> Manager(GUI)
    connect(ioWorker, &FileIoWorker::fileHashed, this, &FileTransferManager::onFileHashed);
    connect(ioWorker, &FileIoWorker::fileHashFailed, this, &FileTransferManager::onFileHashFailed);
    connect(ioWorker, &FileIoWorker::uploadSourceOpened, this, &FileTransferManager::onUploadSourceOpened);
    connect(ioWorker, &FileIoWorker::uploadSourceOpenFailed, this, &FileTransferManager::onUploadSourceOpenFailed);
    connect(ioWorker, &FileIoWorker::uploadBlockRead, this, &FileTransferManager::onUploadBlockRead);
    connect(ioWorker, &FileIoWorker::uploadBlockReadFailed, this, &FileTransferManager::onUploadBlockReadFailed);

    connect(this, &FileTransferManager::reqIoOpenDownloadSink, ioWorker,
                            &FileIoWorker::openDownloadSink, Qt::QueuedConnection);
    connect(this, &FileTransferManager::reqIoWriteDownloadBlock, ioWorker,
                            &FileIoWorker::writeDownloadBlock, Qt::QueuedConnection);
    connect(this, &FileTransferManager::reqIoCloseDownloadSink, ioWorker,
                            &FileIoWorker::closeDownloadSink, Qt::QueuedConnection);

    connect(ioWorker, &FileIoWorker::downloadSinkOpenFailed,
                            this, &FileTransferManager::onDownloadSinkOpenFailed);
    connect(ioWorker, &FileIoWorker::downloadBlockWritten,
                            this, &FileTransferManager::onDownloadBlockWritten);
    connect(ioWorker, &FileIoWorker::downloadBlockWriteFailed,
                            this, &FileTransferManager::onDownloadBlockWriteFailed);
    connect(ioWorker, &FileIoWorker::downloadSinkClosed,
                            this, &FileTransferManager::onDownloadSinkClosed);
}

void FileTransferManager::checkStalledTransfers()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<quint32> ids = m_tasks.keys();
    for (quint32 taskId : ids) {
        Task *task = taskOf(taskId);
        if (!task || task->lastActivityAt <= 0)
            continue;  // Queued（还没放行）不参与超时

        const qint64 time = now - task->lastActivityAt;

        // begin 是队列的唯一推进条件：begin 回执丢了, 队列永久停摆, 单点故障升级成了全局故障，必须补超时
        if (task->state == TransferState::BeginSent) {
            if (time > kBeginReplyTimeoutMs) {
                failTask(taskId, Err::ServiceUnavailable, QStringLiteral("服务器没有响应，请重试"));
            }
            continue;
        }

        if (task->state != TransferState::Streaming)
            continue;
        if (time  <= kTransferIdleTimeoutMs)
            continue;
        failTask(taskId, Err::TransferAborted, QStringLiteral("文件传输长时间没有进展，请重试"));
    }
}

quint32 FileTransferManager::startUpload(const QString &localPath, const QString &fileName, const QString &mime, quint8 biz)
{
    if (localPath.isEmpty() || fileName.isEmpty() || mime.isEmpty()) {
        qWarning() << "startUpload 参数不完整 " << localPath << " " << fileName << " " << mime;
        return 0;
    }

    if (!Proto::isValidBizType(biz)) {
        qWarning() << "startUpload biz 无效 " << biz;
        return 0;
    }

    const QFileInfo info(localPath);
    if (!info.isFile() || info.size() <= 0) {
        qWarning() << "startUpload 源文件不可用 " << localPath;
        return 0;
    }

    if (info.size() > Proto::kMaxFileSize) {
        qWarning() << "startUpload 文件超过 2 GiB " << localPath;
        return 0;
    }

    auto *task = new Task;
    task->t.taskId    = m_nextTaskId++;
    task->t.direction = Proto::TransferUpload;
    task->t.biz       = biz;
    task->t.localPath = localPath;
    task->t.fileName  = fileName;
    task->t.mime      = mime;
    task->t.totalSize = info.size();
    task->priority    = TransferPriority::User;
    task->state       = TransferState::Hashing;
    m_tasks.insert(task->t.taskId, task);

    emit reqIoHashFile(task->t.taskId, localPath);

    qInfo() << "[上传] 开始 taskId=" << task->t.taskId << " size=" << task->t.totalSize << " file=" << fileName;
    return task->t.taskId;
}

void FileTransferManager::onFileHashed(quint32 taskId, const QString &sha256, qint64 size)
{
    Task *task = taskOf(taskId);
    if (!task || task->state != TransferState::Hashing) {
        // 任务已经被取消/失败了，迟到的哈希结果丢弃。
        return;
    }


    if (size != task->t.totalSize) {
        failTask(taskId, Err::FileCorrupted, QStringLiteral("文件在传输前被修改，请重新发送"));
        return;
    }

    task->t.sha256 = sha256;
    task->t.totalSize = size;
    task->state = TransferState::Queued;
    m_pendingQueue.append(taskId);

    tryStartNextTask();  // 交给闸门
}

void FileTransferManager::onFileHashFailed(quint32 taskId, const QString &reason)
{
    failTask(taskId, Err::StorageIoError, reason);
}

void FileTransferManager::onFileMessage(const FileMessage &msg)
{
    if (msg.type == Proto::kFileUploadBeginOk) {
        handleUploadBeginOk(msg);
    }
    else if (msg.type == Proto::kFileUploadEndOk) {
        handleUploadEndOk(msg);
    }
    else if (msg.type == Proto::kFileDownloadBeginOk) {
        handleDownloadBeginOk(msg);
    }

    else {
        qWarning() << "[上传] 未处理的文件控制面消息:" << msg.type;
    }
}

void FileTransferManager::handleUploadBeginOk(const FileMessage &msg)
{
    // 服务端失败时复用了 file_upload_begin_ok，必须按 ok 分流
    if (!msg.ok) {
        Task *task = taskByTransferId(msg.transferId);
        if (!task)
            task = singleTaskInState(TransferState::BeginSent);
        if (!task) {
            qWarning() << "[上传] begin 失败回执无法归属 transferId=" << msg.transferId
                       << "errcode=" << msg.errcode;
            return;
        }
        failTask(task->t.taskId, msg.errcode, msg.text);
        return;
    }

    Task *task = singleTaskInState(TransferState::BeginSent);
    if (!task) {
        qWarning() << "[上传] 收到 begin_ok 但没有等待中的任务 transferId=" << msg.transferId;
        return;
    }

    task->t.transferId = msg.transferId;
    task->t.fileId     = msg.fileId;
    task->t.sentBytes  = msg.offset;      // >0 = 服务端已有 .part，从断点续传
    m_taskIdByTransferId.insert(msg.transferId, task->t.taskId);

    task->state = TransferState::OpeningSource;
    emit reqIoOpenUploadSource(task->t.taskId, task->t.localPath, msg.offset);
    tryStartNextTask();
}

void FileTransferManager::onUploadSourceOpened(quint32 taskId, qint64 size)
{
    Task *task = taskOf(taskId);
    if (!task || task->state != TransferState::OpeningSource)
        return;

    // 打开时磁盘大小和 begin 声明的大小必须一致，否则源文件在期间被改过
    if (size != task->t.totalSize) {
        failTask(taskId, Err::FileCorrupted, QStringLiteral("文件在传输前被修改，请重新发送"));
        return;
    }

    task->state = TransferState::Streaming;
    task->streamClock.start();      // 传输时间
    uploadNextStep(taskId);
}

void FileTransferManager::onUploadBlockRead(quint32 taskId, quint64 offset, const QByteArray &data)
{
    Task *task = taskOf(taskId);
    if (!task || task->state != TransferState::Streaming)
        return;

    FileChunk chunk;
    chunk.transferId = task->t.transferId;
    chunk.offset     = offset;
    chunk.data       = data;
    emit reqSendFileChunk(chunk);

    task->t.sentBytes   = qint64(offset) + data.size();
    task->lastActivityAt = QDateTime::currentMSecsSinceEpoch();

    emit uploadProgress(task->t.taskId, task->t.sentBytes, task->t.totalSize,
                        task->streamClock.isValid() ? task->streamClock.elapsed() : 0);

    uploadNextStep(taskId);
}

void FileTransferManager::uploadNextStep(quint32 taskId)
{
    Task *task = taskOf(taskId);
    if (!task || task->state != TransferState::Streaming)
        return;

    // 已发满
    if (task->t.sentBytes >= task->t.totalSize) {
        // 数据面发完：发 end，等服务端重算 sha256 + 落库
        FileMessage end;
        end.type       = Proto::kFileUploadEnd;
        end.transferId = task->t.transferId;
        end.sha256     = task->t.sha256;
        task->state = TransferState::EndSent;
        emit reqSendFileMessage(end);
        return;
    }

    // 尚未发满
    emit reqIoReadUploadBlock(taskId);
}

void FileTransferManager::handleUploadEndOk(const FileMessage &msg)
{
    // 失败回执可能在 Streaming 中途就到了（例如 TransferCongested），这里要一并处理
    Task *task = taskByTransferId(msg.transferId);
    if (!task)
        task = singleTaskInState(TransferState::EndSent);
    if (!task) {
        qWarning() << "[上传] end_ok 无法归属 transferId=" << msg.transferId;
        return;
    }

    if (!msg.ok) {
        failTask(task->t.taskId, msg.errcode, msg.text);
        return;
    }

    task->t.fileId = msg.fileId.isEmpty() ? msg.meta.fileId : msg.fileId;
    task->state = TransferState::Done;
    finishTask(task->t.taskId);

    qInfo() << "[上传] 完成 taskId=" << task->t.taskId
            << "fileId=" << task->t.fileId << "size=" << msg.meta.size;
    emit uploadFinished(task->t.taskId, task->t.fileId, msg.meta);
}

void FileTransferManager::onUploadSourceOpenFailed(quint32 taskId, const QString &reason)
{
    failTask(taskId, Err::StorageIoError, reason);
}

void FileTransferManager::onUploadBlockReadFailed(quint32 taskId, const QString &reason)
{
    failTask(taskId, Err::StorageIoError, reason);
}

void FileTransferManager::failTask(quint32 taskId, int errcode, const QString &text)
{
    Task *task = taskOf(taskId);
    if (!task || task->state == TransferState::Failed || task->state == TransferState::Done)
        return;

    task->state   = TransferState::Failed;
    task->errcode = errcode;
    task->text    = text;

    if (task->t.direction == Proto::TransferDownload) {
        if (task->t.transferId != 0) {
            sendDownloadAbort(task->t.transferId);
            emit reqIoCloseDownloadSink(taskId, false);   // 不提交：删掉 .part
        }
    }

    finishTask(taskId);
    qWarning() << "[传输] 失败 taskId=" << taskId << (task->t.direction == Proto::TransferUpload ? " [上传]" : " [下载]")
               << " errcode=" << errcode << text;

    if (task->t.direction == Proto::TransferUpload)
        emit uploadFailed(taskId, errcode, text);
    else
        emit downloadFailed(taskId, errcode, text);
}

void FileTransferManager::finishTask(quint32 taskId)
{
    Task *task = taskOf(taskId);
    if (!task)
        return;
    if (task->t.transferId != 0)
        m_taskIdByTransferId.remove(task->t.transferId);
    emit reqIoCloseTask(taskId);
    tryStartNextTask(); // 腾出槽位，放行下一个
}

FileTransferManager::Task *FileTransferManager::taskOf(quint32 taskId) const
{
    return m_tasks.value(taskId, nullptr);
}

FileTransferManager::Task *FileTransferManager::taskByTransferId(quint32 transferId) const
{
    const quint32 taskId = m_taskIdByTransferId.value(transferId, 0);
    return taskId ? taskOf(taskId) : nullptr;
}

// 只在"同一时刻只有一路上传"的前提下可靠；等做多附件并发时换成 transferId 精确匹配
FileTransferManager::Task *FileTransferManager::singleTaskInState(TransferState st) const
{
    Task *found = nullptr;
    for (Task *t : m_tasks) {
        if (t->state != st)
            continue;
        if (found)
            return nullptr; // 发现第二个就立刻放弃, 只有唯一一个时才返回它
        found = t;
    }
    return found;
}


// ============================ 下载 ============================

quint32 FileTransferManager::startDownload(const QString &fileId, const QString &savePath, TransferPriority priority)
{
    if (fileId.isEmpty() || savePath.isEmpty()) {
        qWarning() << "startDownload 参数不完整" << fileId << savePath;
        return 0;
    }

    auto *task = new Task;
    task->t.taskId    = m_nextTaskId++;
    task->t.direction = Proto::TransferDownload;
    task->t.fileId    = fileId;
    task->t.localPath = savePath;
    task->priority    = priority;
    task->state       = TransferState::Queued;
    m_tasks.insert(task->t.taskId, task);
    m_pendingQueue.append(task->t.taskId);
    tryStartNextTask();

    if (priority == TransferPriority::User)
        qInfo() << "[下载] 开始 taskId=" << task->t.taskId << " fileId=" << fileId;
    return task->t.taskId;
}

// 还有没有空槽可以放行这个优先级的任务
bool FileTransferManager::hasFreeSlot(TransferPriority p) const
{
    int active = 0;
    int background = 0;

    for (Task *t : m_tasks) {
        switch (t->state) {
            case TransferState::Idle:
            case TransferState::Hashing:      // 还没发 begin，不占服务端槽位
            case TransferState::Queued:       // 排队中，同样不占
            case TransferState::Done:
            case TransferState::Failed:
            case TransferState::Canceled:
                continue;
            default:
                break;
        }

        // 注意：BeginSent / OpeningSource / Streaming / EndSent / Finalizing
        // 在服务端都占着并发槽（服务端从 begin 那一刻就 addUploadRecord），必须计入。
        // 不要"优化"成只数 BeginSent —— 那等于把整个数据面漏出闸门。
        ++active;
        if (t->priority == TransferPriority::Background)
            ++background;
    }

    if (active >= kMaxActiveTransfers)
        return false;
    if (p == TransferPriority::Background && background >= kMaxBackgroundActive)
        return false;
    return true;
}

// 真正发出 begin。一次只放行一个，下一次由 begin_ok / finishTask 继续推。
void FileTransferManager::upAndDownloadbegin(Task *task)
{
    if (!task)
        return;

    if (task->t.direction == Proto::TransferUpload) {
        FileMessage begin;
        begin.type     = Proto::kFileUploadBegin;
        begin.biz      = task->t.biz;
        begin.fileName = task->t.fileName;
        begin.fileSize = task->t.totalSize;
        begin.sha256   = task->t.sha256;
        begin.mime     = task->t.mime;
        task->state = TransferState::BeginSent;
        emit reqSendFileMessage(begin);
        return;
    }

    FileMessage begin;
    begin.type   = Proto::kFileDownloadBegin;
    begin.fileId = task->t.fileId;
    begin.offset = 0;
    begin.length = -1;
    task->state = TransferState::BeginSent;
    emit reqSendFileMessage(begin);
}

// 队列泵：按优先级找第一个排队任务；begin 全局串行, 一次只放行一个
void FileTransferManager::tryStartNextTask()
{
    // 任意时刻最多只有 1 个任务处于 BeginSent（begin 串行）
    for (Task *t : m_tasks) {
        if (t->state == TransferState::BeginSent)
            return;
    }

    for (int level = 0; level < kTaskLevel; ++level) {
        const TransferPriority want = (level == 0) ? TransferPriority::User : TransferPriority::Background;
        for (int i = 0; i < m_pendingQueue.size(); ++i) {
            Task *task = taskOf(m_pendingQueue.at(i));
            if (!task || task->state != TransferState::Queued || task->priority != want)
                continue;
            if (!hasFreeSlot(want))
                return;
            m_pendingQueue.removeAt(i);
            upAndDownloadbegin(task);
            return;  // 一次只放行一个
        }
    }
}

void FileTransferManager::handleDownloadBeginOk(const FileMessage &msg)
{
    // file_download_begin_ok 被服务端复用了两种语义：
    //   ① begin 阶段就被拒（此时我们还没有 transferId 映射）
    //   ② 传输中途失败（此时映射还在）
    // 靠"能不能按 transferId 找到任务"来区分，别用 type 区分。
    Task *task = taskByTransferId(msg.transferId);

    if (!msg.ok) {
        if (!task)
            task = singleTaskInState(TransferState::BeginSent);
        if (!task) {
            qWarning() << "[下载] 失败回执无法归属 transferId=" << msg.transferId << " errcode=" << msg.errcode;
            return;
        }
        failTask(task->t.taskId, msg.errcode, msg.text);
        return;
    }

    if (!task)
        task = singleTaskInState(TransferState::BeginSent);
    if (!task) {
        qWarning() << "[下载] 收到 begin_ok 但没有等待中的任务 transferId=" << msg.transferId;
        return;
    }

    task->t.transferId    = msg.transferId;
    if (!msg.fileId.isEmpty())
        task->t.fileId = msg.fileId;
    task->t.startOffset   = msg.offset;
    task->t.totalSize     = msg.totalSize;
    task->t.receivedBytes = 0;
    // 下载没有结束包：应收多少字节必须现在就算出来
    task->expectedLength  = (msg.length < 0) ? (msg.totalSize - msg.offset) : msg.length;
    m_taskIdByTransferId.insert(msg.transferId, task->t.taskId);

    task->streamClock.start();
    task->state = TransferState::Streaming;
    emit reqIoOpenDownloadSink(task->t.taskId, task->t.localPath, msg.offset);
    tryStartNextTask();

    // 边界：长度为 0 的文件（服务端不允许上传空文件，这里只是兜底）
    if (task->expectedLength <= 0) {
        task->state = TransferState::Finalizing;
        emit reqIoCloseDownloadSink(task->t.taskId, true);
    }
}

void FileTransferManager::onDownloadChunkReceived(const FileChunk &chunk)
{
    Task *task = taskByTransferId(chunk.transferId);
    if (!task || task->state != TransferState::Streaming)
        return;   // 不认识的 transferId / 已结束的任务，直接丢

    const qint64 expectedOffset = task->t.startOffset + task->t.receivedBytes;
    if (qint64(chunk.offset) != expectedOffset) {
        // 数据面失步：偏移必须严格连续，服务端没有"重传某一块"的语义，只能整体重来
        sendDownloadAbort(task->t.transferId);
        failTask(task->t.taskId, Err::FileCorruption, QStringLiteral("文件数据错位，请重新下载"));
        return;
    }

    const qint64 newTotal = task->t.receivedBytes + chunk.data.size();
    // 超过 begin_ok 声明的长度 = 协议级异常，这一块不能写盘
    if (newTotal > task->expectedLength) {
        sendDownloadAbort(task->t.transferId);
        failTask(task->t.taskId, Err::FileCorruption, QStringLiteral("文件长度与声明不一致，请重新下载"));
        return;
    }

    emit reqIoWriteDownloadBlock(task->t.taskId, chunk.offset, chunk.data);
    task->t.receivedBytes = newTotal;
    task->lastActivityAt  = QDateTime::currentMSecsSinceEpoch();
    emit downloadProgress(task->t.taskId, newTotal, task->expectedLength,
                          task->streamClock.isValid() ? task->streamClock.elapsed() : 0);

    if (newTotal == task->expectedLength) { // 严格相等才算完
        // 收满了：没有服务端的 end 包，这就是唯一的完成判据
        task->state = TransferState::Finalizing;
        emit reqIoCloseDownloadSink(task->t.taskId, true);
    }
}

void FileTransferManager::onDownloadSinkOpened(quint32 taskId)
{
    Q_UNUSED(taskId);   // 只用于记录：块的写入由队列顺序保证在 open 之后
}

void FileTransferManager::onDownloadSinkOpenFailed(quint32 taskId, const QString &reason)
{
    Task *task = taskOf(taskId);
    if (task && task->t.transferId != 0)
        sendDownloadAbort(task->t.transferId);      // 让服务端别再发了
    failTask(taskId, Err::StorageIoError, reason);
}

void FileTransferManager::onDownloadBlockWritten(quint32 taskId, qint64 writtenBytes)
{
    Q_UNUSED(taskId);
    Q_UNUSED(writtenBytes);
}

void FileTransferManager::onDownloadBlockWriteFailed(quint32 taskId, const QString &reason)
{
    Task *task = taskOf(taskId);
    if (task && task->t.transferId != 0)
        sendDownloadAbort(task->t.transferId);
    failTask(taskId, Err::StorageIoError, reason);
}

void FileTransferManager::onDownloadSinkClosed(quint32 taskId, bool ok, const QString &reason)
{
    Task *task = taskOf(taskId);
    if (!task || task->state != TransferState::Finalizing)
        return;

    if (!ok) {
        failTask(taskId, Err::StorageIoError, reason);
        return;
    }

    task->state = TransferState::Done;
    finishTask(taskId);

    if (task->priority == TransferPriority::User)
        qInfo() << "[下载] 完成 taskId=" << taskId << "bytes=" << task->t.receivedBytes;
    emit downloadFinished(taskId, task->t.localPath, task->t.receivedBytes);
}

void FileTransferManager::sendDownloadAbort(quint32 transferId)
{
    if (transferId == 0)
        return;
    FileMessage abort;
    abort.type       = Proto::kFileDownloadAbort;
    abort.transferId = transferId;
    emit reqSendFileMessage(abort);
}
