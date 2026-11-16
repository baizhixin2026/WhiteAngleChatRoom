#ifndef AVATARCACHE_H
#define AVATARCACHE_H

#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>

/*
 * AvatarCache 头像缓存（fileId → QPixmap）
 *
 * 只做两件事：
 *   1. 缓存：内存 + 磁盘（<CacheLocation>/avatars/<fileId>.img）
 *   2. 去重：同一个 fileId 只下载一次
 *
 * 四条约定（破坏任何一条都会出问题）：
 *   1. pixmapForXxx() 是纯查询，绝不发起下载。它会出现在绘制路径里，在里面触发 IO 等于每帧发一个下载请求。
 *   2. 下载只由 setAvatarFileId() 触发, 那是唯一的状态变化入口。
 *   3. 缓存键是 fileId 而不是 uid：换头像 = 新 fileId = 天然新缓存项，老图自动失宠，不需要任何失效逻辑。
 *   4. 下载失败静默回落（返回 null，调用方自己给默认图），本次运行不再重试。
 */
class AvatarCache : public QObject
{
    Q_OBJECT
public:
    static AvatarCache &instance();

    // 纯查询：未命中返回 null QPixmap，由调用方决定用哪张默认图
    QPixmap pixmapForFileId(const QString &fileId) const;
    QPixmap pixmapForUid(qint64 uid) const;

    // 唯一写入入口：记录 uid→fileId，必要时发起一次下载
    void setAvatarFileId(qint64 uid, const QString &fileId);

signals:
    // 某个 uid 的图真的可用了（UI 用它刷新那一行）
    void avatarLoaded(qint64 uid, const QString &fileId);

private slots:
    void onDownloadFinished(quint32 taskId, const QString &localPath, qint64 size);
    void onDownloadFailed(quint32 taskId, int errcode, const QString &text);

private:
    explicit AvatarCache(QObject *parent = nullptr);

    static QString cachePathFor(const QString &fileId);
    void requestDownload(const QString &fileId);
    bool tryLoadFromDisk(const QString &fileId);
    void usersAvatarReady(const QString &fileId);

    QHash<QString, QPixmap> m_memory;         // fileId -> 图
    QHash<qint64, QString>  m_uidToFileId;    // uid -> fileId
    QSet<QString>           m_inFlight;       // 正在下载的 fileId
    QSet<QString>           m_failed;         // 下载失败过的 fileId（本次运行不再重试）
    QHash<quint32, QString> m_fileIdByTask;   // taskId -> fileId，回调时认领
};

#endif // AVATARCACHE_H
