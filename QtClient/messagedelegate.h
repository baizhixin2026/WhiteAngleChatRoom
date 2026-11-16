#ifndef MESSAGEDELEGATE_H
#define MESSAGEDELEGATE_H

#include <QRect>
#include <QStyledItemDelegate>

namespace MessageBubble {
    constexpr int kImageMaxW = 220;   // 气泡内图片最大宽（逻辑像素）
    constexpr int kImageMaxH = 180;   // 气泡内图片最大高（逻辑像素）
}

class MessageDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit MessageDelegate(QObject *parent = nullptr);

    // painting
    void paint(QPainter *painter,
               const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

    // 气泡在 viewport 坐标系里的矩形；paint / 鼠标命中测试必须共用这套几何
    QRect bubbleRect(const QRect &itemRect, const QModelIndex &index) const;
};

#endif // MESSAGEDELEGATE_H
