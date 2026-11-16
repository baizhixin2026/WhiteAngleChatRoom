#include "fileioworker.h"
#include <QCryptographicHash>
#include <QCoreApplication>

#include <QDir>
#include <QFileInfo>

namespace {
        constexpr qint64 kHashSliceBytes = 4 * 1024 * 1024;   // 每次读 4 MiB 就让出一次事件循环
}

FileIoWorker::FileIoWorker(QObject *parent)
    : QObject(parent)
{
}

FileIoWorker::~FileIoWorker()
{
    const QList<quint32> up = m_uploadSources.keys();
    for (quint32 taskId : up)
        closeUploadSource(taskId);

    const QList<quint32> down = m_downloadSinks.keys();
    for (quint32 taskId : down)
        destroyDownloadSink(taskId, false);   // 析构不提交，残留 .part 交给上层判定

    const QList<quint32> hashing = m_hashJobs.keys();
    for (quint32 taskId : hashing)
        destroyHashJob(taskId);
}

// ============================ 上传 ============================

void FileIoWorker::openUploadSource(quint32 taskId, const QString &path, qint64 startOffset)
{
    closeUploadSource(taskId);

    auto *src = new UploadSource;
    src->file.setFileName(path);
    if (!src->file.open(QIODevice::ReadOnly)) {
        emit uploadSourceOpenFailed(taskId, src->file.errorString());
        delete src;
        return;
    }

    src->size = src->file.size();
    if (src->size <= 0) {
        src->file.close();
        emit uploadSourceOpenFailed(taskId, QStringLiteral("源文件为空"));
        delete src;
        return;
    }
    if (startOffset < 0 || startOffset > src->size) {
        src->file.close();
        emit uploadSourceOpenFailed(taskId, QStringLiteral("续传偏移越界"));
        delete src;
        return;
    }
    if (!src->file.seek(startOffset)) {
        src->file.close();
        emit uploadSourceOpenFailed(taskId, src->file.errorString());
        delete src;
        return;
    }

    src->readOffset = startOffset;
    m_uploadSources.insert(taskId, src);
    emit uploadSourceOpened(taskId, src->size);
}

void FileIoWorker::readUploadBlock(quint32 taskId)
{
    UploadSource *src = m_uploadSources.value(taskId, nullptr);
    if (!src) {
        emit uploadBlockReadFailed(taskId, QStringLiteral("源文件未打开"));
        return;
    }
    if (src->readOffset >= src->size) {
        emit uploadBlockReadFailed(taskId, QStringLiteral("已到文件末尾"));
        return;
    }

    const qint64 want = qMin<qint64>(Proto::kFileChunkSize, src->size - src->readOffset);
    if (!src->file.seek(src->readOffset)) {
        emit uploadBlockReadFailed(taskId, src->file.errorString());
        return;
    }
    const QByteArray data = src->file.read(want);
    if (data.size() != want) {
        emit uploadBlockReadFailed(taskId, src->file.errorString());
        return;
    }

    const quint64 offset = quint64(src->readOffset);
    src->readOffset += want;
    emit uploadBlockRead(taskId, offset, data);
}

void FileIoWorker::closeUploadSource(quint32 taskId)
{
    UploadSource *src = m_uploadSources.take(taskId);
    if (!src)
        return;
    if (src->file.isOpen())
        src->file.close();
    delete src;
}

// ============================ 下载 ============================

void FileIoWorker::openDownloadSink(quint32 taskId, const QString &finalPath, qint64 startOffset)
{
    destroyDownloadSink(taskId, false);

    if (finalPath.isEmpty()) {
        emit downloadSinkOpenFailed(taskId, QStringLiteral("落盘路径为空"));
        return;
    }

    auto *sink = new DownloadSink;
    sink->finalPath = finalPath;

    // 临时文件名带进程号：多开客户端时它们共享同一个缓存目录，
    // 同一个 fileId 的 .part 路径会撞车，互相 rename 抢文件导致"无法重命名到目标路径"。
    sink->partPath = finalPath + QStringLiteral(".part.%1").arg(QCoreApplication::applicationPid());
    const QFileInfo info(sink->partPath);
    if (!QDir().mkpath(info.absolutePath())) {
        emit downloadSinkOpenFailed(taskId, QStringLiteral("无法创建落盘目录"));
        delete sink;
        return;
    }

    sink->file.setFileName(sink->partPath);
    // 从头下：截断重建；断点续传：保留已有前缀，从 startOffset 接着写
    QIODevice::OpenMode mode = QIODevice::ReadWrite;
    if (startOffset <= 0)
        mode |= QIODevice::Truncate;
    if (!sink->file.open(mode)) {
        emit downloadSinkOpenFailed(taskId, sink->file.errorString());
        delete sink;
        return;
    }
    if (startOffset > 0) {
        if (sink->file.size() < startOffset) {
            sink->file.close();
            emit downloadSinkOpenFailed(taskId, QStringLiteral("本地断点前缀不足"));
            delete sink;
            return;
        }
        if (!sink->file.seek(startOffset)) {
            sink->file.close();
            emit downloadSinkOpenFailed(taskId, sink->file.errorString());
            delete sink;
            return;
        }
    }

    sink->writeOffset = startOffset;
    m_downloadSinks.insert(taskId, sink);
    emit downloadSinkOpened(taskId);
}

void FileIoWorker::writeDownloadBlock(quint32 taskId, quint64 offset, const QByteArray &data)
{
    DownloadSink *sink = m_downloadSinks.value(taskId, nullptr);
    if (!sink) {
        emit downloadBlockWriteFailed(taskId, QStringLiteral("落盘句柄未打开"));
        return;
    }
    if (qint64(offset) != sink->writeOffset) {
        emit downloadBlockWriteFailed(taskId, QStringLiteral("块偏移不连续"));
        return;
    }
    if (!sink->file.seek(sink->writeOffset)) {
        emit downloadBlockWriteFailed(taskId, sink->file.errorString());
        return;
    }
    const qint64 len = sink->file.write(data);
    if (len != data.size()) {
        emit downloadBlockWriteFailed(taskId, sink->file.errorString());
        return;
    }
    sink->writeOffset += len;
    emit downloadBlockWritten(taskId, sink->writeOffset);
}

void FileIoWorker::closeDownloadSink(quint32 taskId, bool commit)
{
    destroyDownloadSink(taskId, commit);
}

void FileIoWorker::destroyDownloadSink(quint32 taskId, bool commit)
{
    DownloadSink *sink = m_downloadSinks.take(taskId);
    if (!sink)
        return;

    bool ok = false;
    QString reason;

    if (sink->file.isOpen()) {
        sink->file.flush();
        sink->file.close();
    }

    if (commit) {
        // QFile::rename 不会覆盖已存在的目标，先删
        QFile::remove(sink->finalPath);
        if (QFile::rename(sink->partPath, sink->finalPath)) {
            ok = true;
        } else {
            reason = QStringLiteral("无法重命名到目标路径");
        }
    } else {
        reason = QStringLiteral("传输未完成，已丢弃临时文件");
    }

    if (!ok)
        QFile::remove(sink->partPath);

    delete sink;
    emit downloadSinkClosed(taskId, ok, reason);
}

void FileIoWorker::closeTask(quint32 taskId)
{
    closeUploadSource(taskId);
    destroyDownloadSink(taskId, false);
    destroyHashJob(taskId);
}


// ----------------- 哈希 -------------------------------

void FileIoWorker::hashFile(quint32 taskId, const QString &path)
{
    destroyHashJob(taskId);

    auto *job = new HashJob;
    job->file.setFileName(path);
    if (!job->file.open(QIODevice::ReadOnly)) {
        emit fileHashFailed(taskId, job->file.errorString());
        delete job;
        return;
    }

    job->total = job->file.size();
    if (job->total <= 0) {
        job->file.close();
        emit fileHashFailed(taskId, QStringLiteral("文件为空"));
        delete job;
        return;
    }

    m_hashJobs.insert(taskId, job);
    stepHash(taskId);   // 第一片立刻做，不等事件循环
}

void FileIoWorker::destroyHashJob(quint32 taskId)
{
    HashJob *job = m_hashJobs.take(taskId);
    if (!job)
        return;
    if (job->file.isOpen())
        job->file.close();
    delete job;
}

// 一次只处理一片，然后让出：2 GiB 的 SHA256 要跑 5~15 秒，同步跑会把同线程的读写全堵死。
void FileIoWorker::stepHash(quint32 taskId)
{
    HashJob *job = m_hashJobs.value(taskId, nullptr);
    if (!job)
        return;

    const qint64 len = qMin(kHashSliceBytes, job->total - job->hashed);
    const QByteArray block = job->file.read(len);
    if (block.isEmpty()) {
        destroyHashJob(taskId);
        emit fileHashFailed(taskId, QStringLiteral("读取中断"));
        return;
    }

    job->sha.addData(block);
    job->hashed += block.size();
    emit fileHashProgress(taskId, job->hashed, job->total);

    if (job->hashed < job->total) {
        QMetaObject::invokeMethod(this, [this, taskId]() { stepHash(taskId); }, Qt::QueuedConnection);
        return;
    }

    const QString sha256 = QString::fromLatin1(job->sha.result().toHex());
    const qint64  total  = job->total;
    destroyHashJob(taskId);
    emit fileHashed(taskId, sha256, total);
}


