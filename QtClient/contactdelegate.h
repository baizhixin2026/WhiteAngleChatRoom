#ifndef CONTACTDELEGATE_H
#define CONTACTDELEGATE_H

#include <QStyledItemDelegate>

class ContactDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ContactDelegate(QObject *parent = nullptr);

    // painting
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

#endif // CONTACTDELEGATE_H
