#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "contactmodel.h"
#include "contactdelegate.h"
#include "messagemodel.h"
#include "messagedelegate.h"
#include "protocoltypes.h"
#include "userinfodialog.h"
#include "friendrequestdialog.h"

#include <QMainWindow>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QStandardItemModel>
#include <QList>
#include <QHash>
#include <QModelIndex>
#include <QPersistentModelIndex>
#include <QEvent>
#include <QString>
#include <QToolButton>
#include <QSet>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class LoginDialog;  // 前置声明
class EmojiPanel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    // 只让消息气泡区域响应附件点击，行内空白处不触发
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void on_loginBtn_clicked();    // 弹出登录框
    void on_logoutBtn_clicked();
    void on_sendMsgBtn_clicked();
    void onClientMessage(const ChatMessage &msg);
    void onChatMessageAcked(const ChatAck &ack);
    void onHistoryMsgLoaded(const HistoryPage &page);
    void onRecentChatReceived(const RecentChatResult &msg);

    void onChatScroll(int value);
    void nicknameChanged(const QString &uid, const QString &nick);

    void onUserSearchResult(qint64 accountNo, const QString &nick, bool ok, const QString &text);
    void onFriendRequestResult(bool ok, const QString &text);
    void onFriendsFirstUpdate(const QList<SearchResult> &friends);
    void onFriendsOnlineUpdate(qint64 uid, bool online);
    void onFriendAdded(qint64 uid, qint64 accountNo, const QString &nick, bool online, QString avatarFileId);
    void onFriendRemoveResult(bool ok, const QString &text, qint64 removeUid);
    void onFriendRemoved(qint64 uid);

    void on_addFriendBtn_clicked();
    void on_friendRequestBtn_clicked();
    void onMoreMenuAboutToShow();
    void onRemoveFriendBtn();
    void onLeaveGroupBtn();
    void onMessageListContextMenu(const QPoint &pos);

    void onAvatarLoaded(qint64 uid, const QString &fileId);

    void emoji(bool checked);

private:
    void onLoginSuccess();
    void onConnectionLost();

    void showLoginDialog();
    void updateSendButtonState();

    QString nicknameForDisplay(const QString &uid) const;
    void switchToChatTarget(const QString &targetUid);
    void onContactListViewClicked(const QModelIndex &index);
    void onContactTreeViewClicked(const QModelIndex &index);
    void updateUnreadMessageBadge();
    void updatePendingRequestBadge();

    void refreshMyAvatarIcon();
    QPixmap avatarForUid(qint64 uid) const;

    void loadRecentChatList();
    void rebuildFriendsList();
    void rebuildContactTreeView(const QList<UserEntry> &users, const QVariantMap &nickMap);
    void autoUncheckToolButton(QToolButton *btn);

    void friendRemove(qint64 uid);

    void updateRecentChatRow(qint64 peerUid);

    void insertEmojiToUiInput(const QString &code);
    QString editorEmojiPlainText() const;

    // 选文件 → 上传 → 发附件消息
    void on_fileBtn_clicked();
    void onUploadProgress(quint32 taskId, qint64 sentBytes, qint64 totalBytes, qint64 elapsedMs);
    void onFileUploadFinished(quint32 taskId, const QString &fileId, const FileMeta &meta);
    void onFileUploadFailed(quint32 taskId, int errcode, const QString &text);

    // 点附件气泡 → 另存为 → 下载
    void onDownloadAttachmentClicked(const QModelIndex &index);
    void onDownloadProgress(quint32 taskId, qint64 receivedBytes, qint64 totalBytes, qint64 elapsedMs);
    void onDownloadFinished(quint32 taskId, const QString &localPath, qint64 size);
    void onDownloadFailed(quint32 taskId, int errcode, const QString &text);

private:
    Ui::MainWindow *ui;

    // 析构期间禁止再弹模态框：ConnectionManager::shutdown() 会同步回调失败槽
    bool m_shuttingDown = false;

    LoginDialog *m_loginDialog = nullptr;
    UserInfoDialog *m_userInfoDialog = nullptr;
    QString m_userName;

    FriendRequestDialog *m_friendReqDialog = nullptr;
    QLabel *m_contactBadge = nullptr;


    QMenu *m_moreMenu = nullptr;

private:
    QString m_currentTargetUid;
    // 最近会话
    ContactModel *m_recentChatModel = nullptr;
    QHash<qint64, RecentChatItem> m_recentChats; // 最近会话数据源
    // 好友
    ContactModel *m_contactModel = nullptr;
    ContactDelegate *m_contactDelegate = nullptr;
    // 群聊未读消息
    QHash<QString, int> m_groupUnreadCounts;
    QLabel *m_chatBadge;
    // 群组
    QStandardItemModel *m_contactTreeModel = nullptr;
    // 消息气泡
    MessageModel *m_messageModel = nullptr;
    MessageDelegate *m_messageDelegate = nullptr;
    QPersistentModelIndex m_pressedBubbleIndex;   // 按下时命中的气泡行，释放时要求同一行

    // 哈希表: 聊天对象为键, 缓存历史消息
    QHash<QString, QList<MessageItem>> m_sessionMessages;

    // 好友
    QList<SearchResult> m_friendsList;

    // 历史消息
    QSet<QString> m_historyMsgLoadingSessions;  // 每个会话独立 防重入
    QHash<QString, bool> m_historyMsgHasMore;

    // 表情包
    EmojiPanel *m_emojiPanel = nullptr;

//    void resizeEvent(QResizeEvent *event) override; // 调试窗口尺寸

private:
    // 附件
    void refreshChatTitle();      // 统一管理会话标题（含"正在发送/正在下载"）
    void resetUploadState();      // 清当前上传状态 + 刷新标题
    void showDownloadDoneDialog(const QString &localPath);   // 下载完成：只给"打开文件/打开所在文件夹"

    // 图片缩略图本地缓存：fileId -> 小图；重登录后历史图片不必重新下载
    QPixmap cachedImageThumb(const QString &fileId);
    void storeImageThumb(const QString &fileId, const QPixmap &thumb);
    void pruneImageThumbCache(qint64 maxBytes);
    QHash<QString, QPixmap> m_imageThumbCache;
    int m_imageThumbStoreCount = 0;   // 每写若干张小图清理一次磁盘缓存，防止长会话无限增长

    // 当前正在发送的附件（一次只允许一个）
    quint32 m_uploadTaskId = 0;
    QString m_uploadPeerUid;      // 上传期间切会话也不会发错人
    QString m_uploadLocalPath;
    QString m_uploadFileName;
    QString m_uploadMime;
    qint64  m_uploadFileSize = 0;
    quint8  m_uploadMsgType  = Proto::MsgTypeText;
    int     m_uploadPercent   = 0;
    qint64  m_uploadElapsedMs = 0;

    // 用户主动发起的下载：taskId -> 气泡信息，完成/失败时回填缩略图或提示
    struct DownloadUi {
        QString peerUid;
        QString fileId;
        QString fileName;
        QString savePath;
        quint8  msgType = Proto::MsgTypeFile;
        int     lastPercent = 0;
        qint64  elapsedMs   = 0;
    };
    QHash<quint32, DownloadUi> m_downloads;

};
#endif // MAINWINDOW_H
