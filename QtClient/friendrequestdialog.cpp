#include "friendrequestdialog.h"
#include "ui_friendrequestdialog.h"

#include "connectionmanager.h"
#include "friendrequestcard.h"
#include <QResizeEvent>
#include <QDebug>

FriendRequestDialog::FriendRequestDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::FriendRequestDialog)
{
    ui->setupUi(this);

    setWindowTitle(QStringLiteral("好友申请"));
    setModal(false);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setMinimumSize(550, 300);

    // 空状态提示
    ui->emptyTip->setText("没有待处理的好友申请");
    ui->emptyTip->setAlignment(Qt::AlignCenter);
    ui->emptyTip->setStyleSheet("color: #999; font-size: 13px; background: #EEEEF0;");

    ui->frScrollArea->setWidgetResizable(true);
    ui->frScrollArea->setStyleSheet("QScrollArea { background: #FAFAFA; }");
    ui->frContainer->setStyleSheet("QWidget#frContainer { background: #FAFAFA; }");

    updateEmptyState();

    auto& conn = ConnectionManager::instance();
    connect(this, &FriendRequestDialog::handleFriendRequest, &conn, &ConnectionManager::onHandleFriendRequest);
}

FriendRequestDialog::~FriendRequestDialog()
{
    delete ui;
}

void FriendRequestDialog::setRequests(const QVector<FriendRequestInfo> &reqs)
{
    // 全量重建：清空旧卡片
    const auto oldCards = m_cards.values();
    for (FriendRequestCard *card : oldCards) {
        ui->verticalLayout->removeWidget(card);
        card->deleteLater();
    }
    m_cards.clear();

    for (int i = reqs.size() - 1; i >= 0; --i)
        addFriendRequest(reqs[i]);      // 复用增量路径，去重逻辑一处

    updateEmptyState();
}

void FriendRequestDialog::addFriendRequest(const FriendRequestInfo &req)
{
    if(req.reqId <= 0 || m_cards.contains(req.reqId))
        return;
    auto *card = new FriendRequestCard(req);
    m_cards.insert(req.reqId, card);
    ui->verticalLayout->insertWidget(0, card);
    updateEmptyState();

    connect(card, &FriendRequestCard::acceptClicked, this, [this](qint64 reqId) {
        if(auto *c = m_cards.value(reqId))
            c->setBusy();
        emit handleFriendRequest(reqId, true);
    });
    connect(card, &FriendRequestCard::rejectClicked, this, [this](qint64 reqId) {
        if(auto *c = m_cards.value(reqId))
            c->setBusy();
        emit handleFriendRequest(reqId, false);
    });
}

void FriendRequestDialog::removeRequest(qint64 reqId)
{
    auto *card = m_cards.take(reqId);
    if (!card)
        return;
    ui->verticalLayout->removeWidget(card);
    card->deleteLater();
    updateEmptyState();
}

void FriendRequestDialog::updateEmptyState()
{
    const bool empty = m_cards.isEmpty();
    ui->frScrollArea->setVisible(!empty); // 可见
    ui->emptyTip->setVisible(empty);

    const int n = m_cards.size();
    setWindowTitle(n > 0 ? QStringLiteral("好友申请 ( %1 )").arg(n) : QStringLiteral("好友申请"));
}

// 调试窗口尺寸
//void FriendRequestDialog::resizeEvent(QResizeEvent *event)
//{
//    qDebug() << "FriendRequestDialog 窗口大小:" << event->size();
//    QDialog::resizeEvent(event);
//}
