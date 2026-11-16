#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "connectionmanager.h"
#include "logindialog.h"
#include "networkworker.h"
#include "emojipanel.h"
#include "emojitext.h"
#include "protocoltypes.h"
#include "avatarcache.h"
#include "avatardisplay.h"
#include "messagedelegate.h"

#include <QFileDialog>
#include <QProgressBar>
#include <QAbstractItemView>
#include <QAction>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QResizeEvent>
#include <QMenu>
#include <QApplication>
#include <QClipboard>
#include <QInputDialog>
#include <QScrollBar>
#include <QDebug>
#include <QAbstractItemView>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QTextCharFormat>
#include <QTextFragment>
#include <QRegularExpression>
#include <QFileInfo>
#include <QFile>
#include <QMimeDatabase>
#include <QUuid>
#include <QImageReader>
#include <QDesktopServices>
#include <QUrl>
#include <QPushButton>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QMouseEvent>
#include <algorithm>
#include <QDateTime>

namespace {
    // 缩略图磁盘缓存：只存"已经上传/下载过"的小图，避免重登录后历史图片全变空白。
    // 不自动下载原图；缓存上限在每次登录时按最旧文件清理一次。
    constexpr qint64 kImageThumbCacheMaxBytes = 64LL * 1024 * 1024;

    const QPixmap &defaultAvatar() // 默认头像
    {
        static const QPixmap defaultAvatar(QStringLiteral(":/image/user/user-9.png"));
        return defaultAvatar;
    }

    QString imageThumbCacheDir()
    {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (dir.isEmpty())
            dir = QDir::tempPath();
        return QDir(dir).filePath(QStringLiteral("chat_images"));
    }

    QString imageThumbCachePath(const QString &fileId)
    {
        if (fileId.isEmpty())
            return {};
        const QString safeName = QFileInfo(fileId).fileName();
        if (safeName.isEmpty() || safeName == QStringLiteral(".") || safeName == QStringLiteral(".."))
            return {};
        return QDir(imageThumbCacheDir()).filePath(safeName + QStringLiteral(".png"));
    }

    // 附件消息在 1c 之前先显示成一行文本；1c 会换成真正的卡片
    QString attachmentPreview(quint8 msgType, const QString &fileName)
    {
        switch (msgType) {
            case Proto::MsgTypeImage: return QStringLiteral("[图片]");
            case Proto::MsgTypeFile:  return QStringLiteral("[文件] ") + fileName;
            case Proto::MsgTypeVoice: return QStringLiteral("[语音]");
            case Proto::MsgTypeVideo: return QStringLiteral("[视频]");
            default:                  return QString();
        }
    }

    // 按扩展名判断: 上传只允许一个，按扩展名分到"图片"或"普通文件"
    bool isImageSuffix(const QString &suffix)
    {
        static const QStringList kImage = {
            QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
            QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp")
        };
        return kImage.contains(suffix.toLower()); // suffix 返回不带点的后缀, 后缀转成小写
    }

    // 从本地文件生成图片缩略图。
    // 关键：setScaledSize 让解码器按目标尺寸解码，而不是先解整张大图再缩, 一张 50 MB 的 TIFF 全解会卡住 GUI 好几秒。
    QPixmap makeImageThumbnail(const QString &path, int maxW, int maxH)
    {
        QImageReader reader(path);
        reader.setAutoTransform(true);

        QSize size = reader.size();
        if (!size.isValid() || size.isEmpty())
            return QPixmap();

        size.scale(maxW, maxH, Qt::KeepAspectRatio);
        reader.setScaledSize(size);

        const QImage img = reader.read();
        return img.isNull() ? QPixmap() : QPixmap::fromImage(img);
    }

    QString formatElapsedMs(qint64 ms)
    {
        if (ms <= 0)
            return QStringLiteral("0 秒");
        if (ms < 10000)
            return QStringLiteral("%1 秒").arg(ms / 1000.0, 0, 'f', 1);
        return QStringLiteral("%1 秒").arg(ms / 1000);
    }
}


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

#ifdef Q_OS_WIN
    setFixedSize(1100, 600);    // Windows
#else
    resize(800, 480);   // Linux
#endif

    this->setWindowTitle(" ");
    ui->searchLineEdit->setStyleSheet("QLineEdit { font-size: 12px; color: #707273; }");
    ui->searchLineEdit->setPlaceholderText("搜索 账号");
    ui->msg->setPlaceholderText("发送信息, 鼠标右键可以复制消息");
    ui->msg->setAcceptRichText(false);  // 只接受来自外部的纯文本粘贴, 防止外部格式污染消息内容
    QFont msgFont;
    msgFont.setPixelSize(Proto::kMsgSize);
    ui->msg->setFont(msgFont);
    ui->logoutBtn->setEnabled(false);

    // 发送信息按钮
    QIcon msgBtnIcon;
    msgBtnIcon.addFile(":/image/buttton/sendMsg-1.png", QSize(), QIcon::Disabled, QIcon::Off);
    msgBtnIcon.addFile(":/image/buttton/sendMsg-2.png", QSize(), QIcon::Active, QIcon::On);
    ui->sendMsgBtn->setIcon(msgBtnIcon);
    ui->sendMsgBtn->setIconSize(QSize(32, 32));
    ui->sendMsgBtn->setEnabled(false);

    // 用户个人信息，默认头像
    ui->side_user_toolButton->setIcon(QIcon(defaultAvatar()));
    ui->side_user_toolButton->setIconSize(QSize(32, 32));

    // 头像缓存：建立单例并完成内部接线（登录/换头像/下载回调）
    connect(&AvatarCache::instance(), &AvatarCache::avatarLoaded, this, &MainWindow::onAvatarLoaded);

    ui->stackedWidget->setCurrentWidget(ui->friendListPage);  // 默认启动显示, 避免UI里误触
    // 联系人 ListView 数据与视图绑定
    m_contactModel = new ContactModel(this);
    m_contactDelegate = new ContactDelegate(this);
    ui->chat_userListView->setModel(m_contactModel);
    ui->chat_userListView->setItemDelegate(m_contactDelegate);
    ui->chat_userListView->setMouseTracking(true);
    // 最近会话
    m_recentChatModel = new ContactModel(this);
    m_recentChatModel->setAutoSort(true);
    ui->recentChatListView->setModel(m_recentChatModel);
    ui->recentChatListView->setItemDelegate(m_contactDelegate);
    ui->recentChatListView->setMouseTracking(true);

    // 群组 treeView 数据与视图绑定
    m_contactTreeModel = new QStandardItemModel(this);
    ui->contactTreeView->setModel(m_contactTreeModel);
    ui->contactTreeView->setHeaderHidden(true);   // 隐藏树形控件顶部默认的标题栏
    ui->contactTreeView->setEditTriggers(QAbstractItemView::NoEditTriggers);    // 鼠标不能编辑修改
    ui->contactTreeView->setIconSize(QSize(32, 32));
    ui->contactTreeView->setIndentation(10);    // 子节点相对于父节点向右缩进的像素值
    ui->contactTreeView->setTextElideMode(Qt::ElideRight);

    // 消息对话框 气泡
    m_messageModel = new MessageModel(this);
    m_messageDelegate = new MessageDelegate(this);
    ui->chatMsgListView->setModel(m_messageModel);
    ui->chatMsgListView->setItemDelegate(m_messageDelegate);
    ui->chatMsgListView->setResizeMode(QListView::Adjust);  // 尺寸变化时自动调整布局 Delegate动态宽高自动刷新
    ui->chatMsgListView->setContextMenuPolicy(Qt::CustomContextMenu);   // 设置列表支持右键菜单, 用于复制消息
    ui->chatMsgListView->setMouseTracking(true);    // 待扩展：鼠标悬停气泡时 显示时间戳

    // 未读会话消息
    m_chatBadge = new QLabel(this);
    m_chatBadge->setAlignment(Qt::AlignCenter);
    m_chatBadge->setStyleSheet(
                "QLabel {"
                    "  background-color: #F74C30;"
                    "  color: white;"
                    "  border-radius: 10px;"
                    "  font-size: 12px;"
                    "  font-weight: bold;"
                    "}"
                );
    m_chatBadge->hide();
    m_chatBadge->setParent(ui->side_chat_toolButton);
    m_chatBadge->raise();   // 提升到同层级兄弟控件的最上层（Z 轴最上面)

    // 待处理消息
    m_contactBadge = new QLabel(this);
    m_contactBadge->setAlignment(Qt::AlignCenter);
    m_contactBadge->setStyleSheet(
        "QLabel {"
        "  background-color: #F74C30;"
        "  color: white;"
        "  border-radius: 10px;"
        "  font-size: 12px;"
        "  font-weight: bold;"
        "}"
    );
    m_contactBadge->hide();
    m_contactBadge->setParent(ui->friendRequestBtn);
    m_contactBadge->raise();

    // 删除好友 退出群组
    m_moreMenu = new QMenu(ui->moreBtn);
    ui->moreBtn->setMenu(m_moreMenu);
    ui->moreBtn->setPopupMode(QToolButton::InstantPopup);
    ui->moreBtn->setCursor(Qt::PointingHandCursor);
    connect(m_moreMenu, &QMenu::aboutToShow, this, &MainWindow::onMoreMenuAboutToShow);

    // 侧边栏 导航工具按钮
    connect(ui->side_chat_toolButton, &QToolButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->recentChatPage);
    });
    connect(ui->side_contact_toolButton, &QToolButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->friendListPage);
    });
    connect(ui->side_group_toolButton, &QToolButton::clicked, this, [this]() {
        ui->stackedWidget->setCurrentWidget(ui->groupPage);
    });

    connect(ui->chat_userListView, &QListView::clicked, this, &MainWindow::onContactListViewClicked);
    connect(ui->recentChatListView, &QListView::clicked, this, &MainWindow::onContactListViewClicked);
    connect(ui->contactTreeView, &QTreeView::clicked, this, &MainWindow::onContactTreeViewClicked);

    connect(ui->chatMsgListView, &QListView::customContextMenuRequested, this, &MainWindow::onMessageListContextMenu);
    // 不在 QListView::clicked 里处理：那个信号覆盖整行。改成 viewport 事件过滤 + 气泡矩形命中测试。
    ui->chatMsgListView->viewport()->installEventFilter(this);
    connect(ui->msg, &QTextEdit::textChanged, this, [this]() { updateSendButtonState(); });

    // 展示用户个人信息
    connect(ui->side_user_toolButton, &QToolButton::clicked, this, [=]() {
        if(!m_userInfoDialog)
            m_userInfoDialog = new UserInfoDialog(this);
        m_userInfoDialog->show();
    });

    auto& conn = ConnectionManager::instance();

    connect(&conn,&ConnectionManager::connecting, this, [=](){
//        ui->record->append("正在连接服务器...");
    });

    connect(&conn, &ConnectionManager::loginSuccess, this, &MainWindow::onLoginSuccess);
    connect(&conn, &ConnectionManager::connectionLost, this, &MainWindow::onConnectionLost);
    connect(&conn,&ConnectionManager::errorOccurred, this, [=](const QString &err){
        Q_UNUSED(err);
        ui->loginBtn->setEnabled(true);
        ui->logoutBtn->setEnabled(false);
        updateSendButtonState();
    });

    connect(&conn, &ConnectionManager::clientMessage, this, &MainWindow::onClientMessage);
    connect(&conn, &ConnectionManager::chatMessageAcked, this, &MainWindow::onChatMessageAcked);
    connect(&conn, &ConnectionManager::serverMessage, this, [=](const QString& msg) {
        QMessageBox::information(this, "服务器广播", msg);
    });
    connect(&conn, &ConnectionManager::systemMessage, this, [=](const QString& msg) {
        QMessageBox::information(this, "系统消息", msg);
    });


    connect(&conn, &ConnectionManager::nicknameChanged, this, &MainWindow::nicknameChanged);

    // 好友
    connect(&conn, &ConnectionManager::userSearchResult, this, &MainWindow::onUserSearchResult);
    connect(&conn, &ConnectionManager::friendsFirstUpdate, this, &MainWindow::onFriendsFirstUpdate);
    connect(&conn, &ConnectionManager::friendRequestResult, this, &MainWindow::onFriendRequestResult);
    connect(&conn, &ConnectionManager::friendAdded, this, &MainWindow::onFriendAdded);
    connect(&conn, &ConnectionManager::friendsOnlineUpdate, this, &MainWindow::onFriendsOnlineUpdate);
    connect(&conn, &ConnectionManager::friendRemoveResult, this, &MainWindow::onFriendRemoveResult);
    connect(&conn, &ConnectionManager::friendRemoved, this, &MainWindow::onFriendRemoved);
    // 历史消息
    connect(&conn, &ConnectionManager::historyMsgLoaded, this, &MainWindow::onHistoryMsgLoaded);
    connect(ui->chatMsgListView->verticalScrollBar(), &QScrollBar::valueChanged, this, &MainWindow::onChatScroll);
    // 最近会话
    connect(&conn, &ConnectionManager::recentChatReceived, this, &MainWindow::onRecentChatReceived);


    // 输入区 工具栏 按钮
    connect(ui->emojiBtn, &QToolButton::clicked, this,&MainWindow::emoji);
    connect(ui->fileBtn, &QToolButton::clicked, this, &MainWindow::on_fileBtn_clicked);
    connect(ui->musicBtn, &QToolButton::clicked, this,[this]() { autoUncheckToolButton(ui->musicBtn); });
    connect(ui->pictureBtn, &QToolButton::clicked, this,[this]() { autoUncheckToolButton(ui->pictureBtn); });
    connect(ui->screenShotBtn,&QToolButton::clicked,this,[this](){autoUncheckToolButton(ui->screenShotBtn);});
    connect(ui->videoBtn, &QToolButton::clicked, this,[this]() { autoUncheckToolButton(ui->videoBtn); });

    // 待处理申请
    connect(&conn, &ConnectionManager::friendRequestReceived, this, [this](const FriendRequestInfo &info) {
        updatePendingRequestBadge();
        if (m_friendReqDialog && m_friendReqDialog->isVisible())
            m_friendReqDialog->addFriendRequest(info);
    });

    connect(&conn, &ConnectionManager::friendRequestHandled, this, [this](qint64 reqId, bool ok, const QString &text)
    {
        updatePendingRequestBadge();
        if (ok) {
            if (m_friendReqDialog)
                m_friendReqDialog->removeRequest(reqId);
            QMessageBox::information(this, " ", text);
        } else { // 失败时卡片还在，Dialog 里按钮需要恢复可点, 简单做法：把 Dialog 全量刷新一次
            QMessageBox::warning(this, " ", text);
            if (m_friendReqDialog)
                m_friendReqDialog->setRequests( ConnectionManager::instance().pendingFriendRequests());
        }
    });

    // 好友换头像：服务端只在好友在线时增量推送
    connect(&conn, &ConnectionManager::friendAvatarChanged, this, [this](qint64 uid, const QString &fileId) {
        for (SearchResult &f : m_friendsList) {
            if (f.uid == uid) {
                f.avatarFileId = fileId;
                break;
            }
        }
        AvatarCache::instance().setAvatarFileId(uid, fileId);
    });


    // 文件传输
    connect(&conn, &ConnectionManager::uploadFinished, this, &MainWindow::onFileUploadFinished);
    connect(&conn, &ConnectionManager::uploadFailed,   this, &MainWindow::onFileUploadFailed);
    connect(&conn, &ConnectionManager::uploadProgress, this, &MainWindow::onUploadProgress);
    connect(&conn, &ConnectionManager::downloadProgress, this, &MainWindow::onDownloadProgress);
    connect(&conn, &ConnectionManager::downloadFinished, this, &MainWindow::onDownloadFinished);
    connect(&conn, &ConnectionManager::downloadFailed,   this, &MainWindow::onDownloadFailed);


    // 启动后 自动弹出登录框
    QTimer::singleShot(100, this, [this]() {
        showLoginDialog();
    });

}

MainWindow::~MainWindow()
{
    // 先落闸：shutdown() 里的 abortAll() 会同步发 upload/download 失败信号，
    // 槽函数看到这个标记就直接返回，不会在窗口析构途中弹模态框。
    m_shuttingDown = true;
    ConnectionManager::shutdown();
    delete ui;
}

void MainWindow::onLoginSuccess()
{
    ui->loginBtn->setEnabled(false);
    ui->logoutBtn->setEnabled(true);
    m_userName = ConnectionManager::instance().currentUser(); // 保存当前登录用户名，重要！

    // 多个不同账号登录同一台设备，清空隐私
    m_currentTargetUid.clear();
    m_recentChats.clear();
    m_sessionMessages.clear();
    m_groupUnreadCounts.clear();
    m_messageModel->clear();
    refreshChatTitle();
    updateUnreadMessageBadge();
    m_friendsList.clear();
    m_contactModel->setContacts({});
    m_recentChatModel->setContacts({});
    if (m_contactTreeModel)
        m_contactTreeModel->clear();
    m_historyMsgLoadingSessions.clear();
    m_historyMsgHasMore.clear();

    // 重登录后 UI 是空的，内存缩略图也清掉；磁盘只留最近 64MB 的小图
    m_imageThumbCache.clear();
    m_imageThumbStoreCount = 0;
    pruneImageThumbCache(kImageThumbCacheMaxBytes);

    // 用户个人信息，默认头像
    ui->side_user_toolButton->setIcon(QIcon(defaultAvatar()));
    ui->side_user_toolButton->setIconSize(QSize(32, 32));
    refreshMyAvatarIcon();
}

void MainWindow::onConnectionLost()
{
    ui->loginBtn->setEnabled(true);
    ui->logoutBtn->setEnabled(false);
    updateSendButtonState();

    // 如果是登录后网络断了，可以继续看缓存旧数据, 客户端程序重启后不登录,是看不到数据的(涉及隐私)
    if (!m_friendsList.isEmpty()) {
        for (SearchResult &f : m_friendsList) {
            f.online = false;
            const QString uidStr = QString::number(f.uid);
            m_contactModel->updateOnlineStatus(uidStr, false);
            m_recentChatModel->updateOnlineStatus(uidStr, false);
        }
    }

    if (m_friendReqDialog)
            m_friendReqDialog->setRequests({});
    updatePendingRequestBadge();
}

// 显示登录界面
void MainWindow::showLoginDialog()
{
    if (!m_loginDialog) {
        m_loginDialog = new LoginDialog(this);
    }
    m_loginDialog->exec();
}

void MainWindow::on_loginBtn_clicked()
{
    showLoginDialog();
    updateSendButtonState();
}

void MainWindow::on_logoutBtn_clicked()
{
    ConnectionManager::instance().disconnectFromServer();
    updateSendButtonState();
}

// 消息按钮状态
void MainWindow::updateSendButtonState()
{
    const bool hasText = !editorEmojiPlainText().trimmed().isEmpty();
    ui->sendMsgBtn->setEnabled(hasText && ConnectionManager::instance().isLoggedIn());
}


void MainWindow::onFriendsFirstUpdate(const QList<SearchResult> &friends)
{
    m_friendsList = friends;
    rebuildFriendsList();
}

void MainWindow::onFriendsOnlineUpdate(qint64 uid, bool online)
{
    for (SearchResult &f : m_friendsList) {
        if (f.uid == uid) {
            f.online = online;
            m_contactModel->updateOnlineStatus(QString::number(uid), online);
            m_recentChatModel->updateOnlineStatus(QString::number(uid), online);
            return;
        }
    }
}

void MainWindow::onFriendAdded(qint64 uid, qint64 accountNo, const QString &nick, bool online, QString avatarFileId)
{
    ConnectionManager::instance().removePendingRequestsFromUid(uid);
    updatePendingRequestBadge();
    if (m_friendReqDialog && m_friendReqDialog->isVisible())
        m_friendReqDialog->setRequests(ConnectionManager::instance().pendingFriendRequests());

    for (const SearchResult &f : m_friendsList) {
        if (f.uid == uid)
            return;   // 重复推送不重复插入
    }
    SearchResult f;
    f.uid = uid;
    f.accountNo = accountNo;
    f.nick = nick;
    f.avatarFileId = avatarFileId;
    f.online = online;
    m_friendsList.append(f);

    ContactItem item;
    item.uid = QString::number(uid);
    item.name = nicknameForDisplay(item.uid);
    item.online = online;
    item.avatar = avatarForUid(uid);
    item.unreadCount = m_recentChats.value(uid).unreadCount;
    m_contactModel->upsertContact(item);
    AvatarCache::instance().setAvatarFileId(uid, avatarFileId);
}

void MainWindow::onFriendRemoveResult(bool ok, const QString &text, qint64 removeUid)
{
    if(!ok) {
        QMessageBox::warning(this, " ", text);
        return;
    }
    friendRemove(removeUid);
    QMessageBox::information(this, QStringLiteral(" "), text);
}

void MainWindow::friendRemove(qint64 uid)
{
    const QString uidStr = QString::number(uid);
    // 清会话缓存
    m_recentChats.remove(uid);
    m_sessionMessages.remove(uidStr);
    // 清UI
    if (m_currentTargetUid == uidStr) {
        m_currentTargetUid.clear();
        refreshChatTitle();
        m_messageModel->setMessages({});
    }

    // 从好友列表移除
    for (int i = 0; i < m_friendsList.size(); ++i) {
        if (m_friendsList[i].uid == uid) {
            m_friendsList.removeAt(i);
            break;
        }
    }
    m_recentChatModel->removeContact(uidStr);
    m_contactModel->removeContact(uidStr);
    updateUnreadMessageBadge();
}

void MainWindow::onFriendRemoved(qint64 uid)
{
    const QString nick = nicknameForDisplay(QString::number(uid));
    QMessageBox::information(this, QStringLiteral(" "), QStringLiteral("%1 已将你从好友中删除").arg(nick));
    friendRemove(uid);
}

void MainWindow::rebuildFriendsList()
{
    QList<ContactItem> contacts;
    contacts.reserve(m_friendsList.size());
    for (const SearchResult &f : m_friendsList) {
        ContactItem item;
        item.uid         = QString::number(f.uid);
        item.name        = nicknameForDisplay(item.uid);
        item.avatar      = avatarForUid(f.uid);
        item.online      = f.online;
        item.unreadCount = m_recentChats.value(f.uid).unreadCount;
        contacts.append(item);
        AvatarCache::instance().setAvatarFileId(f.uid, f.avatarFileId);
    }
    m_contactModel->setContacts(contacts);
}

// 发送消息时追加气泡
void MainWindow::on_sendMsgBtn_clicked()
{
    if (m_currentTargetUid.startsWith("group:"))       // 群聊未上线，直接屏蔽
        return;

    QString msgText = editorEmojiPlainText().trimmed();
    if (msgText.isEmpty() || m_currentTargetUid.isEmpty()) {
        return;
    }

    MessageItem item;
    item.clientMsgId  = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.sender     = QString::number(ConnectionManager::instance().currentUid());
    item.senderUid  = ConnectionManager::instance().currentUid();
    item.receiver   = m_currentTargetUid;
    item.body       = msgText;
    item.timestamp  = QDateTime::currentMSecsSinceEpoch();
    item.isSelf     = true;
    item.avatar     = avatarForUid(item.senderUid);
    item.showName   = item.receiver.startsWith("group:");  // 群聊显示用户名，私聊不显示用户名
    m_messageModel->appendMessage(item);
    ui->chatMsgListView->scrollToBottom();  // 滚动条拉到最底部
    m_sessionMessages[m_currentTargetUid].append(item);    // 缓存消息

    ConnectionManager::instance().sendChatMessage(m_currentTargetUid, msgText, item.clientMsgId);
    ui->msg->clear();

    bool ok = false;
    const qint64 peerUid = m_currentTargetUid.toLongLong(&ok);
    if(ok && peerUid > 0) {
        RecentChatItem &rc = m_recentChats[peerUid];
        rc.convId     = peerUid;
        rc.peerNick    = nicknameForDisplay(m_currentTargetUid);
        rc.lastMsgId   = QString();          // 还没有服务端 msgId
        rc.lastMsgTs   = item.timestamp;
        rc.lastMsgText = msgText.left(100);

        updateRecentChatRow(peerUid);
    }
}

void MainWindow::onClientMessage(const ChatMessage &msg)
{
    const QString senderUidStr   = QString::number(msg.senderUid);
    const QString receiverUidStr = QString::number(msg.convId);
    const QString selfUidStr     = QString::number(ConnectionManager::instance().currentUid());

    if(senderUidStr == selfUidStr)
        return;

    bool isGroup = receiverUidStr.startsWith("group:");  // 所有群聊都显示用户名，私聊不显示用户名
    QString sessionKey = isGroup ? receiverUidStr : senderUidStr; // 群聊时 会话键是群ID, 私聊时 会话键是发送者uid
    for(const MessageItem &m : m_sessionMessages.value(sessionKey)) {
        if(!m.msgId.isEmpty() && m.msgId == msg.msgId)
            return; // 去重
    }

    // 最近会话缓存 增量
    if(!isGroup) {
        RecentChatItem &r = m_recentChats[msg.senderUid];
        r.convId = msg.senderUid;
        r.peerNick = msg.fromNick;
        r.lastMsgId = msg.msgId;
        r.lastMsgTs = msg.ts;
        r.lastMsgText = msg.text.left(100);
        if(sessionKey == m_currentTargetUid) {
            ConnectionManager::instance().markRecentChatRead(msg.senderUid);
        } else {
            r.unreadCount += 1;  // 私聊未读消息 +1
            // 可扩展：提示音等
        }
    }

    // 气泡缓存
    MessageItem item;
    item.msgId    = msg.msgId;
    item.clientMsgId = msg.clientMsgId;
    item.sender   = msg.fromNick;
    item.senderUid = msg.senderUid;
    item.receiver = receiverUidStr;
    item.msgType  = msg.msgType;
    item.fileId   = msg.fileId;
    item.fileName = msg.fileName;
    item.fileSize = msg.fileSize;
    item.body     = msg.text.isEmpty() ? attachmentPreview(msg.msgType, msg.fileName) : msg.text;
    item.timestamp = msg.ts;
    item.isSelf   = false;
    item.avatar   = avatarForUid(msg.senderUid);
    item.showName = isGroup;
    if (item.msgType == Proto::MsgTypeImage)
        item.thumb = cachedImageThumb(item.fileId);   // 以前下载/上传过的图，重登录后直接恢复

    m_sessionMessages[sessionKey].append(item); // 存到缓存， 权威
    if(sessionKey == m_currentTargetUid) {
        m_messageModel->appendMessage(item); // 发信息人是当前聊天对象，直接追加显示
        ui->chatMsgListView->scrollToBottom();
    }
    if(isGroup && sessionKey != m_currentTargetUid) {
        m_groupUnreadCounts[sessionKey]++;   // 群聊未读消息 +1
        // 可扩展：提示音等
    }

    if(!isGroup)
        updateRecentChatRow(msg.senderUid);
    updateUnreadMessageBadge();
}

void MainWindow::onChatMessageAcked(const ChatAck &ack)
{
    if (!ack.ok || ack.msgId.isEmpty() || ack.clientMsgId.isEmpty())
        return;
    // 群聊还没上线，先按私聊处理；将来群聊 ack 的 to_uid 会是 "group:xxx",
    const QString sessionKey = QString::number(ack.convId);

    auto it = m_sessionMessages.find(sessionKey);
    if(it != m_sessionMessages.end()) {
        for(MessageItem &m : it.value()) {
            if(m.clientMsgId == ack.clientMsgId && m.msgId.isEmpty()) {
                m.msgId = ack.msgId;
                break;
            }
        }
    }

    if(sessionKey == m_currentTargetUid)
        m_messageModel->updateMsgIdByClientId(ack.clientMsgId, ack.msgId);

    auto rc = m_recentChats.find(ack.convId);
    if(rc != m_recentChats.end())
        rc->lastMsgId = ack.msgId;
}

void MainWindow::onChatScroll(int value)
{
    if(value != 0)
        return;
    if (m_currentTargetUid.isEmpty() || m_currentTargetUid.startsWith("group:"))
        return; // 群组未实现
    if (m_historyMsgLoadingSessions.contains(m_currentTargetUid))
        return;
    auto it = m_historyMsgHasMore.find(m_currentTargetUid);
    if(it != m_historyMsgHasMore.end() && !it.value())
        return; // 没有更早的消息,不拉取历史消息

    const QList<MessageItem> msgs = m_sessionMessages.value(m_currentTargetUid);
    if(msgs.isEmpty())
        return;
    const QString beforeId = msgs.first().msgId;
    if(beforeId.isEmpty())
        return;
    m_historyMsgLoadingSessions.insert(m_currentTargetUid);
    ConnectionManager::instance().requestHistoryMsg(m_currentTargetUid, beforeId, Proto::kHistoryPageSize);
}

void MainWindow::onHistoryMsgLoaded(const HistoryPage &page)
{
    const QString sessionKey = QString::number(page.convId);
    m_historyMsgLoadingSessions.remove(sessionKey);

    if(!page.ok)
        return;
    const bool firstLoad = !m_historyMsgHasMore.contains(sessionKey);
    m_historyMsgHasMore[sessionKey] = page.hasMore;

    if (page.messages.isEmpty())
        return;

    // 已存在的消息 id 集合，用于去重
    QSet<QString> existing;
    for (const MessageItem &m : m_sessionMessages.value(sessionKey))
        if (!m.msgId.isEmpty())
            existing.insert(m.msgId);

    const qint64 myUid = ConnectionManager::instance().currentUid();

    QList<MessageItem> newOnes;
    newOnes.reserve(page.messages.size());
    for (const ChatMessage &m : page.messages) {
        if (!m.msgId.isEmpty() && existing.contains(m.msgId))
            continue;
        MessageItem item;
        item.msgId       = m.msgId;
        item.clientMsgId = m.clientMsgId;
        item.sender      = nicknameForDisplay(QString::number(m.senderUid));
        item.senderUid   = m.senderUid;
        item.receiver    = QString::number(m.convId);
        item.msgType     = m.msgType;
        item.fileId      = m.fileId;
        item.fileName    = m.fileName;
        item.fileSize    = m.fileSize;
        item.body        = m.text.isEmpty() ? attachmentPreview(m.msgType, m.fileName) : m.text;
        item.timestamp   = m.ts;
        item.isSelf      = (m.senderUid == myUid);
        item.showName    = false;    // 私聊
        item.avatar      = item.isSelf ? avatarForUid(myUid) : avatarForUid(m.senderUid);
        if (item.msgType == Proto::MsgTypeImage)
            item.thumb = cachedImageThumb(item.fileId);   // 历史图片：有本地小图就恢复，否则显示占位+可点击下载
        newOnes.append(item);
        if (!m.msgId.isEmpty())
            existing.insert(m.msgId);
    }

    if (newOnes.isEmpty())
        return;

    // 写入缓存头部（权威层）
    QList<MessageItem> &cache = m_sessionMessages[sessionKey];
    for (int i = newOnes.size() - 1; i >= 0; --i)
        cache.prepend(newOnes[i]);

    // 若是当前会话，同步渲染视图
    if (sessionKey == m_currentTargetUid ) {
        m_messageModel->prependMessages(newOnes);
        if(firstLoad)
            ui->chatMsgListView->scrollToBottom();
        else { // 延迟一帧：让 QListView 先处理 rowsInserted、重算 layout, 否则 scrollTo 的滚动会被随后的 layout 更新覆盖
            const int anchorRow = newOnes.size();
            QTimer::singleShot(0, this, [this, anchorRow]() {
                const QModelIndex index = m_messageModel->index(anchorRow);
                if(index.isValid())
                    ui->chatMsgListView->scrollTo(index, QAbstractItemView::PositionAtTop);
            });
        }
    }
}

void MainWindow::onRecentChatReceived(const RecentChatResult &msg)
{
    if(!msg.ok) {
        if (!msg.text.isEmpty())
            QMessageBox::warning(this, QStringLiteral(" "), msg.text);
        else
            QMessageBox::warning(this, QStringLiteral(" "), QStringLiteral("最近会话消息,加载失败"));
        return;
    }
    m_recentChats.clear();
    m_recentChats.reserve(msg.items.size());
    for(const RecentChatItem &item : msg.items)
        m_recentChats.insert(item.convId, item);

    loadRecentChatList();
    updateUnreadMessageBadge();
    rebuildFriendsList(); // update unreadCount
}

void MainWindow::updateRecentChatRow(qint64 peerUid)
{
    const QString peerUidStr = QString::number(peerUid);
    ContactItem item;
    item.uid = peerUidStr;
    item.name = nicknameForDisplay(peerUidStr);
    item.unreadCount = m_recentChats.value(peerUid).unreadCount;
    item.sortTime = m_recentChats.value(peerUid).lastMsgTs;
    item.avatar = avatarForUid(peerUid);
    for(const SearchResult &f : m_friendsList) {
        if(f.uid == peerUid) {
            item.online = f.online;
            break;
        }
    }
    m_recentChatModel->upsertContact(item);
    m_contactModel->setUnreadCount(peerUidStr, m_recentChats.value(peerUid).unreadCount);
}

// 消息对话框 ，鼠标右键 复制消息
void MainWindow::onMessageListContextMenu(const QPoint &pos)
{
    // 获取右键位置对应的消息索引
    QModelIndex index = ui->chatMsgListView->indexAt(pos);
    if(!index.isValid())
        return;
    // 获取消息文本
    QString msgText = index.data(MessageModel::BodyRole).toString();
    if(msgText.isEmpty())
        return;
    // 创建右键菜单
    QMenu menu(this);
    menu.setStyleSheet(
        "QMenu::item { padding: 8px 8px; }"
        "QMenu::item:selected { background: #308CC6; color: #F7F7F7; }"  // 点击高亮
    );
    QAction *copyAction = menu.addAction("复制");
    // 执行菜单，获取用户选择
    QAction *selectedAction = menu.exec(ui->chatMsgListView->viewport()->mapToGlobal(pos));
    if(selectedAction == copyAction) {
        QApplication::clipboard()->setText(msgText);
    }
}

// 输入区 工具栏 按钮 1秒
void MainWindow::autoUncheckToolButton(QToolButton *btn)
{
    QTimer::singleShot(1000, btn, [btn]() { btn->setChecked(false); });
}

void MainWindow::emoji(bool checked)
{
    Q_UNUSED(checked);
    autoUncheckToolButton(ui->emojiBtn);
    if (!m_emojiPanel) {
        m_emojiPanel = new EmojiPanel(this);
        connect(m_emojiPanel, &EmojiPanel::emojiSelected, this, [this](const QString &code) {
            insertEmojiToUiInput(code);
        });
    }
    m_emojiPanel->move(ui->emojiBtn->mapToGlobal(QPoint(0, -260)));
    m_emojiPanel->show();
}

// 聊天对象， 保存会话消息， 切换会话消息
void MainWindow::switchToChatTarget(const QString &targetUid)
{
    if(targetUid.isEmpty() || targetUid == m_currentTargetUid)
        return;

    m_currentTargetUid = targetUid;
    refreshChatTitle();
    const QList<MessageItem> messages = m_sessionMessages.value(targetUid);
    m_messageModel->setMessages(messages); // 缓存 → Model 单向派生
    ui->chatMsgListView->scrollToBottom();

    // 数据源：未读清零
    bool ok = false;
    const qint64 peerUid = targetUid.toLongLong(&ok);
    if(ok && peerUid > 0) {
        auto it = m_recentChats.find(peerUid);
        if(it != m_recentChats.end() && it->unreadCount > 0) {
            it->unreadCount = 0;
            ConnectionManager::instance().markRecentChatRead(peerUid);
        }
    }
    m_recentChatModel->setUnreadCount(targetUid, 0);
    m_contactModel->setUnreadCount(targetUid, 0);
    updateUnreadMessageBadge();

    if(targetUid.startsWith("group:")) {
//        m_groupUnreadCounts.remove(targetUid); // 群聊：本地清零 ， 上线后恢复
        return; // 群聊未实现
    }

    if(!m_historyMsgHasMore.contains(targetUid) && !m_historyMsgLoadingSessions.contains(targetUid)) {
        m_historyMsgLoadingSessions.insert(targetUid);
        ConnectionManager::instance().requestHistoryMsg(targetUid, QString(), Proto::kHistoryPageSize);
    }
}

QString MainWindow::nicknameForDisplay(const QString &uid) const
{
    QString nick = ConnectionManager::instance().nicknameOfUid(uid);
    if (!nick.isEmpty())
        return nick;

    // 断线后 nickMap 被清，从本地好友列表兜底
    bool ok = false;
    const qint64 id = uid.toLongLong(&ok);
    if (!ok)
        return {};
    for (const SearchResult &f : m_friendsList) {
        if (f.uid == id)
            return f.nick;
    }
    return {};
}

// 选择 当前对话对象
void MainWindow::onContactListViewClicked(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    QString target = index.data(ContactModel::IdRole).toString();
    if(target.isEmpty())
        return;
    switchToChatTarget(target);
}


void MainWindow::onContactTreeViewClicked(const QModelIndex &index)
{
    if (!index.isValid())
        return;
    QString target = index.data(Qt::UserRole).toString();
    if(target.isEmpty())
        return; // 分组行
    switchToChatTarget(target);
}

// 导航栏 未读会话消息提示
void MainWindow::updateUnreadMessageBadge()
{
    int total = 0;
    for(const RecentChatItem &item : m_recentChats)
        total += item.unreadCount;
    for(int n : m_groupUnreadCounts)
        total += n;
    if(total <= 0) {
        m_chatBadge->hide();
        return;
    }
    QString text = total > 99 ? "99+" : QString::number(total);
    m_chatBadge->setText(text);

    int badgeHigh = 20; // 20x20
    QFontMetrics fm(m_chatBadge->font());
    const int textWidth = fm.horizontalAdvance(text);
    const int badgeWidth = qMax(20, textWidth + 8);
    m_chatBadge->setFixedSize(badgeWidth, badgeHigh);

    // 贴到按钮右上角
    m_chatBadge->move(ui->side_chat_toolButton->width() - badgeWidth, 0);
    m_chatBadge->show();
}

// 导航栏 待处理数量提示
void MainWindow::updatePendingRequestBadge()
{
    const int n = ConnectionManager::instance().pendingFriendRequests().size();
    if (n <= 0) {
        m_contactBadge->hide();
        return;
    }
    const QString text = n > 99 ? QStringLiteral("99+") : QString::number(n);
    m_contactBadge->setText(text);

    QFontMetrics fm(m_contactBadge->font());
    const int badgeWidth = qMax(20, fm.horizontalAdvance(text) + 8);
    m_contactBadge->setFixedSize(badgeWidth, 20);
    m_contactBadge->move(ui->friendRequestBtn->width() - badgeWidth, 0);
    m_contactBadge->show();
}

void MainWindow::loadRecentChatList()
{
    QList<RecentChatItem> sorted = m_recentChats.values();
    std::sort(sorted.begin(), sorted.end(), [](const RecentChatItem &a, const RecentChatItem &b)
                                                { return a.lastMsgTs > b.lastMsgTs; });
    QList<ContactItem> items;
    items.reserve(sorted.size());
    for (const RecentChatItem &r : sorted) {
        ContactItem item;
        item.uid         = QString::number(r.convId);
        item.name        = nicknameForDisplay(item.uid);
        item.unreadCount = r.unreadCount;
        item.avatar      = avatarForUid(r.convId);
        item.sortTime    = r.lastMsgTs;

        bool online = false;
        for (const SearchResult &f : m_friendsList) {
            if (f.uid == r.convId) {
                online = f.online;
                break;
            }
        }
        item.online = online;
        items.append(item);
    }
    m_recentChatModel->setContacts(items);
}

// 更新后的头像，没有就用默认头像
void MainWindow::refreshMyAvatarIcon()
{
    const qint64 uid = ConnectionManager::instance().currentUid();
    const QPixmap pixmap = (uid > 0) ? AvatarCache::instance().pixmapForUid(uid) : QPixmap();

    const QPixmap shown = pixmap.isNull() ? QPixmap(defaultAvatar())
                          : displayScaledPixmap(pixmap, 32, devicePixelRatioF());
    ui->side_user_toolButton->setIcon(QIcon(shown));
    ui->side_user_toolButton->setIconSize(QSize(32, 32));
}

// 从缓存取某个人的头像；没有就用默认图
QPixmap MainWindow::avatarForUid(qint64 uid) const
{
    const QPixmap pm = AvatarCache::instance().pixmapForUid(uid);
    return pm.isNull() ? defaultAvatar() : pm;
}

// 头像缓存就绪
void MainWindow::onAvatarLoaded(qint64 uid, const QString &fileId)
{
    Q_UNUSED(fileId);

    if (uid == ConnectionManager::instance().currentUid()) {
        refreshMyAvatarIcon();
    }

    const QPixmap pm = AvatarCache::instance().pixmapForUid(uid);
    if (pm.isNull())
        return;

    const QString uidStr = QString::number(uid);
    m_contactModel->updateAvatar(uidStr, pm);
    m_recentChatModel->updateAvatar(uidStr, pm);

    m_messageModel->refreshAvatar(uid, pm); // 正在显示的对话消息, 更新头像
    // 权威缓存也要更新头像，否则切走会话再切回来又变回旧头像
    for (auto it = m_sessionMessages.begin(); it != m_sessionMessages.end(); ++it) {
        for (MessageItem &m : it.value()) {
            if (m.senderUid == uid)
                m.avatar = pm;
        }
    }
}

// 群组暂时不实现, 成员不展示在线状态, 可以额外显示群人数
void MainWindow::rebuildContactTreeView(const QList<UserEntry> &users, const QVariantMap &nickMap)
{
    Q_UNUSED(nickMap);
    if(!m_contactTreeModel)
        return;
    // 清空模型中所有的旧数据（旧的好友列表、群组等), 每次刷新列表时不会和旧数据重复叠加
    m_contactTreeModel->clear();

    // 头像准备
    QPixmap defaultAvatar(":/image/user/user-10.png");
    QIcon avatar(defaultAvatar);

    // 公共群 public
    int onlineCount = 0;
    for(const auto &u : users) {
        if(u.online)
            onlineCount++;
    }
    QStandardItem *publicGroup = new QStandardItem(QString("公共群 (在线%1/%2)")
                                            .arg(onlineCount).arg(users.size()));
    publicGroup->setData("group:public",Qt::UserRole); // 群public 的ID
    publicGroup->setSelectable(false);
    publicGroup->setEditable(false);
    m_contactTreeModel->appendRow(publicGroup);

    for(const auto &u : users) {
        const QString displayName = u.nick.isEmpty() ? u.name : u.nick;
        QStandardItem *item = new QStandardItem(displayName);
        item->setIcon(avatar);
        item->setData(QString::number(u.uid), Qt::UserRole);    // 用户名隐藏存储在该节点内部
        item->setEditable(false);
        publicGroup->appendRow(item);
    }
}

void MainWindow::nicknameChanged(const QString &uid, const QString &nick)
{
    m_contactModel->updateNickname(uid, nick);
    m_recentChatModel->updateNickname(uid, nick);

    bool ok = false;
    const qint64 id = uid.toLongLong(&ok);
    if (ok && id > 0) {
        for(SearchResult &f : m_friendsList) {
            if(f.uid == id) {
                f.nick = nick;
                break;
            }
        }
        auto it = m_recentChats.find(id);
        if(it != m_recentChats.end())
            it->peerNick = nick;
    }
}

// 添加好友群组
void MainWindow::on_addFriendBtn_clicked()
{
    if (!ConnectionManager::instance().isLoggedIn()) {
        QMessageBox::information(this, " ", "未登录");
        return;
    }

    bool ok = false;
    qint64 accountNo = ui->searchLineEdit->text().toLongLong(&ok);
    if(!ok || !Proto::isValidAccountNo(accountNo)) {
        QMessageBox::information(this, " ", "请输入正确的 8 位账号");
        return;
    }
    if (accountNo == ConnectionManager::instance().currentAccountNo()) {
        QMessageBox::information(this, " ", "不能添加自己的账号");
        return;
    }
    for (const SearchResult &f : m_friendsList) {
        if (f.accountNo == accountNo) {
            QMessageBox::information(this, " ", QStringLiteral("你们已经是好友"));
            return;
        }
    }

    ConnectionManager::instance().searchUser(accountNo);
}

void MainWindow::onUserSearchResult(qint64 accountNo, const QString &nick, bool ok, const QString &text)
{
    if (!ok) {
        QMessageBox::information(this, " ", text);
        return;
    }

    QString reqText;
    for(;;) {
        bool inputOk = false;
        reqText = QInputDialog::getText(this, "添加好友", QString("账号: %1\n昵称: %2\n填写验证信息")
                                             .arg(accountNo).arg(nick), QLineEdit::Normal, reqText, &inputOk);
        if(!inputOk)
            return;
        reqText = reqText.trimmed();
        if(reqText.size() <= Proto::kMaxFriendReqTextLen)
            break;
        QMessageBox::information(this, " ", QStringLiteral("请修改 验证信息\n不能超过 %1个字符")
                                                .arg(Proto::kMaxFriendReqTextLen));
    }

    ConnectionManager::instance().sendFriendRequest(accountNo, reqText);
}

void MainWindow::onFriendRequestResult(bool ok, const QString &text)
{
    Q_UNUSED(ok);
    QMessageBox::information(this, " ", text);
}

void MainWindow::on_friendRequestBtn_clicked()
{
    if (!m_friendReqDialog)
        m_friendReqDialog = new FriendRequestDialog(this);
    m_friendReqDialog->setRequests(ConnectionManager::instance().pendingFriendRequests());
    m_friendReqDialog->show();
    m_friendReqDialog->raise();
    m_friendReqDialog->activateWindow();
}

void MainWindow::onMoreMenuAboutToShow()
{
    m_moreMenu->clear();
    if (m_currentTargetUid.isEmpty()) {
        m_moreMenu->addAction(QStringLiteral("未选择聊天对象"));
        return;
    }
    if (m_currentTargetUid.startsWith("group:"))
        m_moreMenu->addAction(QStringLiteral("退出群组"), this, &MainWindow::onLeaveGroupBtn);
    else
        m_moreMenu->addAction(QStringLiteral("删除好友"), this, &MainWindow::onRemoveFriendBtn);
}

void MainWindow::onRemoveFriendBtn()
{
    if (m_currentTargetUid.isEmpty() || m_currentTargetUid.startsWith("group:"))
        return;
    bool ok = false;
    const qint64 removeUid = m_currentTargetUid.toLongLong(&ok);
    if(!ok || removeUid <= 0)
        return;
    const QString nick = nicknameForDisplay(m_currentTargetUid);
    const auto ret = QMessageBox::question(this, QStringLiteral("删除好友"),QStringLiteral("确定要删除好友 %1 吗？")
                            .arg(nick), QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes)
        return;
    ConnectionManager::instance().removeFriend(removeUid);
}

void MainWindow::onLeaveGroupBtn()
{
    if (m_currentTargetUid.isEmpty() || !m_currentTargetUid.startsWith("group:"))
        return;
}

void MainWindow::insertEmojiToUiInput(const QString &code)
{
    const int emojiSize = qMax(Proto::kEmojiUiInputSize, ui->msg->fontMetrics().height());
    QTextCursor cur = ui->msg->textCursor();
    if (EmojiText::insertTo(cur, ui->msg->document(), code, emojiSize)) {
        cur.insertText(QStringLiteral(" "));
    } else {
        cur.insertText(QStringLiteral("[表情]"));
    }
    ui->msg->setTextCursor(cur);
    ui->msg->setFocus();
}

// 遍历 QTextDocument，图片片段输出为 [code]，其余输出原始文本
QString MainWindow::editorEmojiPlainText() const
{
    const QTextDocument *doc = ui->msg->document();
    QString out;
    for (QTextBlock b = doc->firstBlock(); b.isValid(); b = b.next()) {
        if (!out.isEmpty())
            out += QLatin1Char('\n');
        for (QTextBlock::iterator it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment f = it.fragment();
            if (!f.isValid())
                continue;
            if (f.charFormat().isImageFormat()) {
                QString code = f.charFormat().toImageFormat().name();
                if(code.isEmpty())
                    continue;
                // 去掉在对话框复制表情的自定义 scheme 前缀(emoji:)，两条插入路径（点表情包面板 + 粘贴表情）都兼容
                const QString kPrefix = QStringLiteral("emoji:");
                if (code.startsWith(kPrefix))
                    code = code.mid(kPrefix.size());
                out += Proto::kEmojiBegin;
                out += code;
                out += Proto::kEmojiEnd;
            }
            else
                out += f.text();
        }
    }
    return out;
}







/****************** 文件传输 **********************/

// 只缓存缩略图，不复制原图：既能让重登录后的历史图片有图，又不会给本地制造整份附件垃圾
QPixmap MainWindow::cachedImageThumb(const QString &fileId)
{
    if (fileId.isEmpty())
        return QPixmap();

    auto it = m_imageThumbCache.constFind(fileId);
    if (it != m_imageThumbCache.constEnd())
        return it.value();     // 内存命中

    const QString path = imageThumbCachePath(fileId);
    if (path.isEmpty() || !QFileInfo::exists(path))
        return QPixmap();

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QPixmap pm = QPixmap::fromImage(reader.read());
    if (pm.isNull()) {
        QFile::remove(path);   // 缓存坏了，删掉重下
        return QPixmap();
    }

    m_imageThumbCache.insert(fileId, pm);
    return pm;
}

void MainWindow::storeImageThumb(const QString &fileId, const QPixmap &thumb)
{
    if (fileId.isEmpty() || thumb.isNull())
        return;

    m_imageThumbCache.insert(fileId, thumb);

    const QString path = imageThumbCachePath(fileId);
    if (path.isEmpty())
        return;
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        qWarning() << "[图片缓存] 无法创建缓存目录" << QFileInfo(path).absolutePath();
        return;
    }
    if (!thumb.save(path, "PNG"))
        qWarning() << "[图片缓存] 保存缩略图失败" << path;

    // 不每张都扫目录；每 32 张清理一次，长会话也不会无限堆
    if (++m_imageThumbStoreCount >= 32) {
        m_imageThumbStoreCount = 0;
        pruneImageThumbCache(kImageThumbCacheMaxBytes);
    }
}

// 登录时按"最久未修改"清理一次；上限 64MB，只清缩略图，不动用户自己另存的原图
void MainWindow::pruneImageThumbCache(qint64 maxBytes)
{
    QDir dir(imageThumbCacheDir());
    if (!dir.exists())
        return;

    QFileInfoList files = dir.entryInfoList({QStringLiteral("*.png")}, QDir::Files, QDir::NoSort);
    qint64 total = 0;
    for (const QFileInfo &f : files)
        total += f.size();
    if (total <= maxBytes)
        return;

    std::sort(files.begin(), files.end(), [](const QFileInfo &a, const QFileInfo &b) {
        return a.lastModified() < b.lastModified();   // 最老的先删
    });

    for (const QFileInfo &f : files) {
        if (total <= maxBytes)
            break;
        const qint64 size = f.size();
        if (QFile::remove(f.absoluteFilePath())) {
            total -= size;
            m_imageThumbCache.remove(f.completeBaseName());
        }
    }
}

// 发送附件：选文件 → 校验 → 上传 → 上传完成后发附件消息
void MainWindow::on_fileBtn_clicked()
{
    autoUncheckToolButton(ui->fileBtn);

    auto &conn = ConnectionManager::instance();
    if (!conn.isLoggedIn()) {
        QMessageBox::information(this, QStringLiteral(" "), QStringLiteral("请先登录"));
        return;
    }
    if (m_currentTargetUid.isEmpty() || m_currentTargetUid.startsWith("group:")) {
        QMessageBox::information(this, QStringLiteral(" "), QStringLiteral("请先选择一个好友会话"));
        return;
    }
    if (m_uploadTaskId != 0) {
        QMessageBox::information(this, QStringLiteral(" "), QStringLiteral("正在发送上一个文件，请稍候"));
        return;
    }

    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要发送的文件"));
    if (path.isEmpty())
        return;

    const QFileInfo info(path);
    if (!info.isFile()) {
        QMessageBox::warning(this, QStringLiteral(" "), QStringLiteral("无法读取这个文件"));
        return;
    }
    if (info.size() <= 0) {
        QMessageBox::warning(this, QStringLiteral(" "), QStringLiteral("这个文件是空的，请选择其他文件"));
        return;
    }
    if (info.size() > Proto::kMaxFileSize) {
        QMessageBox::warning(this, QStringLiteral(" "), QStringLiteral("文件超过 2 GB，暂不支持发送"));
        return;
    }
    const QString fileName = info.fileName();
    if (fileName.size() > Proto::kMaxFileNameLen) {
        QMessageBox::warning(this, QStringLiteral(" "),
                             QStringLiteral("文件名过长（最多 %1 个字符）").arg(Proto::kMaxFileNameLen));
        return;
    }

    const bool    isImage = isImageSuffix(info.suffix());
    const quint8  biz     = isImage ? Proto::BizChatImage : Proto::BizChatFile;
    const quint8  msgType = isImage ? Proto::MsgTypeImage : Proto::MsgTypeFile;
    const QString mime    = QMimeDatabase().mimeTypeForFile(info).name();

    // 先把"发给谁"记下来：上传期间用户可能切到别的会话
    m_uploadPeerUid   = m_currentTargetUid;
    m_uploadLocalPath = path;
    m_uploadFileName  = fileName;
    m_uploadMime      = mime;
    m_uploadFileSize  = info.size();
    m_uploadMsgType   = msgType;
    m_uploadTaskId    = conn.startFileUpload(path, fileName, mime, biz);

    if (m_uploadTaskId == 0) {
        resetUploadState();
        QMessageBox::warning(this, QStringLiteral(" "), QStringLiteral("文件发送失败，请重试"));
        return;
    }

    refreshChatTitle();     // 会话框标题 名字后面挂上"正在发送 xxx"
}

void MainWindow::onFileUploadFinished(quint32 taskId, const QString &fileId, const FileMeta &meta)
{
    // 析构 MainWindow 时会走 ConnectionManager::shutdown() -> abortAll()，
    // 若此时还有上传任务，失败信号会同步回调到这里。窗口正在销毁，绝不能再弹模态框。
    if (m_shuttingDown || QCoreApplication::closingDown())
        return;

    if (taskId == 0 || taskId != m_uploadTaskId)
        return;      // 不是本窗口发起的（比如头像更新）

    // 先把状态取下来：后面弹窗、切会话都可能重入
    const QString peerUid  = m_uploadPeerUid;
    const QString fileName = meta.fileName.isEmpty() ? m_uploadFileName : meta.fileName;
    const qint64  fileSize = meta.size > 0 ? meta.size : m_uploadFileSize;
    const quint8  msgType  = m_uploadMsgType;
    const QString localPath = m_uploadLocalPath;
    const QString clientMsgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64  selfUid  = ConnectionManager::instance().currentUid();

    bool okPeer = false;
    const qint64 peerUidValue = peerUid.toLongLong(&okPeer);

    ChatMessage out;
    out.clientMsgId = clientMsgId;
    out.convId      = okPeer ? peerUidValue : 0;
    out.senderUid   = selfUid;
    out.msgType     = msgType;
    out.fileId      = fileId;
    out.fileName    = fileName;
    out.fileSize    = fileSize;

    if (!ConnectionManager::instance().sendAttachmentMessage(out)) {
        resetUploadState();
        QMessageBox::warning(this, QStringLiteral(" "), QStringLiteral("附件消息发送失败，请重试"));
        return;  // fileId 保留在服务端，1d 之后可以做"只重发消息"
    }

    // 本地插一条真正的气泡
    MessageItem item;
    item.clientMsgId = clientMsgId;
    item.senderUid   = selfUid;
    item.sender      = QString::number(selfUid);
    item.receiver    = peerUid;
    item.msgType     = msgType;
    item.fileId      = fileId;
    item.fileName    = fileName;
    item.fileSize    = fileSize;
    item.body        = attachmentPreview(msgType, fileName);
    if (msgType == Proto::MsgTypeImage && !localPath.isEmpty()) {
        // 按设备像素生成：1x 屏是 220×180，2x 屏是 440×360，delegate 再缩到实际气泡大小
        const qreal dpr = devicePixelRatioF();
        item.thumb = makeImageThumbnail(localPath, qRound(MessageBubble::kImageMaxW * dpr),
                                        qRound(MessageBubble::kImageMaxH * dpr));
        if (!item.thumb.isNull())
            storeImageThumb(fileId, item.thumb);   // 自己发的图也缓存小图，重登录后能恢复
    }
    item.timestamp   = QDateTime::currentMSecsSinceEpoch();
    item.isSelf      = true;
    item.showName    = false;
    item.avatar      = avatarForUid(selfUid);

    m_sessionMessages[peerUid].append(item);          // 权威缓存
    if (peerUid == m_currentTargetUid) {
        m_messageModel->appendMessage(item);
        ui->chatMsgListView->scrollToBottom();
    }

    // 最近会话预览
    if (okPeer && peerUidValue > 0) {
        RecentChatItem &rc = m_recentChats[peerUidValue];
        rc.convId      = peerUidValue;
        rc.peerNick    = nicknameForDisplay(peerUid);
        rc.lastMsgId   = QString();      // 等服务端 ack 回填
        rc.lastMsgTs   = item.timestamp;
        rc.lastMsgText = item.body.left(100);
        updateRecentChatRow(peerUidValue);
    }

    resetUploadState();
}

void MainWindow::onFileUploadFailed(quint32 taskId, int errcode, const QString &text)
{
    // 同 onFileUploadFinished：关闭窗口时的 abort 不能弹模态框
    if (m_shuttingDown || QCoreApplication::closingDown())
        return;

    if (taskId == 0 || taskId != m_uploadTaskId)
        return;

    const QString fileName = m_uploadFileName;
    resetUploadState();
    QMessageBox::warning(this, QStringLiteral(" "),
                         text.isEmpty() ? QStringLiteral("%1 发送失败，请重试").arg(fileName) : text);
    Q_UNUSED(errcode);
}

// 只让气泡区域响应附件点击：QListView::clicked 是整个 item/整行，气泡左右空白也会触发。
// 这里在 viewport 上自己判断：按下和释放都必须落在同一行的气泡矩形里，才算一次点击。
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != ui->chatMsgListView->viewport() || !m_messageDelegate)
        return QMainWindow::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        m_pressedBubbleIndex = QPersistentModelIndex();
        if (mouse->button() == Qt::LeftButton) {
            const QModelIndex index = ui->chatMsgListView->indexAt(mouse->pos());
            if (index.isValid()) {
                const QRect bubble = m_messageDelegate->bubbleRect(
                    ui->chatMsgListView->visualRect(index), index);
                if (bubble.contains(mouse->pos()))
                    m_pressedBubbleIndex = QPersistentModelIndex(index);
            }
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            const QPersistentModelIndex pressed = m_pressedBubbleIndex;
            m_pressedBubbleIndex = QPersistentModelIndex();

            if (pressed.isValid()) {
                const QModelIndex index = ui->chatMsgListView->indexAt(mouse->pos());
                if (index.isValid() && pressed == index) {
                    const QRect bubble = m_messageDelegate->bubbleRect(
                        ui->chatMsgListView->visualRect(index), index);
                    if (bubble.contains(mouse->pos()))
                        onDownloadAttachmentClicked(index);
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// 点击图片/文件气泡：每次弹另存为，用户确认后才发起下载
void MainWindow::onDownloadAttachmentClicked(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    const quint8 msgType = quint8(index.data(MessageModel::MsgTypeRole).toInt());
    if (msgType != Proto::MsgTypeImage && msgType != Proto::MsgTypeFile)
        return;     // 普通聊天文字/语音/聊天视频消息 暂不触发下载,

    const QString fileId = index.data(MessageModel::FileIdRole).toString();
    if (fileId.isEmpty())
        return;

    auto &conn = ConnectionManager::instance();
    if (!conn.isLoggedIn()) {
        QMessageBox::information(this, QStringLiteral(" "), QStringLiteral("请先登录"));
        return;
    }
    if (m_currentTargetUid.isEmpty() || m_currentTargetUid.startsWith("group:"))
        return;

    // 产品策略：用户发起的下载，同一时刻只允许 1 路。（服务端每会话 2 个槽: 1 传 1 下）
    if (!m_downloads.isEmpty()) {
        const DownloadUi &running = *m_downloads.constBegin();
        QMessageBox::information(this, QStringLiteral(" "), QStringLiteral("正在下载 %1，请稍等").arg(running.fileName));
        return;
    }

    QString fileName = QFileInfo(index.data(MessageModel::FileNameRole).toString()).fileName();
    if (fileName.isEmpty())
        fileName = QStringLiteral("下载文件");

    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();
    const QString savePath = QFileDialog::getSaveFileName(
                    this, QStringLiteral("保存文件"), QDir(dir).filePath(fileName));
    if (savePath.isEmpty())
        return;     // 用户取消

    const quint32 taskId = conn.downloadFile(fileId, savePath, TransferPriority::User);
    if (taskId == 0) {
        QMessageBox::warning(this, QStringLiteral(" "), QStringLiteral("下载失败，请重试"));
        return;
    }

    DownloadUi dl;
    dl.peerUid    = m_currentTargetUid;
    dl.fileId     = fileId;
    dl.fileName   = fileName;
    dl.savePath   = savePath;
    dl.msgType    = msgType;
    dl.lastPercent = 0;
    m_downloads.insert(taskId, dl);

    refreshChatTitle();
}

void MainWindow::onUploadProgress(quint32 taskId, qint64 sentBytes, qint64 totalBytes, qint64 elapsedMs)
{
    if (m_shuttingDown)
        return;
    if (m_uploadTaskId == 0 || taskId != m_uploadTaskId)
        return; // 丢弃头像上传进度

    const int percent = totalBytes > 0 ? qBound(0, int((sentBytes * 100) / totalBytes), 100) : 0;
    m_uploadElapsedMs = elapsedMs;
    if (percent == m_uploadPercent)
        return; // 同一百分比不重复刷标题

    m_uploadPercent = percent;
    refreshChatTitle();
}

void MainWindow::onDownloadProgress(quint32 taskId, qint64 receivedBytes, qint64 totalBytes, qint64 elapsedMs)
{
    if (m_shuttingDown)
        return;

    auto it = m_downloads.find(taskId);
    if (it == m_downloads.end())
        return;     // 不是本窗口发起的（比如头像缓存），不显示进度

    it->elapsedMs = elapsedMs;
    const int percent = totalBytes > 0 ? qBound(0, int((receivedBytes * 100) / totalBytes), 100) : 0;
    if (percent == it->lastPercent)
        return;     // 同一百分比不重复刷标题

    it->lastPercent = percent;
    if (it->peerUid == m_currentTargetUid)
        refreshChatTitle();
}

void MainWindow::onDownloadFinished(quint32 taskId, const QString &localPath, qint64 size)
{
    Q_UNUSED(size);

    if (m_shuttingDown || QCoreApplication::closingDown())
        return;     // 析构期间的 abort/download 回调不能再弹模态框

    auto it = m_downloads.find(taskId);
    if (it == m_downloads.end())
        return;     // 头像缓存等其他下载，不归聊天窗口管

    const DownloadUi dl = it.value();
    m_downloads.erase(it);

    // 图片下载完成：用本地文件生成真缩略图，替换气泡里的占位框
    if (dl.msgType == Proto::MsgTypeImage && !localPath.isEmpty()) {
        const qreal dpr = devicePixelRatioF();
        const QPixmap thumb = makeImageThumbnail(localPath,
                                                 qRound(MessageBubble::kImageMaxW * dpr),
                                                 qRound(MessageBubble::kImageMaxH * dpr));
        if (!thumb.isNull()) {
            storeImageThumb(dl.fileId, thumb);   // 只缓存小图，不复制整张原图
            // 权威缓存也要更新，否则切走会话再切回来又变回占位框
            for (auto sit = m_sessionMessages.begin(); sit != m_sessionMessages.end(); ++sit) {
                for (MessageItem &m : sit.value()) {
                    if (m.fileId == dl.fileId)
                        m.thumb = thumb;
                }
            }
            m_messageModel->refreshThumb(dl.fileId, thumb);
            ui->chatMsgListView->doItemsLayout();   // 缩略图改变了行高，让视图重算布局
        }
    }

    refreshChatTitle();
    showDownloadDoneDialog(localPath.isEmpty() ? dl.savePath : localPath);
}

void MainWindow::onDownloadFailed(quint32 taskId, int errcode, const QString &text)
{
    if (m_shuttingDown || QCoreApplication::closingDown())
        return;

    auto it = m_downloads.find(taskId);
    if (it == m_downloads.end())
        return;

    const QString fileName = it->fileName;
    m_downloads.erase(it);
    refreshChatTitle();

    Q_UNUSED(errcode);   // 详细错误由 FileTransferManager::failTask 统一记录，这里只负责 UI 提示
    QMessageBox::warning(this, QStringLiteral(" "),
                         text.isEmpty() ? QStringLiteral("%1 下载失败，请重试").arg(fileName) : text);
}

void MainWindow::showDownloadDoneDialog(const QString &localPath)
{
    if (localPath.isEmpty() || m_shuttingDown || QCoreApplication::closingDown())
        return;

    // 不能 exec()：那会开嵌套事件循环，完成槽没返回时又能处理别的信号（再次下载完成、关窗口）。
    // open() 只做窗口级模态，立即返回；按钮结果用 finished 信号取，box 关闭后自动 delete。
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle(QStringLiteral("下载完成"));
    box->setIcon(QMessageBox::Information);
    box->setText(QStringLiteral("文件已保存到：\n%1").arg(QDir::toNativeSeparators(localPath)));

    // 产品决策：完成提示只给这两个动作，不再提供"取消/重试"之类第三按钮
    QPushButton *openFileBtn   = box->addButton(QStringLiteral("打开文件"), QMessageBox::AcceptRole);
    QPushButton *openFolderBtn = box->addButton(QStringLiteral("打开所在文件夹"), QMessageBox::ActionRole);
    box->setEscapeButton(openFolderBtn);

    connect(openFileBtn, &QPushButton::clicked, box, [localPath]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
    });
    connect(openFolderBtn, &QPushButton::clicked, box, [localPath]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(localPath).absolutePath()));
    });

    box->open();
}

// 会话标题统一出口：切会话、上传/下载开始/结束都走它，避免"正在发送"被切会话冲掉
void MainWindow::refreshChatTitle()
{
    if (m_currentTargetUid.isEmpty()) {
        ui->title_label->clear();
        return;
    }

    QString title = nicknameForDisplay(m_currentTargetUid);
    const QFontMetrics fm(ui->title_label->font());

    // 发送附件
    if (m_uploadTaskId != 0 && !m_uploadFileName.isEmpty()) {
        const QString shownName = fm.elidedText(m_uploadFileName, Qt::ElideMiddle, 160);
        title += QStringLiteral("   正在发送 %1  %2%  %3").arg(shownName)
                            .arg(m_uploadPercent).arg(formatElapsedMs(m_uploadElapsedMs));
    }

    // 当前会话里用户主动发起的下载
    for (auto it = m_downloads.constBegin(); it != m_downloads.constEnd(); ++it) {
        if (it->peerUid != m_currentTargetUid)
            continue;

        const QString shownName = fm.elidedText(it->fileName, Qt::ElideMiddle, 160);
        title += QStringLiteral("   正在下载 %1  %2%  %3").arg(shownName)
                            .arg(it->lastPercent).arg(formatElapsedMs(it->elapsedMs));
        break;
    }

    ui->title_label->setText(title);
}

void MainWindow::resetUploadState()
{
    m_uploadTaskId = 0;
    m_uploadPeerUid.clear();
    m_uploadLocalPath.clear();
    m_uploadFileName.clear();
    m_uploadMime.clear();
    m_uploadFileSize = 0;
    m_uploadMsgType  = Proto::MsgTypeText;
    m_uploadPercent   = 0;
    m_uploadElapsedMs = 0;
    refreshChatTitle();
}

// 调试窗口尺寸
//void MainWindow::resizeEvent(QResizeEvent *event)
//{
//    qDebug() << "窗口大小:" << event->size();
//    QMainWindow::resizeEvent(event);
//}


