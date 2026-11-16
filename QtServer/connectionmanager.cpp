#include "connectionmanager.h"
#include "networkworker.h"
#include "dbworker.h"
#include "protocoltypes.h"
#include "fileservice.h"
#include "storageworker.h"
#include <QSemaphore>
#include <QDebug>

namespace {
    constexpr int kThreadWaitMs = 3000; // 单线程退出等待上限
    constexpr int kDbThreadWaitMs  = 10000;
    const QString kstoragePath = qEnvironmentVariable("CHAT_STORAGE", QStringLiteral("/home/ubuntu/chatFiles")); // 数据库保存用户文件目录
    static constexpr int kStorageShards = 2;
}

ConnectionManager::ConnectionManager(QObject *parent)
    : QObject{parent}
{
    // 跨线程信号槽（队列连接）传参时：自定义类型必须先 qRegisterMetaType 注册；Qt 内置类型不用（Qt 已注册）
    // 验证   ./QtServer_test 2>&1 | grep -i "cannot queue\|qRegisterMetaType"
    qRegisterMetaType<QSet<QString>>("QSet<QString>");  // NetworkWorker -> ConnectionManager
    qRegisterMetaType<UserInfo>("UserInfo");
    qRegisterMetaType<QVector<UserInfo>>("QVector<UserInfo>");
    qRegisterMetaType<ChatMessage>("ChatMessage");
    qRegisterMetaType<QVector<ChatMessage>>("QVector<ChatMessage>");
    qRegisterMetaType<FriendRequestInfo>("FriendRequestInfo");
    qRegisterMetaType<QVector<FriendRequestInfo>>("QVector<FriendRequestInfo>");
    qRegisterMetaType<SearchResult>("SearchResult");
    qRegisterMetaType<QVector<SearchResult>>("QVector<SearchResult>");
    qRegisterMetaType<MysqlUserStore::HandleFriendResult>("MysqlUserStore::HandleFriendResult");
    qRegisterMetaType<MysqlUserStore::RemoveFriendResult>("MysqlUserStore::RemoveFriendResult");
    qRegisterMetaType<HistoryPage>("HistoryPage");
    qRegisterMetaType<RecentChatResult>("RecentChatResult");
    // 存储线程
    qRegisterMetaType<FileMeta>("FileMeta");
    qRegisterMetaType<TransferCtx>("TransferCtx");
    qRegisterMetaType<ChunkResult>("ChunkResult");
    qRegisterMetaType<FileChunk>("FileChunk");
    qRegisterMetaType<FileMessage>("FileMessage");
    // 存储对象回收：DB 驱动，定时巡检
    qRegisterMetaType<ReclaimableObject>("ReclaimableObject");
    qRegisterMetaType<QVector<ReclaimableObject>>("QVector<ReclaimableObject>");

    // NetworkWorker 子线程
    m_netWorkThread = new QThread(this);
    m_netWorker = new NetworkWorker;
    m_netWorker->moveToThread(m_netWorkThread);
    connect(m_netWorkThread, &QThread::finished, m_netWorker, &QObject::deleteLater);

    connect(this, &ConnectionManager::reqStartServerListen, m_netWorker, &NetworkWorker::startServerListen);
    connect(this, &ConnectionManager::reqStopServerListen, m_netWorker, &NetworkWorker::stopServerListen);
    connect(m_netWorker, &NetworkWorker::serverListenOk, this, &ConnectionManager::serverListenOk);
    connect(m_netWorker, &NetworkWorker::serverListenFail, this, &ConnectionManager::serverListenFail);
    connect(m_netWorker, &NetworkWorker::serverListenStopped, this, &ConnectionManager::serverListenStopped);
    connect(m_netWorker, &NetworkWorker::logMessage, this, &ConnectionManager::logMessage);

    // DBWorker 子线程
    m_dbThread = new QThread(this);
    m_dbWorker = new DBWorker;
    m_dbWorker->moveToThread(m_dbThread);
    connect(m_dbThread, &QThread::finished, m_dbWorker, &QObject::deleteLater);
    connect(m_dbThread, &QThread::started, m_dbWorker, &DBWorker::init);
    connect(m_dbWorker, &DBWorker::logMessage, this, &ConnectionManager::logMessage);
    connect(m_dbWorker, &DBWorker::dbReady, m_netWorker, &NetworkWorker::onDbReady);
    connect(m_dbWorker, &DBWorker::dbReady, this, &ConnectionManager::dbReady);

    // 用户认证
    connect(m_netWorker, &NetworkWorker::reqDbAuthRegister, m_dbWorker, &DBWorker::authRegister);
    connect(m_netWorker, &NetworkWorker::reqDbAuthLogin, m_dbWorker, &DBWorker::authLogin);
    connect(m_dbWorker, &DBWorker::authResult, m_netWorker, &NetworkWorker::onDbAuthResult);
    connect(m_netWorker, &NetworkWorker::reqDbUpdateLastActiveTime, m_dbWorker,  &DBWorker::updateLastActiveTime);

    connect(m_netWorker, &NetworkWorker::onlineUsersChanged, this, [this](const QSet<QString> &users){
        m_onlineUsers = users;
    });

    // 修改昵称
    connect(m_netWorker, &NetworkWorker::reqDbUpdateNickname, m_dbWorker, &DBWorker::updateNickname);
    connect(m_dbWorker, &DBWorker::nicknameUpdateResult, m_netWorker, &NetworkWorker::onNicknameUpdateResult);
    // 修改密码
    connect(m_netWorker, &NetworkWorker::reqDbChangePassword, m_dbWorker, &DBWorker::changePassword);
    connect(m_dbWorker, &DBWorker::passwordChangeResult, m_netWorker, &NetworkWorker::onDbPasswordChangeResult);
    // 好友搜索 好友申请
    connect(m_netWorker, &NetworkWorker::reqDbSearchUser, m_dbWorker,  &DBWorker::searchUser);
    connect(m_dbWorker,  &DBWorker::userSearchResult, m_netWorker, &NetworkWorker::onDbUserSearchResult);
    connect(m_netWorker, &NetworkWorker::reqDbSendAddFriendRequest,m_dbWorker, &DBWorker::sendAddFriendRequest);
    connect(m_dbWorker, &DBWorker::friendRequestResult, m_netWorker, &NetworkWorker::onDbFriendRequestResult);
    connect(m_netWorker, &NetworkWorker::reqDbHandleFriendRequest, m_dbWorker, &DBWorker::handleFriendRequest);
    connect(m_dbWorker, &DBWorker::friendHandleResult, m_netWorker, &NetworkWorker::onDbFriendHandleResult);
    // 待处理申请
    connect(m_netWorker, &NetworkWorker::reqDbListPendingRequests, m_dbWorker, &DBWorker::listPendingRequests);
    connect(m_dbWorker, &DBWorker::pendingRequestsResult, m_netWorker, &NetworkWorker::onPendingRequestsResult);
    // 拉取好友列表 + 通知好友我上线了
    connect(m_netWorker, &NetworkWorker::reqDbListFriends, m_dbWorker, &DBWorker::listFriends);
    connect(m_dbWorker, &DBWorker::listfriendsResult, m_netWorker, &NetworkWorker::onListFriendsResult);
    // 好友删除
    connect(m_netWorker, &NetworkWorker::reqDbRemoveFriend, m_dbWorker, &DBWorker::removeFriend);
    connect(m_dbWorker, &DBWorker::removeFriendResult, m_netWorker, &NetworkWorker::onDbRemoveFriendResult);


    // 查询数据库, 更新在线状态
    connect(this, &ConnectionManager::reqQueryUsers, m_dbWorker, &DBWorker::queryUsers);
    connect(m_dbWorker, &DBWorker::usersResult, this, [this](const QVector<UserInfo> &users) {
        QVector<UserInfo> allUsers = users;                       // 隐式共享，O(1)
        for (UserInfo &u : allUsers)                              // 触发深拷贝，几十微秒
            u.online = m_onlineUsers.contains(u.username);      // QSet O(1)
        emit usersReady(allUsers);
    });

    // 消息落库
    connect(m_netWorker, &NetworkWorker::reqDbSaveMessage, m_dbWorker, &DBWorker::saveMessage);
    connect(m_dbWorker, &DBWorker::messageSaved, m_netWorker, &NetworkWorker::onMessageSaved);
    // 拉取离线消息
    connect(m_netWorker, &NetworkWorker::reqDbLoadOfflineMsg, m_dbWorker, &DBWorker::loadofflineMsg);
    connect(m_dbWorker, &DBWorker::offlineMsgLoaded, m_netWorker, &NetworkWorker::onOfflineMsgLoaded);
    connect(m_netWorker, &NetworkWorker::reqDbMarkDelivered, m_dbWorker, &DBWorker::markDelivered);
    // 拉取历史消息
    connect(m_netWorker, &NetworkWorker::reqDbLoadHistoryMsg, m_dbWorker,  &DBWorker::loadHistoryMsg);
    connect(m_dbWorker,  &DBWorker::historyMsgLoaded, m_netWorker, &NetworkWorker::onDbHistoryMsgLoaded);
    // 最近会话
    connect(m_netWorker, &NetworkWorker::reqDbListRecentChat, m_dbWorker,  &DBWorker::listRecentChat);
    connect(m_dbWorker,  &DBWorker::recentChatLoaded, m_netWorker, &NetworkWorker::onDbRecentChatLoaded);
    connect(m_netWorker, &NetworkWorker::reqDbMarkRecentChatRead, m_dbWorker,  &DBWorker::markRecentChatRead);
    // 头像
    connect(m_netWorker, &NetworkWorker::reqDbUpdateAvatar, m_dbWorker, &DBWorker::updateAvatar);
    connect(m_dbWorker, &DBWorker::avatarUpdateResult, m_netWorker, &NetworkWorker::onDbAvatarUpdateResult);


    // 文件服务多线程 ( 存储根目录：建议放到独立数据盘 )
    m_storageThreads.reserve(kStorageShards);
    m_storageWorkers.reserve(kStorageShards);
    for(int i = 0; i < kStorageShards; ++i) {
        QThread *thread = new QThread(this);
        StorageWorker *worker = new StorageWorker(i, kStorageShards, kstoragePath);
        worker->moveToThread(thread);
        connect(thread, &QThread::started, worker, &StorageWorker::init);
        connect(thread, &QThread::finished, worker, &QObject::deleteLater);
        connect(worker, &StorageWorker::logMessage, this, &ConnectionManager::logMessage);
        m_storageThreads.append(thread);
        m_storageWorkers.append(worker);
    }

    // 在主线程 文件服务信号的转发
    m_fileService = new FileService(kStorageShards, this);
    connect(m_fileService, &FileService::logMessage, this, &ConnectionManager::logMessage);

    // FileService -> 存储分片：信号里带 shard，按分片过滤后投递, worker 对应线程里执行
    for(int i = 0; i < kStorageShards; ++i) {
        StorageWorker *storageWorker = m_storageWorkers[i];

        connect(m_fileService, &FileService::reqProbeUpload, storageWorker, [storageWorker, i](const TransferCtx &ctx){
            if (ctx.shard == i) storageWorker->probeUpload(ctx);
        });
        connect(m_fileService, &FileService::reqOpenDownload, storageWorker, [storageWorker, i](const TransferCtx &ctx){
            if (ctx.shard == i) storageWorker->openDownload(ctx);
        });
        connect(m_fileService, &FileService::reqFinishUpload, storageWorker, [storageWorker, i](int shard, quint32 transferId){
            if (shard == i) storageWorker->finishUpload(transferId);
        });
        connect(m_fileService, &FileService::reqDeleteUploadPart, storageWorker, [storageWorker, i](int shard, quint32 transferId){
            if (shard == i) storageWorker->cancelUploadAndDeletePart(transferId);
        });
        connect(m_fileService, &FileService::reqKeepUploadPart, storageWorker, [storageWorker, i](int shard, quint32 transferId){
            if (shard == i) storageWorker->cancelUploadAndKeepPart(transferId);
        });
        connect(m_fileService, &FileService::reqCloseDownload, storageWorker, [storageWorker, i](int shard, quint32 transferId){
            if (shard == i) storageWorker->closeDownload(transferId);
        });

        // 存储分片 -> FileService（回执）
        connect(storageWorker, &StorageWorker::uploadProbed,   m_fileService, &FileService::onUploadProbed);
        connect(storageWorker, &StorageWorker::uploadFinished, m_fileService, &FileService::onUploadFinished);
        connect(storageWorker, &StorageWorker::downloadOpened, m_fileService, &FileService::onDownloadOpened);
        connect(storageWorker, &StorageWorker::uploadFailed,   m_fileService, &FileService::onUploadFailed);
        connect(storageWorker, &StorageWorker::downloadFailed, m_fileService, &FileService::onDownloadFailed);

        connect(m_netWorker, &NetworkWorker::reqDownloadSentComplete,
                                        m_fileService, &FileService::onDownloadSentComplete);
    }

    // NetworkWorker <-> 存储分片（按 shard 过滤)
    for (int i = 0; i < kStorageShards; ++i) {
        StorageWorker *storageWorker = m_storageWorkers[i];
        connect(m_netWorker, &NetworkWorker::reqWriteChunk, storageWorker,
                [storageWorker, i](int shard, quint32 transferId, quint64 off, const QByteArray &data) {
                    if (shard == i) storageWorker->writeChunk(transferId, off, data);
                });
        connect(m_netWorker, &NetworkWorker::reqReadChunk, storageWorker,
                [storageWorker, i](int shard, quint32 transferId, qint64 off, int maxLen) {
                    if (shard == i) storageWorker->readChunk(transferId, off, maxLen);
                });
        // 上传块落盘回执：网络线程扣减 pendingWriteBytes，解除 socket 接收反压
        connect(storageWorker, &StorageWorker::chunkWritten,
                m_netWorker, &NetworkWorker::onChunkWritten);
        // 下载块出站：存储 -> 网络
        connect(storageWorker, &StorageWorker::chunkRead, m_netWorker, [this](const FileChunk &c, bool eof) {
                    m_netWorker->onChunkRead(c, eof);
                });
    }

    /* 上传文件：开始，服务端检测 */
    connect(m_netWorker, &NetworkWorker::reqBeginUpload,      m_fileService, &FileService::beginUpload);// emit reqProbeUpload
    // m_fileService -> reqProbeUpload -->  storageWorker->probeUpload : emit uploadProbed
    // storageWorker -> uploadProbed -->  m_fileService, &FileService::onUploadProbed : emit uploadBeginOk
    connect(m_fileService, &FileService::uploadBeginFailed,   m_netWorker, &NetworkWorker::onUploadBeginFailed);
    connect(m_fileService, &FileService::uploadBeginOk,  m_netWorker, &NetworkWorker::onUploadBeginOk); // send to client uploadBeginOk


    /* 上传文件：传输 0x02通道 upload writeChunk */
    // m_netWorker -> reqWriteChunk --> storageWorker -> writeChunk  ： 存储线程:文件数据入队，出队写入磁盘
    // storageWorker -> uploadFailed --> m_fileService -> onUploadFailed : emit uploadEndFailed // 存储失败
    connect(m_fileService, &FileService::uploadEndFailed,     m_netWorker, &NetworkWorker::onUploadEndFailed);

    /* 上传文件: 结束 */
    connect(m_netWorker, &NetworkWorker::reqFinishUpload,  m_fileService, &FileService::finishUpload); // 校验 sha256, emit reqFinishUpload
    // m_fileService -> reqFinishUpload --> storageWorker->finishUpload :  job->finishPending = true 上传文件结束
    // storageWorker -> uploadFinished --> m_fileService -> onUploadFinished : emit reqCommitFile
    connect(m_fileService, &FileService::reqCommitFile, m_dbWorker, &DBWorker::commitFile); // emit commitFileResult
    connect(m_dbWorker, &DBWorker::commitFileResult,    m_fileService, &FileService::onCommitResult);
    connect(m_fileService, &FileService::uploadEndOk,   m_netWorker, &NetworkWorker::onUploadEndOk); // send to client uploadEndOk

    /* 上传文件: 用户取消上传 */
    connect(m_netWorker, &NetworkWorker::reqAbortUpload,   m_fileService, &FileService::abortUpload); // emit reqDeleteUploadPart
    // m_fileService -> reqDeleteUploadPart --> storageWorker -> cancelUploadAndDeletePart :

    /* 文件下载：开始  */
    connect(m_netWorker, &NetworkWorker::reqBeginDownload, m_fileService, &FileService::beginDownload); // emit reqLoadFileForRead
    connect(m_fileService, &FileService::reqLoadFileForRead, m_dbWorker, &DBWorker::loadFileForRead); // emit fileReadableResult
    connect(m_dbWorker, &DBWorker::fileReadableResult, m_fileService, &FileService::onReadableResult);// emit reqOpenDownload
    // m_fileService -> reqOpenDownload --> storageWorker -> openDownload : emit downloadOpened
    // storageWorker -> downloadOpened --> m_fileService -> onDownloadOpened : emit downloadBeginOk
    connect(m_fileService, &FileService::downloadBeginOk, m_netWorker, &NetworkWorker::onDownloadBeginOk); // emit reqReadChunk

    connect(m_fileService, &FileService::downloadBeginFailed, m_netWorker, &NetworkWorker::onDownloadBeginFailed);
    connect(m_fileService, &FileService::downloadFailed,      m_netWorker, &NetworkWorker::onDownloadFailed);

    /* 文件下载：传输 0x02通道 upload readChunk */
    // m_netWorker -> reqReadChunk --> storageWorker -> readChunk : emit chunkRead
    // storageWorker -> chunkRead --> m_netWorker -> onChunkRead :  offset += data.size

    /* 文件下载：用户取消下载 */
    connect(m_netWorker, &NetworkWorker::reqAbortDownload, m_fileService, &FileService::abortDownload); // emit reqCloseDownload
    // m_fileService -> reqCloseDownload --> storageWorker -> closeDownload :

    /* 文件下载：结束 客户端自己计算是否结束，需要超时判断是否丢失数据 */


    // 删除文件 deleteFile ， reqMarkFileDeleted 需要改为直接删除文件数据和数据库中的对应文件信息
    connect(m_netWorker, &NetworkWorker::reqDeleteFile,      m_fileService, &FileService::deleteFile);  // emit reqMarkFileDeleted
    connect(m_fileService, &FileService::reqMarkFileDeleted, m_dbWorker, &DBWorker::markFileDeleted);
    connect(m_dbWorker, &DBWorker::fileDeleteResult,         m_fileService, &FileService::onDeleteResult);
    connect(m_fileService, &FileService::fileDeleted,        m_netWorker, &NetworkWorker::onFileDeleted);


    // 文件传输时，用户断连或者下线：上传保留 .part 等续传，下载直接关句柄
    connect(m_netWorker, &NetworkWorker::reqCloseSession,  m_fileService, &FileService::closeSession);

    // 存储对象回收
    connect(m_fileService, &FileService::reqQueryReclaimableObjects, m_dbWorker, &DBWorker::queryReclaimableObjects);
    connect(m_dbWorker, &DBWorker::reclaimableObjectsResult, m_fileService, &FileService::onReclaimableObjects);
    connect(m_fileService, &FileService::reqDropReclaimedRows, m_dbWorker, &DBWorker::dropReclaimedRows);
    connect(m_dbWorker, &DBWorker::reclaimedRowsDropped, m_fileService, &FileService::onReclaimedRowsDropped);

    for (int i = 0; i < kStorageShards; ++i) {
        StorageWorker *work = m_storageWorkers[i];
        connect(m_fileService, &FileService::reqDeleteObject, work,
                [work, i](int shard, quint8 biz, const QString &sha) {
                    if (shard == i)
                        work->deleteObject(biz, sha);
                });
        connect(work, &StorageWorker::objectDeleted,      m_fileService, &FileService::onObjectDeleted);
        connect(work, &StorageWorker::objectDeleteFailed, m_fileService, &FileService::onObjectDeleteFailed);
    }


    /* 磁盘对账：扫盘找回收巡检看不见的垃圾（孤儿对象 / .part 残留） */
    connect(m_fileService, &FileService::reqQueryLiveObjects, m_dbWorker, &DBWorker::queryLiveObjects);
    connect(m_dbWorker, &DBWorker::liveObjectsResult, m_fileService, &FileService::onLiveObjects);
    // 整个扫盘过程占用存储分片 0 的线程，分片 1 完全不受影响
    StorageWorker *scanWorker = m_storageWorkers[0];
    connect(m_fileService, &FileService::reqScanStorage, scanWorker,[scanWorker](int shard, const QSet<QString> &live)
    {
        Q_UNUSED(shard);
        if (scanWorker)
            scanWorker->scanStorage(live);
    });
    connect(scanWorker, &StorageWorker::storageScanFinished, m_fileService, &FileService::onStorageScanFinished);

    m_netWorkThread->start();
    m_dbThread->start();
    for(QThread * thread : m_storageThreads)
        thread->start();
    m_fileService->init(); // 启动清扫定时器
}

static ConnectionManager *g_instance = nullptr;

ConnectionManager &ConnectionManager::instance()
{
    if(!g_instance)
        g_instance = new ConnectionManager;
    return *g_instance;
}

void ConnectionManager::shutdown()
{
    if(!g_instance)
        return;
    ConnectionManager *inst = g_instance;
    g_instance = nullptr;
    delete inst;
}

ConnectionManager::~ConnectionManager()
{
    teardownThreads();
}

// 关闭顺序: 网络 → 存储 → 数据库
// 关闭反序会出现磁盘有数据、元数据没更新 或 元数据已就绪、文件不存在,
void ConnectionManager::teardownThreads()
{
    if(m_shutdownDone)
        return;
    m_shutdownDone = true;

    // 先停 NetworkWorker 不产生任何新的 DB 请求。
    if (m_netWorkThread && m_netWorkThread->isRunning()) {
        QSemaphore drained;
        QMetaObject::invokeMethod(m_netWorker, [worker = m_netWorker, &drained]() {
            if (worker)
                worker->cleanup();   // 在 NetworkWorker 线程里执行: close server + closeAllSessions
            drained.release();       // 通知主线程：做完了
        }, Qt::QueuedConnection);    // 投递到 NetworkWorker 线程事件队列末尾，异步执行

        if (!drained.tryAcquire(1, kThreadWaitMs))
            qWarning() << "NetworkWorker cleanup 超时，会话可能未正常关闭";

        m_netWorkThread->quit();
        if (!m_netWorkThread->wait(kThreadWaitMs)) {
            qWarning() << "网络线程 3s 内未退出，强制终止（worker 可能泄漏）";
            m_netWorkThread->terminate();
            m_netWorkThread->wait();
        }
    }
    m_netWorker = nullptr;


    if (!m_storageThreads.isEmpty() && m_fileService) {
        m_fileService->cleanup(); // 同在主线程，直接调用
    }

    for (QThread *th : m_storageThreads) {
        if (!th || !th->isRunning())
            continue;
        QSemaphore drained;
        StorageWorker *w = nullptr;
        for (StorageWorker *cand : m_storageWorkers) {
            if (cand && cand->thread() == th) { w = cand; break; }
        }
        QMetaObject::invokeMethod(w, [worker = w, &drained]() {
            if (worker) worker->cleanup();
            drained.release();
        }, Qt::QueuedConnection);
        if (!drained.tryAcquire(1, kThreadWaitMs))
            qWarning() << "存储线程 cleanup 超时";
        th->quit();
        if (!th->wait(kThreadWaitMs)) {
            qWarning() << "存储线程 3s 内未退出，强制终止（可能有文件句柄泄漏）";
            th->terminate();
            th->wait();
        }
    }
    m_storageWorkers.clear();
    m_storageThreads.clear();
    m_fileService = nullptr;

    if (m_dbThread && m_dbThread->isRunning()) {
        QSemaphore drained;
        QMetaObject::invokeMethod(m_dbWorker, [worker = m_dbWorker, &drained]() {
            if (worker)
                worker->cleanup();
            drained.release();
        }, Qt::QueuedConnection);

        if (!drained.tryAcquire(1, kThreadWaitMs))
            qWarning() << "DBWorker cleanup 超时，可能有落库请求被丢弃";

        m_dbThread->quit();
        if (!m_dbThread->wait(kDbThreadWaitMs)) {
            qWarning() << "数据库线程, 关闭超时(等了10秒)，已强制关闭。可能有数据库操作卡住了，未保存的数据可能丢失";
            m_dbThread->terminate();
            m_dbThread->wait();
        }
    }
    m_dbWorker = nullptr;
}

void ConnectionManager::startServerListen(unsigned short port)
{
    emit reqStartServerListen(port);
}

void ConnectionManager::stopServerListen()
{
    emit reqStopServerListen();
}

void ConnectionManager::requestUsers()
{
    emit reqQueryUsers();
}

