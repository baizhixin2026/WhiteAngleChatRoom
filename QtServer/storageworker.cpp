#include "storageworker.h"
#include "storageremovekey.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QStorageInfo>
#include <cerrno>

#ifdef Q_OS_UNIX
    #include <unistd.h>
#endif

namespace {

    constexpr qint64 kMaxQueuedBytes = 4 * 1024 * 1024;   // 单 transfer 积压上限（16 块）
    constexpr qint64 kSweepIntervalMs = 1000;
    constexpr int    kHashSeedBlock   = 1024 * 1024;      // 续传补算哈希的读块大小

    inline qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

    // 把系统 IO 错误映射成用户友好的错误码
    int mapIoError()
    {
        #ifdef Q_OS_UNIX
            if (errno == ENOSPC)
                return Err::StorageFull;
            #ifdef EDQUOT // POSIX 标准里的"磁盘配额超限"错误码
                if (errno == EDQUOT)
                    return Err::QuotaExceeded;
            #endif
        #endif
        return Err::StorageIoError;
    }

    // 强制数据写入存储设备（防断电丢数据）
    void syncToDisk(QFile &f)
    {
        #ifdef Q_OS_UNIX
            const int fd = f.handle();
            if (fd >= 0)
                ::fsync(fd);      // 只在收尾做一次；逐块 fsync 会把吞吐打到百分之一
        #else // Windows
            f.flush();
        #endif
    }

    ChunkResult makeFailed(quint32 tid, int shard, int errcode)
    {
        ChunkResult r;
        r.transferId = tid;
        r.shard      = shard;
        r.ok         = false;
        r.errcode    = errcode;
        return r;
    }

    // 删掉文件之后，删掉两级空的 sha 分片目录（<root>/<biz>/xx/yy）
    void removeEmptyShaDirs(const QString &filePath)
    {
        if (filePath.isEmpty())
            return;

        QDir dir = QFileInfo(filePath).absoluteDir();   // 拿到文件所在的目录, dir 指向（第一级，源文件直接所在的目录 /yy）
        for (int level = 0; level < 2; ++level) {       // 从最里层开始，最多往上删两级
            const QString dirPath = dir.absolutePath(); // ".../xx/yy"
            if (QFileInfo(dirPath).fileName().size() != 2) // "yy" 长度是 2 → 通过
                break;                                     // != 2 , 到 <biz> 了，收手
            if (!QDir().rmdir(dirPath))         // rmdir 只能删空目录，目录里还有文件/子目录就删不掉，返回 false
                break;                          // 非空 / 无权限 → 到此为止
            if (!dir.cdUp())                    // dir 上移到 ".../xx"
                break;
        }
    }
} // namespace

// ============================== UploadJob =====================================

bool StorageWorker::UploadJob::openPart(const QString &partPath, int *errcode)
{
    const QFileInfo info(partPath);
    if (!QDir().mkpath(info.absolutePath())) { // 转成绝对路径（不含文件名）
        qWarning() << "openPart mkpath 失败 " << info.absolutePath();
        *errcode = Err::StorageIoError;
        return false;
    }
    file.setFileName(partPath);
    if (!file.open(QIODevice::ReadWrite)) {
        qWarning() << "openPart 失败 " << partPath << file.errorString();
        *errcode = Err::StorageIoError;
        return false;
    }

    const qint64 existing = file.size();
    if (existing > 0 && !seedHash(errcode)) // 续传：数据库已落盘的文件数据 重新计算hash，否则最终 digest 只覆盖后半段
        return false;
    if (!file.seek(existing)) {
        *errcode = Err::StorageIoError;
        return false;
    }

    nextOffset = existing;
    queuedEnd  = existing;
    return true;
}

/* seedHsah 断点续传时，把已有 .part 前缀重新喂给 SHA256 计算器，使哈希状态恢复到断点位置，
           保证最终 digest 覆盖完整文件、可与客户端声明值比对。*/
bool StorageWorker::UploadJob::seedHash(int *errcode)
{
    if (!file.seek(0)) {
        *errcode = Err::StorageIoError;
        return false;
    }
    qint64 len = file.size();
    while (len > 0) {
        const QByteArray block = file.read(qMin<qint64>(len, kHashSeedBlock));
        if (block.isEmpty()) {
            qWarning() << "seedHash 读中断，剩余" << len;
            *errcode = Err::StorageIoError;
            return false;
        }
        sha.addData(block);
        len -= block.size();
    }
    return true;
}

bool StorageWorker::UploadJob::appendData(const QByteArray &data, int *errcode)
{
    if (!file.isOpen()) {
        *errcode = Err::StorageIoError;
        return false;
    }
    const qint64 n = file.write(data);
    if (n != data.size()) {
        *errcode = mapIoError();
        qWarning() << "appendData 写失败" << data.size() << n << file.errorString();
        return false;
    }
    file.flush(); // 每块一次 write 系统调用；fsync 留到 commit
    sha.addData(data);
    return true;
}

// 文件去重  .part改为文件名
bool StorageWorker::UploadJob::commitTo(const QString &finalPath, int *errcode)
{
    if (!file.isOpen()) {
        *errcode = Err::StorageIoError;
        return false;
    }
    file.flush();
    syncToDisk(file);
    file.close();

    const QFileInfo info(finalPath);
    if (!QDir().mkpath(info.absolutePath())) { // 去掉文件名字，建目录
        *errcode = Err::StorageIoError;
        return false;
    }

    // 文件去重：同 sha256 的最终对象已经在盘上， 只删掉自己的 .part ，磁盘只存一份
    if (QFile::exists(finalPath)) {
        QFile::remove(file.fileName());
        return true;
    }
    // 同一 sha 的上传都路由到同一分片线程（writeShard(sha)），这里不存在跨线程竞态。
    if (!QFile::rename(file.fileName(), finalPath)) { // .part 原子改名 -> sha256
        qWarning() << "commitTo rename 失败" << file.fileName() << "->" << finalPath;
        *errcode = Err::StorageIoError;
        return false;
    }
    return true;
}

// 回收: 幂等：对象已经不在了也算成功。这一条是"能不能收敛"的关键, 把"不存在"当失败，就会每次巡检都失败，永远删不掉。
void StorageWorker::deleteObject(quint8 biz, const QString &sha256)
{
    const QString path = finalPath(biz, sha256);
    if (path.isEmpty()) {
        // sha 非法，路径都算不出来：库里是坏数据，报成功, 让上层去清行
        emit logMessage(QStringLiteral("deleteObject 回收对象：sha256 不合法 biz=%1 sha=%2，只清记录").arg(biz).arg(sha256));
        emit objectDeleted(biz, sha256);
        return;
    }

    const bool existed = QFile::exists((path));
    if (existed && !QFile::remove(path)) {
        emit objectDeleteFailed(biz, sha256, Err::StorageIoError);
        return;
    }

    removeEmptyShaDirs(path);

    emit objectDeleted(biz, sha256);
}

// 磁盘对账
void StorageWorker::scanStorage(const QSet<QString> &liveObjects)
{
    if (m_stopping)
        return;

    // 有进行中的传输就跳过本轮。
    // commitTo 里"检查 finalPath 是否存在 → 删掉自己的 .part"之间有个窗口，
    // 此时把正式对象删掉，会让刚提交的 files 行指向一个不存在的文件。
    if (!m_uploadJobs.isEmpty() || !m_downloadJobs.isEmpty()) {
        emit logMessage(QStringLiteral("存储分片 %1：有进行中的传输，本轮磁盘对账跳过").arg(m_shardIndex));
        emit storageScanFinished(0, 0, 0);
        return;
    }

    // 上线初期先只报告不删除：看日志确认合理，kDeleteEnabled 再改成 true
    constexpr bool kDeleteEnabled = true;

    const qint64 now = nowMs();
    const qint64 kObjectWindowMs = 60LL * 60 * 1000;          // 正式对象：1 小时, 没到删除时间
    const qint64 kPartWindowMs   = 24LL * 60 * 60 * 1000;     // .part：24 小时

    int removedObjects = 0;
    int removedParts   = 0;
    int keptTooNew     = 0;

    // 只遍历已知的 biz 目录，不认得的一律不碰
    for (int b = Proto::BizAvatar; b <= Proto::BizPublicVideo; ++b) {
        const quint8 biz = quint8(b);
        const QDir bizDir(QDir(m_rootDir).filePath(bizDirName(biz)));
        if (!bizDir.exists())
            continue;

        const QFileInfoList level1 = bizDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &d1 : level1) {
            const QDir dir1(d1.absoluteFilePath());
            const QFileInfoList level2 = dir1.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QFileInfo &d2 : level2) {
                const QDir leaf(d2.absoluteFilePath());
                const QFileInfoList files = leaf.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);

                for (const QFileInfo &f : files) {
                    const QString name = f.fileName();
                    const qint64  mtime = f.lastModified().toMSecsSinceEpoch();
                    const QString path = f.absoluteFilePath();

                    // ---- .part 残留（断线 / 崩溃 / 服务端被 kill）----
                    if (name.endsWith(QStringLiteral(".part"))) {
                        if (now - mtime < kPartWindowMs) {
                            ++keptTooNew;          // 给断点续传留时间
                            continue;
                        }
                        if (!kDeleteEnabled) {
                            emit logMessage(QStringLiteral("[对账·试运行] 将删除残留分片 %1").arg(path));
                            ++removedParts;
                            continue;
                        }
                        if (QFile::remove(path)) {
                            removeEmptyShaDirs(path);
                            ++removedParts;
                        }
                        continue;
                    }

                    // ---- 正式对象 ----
                    // 文件名必须正好是 64 位小写 hex，否则不认、不删
                    if (!isValidSha256(name))
                        continue;

                    // 安全窗口：刚 rename 完、files 行还在 DB 线程排队的对象
                    if (now - mtime < kObjectWindowMs) {
                        ++keptTooNew;
                        continue;
                    }

                    if (liveObjects.contains(storageObjectKey(biz, name)))
                        continue;                  // 还有活引用

                    if (!kDeleteEnabled) {
                        emit logMessage(QStringLiteral("[对账·试运行] 将删除孤儿对象 biz=%1 %2")
                                            .arg(biz).arg(path));
                        ++removedObjects;
                        continue;
                    }
                    if (QFile::remove(path)) {
                        removeEmptyShaDirs(path);
                        ++removedObjects;
                    }
                }
            }
        }
    }

    emit storageScanFinished(removedObjects, removedParts, keptTooNew);
}


void StorageWorker::UploadJob::abortAndRemove()
{
    const QString path = file.fileName();
    if (file.isOpen())
        file.close();
    if (path.isEmpty())
        return;
    QFile::remove(path);
    removeEmptyShaDirs(path);
}

QString StorageWorker::UploadJob::digestHex() const
{
    return QString::fromLatin1(sha.result().toHex());
}

// =========================== DownloadJob =====================================


bool StorageWorker::DownloadJob::openFile(const QString &path, int *errcode)
{
    file.setFileName(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "openFile 失败" << path << file.errorString();
        *errcode = Err::FileNotFound;
        return false;
    }
    size = file.size();
    return true;
}

// 从 offset 读 maxLen 字节写入 out；读不满表示已到文件末尾。offset 越界返回 Err::RangeInvalid。
bool StorageWorker::DownloadJob::readAt(qint64 offset, int maxLen, QByteArray *out, int *errcode)
{
    if (!out || maxLen <= 0 || offset < 0 || offset > size) {
        *errcode = Err::RangeInvalid;
        return false;
    }
    if (!file.seek(offset)) {
        *errcode = Err::StorageIoError;
        return false;
    }
    const qint64 want = qMin<qint64>(maxLen, size - offset);
    const QByteArray data = file.read(want);
    if (data.size() != want) {
        qWarning() << "readAt 短读" << offset << want << data.size();
        *errcode = Err::StorageIoError;
        return false;
    }
    *out = data;
    return true;
}

// ============================ StorageWorker ==================================

StorageWorker::StorageWorker(int shardIndex, int shardCount, const QString &rootDir,QObject *parent)
    : QObject(parent)
    , m_shardIndex(shardIndex)
    , m_shardCount(qMax(1, shardCount))
    , m_rootDir(QDir(rootDir).absolutePath()) // 去掉文件名
{
}

StorageWorker::~StorageWorker()
{
    cleanup();
}

void StorageWorker::init()
{
    if (!m_sweep) {
        m_sweep = new QTimer(this);
        m_sweep->setInterval(int(kSweepIntervalMs)); // 1s
        connect(m_sweep, &QTimer::timeout, this, &StorageWorker::uploadsTimeout);
        connect(m_sweep, &QTimer::timeout, this, &StorageWorker::downloadsTimeout);
    }
    m_sweep->start();
    emit logMessage(QStringLiteral("存储分片 %1/%2 就绪，根目录 %3")
                    .arg(m_shardIndex).arg(m_shardCount).arg(m_rootDir));
}

void StorageWorker::cleanup()
{
    m_stopping = true;
    if (m_sweep)
        m_sweep->stop();

    // 只关句柄、保留 .part：正常关闭不是数据错误，客户端重连后可以续传
    for (UploadJob *job : m_uploadJobs) {
        if (job->file.isOpen())
            job->file.close();
        delete job;
    }
    m_uploadJobs.clear();
    m_writePendingQueue.clear();
    m_writePendingSet.clear();

    for (DownloadJob *job : m_downloadJobs) {
        if (job->file.isOpen())
            job->file.close();
        delete job;
    }
    m_downloadJobs.clear();
}

// -----------------  路径布局 -------------------------------

QString StorageWorker::bizDirName(quint8 biz)
{
    switch (biz) {
        case Proto::BizAvatar:      return QStringLiteral("avatar");
        case Proto::BizChatImage:   return QStringLiteral("chat_image");
        case Proto::BizChatFile:    return QStringLiteral("chat_file");
        case Proto::BizVoice:       return QStringLiteral("voice");
        case Proto::BizVideo:       return QStringLiteral("video");
        case Proto::BizPublicVideo: return QStringLiteral("public_video");
        default:                    return QStringLiteral("other");
    }
}

// 0-9 or a-f
bool StorageWorker::isValidSha256(const QString &sha)
{
    if (sha.size() != Proto::kSha256HexLen)
        return false;
    for (const QChar c : sha) {
        const ushort u = c.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f')))
            return false;
    }
    return true;
}

// finalPath: /var/lib/qtserver/files/biz/sha256.left(2)/sha256.mid(2, 2)
QString StorageWorker::dirFor(quint8 biz, const QString &sha256) const
{
    // sha256 已严格校验为 64 位小写十六进制，不含 '/' '.' '\'，不可能路径穿越
    if (!isValidSha256(sha256))
        return QString();
    return QDir(m_rootDir).filePath(
        bizDirName(biz) + QLatin1Char('/') + sha256.left(2) + QLatin1Char('/') + sha256.mid(2, 2));
}

// finalPath: /var/lib/qtserver/files/biz/sha256.left(2)/sha256.mid(2, 2)/uid-sha256.part
QString StorageWorker::partPath(quint8 biz, qint64 uid, const QString &sha256) const
{
    const QString dir = dirFor(biz, sha256);
    // (biz, uid, sha256) 决定唯一临时文件：同一内容的并发上传互不覆盖，同用户重连续传仍命中同一个 .part
    return dir.isEmpty() ? QString() : dir + QLatin1Char('/') + QString::number(uid) + QLatin1Char('-')
                                                 + sha256 + QStringLiteral(".part");
}

// finalPath: /var/lib/qtserver/files/biz/sha256.left(2)/sha256.mid(2, 2)/sha256
QString StorageWorker::finalPath(quint8 biz, const QString &sha256) const
{
    const QString d = dirFor(biz, sha256);
    return d.isEmpty() ? QString() : d + QLatin1Char('/') + sha256;
}

qint64 StorageWorker::availableBytes() const
{
    QStorageInfo si(m_rootDir);
    si.refresh();
    return si.isValid() ? si.bytesAvailable() : -1;
}

// ----------------- 上传 -------------------------------

// 打开/复用 <uid>-<sha>.part、续传 seedHash、磁盘剩余空间检查
void StorageWorker::probeUpload(const TransferCtx &ctx)
{
    const quint32 transferId = ctx.transferId;
    if (m_uploadJobs.contains(transferId)) {
        emit uploadProbed(makeFailed(transferId, m_shardIndex, Err::UploadBusy));
        return;
    }

    // .part 的真实路径：<root>/<biz>/<sha2>/<sha2>/<uid>-<sha256>.part
    const QString part = partPath(ctx.biz, ctx.uid, ctx.sha256);
    if (part.isEmpty()) {
        emit uploadProbed(makeFailed(transferId, m_shardIndex, Err::InvalidParam));
        return;
    }

    // 占位键只管"同一用户 + 同一内容"的并发互斥，与磁盘路径无关
    const QString partKey = QStringLiteral("%1-%2").arg(ctx.uid).arg(ctx.sha256);
    const quint32 tid  = m_activeParts.value(partKey, 0);
    if (tid != 0 && tid != transferId) {
        emit uploadProbed(makeFailed(transferId, m_shardIndex, Err::UploadBusy));
        return;
    }

    UploadJob *job = new UploadJob;
    job->ctx = ctx;
    job->lastActiveAt = nowMs();

    int errcode = Err::Ok;
    if (!job->openPart(part, &errcode)) {
        delete job;
        emit uploadProbed(makeFailed(transferId, m_shardIndex, errcode));
        return;
    }

    // 磁盘剩余空间检查, 去掉 .part 已有字节，只需补齐剩余部分
    const qint64 need  = qMax<qint64>(0, ctx.declaredSize - job->nextOffset);
    const qint64 avail = availableBytes();
    if (avail >= 0 && need > 0 && avail < need) {
        if (job->file.isOpen())
            job->file.close();
        delete job;
        const QString msg = QStringLiteral("磁盘空间不足 uid=%1 tid=%2 need=%3 avail=%4")
                                .arg(ctx.uid).arg(transferId).arg(need).arg(avail);
        qWarning().noquote() << msg;
        emit logMessage(msg);
        emit uploadProbed(makeFailed(transferId, m_shardIndex, Err::StorageFull));
        return;
    }

    m_activeParts.insert(partKey, transferId);
    m_uploadJobs.insert(transferId, job);

    ChunkResult r;
    r.transferId = transferId;
    r.shard      = m_shardIndex;
    r.ok         = true;
    r.received   = job->nextOffset;
    emit uploadProbed(r);
}

// offset 必须等于 queuedEnd（顺序严格）、不超声明大小、单 transfer 积压 ≤4MiB
void StorageWorker::writeChunk(quint32 transferId, quint64 offset, const QByteArray &data)
{
    UploadJob *job = m_uploadJobs.value(transferId, nullptr);
    if (!job) {
        emit uploadFailed(makeFailed(transferId, m_shardIndex, Err::TransferNotFound));
        return;
    }

    auto rejectChunk = [this, job](int errcode) {
        const quint32 tid = job->ctx.transferId;
        destroyUploadJobAndKeepPart(job);
        emit uploadFailed(makeFailed(tid, m_shardIndex, errcode));
    };

    // 数据面不做业务判断，但范围/顺序校验必须在这里（只有本线程持有句柄与游标）
    if (data.isEmpty()) {
        rejectChunk(Err::InvalidParam);
        return;
    }
    if (data.size() > Proto::kFileChunkSize) {
        rejectChunk(Err::PayloadTooLarge);
        return;
    }
    // offset == queuedEnd 校验在入队之前，写错 offset 的块，进不了 pending 队列，.part 只会顺序增长，永远不会被写坏
    if (qint64(offset) != job->queuedEnd) {
        emit logMessage(QStringLiteral("writeChunk 上传块 offset 失配 tid=%1 uid=%2 期望=%3 实际=%4 已落盘=%5")
                    .arg(transferId).arg(job->ctx.uid).arg(job->queuedEnd).arg(qint64(offset)).arg(job->nextOffset));
        rejectChunk(Err::UploadOffsetMismatch);
        return;
    }
    if (qint64(offset) + data.size() > job->ctx.declaredSize) {
        rejectChunk(Err::FileTooLarge);
        return;
    }
    // 单 transfer 待写积压超上限：磁盘写入跟不上网络，或客户端不守节流。中断本次传输（.part 保留），客户端重新 begin 会拿到断点 offset 继续传
    if ((job->queuedEnd - job->nextOffset) + data.size() > kMaxQueuedBytes) {
        rejectChunk(Err::TransferCongested);
        return;
    }

    job->pending.enqueue(data);
    job->queuedEnd    += data.size();
    job->lastActiveAt  = nowMs();
    enqueueWrite(transferId);
}

void StorageWorker::enqueueWrite(quint32 transferId)
{
    if (m_writePendingSet.contains(transferId))
        return;
    m_writePendingSet.insert(transferId);
    m_writePendingQueue.enqueue(transferId);
    // processStep() 投递到存储线程的事件队列末尾，等当前事件处理完再执行
    QMetaObject::invokeMethod(this, [this]() { processStep(); }, Qt::QueuedConnection);
}

// 每轮连续写多块，和网络层"每轮最多消化 4 帧"对齐：
// 减少网络线程/存储线程之间的事件循环往返，同时保持单块轮转的公平性。
void StorageWorker::processStep()
{
    if (m_stopping)
        return;

    constexpr int kWriteBatchBlocks = 4;
    for (int i = 0; i < kWriteBatchBlocks && !m_writePendingQueue.isEmpty(); ++i) {
        const quint32 transferId = m_writePendingQueue.dequeue();
        m_writePendingSet.remove(transferId);

        UploadJob *job = m_uploadJobs.value(transferId, nullptr);
        if (!job)
            continue;   // 已取消/失败的残留项，直接丢弃

        if (job->pending.isEmpty()) {
            if (job->uploadFinish)
                finalizeUpload(job);
            continue;
        }

        // 写一块
        const QByteArray data = job->pending.dequeue();
        int errcode = Err::Ok;
        if (!job->appendData(data, &errcode)) {
            destroyUploadJobAndDeletePart(job);   // 写盘失败：删 .part
            emit uploadFailed(makeFailed(transferId, m_shardIndex, errcode));
            if (!m_writePendingQueue.isEmpty())
                QMetaObject::invokeMethod(this, [this]() { processStep(); },
                                          Qt::QueuedConnection);
            return;
        }
        job->nextOffset  += data.size();
        job->lastActiveAt = nowMs();
        emit chunkWritten(transferId, data.size());   // 网络层解除接收反压

        if (job->pending.isEmpty() && job->uploadFinish) {
            finalizeUpload(job);
            continue;
        }

        if (!job->pending.isEmpty()) {
            // 本轮已写一块，剩余块排到队尾；这里只排队不重复投递 processStep
            if (!m_writePendingSet.contains(transferId)) {
                m_writePendingSet.insert(transferId);
                m_writePendingQueue.enqueue(transferId);
            }
        }
    }

    // 还有待写块：只投递一轮，下一轮再连续写 4 块
    if (!m_writePendingQueue.isEmpty())
        QMetaObject::invokeMethod(this, [this]() { processStep(); }, Qt::QueuedConnection);
}

void StorageWorker::finishUpload(quint32 transferId)
{
    UploadJob *job = m_uploadJobs.value(transferId, nullptr);
    if (!job) {
        emit uploadFailed(makeFailed(transferId, m_shardIndex, Err::TransferNotFound));
        return;
    }
    job->uploadFinish = true;
    job->lastActiveAt  = nowMs();

    // 还有块在路上：等 processStep 把队列清空后再收尾（客户端 发完网卡就发 end」，服务端必然可能仍有积压）
    if (job->pending.isEmpty() && job->nextOffset == job->queuedEnd)
        finalizeUpload(job);
}

// 客户端主动取消：临时文件不要了
void StorageWorker::cancelUploadAndDeletePart(quint32 transferId)
{
    UploadJob *job = m_uploadJobs.value(transferId, nullptr);
    if (!job)
        return;
    destroyUploadJobAndDeletePart(job);
}

// 断线/关停：临时文件留着
void StorageWorker::cancelUploadAndKeepPart(quint32 transferId)
{
    UploadJob *job = m_uploadJobs.value(transferId, nullptr);
    if (!job)
        return;
    destroyUploadJobAndKeepPart(job);
}

void StorageWorker::finalizeUpload(UploadJob *job)
{
    if (!job)
        return;
    const quint32 tid = job->ctx.transferId;
    m_activeParts.remove(QStringLiteral("%1-%2").arg(job->ctx.uid).arg(job->ctx.sha256));

    const QString sha256 = job->digestHex();
    if (sha256 != job->ctx.sha256) {
        qWarning() << "上传校验失败 tid=" << tid << " expect=" << job->ctx.sha256 << " actual=" << sha256;
        job->abortAndRemove();  // 内容错了 删 .part
        m_uploadJobs.remove(tid);
        delete job;
        emit uploadFinished(makeFailed(tid, m_shardIndex, Err::FileCorrupted), QString());
        return;
    }

    const QString path = finalPath(job->ctx.biz, job->ctx.sha256);
    int errcode = Err::Ok;
    const bool committed = !path.isEmpty() && job->commitTo(path, &errcode);

    m_uploadJobs.remove(tid);
    delete job;

    if (!committed) {
        emit uploadFinished(makeFailed(tid, m_shardIndex, errcode), QString());
        return;
    }

    ChunkResult r;
    r.transferId = tid;
    r.shard      = m_shardIndex;
    r.ok         = true;
    r.received   = -1; // 上传完成，字节数以 sha256 为准
    emit uploadFinished(r, sha256);
}


void StorageWorker::destroyUploadJob(UploadJob *job, bool removePart)
{
    if (!job)
        return;
    const quint32 tid = job->ctx.transferId;
    m_activeParts.remove(QStringLiteral("%1-%2").arg(job->ctx.uid).arg(job->ctx.sha256));

    if (removePart)
        job->abortAndRemove();
    else if (job->file.isOpen())
        job->file.close();

    m_uploadJobs.remove(tid);
    m_writePendingSet.remove(tid);
    // m_writePendingQueue 里可能还留着 tid：transferId 单调递增不复用，
    // 残留项由 processStep() 的 !job 分支安全丢弃。
    delete job;
}

// 数据确实坏了 或 用户明确放弃: 删 .part
void StorageWorker::destroyUploadJobAndDeletePart(UploadJob *job)
{
    if (!job)
        return;

    m_activeParts.remove(QStringLiteral("%1-%2").arg(job->ctx.uid).arg(job->ctx.sha256));

    const quint32 transferId = job->ctx.transferId;
    job->abortAndRemove();   // close + 删 .part
    m_uploadJobs.remove(transferId);
    m_writePendingSet.remove(transferId);
    delete job;
}

// 数据是好的：只关句柄，.part 留着给客户端续传
void StorageWorker::destroyUploadJobAndKeepPart(UploadJob *job)
{
    if (!job)
        return;

    m_activeParts.remove(QStringLiteral("%1-%2").arg(job->ctx.uid).arg(job->ctx.sha256));

    const quint32 transferId = job->ctx.transferId;
    if (job->file.isOpen())
        job->file.close();
    m_uploadJobs.remove(transferId);
    m_writePendingSet.remove(transferId);
    delete job;
}


// -------------------- 下载 ------------------------

void StorageWorker::openDownload(const TransferCtx &ctx)
{
    const quint32 tid = ctx.transferId;
    if (m_downloadJobs.contains(tid)) {
        emit downloadOpened(makeFailed(tid, m_shardIndex, Err::DownloadBusy), 0);
        return;
    }

    const QString path = finalPath(ctx.biz, ctx.sha256);
    if (path.isEmpty()) {
        emit downloadOpened(makeFailed(tid, m_shardIndex, Err::FileCorruption), 0);
        return;
    }

    DownloadJob *job = new DownloadJob;
    job->ctx = ctx;

    int errcode = Err::Ok;
    if (!job->openFile(path, &errcode)) {
        delete job;
        emit downloadOpened(makeFailed(tid, m_shardIndex, errcode), 0);
        return;
    }
    job->lastActiveAt = nowMs();
    m_downloadJobs.insert(tid, job);

    ChunkResult r;
    r.transferId = tid;
    r.shard      = m_shardIndex;
    r.ok         = true;
    emit downloadOpened(r, job->size);
}

void StorageWorker::readChunk(quint32 transferId, qint64 offset, int maxLen)
{
    DownloadJob *job = m_downloadJobs.value(transferId, nullptr);
    if (!job) {
        emit downloadFailed(makeFailed(transferId, m_shardIndex, Err::TransferNotFound));
        return;
    }
    job->lastActiveAt = nowMs();

    QByteArray data;
    int errcode = Err::Ok;
    if (!job->readAt(offset, maxLen, &data, &errcode)) {
        closeDownloadJob(job);
        emit downloadFailed(makeFailed(transferId, m_shardIndex, errcode));
        return;
    }

    FileChunk c;
    c.transferId = transferId;
    c.offset     = quint64(offset);
    c.data       = data;
    emit chunkRead(c, offset + data.size() >= job->size);
}

void StorageWorker::closeDownload(quint32 transferId)
{
    DownloadJob *job = m_downloadJobs.value(transferId, nullptr);
    if (!job)
        return;
    closeDownloadJob(job);
}

void StorageWorker::closeDownloadJob(DownloadJob *job)
{
    if (!job)
        return;
    m_downloadJobs.remove(job->ctx.transferId);
    if (job->file.isOpen())
        job->file.close();
    delete job;
}

void StorageWorker::uploadsTimeout()
{
    if (m_stopping)
        return;
    const qint64 now = nowMs();

    const QList<quint32> transferIds = m_uploadJobs.keys();
    for (quint32 transferId : transferIds) {
        UploadJob *job = m_uploadJobs.value(transferId, nullptr);
        if (!job)
            continue;
        if (now - job->lastActiveAt <= Proto::kUploadIdleTimeoutMs)
            continue;

        emit logMessage(QStringLiteral("上传空闲超时 uid=%1 tid=%2 已收%3").arg(job->ctx.uid).
                                                    arg(transferId).arg(job->nextOffset));
        destroyUploadJobAndKeepPart(job);      // 超时不是数据错误，留着续传
        emit uploadFailed(makeFailed(transferId, m_shardIndex, Err::UploadExpired));
    }
}

void StorageWorker::downloadsTimeout()
{
    if (m_stopping)
        return;
    const qint64 now = nowMs();

    const QList<quint32> transferIds = m_downloadJobs.keys();
    for (quint32 transferId : transferIds) {
        DownloadJob *job = m_downloadJobs.value(transferId, nullptr);
        if (!job)
            continue;
        // 弱网时 出口队列排空可能要好几分钟
        if (now - job->lastActiveAt <= Proto::kDownloadIdleTimeoutMs)
            continue;

        emit logMessage(QStringLiteral("下载空闲超时 uid=%1 tid=%2").arg(job->ctx.uid).arg(transferId));
        closeDownloadJob(job);
        emit downloadFailed(makeFailed(transferId, m_shardIndex, Err::TransferAborted));
    }
}

