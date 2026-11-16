#include "contactdelegate.h"
#include "contactmodel.h"
#include "avatardisplay.h"
#include <QPainter>
#include <QFontMetrics>
#include <QPixmapCache>

namespace {
    constexpr int avatarSize = 40;      // 头像 40x40
    constexpr int marginX = 6;          // 边距
    constexpr int marginY = 2;          // 边距
    constexpr int radius = 6;           // 圆角半径
    constexpr int avatarRectLeft = 8;   // 头像与矩形左边距
    constexpr int avatarRectTop = 4;    // 头像与矩形上边距
    constexpr int rectLeft = 6;         // 和矩形左边的边距
    constexpr int nameRectHigh = 20;    //
    constexpr int optionRectHigh = avatarSize + avatarRectTop*2 + marginY*2;
    constexpr int nameRectTop = (optionRectHigh - nameRectHigh)/2;      // 和矩形顶部的边距
    constexpr int onlineRectTop = (optionRectHigh - nameRectHigh)/2;
    constexpr int onlineRectWidth = 56; // 在线状态矩形
    constexpr int onlineTextWidth = 48; // 在线状态矩形
    constexpr int onlineTextHigh = 20;  // 在线状态矩形
}

ContactDelegate::ContactDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void ContactDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // 把 painter 当前的状态（画笔、字体、笔刷等）压栈存档, 不污染 View 之后的其他绘制
    painter->save();
    // 开启抗锯齿：让圆、斜线的边缘平滑，否则头像圆形占位图、文字边缘会有毛刺感。
    painter->setRenderHint(QPainter::Antialiasing);
    // 背景绘制：选中/悬停/默认
    if(option.state & QStyle::State_Selected) {
        painter->setBrush(QColor(0x00, 0x99, 0xFF));   // 鼠标选中: 蓝色
        painter->setPen(Qt::NoPen);                    // 去掉边框
        QRect bgRect = option.rect.adjusted(marginX, marginY, -marginX, -marginY); // 留一点边距，让圆角更明显
        painter->drawRoundedRect(bgRect, radius, radius);        // 圆角半径 6 像素
        painter->setPen(Qt::white);            // 改变文字颜色
    } else if(option.state & QStyle::State_MouseOver) {
        painter->setBrush(QColor(0xCC, 0xE9, 0xFF));   // 鼠标悬停: 浅蓝
        painter->setPen(Qt::NoPen);
        QRect bgRect = option.rect.adjusted(marginX, marginY, -marginX, -marginY);
        painter->drawRoundedRect(bgRect, radius, radius);
        painter->setPen(Qt::black);
    } else {
        painter->setPen(Qt::black);
    }

    // 获取数据, index.data(role) 内部会调用 data(index, role)
    QString name = index.data(ContactModel::NameRole).toString();
    const bool online = index.data(ContactModel::OnlineRole).toBool();
    const int unread = index.data(ContactModel::UnreadRole).toInt();

    const qreal pixelRatio = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    const QPixmap avatar = displayScaledPixmap(index.data(ContactModel::AvatarRole).value<QPixmap>(),
                                                avatarSize, pixelRatio);

    // 绘制头像（已经缩到物理尺寸，这里是 1:1 贴图，不再有重采样）
    const QRect avatarRect(option.rect.left() + avatarRectLeft, option.rect.top() + avatarRectTop, avatarSize, avatarSize);
    paintRoundedPixmap(painter, avatarRect, avatar, 6);

    // 名字框的几何计算: x, y, 宽, 高, 右边预留 70px 给“[在线]”文字
    int nameRect_width = option.rect.width() - avatarRect.width() - onlineRectWidth;
    QRect nameRect = QRect(avatarRect.right() + rectLeft, option.rect.top() + nameRectTop,
                                                    nameRect_width, nameRectHigh);

    QFont font = painter->font();
    font.setPixelSize(13);
    painter->setFont(font);
    // 水平靠左+垂直居中地画出名字。名字过长超出矩形会被截断省略
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, name);

    // 状态文字, 在行尾部
    QRect statusRect = QRect(option.rect.right() - onlineRectWidth,
                             option.rect.top() + onlineRectTop, onlineTextWidth, onlineTextHigh);
    QFont onlineFont = painter->font();
    onlineFont.setPixelSize(11);
    painter->setFont(onlineFont);

    const bool selected = (option.state & QStyle::State_Selected);
    if(selected)
        painter->setPen(QColor(Qt::white)); // 选中行背景是蓝的，必须白字
    else if(online)
        painter->setPen(QColor(0x07, 0xC1, 0x60));                // 在线：绿
    else
        painter->setPen(QColor(0x99, 0x99, 0x99));                // 离线：灰

    painter->drawText(statusRect, Qt::AlignRight | Qt::AlignVCenter,
                          online ? QStringLiteral("[在线]") : QStringLiteral("[离线]"));
    if(!selected)
        painter->setPen(Qt::black);

    if(unread > 0) {
        const QString badgeText = (unread > 99) ? "99+" : QString::number(unread);
        QFont badgeFont = painter->font();
        badgeFont.setPixelSize(12);
        badgeFont.setBold(true);    // 粗体
        painter->setFont(badgeFont);

        QFontMetrics badgeFm(badgeFont);
        const int textWidth = badgeFm.horizontalAdvance(badgeText);
        const int badgeHigh = 20;
        const int badgeWidth = qMax(20, textWidth + 8);
        // 贴在头像右上角，稍微向内偏 2px，不超出条目顶部
        const int badgeX = avatarRect.right() - badgeWidth + 2;
        const int badgeY = avatarRect.top() - 2;
        QRect badgeRect(badgeX, badgeY, badgeWidth, badgeHigh);
        painter->setBrush(QColor(0xF7, 0x4C, 0x30));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(badgeRect, badgeHigh / 2.0, badgeWidth / 2.0);
        painter->setPen(Qt::white);
        painter->drawText(badgeRect,Qt::AlignCenter, badgeText);
    }

    painter->restore(); // 恢复 save 存档的状态
}

QSize ContactDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index);
    // ListView 真正的宽度由窗口大小决定,  高度有效值，必须精确匹配绘制的行高
    return QSize(option.rect.width(), optionRectHigh);
}
