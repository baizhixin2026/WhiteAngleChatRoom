#ifndef USERTABLEMODEL_H
#define USERTABLEMODEL_H

#include <QAbstractTableModel>
#include <QVector>
#include "userinfo.h"

class UserTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        ColId = 0,
        ColAccountNo,
        ColUsername,
        ColNickname,
        ColCreated,
        ColLastLogin,
        ColLastActive,
        ColOnline,
        ColCount
    };

    explicit UserTableModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    void setUsers(const QVector<UserInfo> &users);

protected:
    virtual QVector<UserInfo> filterUsers(const QVector<UserInfo> &allUser) const;

private:
    QVector<UserInfo> m_users;
};

#endif // USERTABLEMODEL_H
