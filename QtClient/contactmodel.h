#ifndef CONTACTMODEL_H
#define CONTACTMODEL_H

#include <QAbstractListModel>
#include <QList>
#include "contactitem.h"

class ContactModel : public QAbstractListModel
{
    Q_OBJECT

public:
    explicit ContactModel(QObject *parent = nullptr);

    enum ContactRole {
        IdRole = Qt::UserRole + 1,   // 唯一标识, 自定义角色必须从 Qt::UserRole + 1 开始，避免和系统冲突
        NameRole,        // 显示名 nickname
        AvatarRole,      // 头像 QPixmap
        OnlineRole,
        UnreadRole,     // 未读消息
        PreviewRole     // 待扩展：消息预览
    };

    // Basic functionality:
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setContacts(const QList<ContactItem> &contacts); // 更新整个列表
    void updateOnlineStatus(const QString &uid, bool online);
    ContactItem getContact(const QString &uid) const;
    void setUnreadCount(const QString &uid, int count);
    void updateNickname(const QString &uid, const QString &nick);

    void setAutoSort(bool on) { m_autoSort = on; }
    void upsertContact(const ContactItem &item);
    void removeContact(const QString &uid);
    void updateAvatar(const QString &uid, const QPixmap &pixmap);

private:
    QList<ContactItem> m_contacts;
    bool m_autoSort = false;
};

#endif // CONTACTMODEL_H
