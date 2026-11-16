#include "avatarcache.h"
#include "connectionmanager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QStandardPaths>

namespace {
    const char *kAvatarDirName = "avatars";
}

AvatarCache &AvatarCache::instance()
{
    // 故意不析构：和 ConnectionManager 一样，避免退出时和 Qt 的销毁顺序打架
    static AvatarCache *cache = new AvatarCache;
    return *cache;
}

AvatarCache::AvatarCache(QObject *parent)
    : QObject(parent)
{
    auto &conn = ConnectionManager::instance();

    connect(&conn, &ConnectionManager::loginSuccess, this, [this]() {
        m_failed.clear();   // 重新登录，给失败过的图一次新机会
        auto &c = ConnectionManager::instance();
        setAvatarFileId(c.currentUid(), c.currentAvatarFileId());
    });

    // 用户更新头像
    connect(&conn, &ConnectionManager::avatarUpdateResult, this, [this]
                 (bool ok, const QString &text, const QString &fileId) {
        Q_UNUSED(text);
        if (!ok || fileId.isEmpty())
            return;
        auto &c = ConnectionManager::instance();
        setAvatarFileId(c.currentUid(), fileId);
    });

    connect(&conn, &ConnectionManager::downloadFinished, this, &AvatarCache::onDownloadFinished);
    connect(&conn, &ConnectionManager::downloadFailed,   this, &AvatarCache::onDownloadFailed);
}

// 头像缓存位置 = 操作系统约定的缓存根目录 + 程序自己拼的子目录
// .cache 里的内容: 默认会长期保留，系统不会自动清理 ~/.cache （程序目前没有实现 应用自己管理删缓存：遍历 avatars/*.img，删掉不在 m_uidToFileId 里的文件）
QString AvatarCache::cachePathFor(const QString &fileId)
{
    // 计算当前应用专属的"缓存根目录"的绝对路径, 作为后续拼头像文件路径的起点
    // 如果环境变量 $XDG_CACHE_HOME 有值 → $XDG_CACHE_HOME/<APPNAME> , 否则 → $HOME/.cache/<APPNAME>
    // 程序里没有调用 setApplicationName() / setOrganizationName()，所以 <APPNAME> 就是编译出来的可执行文件名。
    // $HOME/.cache/APPNAME == /home/username/.cache/APPNAME
    QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath(); // 兜底：系统临时目录
    dir += QLatin1Char('/') + QLatin1String(kAvatarDirName); // $HOME/.cache/APPNAME/avatars
    QDir().mkpath(dir); // 创建目录
    return dir + QLatin1Char('/') + fileId + QStringLiteral(".img"); // $HOME/.cache/APPNAME/avatars/fileId.img
}

QPixmap AvatarCache::pixmapForFileId(const QString &fileId) const
{
    if (fileId.isEmpty())
        return QPixmap();
    return m_memory.value(fileId);
}

QPixmap AvatarCache::pixmapForUid(qint64 uid) const
{
    return pixmapForFileId(m_uidToFileId.value(uid));
}

void AvatarCache::setAvatarFileId(qint64 uid, const QString &fileId)
{
    if (uid <= 0)
        return;

    if (fileId.isEmpty()) {     // 这个用户没有头像
        m_uidToFileId.remove(uid);
        return;
    }

    m_uidToFileId.insert(uid, fileId);

//    if (m_memory.contains(fileId))  // 内存命中，什么都不用做
//        return;
//    if (m_inFlight.contains(fileId) || m_failed.contains(fileId))
//        return;             // 正在下 / 失败过，不重复发起

    // 让出一轮事件循环再读盘解码：登录时会一次灌进来 N 个好友，同步解码会连成一片卡顿，排队后每轮只解一张，界面不会被冻住。
    QTimer::singleShot(0, this, [this, fileId]() {
        if (m_memory.contains(fileId) || m_inFlight.contains(fileId) || m_failed.contains(fileId))
            return; // 磁盘缓存命中 什么都不用做， 正在下/失败过 不重复发起
        if (tryLoadFromDisk(fileId)) // 磁盘缓存命中，省一次往返
            return;

        requestDownload(fileId); // 磁盘缓存未命中
    });
}

void AvatarCache::requestDownload(const QString &fileId)
{
    // avatarFileId 用户设置头像后，从服务端生成avatarFileId，登录后 服务端再次推送 avatarFileId
    const quint32 taskId = ConnectionManager::instance().downloadFile(fileId, cachePathFor(fileId),
                                                    TransferPriority::Background);
    if (taskId == 0)
        return;     // 没发起成功（比如未登录），不记失败，下次状态变化再试

    m_inFlight.insert(fileId);
    m_fileIdByTask.insert(taskId, fileId);
}

// 从缓存读取头像
bool AvatarCache::tryLoadFromDisk(const QString &fileId)
{
    const QString path = cachePathFor(fileId); // 拼文件路径
    if (!QFileInfo::exists(path))
        return false;

    QImageReader reader(path);
    reader.setAutoTransform(true); // 让读取器自动处理图像的"方向/变换"元数据
    const QImage img = reader.read();
    if (img.isNull()) {
        QFile::remove(path);          // 缓存文件坏了，删掉重下
        return false;
    }

    m_memory.insert(fileId, QPixmap::fromImage(img));
    usersAvatarReady(fileId);
    return true;
}

void AvatarCache::onDownloadFinished(quint32 taskId, const QString &localPath, qint64 size)
{
    Q_UNUSED(size);
    const QString fileId = m_fileIdByTask.take(taskId);
    if (fileId.isEmpty())
        return;            // 不是头像缓存发起的下载，忽略

    m_inFlight.remove(fileId);

    QImageReader reader(localPath);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) {
        QFile::remove(localPath);
        m_failed.insert(fileId);
        qWarning() << "[头像缓存] 图片解码失败 fileId=" << fileId << localPath;
        return;
    }

    m_memory.insert(fileId, QPixmap::fromImage(img));
    usersAvatarReady(fileId);
}

void AvatarCache::onDownloadFailed(quint32 taskId, int errcode, const QString &text)
{
    const QString fileId = m_fileIdByTask.take(taskId);
    if (fileId.isEmpty())
        return;

    m_inFlight.remove(fileId);

    // 连接断了 / 版本不符 / 被取消：这不是"这张图坏了"， 不该进永久黑名单，下次登录重新试（loginSuccess 也会 clear 一次）
    if (Err::isConnectionFatal(errcode) || errcode == Err::TransferAborted)
        return;

    m_failed.insert(fileId);          // 本次运行不再重试，避免每次状态变化都发请求
    qWarning() << "[头像缓存] 下载失败 fileId=" << fileId << " errcode=" << errcode << text;
}

void AvatarCache::usersAvatarReady(const QString &fileId)
{
    for (auto it = m_uidToFileId.constBegin(); it != m_uidToFileId.constEnd(); ++it) {
        if (it.value() == fileId)
            emit avatarLoaded(it.key(), fileId);
    }
}
