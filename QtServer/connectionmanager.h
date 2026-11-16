#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include "userinfo.h"
#include "mysqluserstore.h"
#include "fileservice.h"
#include <QObject>
#include <QThread>
#include <QSet>
#include <QVariantMap>
#include <QVector>

class NetworkWorker;    // 前置声明
class DBWorker;
class FileService;
class StorageWorker;

class ConnectionManager : public QObject
{
    Q_OBJECT

private:
    explicit ConnectionManager(QObject *parent = nullptr);
    ~ConnectionManager();
    ConnectionManager(const ConnectionManager &) = delete;
    ConnectionManager &operator = (const ConnectionManager &) = delete;

public:
    static ConnectionManager &instance();

    static void shutdown();
    void startServerListen(unsigned short port);
    void stopServerListen();
    void requestUsers();

signals:
    // NetworkWorker UI 信号
    void serverListenOk(QString msg);
    void serverListenFail(QString msg);
    void serverListenStopped(bool);
    void logMessage(QString msg);

    void reqStartServerListen(unsigned short);
    void reqStopServerListen();

    // 服务器的数据库
    void usersReady(const QVector<UserInfo> &users);
    void reqQueryUsers();
    void dbReady(bool ok);

private:
    void teardownThreads();     // 幂等；停线程 + 回收 worker

private:
    // NetworkWorker
    QThread *m_netWorkThread = nullptr;
    NetworkWorker *m_netWorker = nullptr;

    // DBWorker
    QThread *m_dbThread = nullptr;
    DBWorker *m_dbWorker = nullptr;

    QSet<QString> m_onlineUsers;
    bool m_shutdownDone = false; // 幂等保护, 防止析构重入、shutdown() 多次调用

    // 文件服务（主线程）
    FileService *m_fileService = nullptr;

    // 存储分片线程
    QVector<QThread *> m_storageThreads;
    QVector<StorageWorker *> m_storageWorkers;
};

#endif // CONNECTIONMANAGER_H
