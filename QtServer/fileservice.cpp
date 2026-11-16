#include "fileservice.h"
#include "shardroute.h"

#include <QDateTime>
#include <QUuid>

namespace {
    constexpr int    kDbRequestTimeoutMs = 10 * 1000;
    constexpr qint64 kSweepIntervalMs    = 5000;

    // 回收巡检
    // 存储层的两条回收链: 用户删 → status=2 → 巡检回收（5 分钟）,
    constexpr int    kReclaimIntervalMs     = 1 * 60 * 1000;   // 5 分钟一轮
    constexpr int    kReclaimStartupDelayMs = 5 * 1000;        // 启动后先等 5 秒（等数据库就绪）
    constexpr int    kReclaimBatch          = 200;             // 单轮最多处理多少个对象

    // 孤儿/残留 → 磁盘对账（启动后 30 秒 + 每 6 小时）
    constexpr int kStorageSweepIntervalMs   = 6 * 60 * 60 * 1000;   // 6 小时一轮
    constexpr int kStorageSweepStartupDelay = 30 * 1000;            // 启动后 30 秒先跑一次


    inline qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

    // sha 必须是 64 位小写十六进制
    bool isSha256Hex(const QString &s)
    {
        if (s.size() != Proto::kSha256HexLen)
            return false;
        for (const QChar c : s) {
            const ushort u = c.unicode();
            if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f')))
                return false;
        }
        return true;
    }

    // 服务端独立清洗文件名：客户端做没做过都不可信, 清洗只影响显示名，物理文件名永远是 sha256，不参与拼接。
    QString sanitizeFileName(const QString &in)
    {
        QString s = in.simplified(); // 规范化空白: 去掉首尾的空白字符 + 把中间连续空白折叠成一个空格( \t \n \r 也当空白处理 )
        const int slash = qMax(s.lastIndexOf(QLatin1Char('/')), s.lastIndexOf(QLatin1Char('\\'))); // 去掉路径
        if (slash >= 0)
            s = s.mid(slash + 1);

        // 去掉控制字符
        QString out;
        out.reserve(s.size());
        for (const QChar c : s) {
            const ushort u = c.unicode();
            if (u < 0x20 || u == 0x7F)
                continue;
            out.append(c);
        }
        // Windows 保留名
        static const QSet<QString> kReserved = {
            QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
            QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
            QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("LPT1"),
            QStringLiteral("LPT2"), QStringLiteral("LPT3"),
        };
        const QString stem = out.section(QLatin1Char('.'), 0, 0).toUpper();
        if (kReserved.contains(stem))
            out.prepend(QLatin1Char('_')); // 示例 CON.png → _CON.png

        // 以点开头（.'/..' 或隐藏名）一律去掉前导点
        while (out.startsWith(QLatin1Char('.')) || out.startsWith(QLatin1Char(' ')))
            out.remove(0, 1);

        // 结尾的点和空格在 Windows 上非法
        while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
            out.chop(1);

        out = out.left(Proto::kMaxFileNameLen).trimmed();
        if (out.isEmpty())
            out = QStringLiteral("未命名文件");
        return out;  // 非空、单行、无控制字符、无路径、长度合规的字符串, “安全可显示”的名字
    }
}

FileService::FileService(int shardCount, QObject *parent)
    : QObject(parent), m_shardCount(qMax(1, shardCount))
{

}

FileService::~FileService()
{
    cleanup();
}

void FileService::init()
{
    if (!m_sweep) {
        m_sweep = new QTimer(this);
        m_sweep->setInterval(int(kSweepIntervalMs));
        connect(m_sweep, &QTimer::timeout, this, &FileService::expireCommitWaits);
        connect(m_sweep, &QTimer::timeout, this, &FileService::expireFileReadWaits);
        connect(m_sweep, &QTimer::timeout, this, &FileService::expireMarkDeletedWaits);
    }
    m_sweep->start();

    if (!m_reclaimTimer) {
        m_reclaimTimer = new QTimer(this);
        m_reclaimTimer->setInterval(int(kReclaimIntervalMs));
        connect(m_reclaimTimer, &QTimer::timeout, this, &FileService::sweepReclaimableObjects);
    }
    m_reclaimTimer->start();
    // 启动后 延时5s, 先扫一轮：进程重启是最常见的"定时任务丢失"场景，不能只靠周期触发
    QTimer::singleShot(kReclaimStartupDelayMs, this, &FileService::sweepReclaimableObjects);

    if (!m_storageSweepTimer) {
        m_storageSweepTimer = new QTimer(this);
        m_storageSweepTimer->setInterval(kStorageSweepIntervalMs);
        connect(m_storageSweepTimer, &QTimer::timeout, this, &FileService::sweepStorage);
    }
    m_storageSweepTimer->start();
    QTimer::singleShot(kStorageSweepStartupDelay, this, &FileService::sweepStorage);

    emit logMessage(QStringLiteral("文件服务就绪，存储分片数 %1").arg(m_shardCount));
}

void FileService::cleanup()
{
    if (m_sweep)
        m_sweep->stop();
    if (m_reclaimTimer)
        m_reclaimTimer->stop();
    if (m_storageSweepTimer)
        m_storageSweepTimer->stop();

    const QList<quint32> uploadIds = m_transfers.keys();
    for (quint32 transferId : uploadIds) {
        auto live = m_transfers.find(transferId);
        if (live != m_transfers.end() && live->ctx.dir == Proto::TransferUpload)
            suspendUpload(transferId);
    }

    const QList<quint32> downloadIds = m_transfers.keys();
    for (quint32 transferId : downloadIds) {
        auto live = m_transfers.find(transferId);
        if (live != m_transfers.end() && live->ctx.dir == Proto::TransferDownload)
            closeDownload(transferId);
    }

    m_transfers.clear();
    m_transferIdsBySession.clear();
    m_commitWaits.clear();
    m_fileReadWaits.clear();
    m_markDeletedWaits.clear();
}

// 收尾 INSERT 超时：客户端已经拿到 begin_ok，只能回 end 失败；磁盘对象交给离线清扫
void FileService::expireCommitWaits()
{
    const qint64 now = nowMs();
    const QList<quint32> transferIds = m_commitWaits.keys();
    for (quint32 transferId : transferIds) {
        auto it = m_commitWaits.find(transferId);
        if (it == m_commitWaits.end())
            continue;
        if (now - it.value().enqueuedAt <= kDbRequestTimeoutMs)
            continue;

        const CommitWait wait = it.value();
        m_commitWaits.erase(it);

        removeUploadRecord(transferId);
        emit logMessage(QStringLiteral("文件上传:落库超时 transferId=%1 fileId=%2")
                            .arg(transferId).arg(wait.meta.fileId));
        emit uploadEndFailed(wait.sessionId, transferId, Err::ServiceUnavailable);
    }
}

// 权限查询超时：客户端还在等 download begin_ok
void FileService::expireFileReadWaits()
{
    const qint64 now = nowMs();
    const QList<quint32> ids = m_fileReadWaits.keys();
    for (quint32 transferId : ids) {
        auto it = m_fileReadWaits.find(transferId);
        if (it == m_fileReadWaits.end())
            continue;
        if (now - it.value().enqueuedAt <= kDbRequestTimeoutMs)
            continue;

        const FileReadWait wait = it.value();
        m_fileReadWaits.erase(it);

        emit logMessage(QStringLiteral("文件下载:读取权限查询超时 uid=%1").arg(wait.uid));
        emit downloadBeginFailed(wait.sessionId, wait.transferId, Err::ServiceUnavailable);
    }
}

// 标记删除超时：只能回 file_delete_ok 失败
void FileService::expireMarkDeletedWaits()
{
    const qint64 now = nowMs();
    const QList<QString> ids = m_markDeletedWaits.keys();
    for (const QString &fileId : ids) {
        auto it = m_markDeletedWaits.find(fileId);
        if (it == m_markDeletedWaits.end())
            continue;
        if (now - it.value().enqueuedAt <= kDbRequestTimeoutMs)
            continue;

        const MarkDeletedWait wait = it.value();
        m_markDeletedWaits.erase(it);

        emit logMessage(QStringLiteral("文件标记删除超时 sessionId=%1 fileId=%2").arg(wait.sessionId).arg(wait.fileId));
        emit fileDeleted(wait.sessionId, wait.fileId, Err::ServiceUnavailable);
    }
}


// 校验
int FileService::validateUpload(qint64 uid, const FileMessage &req) const
{
    if (uid <= 0)
        return Err::NotAuthenticated;
    if (!Proto::isValidBizType(req.biz))
        return Err::InvalidParam;
    if (req.fileSize <= 0 || req.fileSize > Proto::kMaxFileSize)
        return Err::FileTooLarge;
    if (req.biz == Proto::BizAvatar && req.fileSize > Proto::kMaxAvatarFileSize)
        return Err::FileTooLarge;
    if (!isSha256Hex(req.sha256))
        return Err::InvalidParam;
    if (req.fileName.trimmed().isEmpty())
        return Err::FileNameEmpty;
    if (req.fileName.size() > Proto::kMaxFileNameLen * 4)
        return Err::FileNameTooLong;
    return Err::Ok;
}

int FileService::validateDownload(qint64 uid, const FileMessage &req) const
{
    if (uid <= 0)
        return Err::NotAuthenticated;
    if (req.fileId.isEmpty() || req.fileId.size() > 36)  // file_id VARCHAR(36)
        return Err::FileCorruption;
    return Err::Ok;
}

// 传输上限
bool FileService::hasCapacity(quint64 sessionId) const
{
    if(m_transfers.size() >= Proto::kMaxConcurrentTransfers) // 服务端全局并发传输上限
        return false;
    return m_transferIdsBySession.value(sessionId).size() < Proto::kMaxTransfersPerSession; // 每个用户传输上限
}

// 记录：每个用户传输数量
void FileService::addUploadRecord(const Live &live)
{
    m_transfers.insert(live.ctx.transferId, live);
    m_transferIdsBySession[live.sessionId].insert(live.ctx.transferId);
}

void FileService::addDownloadRecord(const Live &live)
{
    m_transfers.insert(live.ctx.transferId, live);
    m_transferIdsBySession[live.sessionId].insert(live.ctx.transferId);
}

// 会话索引
void FileService::removeTransferIdFromSession(quint64 sessionId, quint32 transferId)
{
    auto transferIds = m_transferIdsBySession.find(sessionId);
    if (transferIds == m_transferIdsBySession.end())
        return;

    transferIds->remove(transferId);
    if (transferIds->isEmpty())
        m_transferIdsBySession.erase(transferIds);
}

// 只删记录
void FileService::removeUploadRecord(quint32 transferId)
{
    auto live = m_transfers.find(transferId);
    if (live == m_transfers.end() || live->ctx.dir != Proto::TransferUpload)
        return;

    const quint64 sessionId = live->sessionId;
    m_transfers.erase(live);

    removeTransferIdFromSession(sessionId, transferId);
}

void FileService::removeDownloadRecord(quint32 transferId)
{
    auto live = m_transfers.find(transferId);
    if (live == m_transfers.end() || live->ctx.dir != Proto::TransferDownload)
        return;

    const quint64 sessionId = live->sessionId;
    m_transfers.erase(live);

    removeTransferIdFromSession(sessionId, transferId);
}

// 用户取消
void FileService::cancelUpload(quint32 transferId)
{
    auto it = m_transfers.find(transferId);
    if (it == m_transfers.end() || it->ctx.dir != Proto::TransferUpload)
        return;

    emit reqDeleteUploadPart(it->ctx.shard, transferId);
    removeUploadRecord(transferId);
}

void FileService::closeDownload(quint32 transferId)
{
    auto it = m_transfers.find(transferId);
    if (it == m_transfers.end() || it->ctx.dir != Proto::TransferDownload)
        return;

    emit reqCloseDownload(it->ctx.shard, transferId);
    removeDownloadRecord(transferId);
}

// 断线不是数据错误：.part 留着，客户端重连能续传
void FileService::suspendUpload(quint32 transferId)
{
    auto it = m_transfers.find(transferId);
    if (it == m_transfers.end() || it->ctx.dir != Proto::TransferUpload)
        return;

    emit reqKeepUploadPart(it->ctx.shard, transferId);
    removeUploadRecord(transferId);
}

/*
 * 校验 uid / biz 合法性 / 2GiB 上限 / 头像 512KiB 上限 / sha 必须 64 位小写十六进制 / 文件名非空
 * 每会话 2 路、全局 200 路并发闸门
 * 服务端本地生成 fileId(QUuid) + 分配 transferId
 * writeShard(sha) 选存储分片
 */
void FileService::beginUpload(quint64 sessionId, qint64 uid, const FileMessage &req)
{
    const quint32 transferId = m_nextTransferId++;   // 先分配：失败回包也要能定位是哪一路
    const int err = validateUpload(uid, req);
    if (err != Err::Ok) {
        emit uploadBeginFailed(sessionId, transferId, err);
        return;
    }
    if (!hasCapacity(sessionId)) {
        emit uploadBeginFailed(sessionId, transferId, Err::TooManyTransfers);
        return;
    }

    const qint64 now = nowMs();

    Live live;
    live.sessionId = sessionId;
    live.fileName  = sanitizeFileName(req.fileName);
    live.mime      = req.mime;

    // 每次上传 一个独立file_id，服务端本地生成
    live.meta.fileId    = QUuid::createUuid().toString(QUuid::WithoutBraces);
    live.meta.sha256    = req.sha256;
    live.meta.size      = req.fileSize;
    live.meta.mime      = live.mime;
    live.meta.fileName  = live.fileName;
    live.meta.ownerUid  = uid;
    live.meta.biz       = req.biz;
    live.meta.status    = Proto::FileUploading;
    live.meta.createdAt = now;

    live.ctx.transferId   = transferId;
    live.ctx.dir          = Proto::TransferUpload;
    live.ctx.biz          = req.biz;
    live.ctx.uid          = uid;
    live.ctx.fileId       = live.meta.fileId;
    live.ctx.sha256       = req.sha256;
    live.ctx.declaredSize = req.fileSize;
    live.ctx.totalSize    = req.fileSize;
    live.ctx.shard        = ShardRoute::writeShard(req.sha256, m_shardCount);
    live.ctx.createdAt    = now;
    live.ctx.lastActiveAt = now;

    addUploadRecord(live);
    emit reqProbeUpload(live.ctx);
}

void FileService::onUploadProbed(const ChunkResult &r)
{
    auto live = m_transfers.find(r.transferId);
    if (live == m_transfers.end() || live->ctx.dir != Proto::TransferUpload)
        return;
    const quint64 sessionId = live->sessionId;
    if (!r.ok) {
        removeUploadRecord(r.transferId);
        emit uploadBeginFailed(sessionId, r.transferId, r.errcode);
        return;
    }
    // 通过检测
    live->ctx.received     = r.received;
    live->ctx.lastActiveAt = nowMs();
    emit uploadBeginOk(sessionId, r.transferId, live->ctx.shard, live->ctx.fileId, r.received, live->ctx.declaredSize);
}

void FileService::finishUpload(quint64 sessionId, quint32 transferId, const QString &finalSha256)
{
    auto live = m_transfers.find(transferId);
    if (live == m_transfers.end() || live->sessionId != sessionId) {
        emit uploadEndFailed(sessionId, transferId, Err::TransferNotFound);
        return;
    }

    if (!finalSha256.isEmpty() && finalSha256 != live->ctx.sha256) {
        cancelUpload(transferId);  // delete .part
        emit uploadEndFailed(sessionId, transferId, Err::FileCorrupted);
        return;
    }

    live->ctx.lastActiveAt = nowMs();
    emit reqFinishUpload(live->ctx.shard, transferId);
}

void FileService::onUploadFinished(const ChunkResult &r, const QString &sha256)
{
    auto it = m_transfers.find(r.transferId);
    if (it == m_transfers.end() || it->ctx.dir != Proto::TransferUpload)
        return;

    const quint64 sessionId = it->sessionId;

    if (!r.ok) {
        removeUploadRecord(r.transferId);
        emit uploadEndFailed(sessionId, r.transferId, r.errcode);
        return;
    }

    CommitWait wait;
    wait.sessionId   = sessionId;
    wait.meta          = it->meta;
    wait.meta.fileName = it->fileName;          // 用本次上传者自己的显示名
    wait.meta.mime     = it->mime;
    wait.meta.sha256   = sha256;                // 以落盘实测为准
    wait.meta.size     = it->ctx.declaredSize;
    wait.meta.status = Proto::FileReady;
    wait.enqueuedAt  = nowMs();

    m_commitWaits.insert(r.transferId, wait);
    emit reqCommitFile(r.transferId, wait.meta);
}


void FileService::onCommitResult(quint32 transferId, bool ok, int errcode)
{
    auto commitWait = m_commitWaits.find(transferId);
    if (commitWait == m_commitWaits.end())
        return;
    const CommitWait wait = commitWait.value();
    m_commitWaits.erase(commitWait);

    removeUploadRecord(transferId);        // 无论成败，这次传输都结束了

    if (!ok) {
        emit uploadEndFailed(wait.sessionId, transferId, errcode);
        return;
    }
    emit uploadEndOk(wait.sessionId, transferId, wait.meta);
}

/************************** 下载 *****************************/
void FileService::beginDownload(quint64 sessionId, qint64 uid, const FileMessage &req)
{
    const quint32 transferId = m_nextTransferId++;   // 先分配：失败回包也要能定位是哪一路
    const int err = validateDownload(uid, req);
    if(err != Err::Ok) {
        emit downloadBeginFailed(sessionId, transferId, err);
        return;
    }
    if(!hasCapacity(sessionId)) {
        emit downloadBeginFailed(sessionId, transferId, Err::TooManyTransfers);
        return;
    }

    FileReadWait wait;
    wait.sessionId  = sessionId;
    wait.uid        = uid;
    wait.transferId = transferId;
    wait.req        = req;
    wait.enqueuedAt = nowMs();

    m_fileReadWaits.insert(wait.transferId, wait);
    emit reqLoadFileForRead(wait.transferId, uid, req.fileId);
}

void FileService::abortUpload(quint64 sessionId, quint32 transferId)
{
    auto it = m_transfers.find(transferId);
    if (it == m_transfers.end() || it->sessionId != sessionId)
        return;
    cancelUpload(transferId);
}

void FileService::abortDownload(quint64 sessionId, quint32 transferId)
{
    auto it = m_transfers.find(transferId);
    if (it == m_transfers.end() || it->sessionId != sessionId)
        return;

    closeDownload(transferId);
}

void FileService::onReadableResult(quint32 transferId, bool ok, int errcode, const FileMeta &meta)
{
    auto fileReadWait = m_fileReadWaits.find(transferId);
    if (fileReadWait == m_fileReadWaits.end())
        return;
    const FileReadWait wait = fileReadWait.value();
    m_fileReadWaits.erase(fileReadWait);

    if(!ok) {
        downloadBeginFailed(wait.sessionId, wait.transferId, errcode);
        return;
    }

    // 取值范围校验：下载起点/长度 在文件范围内
    qint64 start = wait.req.offset;
    if(start < 0 || start > meta.size) {
        downloadBeginFailed(wait.sessionId, wait.transferId, Err::RangeInvalid);
        return;
    }
    const qint64 len = (wait.req.length < 0) ? (meta.size - start) : wait.req.length;
    if(len < 0 || start + len > meta.size) {
        downloadBeginFailed(wait.sessionId, wait.transferId, Err::RangeInvalid);
        return;
    }

    Live live;
    live.sessionId = wait.sessionId;
    live.ctx.uid   = wait.uid;
    live.ctx.transferId   = transferId;
    live.ctx.dir          = Proto::TransferDownload;
    live.fileName         = meta.fileName;
    live.mime             = meta.mime;
    live.meta             = meta;
    live.ctx.biz          = meta.biz;
    live.ctx.fileId       = meta.fileId;
    live.ctx.sha256       = meta.sha256;
    live.ctx.totalSize    = meta.size;
    live.ctx.declaredSize = meta.size;
    live.ctx.readOffset   = start;
    live.ctx.readLength   = len;
    live.ctx.shard        = ShardRoute::readShard(wait.transferId, m_shardCount);
    live.ctx.createdAt    = nowMs();
    live.ctx.lastActiveAt = live.ctx.createdAt;
    addDownloadRecord(live);
    emit reqOpenDownload(live.ctx);
}

void FileService::onDownloadOpened(const ChunkResult &r, qint64 totalSize)
{
    auto live = m_transfers.find(r.transferId);
    if (live == m_transfers.end())
        return;
    const quint64 sessionId = live->sessionId;
    if (!r.ok) {
        removeDownloadRecord(r.transferId);
        downloadBeginFailed(sessionId, r.transferId, r.errcode);
        return;
    }
    live->ctx.lastActiveAt = nowMs();
    if (totalSize > 0)
        live->ctx.totalSize = totalSize;

    emit downloadBeginOk(sessionId, r.transferId, live->ctx.shard, live->meta, live->ctx.readOffset, live->ctx.readLength);
}

// deleteFile：用 fileId 当键，杜绝重复下发
void FileService::deleteFile(quint64 sessionId, qint64 uid, const QString &fileId)
{
    if (uid <= 0 || fileId.isEmpty()) {
        emit fileDeleted(sessionId, fileId, Err::InvalidParam);
        return;
    }
    if (m_markDeletedWaits.contains(fileId)) {  // 同一个文件已经在删了
        emit fileDeleted(sessionId, fileId, Err::FileNotReady);
        return;
    }

    MarkDeletedWait wait;
    wait.sessionId  = sessionId;
    wait.fileId     = fileId;
    wait.enqueuedAt = nowMs();
    m_markDeletedWaits.insert(fileId, wait);

    emit reqMarkFileDeleted(uid, fileId);
}

void FileService::onDeleteResult(const QString &fileId, bool ok, int errcode)
{
    auto waitIt = m_markDeletedWaits.find(fileId);
    if (waitIt == m_markDeletedWaits.end())
        return;
    const MarkDeletedWait wait = waitIt.value();
    m_markDeletedWaits.erase(waitIt);
    emit fileDeleted(wait.sessionId, wait.fileId, ok ? Err::Ok : errcode);
}

void FileService::closeSession(quint64 sessionId)
{
    auto it = m_transferIdsBySession.find(sessionId);
    if (it == m_transferIdsBySession.end())
        return;

    // 先拷贝一份：下面会边遍历边从索引里删
    const QSet<quint32> transferIds = it.value();
    for (quint32 transferId : transferIds) {
        auto live = m_transfers.find(transferId);
        if (live == m_transfers.end())
            continue;

        // 断线不区分「谁先谁后」，但上传和下载的处理不同：
        // 上传保留 .part 等续传，下载直接关句柄。
        if (live->ctx.dir == Proto::TransferUpload)
            suspendUpload(transferId);
        else
            closeDownload(transferId);
    }
    m_transferIdsBySession.remove(sessionId);
}

// 存储侧失败 / 清扫
void FileService::onUploadFailed(const ChunkResult &r)
{
    auto live = m_transfers.find(r.transferId);
    if (live == m_transfers.end() || live->ctx.dir != Proto::TransferUpload)
        return;

    const quint64 sessionId = live->sessionId;

    // 存储线程在发这条信号之前，已经处理了是否删除 .part
    removeUploadRecord(r.transferId);
    emit uploadEndFailed(sessionId, r.transferId, r.errcode);
}

void FileService::onDownloadFailed(const ChunkResult &r)
{
    auto it = m_transfers.find(r.transferId);
    if (it == m_transfers.end() || it->ctx.dir != Proto::TransferDownload)
        return;

    const quint64 sessionId = it->sessionId;

    // 存储侧在发这条信号之前已经关掉了读句柄
    removeDownloadRecord(r.transferId);
    emit downloadFailed(sessionId, r.transferId, r.errcode);
}

// 下载结束
void FileService::onDownloadSentComplete(quint32 transferId)
{
    auto it = m_transfers.find(transferId);
    if (it == m_transfers.end() || it->ctx.dir != Proto::TransferDownload)
        return;

    // removeDownloadRecord 会把 it 擦掉，先取出 shard
    const int shard = it->ctx.shard;

    emit reqCloseDownload(shard, transferId);   // 让存储线程关句柄、删 DownloadJob
    removeDownloadRecord(transferId);           // 释放并发名额
}


// ============================ 存储对象回收 ============================
//
// 链路：DBWorker 查候选 → FileService 按内容路由到分片 → StorageWorker 删盘 → FileService → DBWorker 删记录
// 不变式（破坏任何一条都会"永远删不掉"或"误删正在用的对象"）：
//   1. 候选每次从 DB 重新推导，不维护待删队列
//   2. 删盘在删记录之前；崩在中间，下一轮会重新发现并收敛
//   3. "对象已不存在"算成功
//   4. 删除失败不标记完成，下一轮重试，且必须打日志

QString FileService::reclaimKey(quint8 biz, const QString &sha256)
{
    return QString::number(biz) + QLatin1Char(':') + sha256;
}

void FileService::sweepReclaimableObjects()
{
    // 上一轮还没消化完就跳过这一轮，避免任务堆积
    if (m_reclaimInFlight.size() >= kReclaimBatch * 2)
        return;
    emit reqQueryReclaimableObjects(kReclaimBatch);
}

void FileService::onReclaimableObjects(const QVector<ReclaimableObject> &list)
{
    for (const ReclaimableObject &o : list) {
        const QString key = reclaimKey(o.biz, o.sha256);
        if (m_reclaimInFlight.contains(key))
            continue;
        m_reclaimInFlight.insert(key);

        // 和写入用同一套路由（按内容 sha256）：删对象和 commitTo 会落在同一个分片线程，
        // 把跨线程的时序窗口压到最小
        const int shard = ShardRoute::writeShard(o.sha256, m_shardCount);
        emit reqDeleteObject(shard, o.biz, o.sha256);
    }
}

void FileService::onObjectDeleted(quint8 biz, const QString &sha256)
{
    const QString key = reclaimKey(biz, sha256);
    m_reclaimFailures.remove(key);

    // 盘上已经没了，最后一步才是删记录。
    // 顺序不能反：记录先删掉的话，万一盘上还在，就再也没人知道该删它了。
    emit reqDropReclaimedRows(biz, sha256);
}

void FileService::onObjectDeleteFailed(quint8 biz, const QString &sha256, int errcode)
{
    const QString key = reclaimKey(biz, sha256);
    m_reclaimInFlight.remove(key);

    const int times = m_reclaimFailures.value(key, 0) + 1;
    m_reclaimFailures.insert(key, times);

    // 第一次必报，之后每 12 次（约 1 小时）报一次：不刷屏，但绝不静默
    if (times == 1 || times % 12 == 0) {
        emit logMessage(QStringLiteral("存储对象回收失败 biz=%1 sha=%2 errcode=%3 连续失败 %4 次")
                                    .arg(biz).arg(sha256).arg(errcode).arg(times));
    }
}

void FileService::onReclaimedRowsDropped(quint8 biz, const QString &sha256, bool ok, int errcode)
{
    const QString key = reclaimKey(biz, sha256);
    m_reclaimInFlight.remove(key);

    if (ok) {
        m_reclaimFailures.remove(key);
        emit logMessage(QStringLiteral("回收巡检, 删除成功"));
        return;
    }
    // 盘已经删了、记录没删掉：下一轮会重新发现这个对象 → 走"不存在=成功" → 再删一次记录
    emit logMessage(QStringLiteral("回收记录删除失败 biz=%1 sha=%2 errcode=%3（下一轮回收重试）").arg(biz).arg(sha256).arg(errcode));
}

// ============================ 磁盘对账 ============================
// 解决的问题是"回收巡检看不见的垃圾"：
//   1. 盘上有、库里没有任何行的孤儿对象（commit 到一半崩溃、手工删库等）
//   2. 断线/崩溃留下的 .part
// 链路：查活集合 → 交给存储分片扫盘 → 求差集 → 删
// 红线：查询失败时**必须放弃本轮**。空集合意味着"一个活引用都没有"，拿它去扫盘等于删光整个存储。

void FileService::sweepStorage()
{
    if (m_storageSweepPending)
        return;             // 上一轮还没回来
    m_storageSweepPending = true;
    emit reqQueryLiveObjects();
}

void FileService::onLiveObjects(bool ok, const QSet<QString> &live)
{
    if (!ok) {
        m_storageSweepPending = false;
        emit logMessage(QStringLiteral("磁盘对账：查询活对象失败，本轮放弃（不会删任何文件）"));
        return;
    }

    emit logMessage(QStringLiteral("磁盘对账：活对象 %1 个，开始扫盘").arg(live.size()));

    // 只让分片 0 扫：一棵树扫一遍就够，多分片扫只是重复 IO。
    // 删除不需要分片亲和性（deleteObject 只是 unlink 一个确定路径）。
    emit reqScanStorage(0, live);
}

void FileService::onStorageScanFinished(int removedObjects, int removedParts, int keptTooNew)
{
    m_storageSweepPending = false;

    if (removedObjects == 0 && removedParts == 0)
        return;   // 干净，不刷日志

    emit logMessage(QStringLiteral("磁盘对账结束：清理孤儿对象 %1 个、残留分片 %2 个，未到期跳过 %3 个")
                        .arg(removedObjects).arg(removedParts).arg(keptTooNew));
}
