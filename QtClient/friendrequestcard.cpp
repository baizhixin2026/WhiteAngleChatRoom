#include "friendrequestcard.h"

#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDateTime>
#include <QPixmap>

namespace {
    constexpr int kAvatarSize = 40;
    constexpr int kCardHeight = 72;
    constexpr int kBtnWidth   = 56;
    constexpr int kBtnHeight  = 26;
}

FriendRequestCard::FriendRequestCard(const FriendRequestInfo &info, QWidget *parent)
    : QWidget(parent), m_reqId(info.reqId)
{
    setFixedHeight(kCardHeight);
    setAttribute(Qt::WA_StyledBackground, true);   // 让 QSS 背景生效
    setObjectName("friendRequestCard");
    setStyleSheet(
        "QWidget#friendRequestCard {"
        "  background: #FFFFFF;"
        "  border-bottom: 1px solid #F0F0F0;"
        "}"
    );

    // 头像
    auto *avatar = new QLabel(this);
    avatar->setFixedSize(kAvatarSize, kAvatarSize);
    avatar->setPixmap(QPixmap(":/image/user/user-6.png").scaled(
    kAvatarSize, kAvatarSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    // 昵称 粗体
    auto *nickLabel = new QLabel(info.fromNick, this);
    nickLabel->setStyleSheet("font-weight: bold; font-size: 13px; color: #333;");

    // 验证信息
    const QString text = info.text;
    auto *textLabel = new QLabel(text, this);
    textLabel->setStyleSheet("font-size: 12px; color: #666;");

    // 时间
    const QString timeStr = QDateTime::fromMSecsSinceEpoch(info.createdAt).toString("yyyy-MM-dd  HH:mm:ss");
    auto *timeLabel = new QLabel(timeStr, this);
    timeLabel->setStyleSheet("font-size: 11px; color: #AAA;");

    // 文字列, 垂直布局
    auto *messageTextCol = new QVBoxLayout;
    messageTextCol->setContentsMargins(0, 0, 0, 0);
    messageTextCol->setSpacing(2);
    messageTextCol->addWidget(nickLabel);
    messageTextCol->addWidget(textLabel);
    messageTextCol->addWidget(timeLabel);

    // 按钮
    m_acceptBtn = new QPushButton(QStringLiteral("同意"), this);
    m_acceptBtn->setFixedSize(kBtnWidth, kBtnHeight);
    m_acceptBtn->setCursor(Qt::PointingHandCursor);
    m_acceptBtn->setStyleSheet(
        "QPushButton {"
        "  background: #0099FF; color: white;"
        "  border: none; border-radius: 3px; font-size: 12px;"
        "}"
        "QPushButton:hover { background: #007ACC; }"
        "QPushButton:disabled { background: #B8D9F2; }"
    );

    m_rejectBtn = new QPushButton(QStringLiteral("拒绝"), this);
    m_rejectBtn->setFixedSize(kBtnWidth, kBtnHeight);
    m_rejectBtn->setCursor(Qt::PointingHandCursor);
    m_rejectBtn->setStyleSheet(
        "QPushButton {"
        "  background: #F5F5F5; color: #555;"
        "  border: 1px solid #DDD; border-radius: 3px; font-size: 12px;"
        "}"
        "QPushButton:hover { background: #E8E8E8; }"
        "QPushButton:disabled { color: #BBB; }"
    );

    auto *btnCol = new QVBoxLayout;
    btnCol->setContentsMargins(0, 0, 0, 0);
    btnCol->setSpacing(4);
    btnCol->addStretch();   // 弹簧
    btnCol->addWidget(m_acceptBtn);
    btnCol->addWidget(m_rejectBtn);
    btnCol->addStretch();

    // 文字列 + 按钮, 水平布局
    auto *main = new QHBoxLayout(this);
    main->setContentsMargins(12, 8, 12, 8);
    main->setSpacing(10);
    main->addWidget(avatar);
    main->addLayout(messageTextCol, 1);
    main->addLayout(btnCol);

    connect(m_acceptBtn, &QPushButton::clicked, this, [this]() {
        emit acceptClicked(m_reqId);
    });
    connect(m_rejectBtn, &QPushButton::clicked, this, [this]() {
        emit rejectClicked(m_reqId);
    });
}

void FriendRequestCard::setBusy()
{
    m_acceptBtn->setEnabled(false);
    m_rejectBtn->setEnabled(false);
}

void FriendRequestCard::resetBusy()
{
    m_acceptBtn->setEnabled(true);
    m_rejectBtn->setEnabled(true);
}
