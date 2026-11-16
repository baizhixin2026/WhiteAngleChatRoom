#ifndef MYSQLUSERSTORE_H
#define MYSQLUSERSTORE_H

#include "protocoltypes.h"
#include <QObject>
#include <QString>

class MysqlUserStore : public QObject
{
    Q_OBJECT

public:
    explicit MysqlUserStore(const QString &connectionName);
    ~MysqlUserStore() = default;

    struct Result {
        bool ok = false;
        QString text;       // 客户端提示
        qint64 uid = 0;     // 登录成功时非 0
        qint64 toUid = 0;
        qint64 accountNo = 0;
        QString nickname;
        QString avatarFileId;
    };

    struct HandleFriendResult {
        bool    ok = false;
        QString text;
        qint64  reqId    = 0;
        qint64  selfUid  = 0;       // 接收方（点通过/拒绝的那个人）
        qint64  fromUid  = 0;       // 申请人
        qint64  selfAccountNo = 0;
        qint64  fromAccountNo = 0;
        QString selfNick;
        QString fromNick;
        QString selfAvatarFileId;
        QString fromAvatarFileId;
        bool    firstTime = false;  // 首次建立好友关系（false = 重复通过）
        bool    accepted  = false;  // 通过 true；拒绝 false
    };

    struct RemoveFriendResult {
        bool    ok = false;
        QString text;
        qint64  removeUid = 0;
        QString removeNick;
    };


    Result verifyLogin(const QString &user, const QString &clientHash);
    Result registerUser(const QString &user, const QString &clientHash);
    bool changePassword(qint64 uid, const QString &oldPassWordHash,
                                const QString &newPassWordHash, int *errcode);
    Result searchUserByAccountNo(qint64 accountNo);
    Result sendAddFriendRequest(qint64 fromUid, qint64 toAccountNo, const QString &text);
    QVector<FriendRequestInfo> listPendingRequests(qint64 toUid);
    QVector<SearchResult> listFriends(qint64 uid);
    HandleFriendResult handleFriendRequest(qint64 reqId, qint64 selfUid, bool accept);
    RemoveFriendResult removeFriend(qint64 uid, qint64 removeUid);

    HistoryPage loadHistoryMsg(qint64 me, qint64 peerUid, const QString &beforeMsgId, int limit);
    bool upsertRecentChat(qint64 ownerUid, qint64 peerUid, const QString &msgId, qint64 ts,
                                const QString &MsgText, int insertUnread, int updateUnread);
    RecentChatResult listRecentChat(qint64 uid);
    bool clearUnread(qint64 uid, qint64 peerUid);

    void updateLoginActiveTime(qint64 uid);
    void updateLastActiveTime(qint64 uid, qint64 ts);

    bool setAvatar(qint64 uid, const QString &fileId, int *errcode);
    QString avatarFileIdOf(qint64 uid);

signals:
    void logMessage(const QString &msg);

private:
    QString m_connectionName;
};

#endif // MYSQLUSERSTORE_H
