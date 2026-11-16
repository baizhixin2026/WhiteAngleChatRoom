#ifndef DBWORKER_H
#define DBWORKER_H

#include "userinfo.h"
#include "protocoltypes.h"
#include "mysqluserstore.h"
#include "mysqlfilestore.h"
#include <QObject>
#include <QString>
#include <QVector>
#include <QSet>

class MysqlFileStore;
class QTimer;

class DBWorker : public QObject
{
    Q_OBJECT

public:
    explicit DBWorker(QObject *parent = nullptr);
    ~DBWorker();

public slots:
    void init();
    void cleanup();
    void sweepRetention();

    void authLogin(quint64 sessionId, const QString &user, const QString &clientHash);
    void authRegister(quint64 sessionId, const QString &user, const QString &clientHash);
    void queryUsers();
    void saveMessage(quint64 sessinId, const ChatMessage &msg);
    void loadofflineMsg(quint64 sessionId, qint64 uid);
    void markDelivered(const QStringList &msgIds);
    void loadHistoryMsg(quint64 sessionId, qint64 me, qint64 peerUid, const QString &beforeMsgId, int limit);
    void updateNickname(quint64 sessionId, const QString &username, const QString &newNick);
    void changePassword(quint64 sessionId, qint64 uid, const QString &oldPassWordHash, const QString &newPassWordHash);
    void updateAvatar(quint64 sessionId, qint64 uid, const QString &fileId);
    void updateLastActiveTime(qint64 uid, qint64 ts);

    void searchUser(quint64 sessionId, qint64 accountNo);
    void sendAddFriendRequest(quint64 sessionId, qint64 fromUid, qint64 toAccountNo, const QString &text);
    void listPendingRequests(quint64 sessionId, qint64 uid);
    void listFriends(quint64 sessionId, qint64 uid);
    void handleFriendRequest(quint64 sessionId, qint64 reqId, qint64 selfUid, bool accept);
    void removeFriend(quint64 sessionId, qint64 uid, qint64 removeUid);

    void listRecentChat(quint64 sessionId, qint64 uid);
    void markRecentChatRead(qint64 uid, qint64 peerUid);

    void commitFile(quint32 transferId, const FileMeta &meta);
    void loadFileForRead(quint32 transferId, qint64 uid, const QString &fileId);
    void markFileDeleted(qint64 uid, const QString &fileId);

    // 回收：查候选 / 删除已回收记录
    void queryReclaimableObjects(int limit);
    void dropReclaimedRows(quint8 biz, const QString &sha256);
    void queryLiveObjects();

private:
    void closeDatabase();

    // 留存消息/文件清理：分批执行，批之间让出事件循环（DB 线程是串行的，不能一条 DELETE 占到底）
    void markFileAndDeleteChatMessages();
    void deleteExpiredMessages();
    void retentionFinish();

signals:
    void dbReady(bool ok);
    void logMessage(const QString &msg);
    // op: 0=login, 1=register
    void authResult(quint64 sessionId, int op, const QString &username, bool ok, const QString &text,
                qint64 uid, qint64 accountNo, const QString &nickname, const QString &avatarFileId);
    void nicknameUpdateResult(quint64 sessionId, const QString &username,
                                  bool ok, const QString &newNick, const QString &text);
    void passwordChangeResult(quint64 sessionId, bool ok, int errcode);
    void avatarUpdateResult(quint64 sessionId, bool ok, int errcode, const QString &avatarFileId);
    void usersResult(const QVector<UserInfo> &users);

    void userSearchResult(quint64 sessionId, bool ok, const QString &text, const SearchResult &user);
    void friendRequestResult(quint64 sessionId, bool ok, const QString &text, qint64 toUid);
    void pendingRequestsResult(quint64 sessionId, const QVector<FriendRequestInfo> &reqs);
    void listfriendsResult(quint64 sessionId, const QVector<SearchResult> &friends);
    void friendHandleResult(quint64 sessionId, const MysqlUserStore::HandleFriendResult &r);
    void removeFriendResult(quint64 sessionId, const MysqlUserStore::RemoveFriendResult &r);

    void messageSaved(quint64 sessionId, const  ChatMessage &msg, bool ok, const QString &reason);
    void offlineMsgLoaded(quint64 sessionId, const QVector<ChatMessage> &msgs);
    void historyMsgLoaded(quint64 sessionId, const HistoryPage &page);
    void recentChatLoaded(quint64 sessionId, const RecentChatResult &r);

    void commitFileResult(quint32 transferId, bool ok, int errcode);
    void fileReadableResult(quint32 transferId, bool ok, int errcode, const FileMeta &meta);
    void fileDeleteResult(const QString &fileId, bool ok, int errcode);

    void reclaimableObjectsResult(const QVector<ReclaimableObject> &list);
    void reclaimedRowsDropped(quint8 biz, const QString &sha256, bool ok, int errcode);
    void liveObjectsResult(bool ok, const QSet<QString> &live);

private:
    bool m_dbOk = false;
    QString m_connectionName;
    MysqlUserStore *m_userStore = nullptr;

    MysqlFileStore *m_fileStore = nullptr;

    // 留存清理
    QTimer *m_retentionTimer = nullptr;
    bool    m_retentionRunning      = false;
    int     m_retentionFileRows     = 0;
    int     m_retentionMessageRows  = 0;
    qint64  m_retentionFileCutoff   = 0;
    qint64  m_retentionMessageCutoff = 0;
};

#endif // DBWORKER_H
