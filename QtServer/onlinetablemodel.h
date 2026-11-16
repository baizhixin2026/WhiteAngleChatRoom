#ifndef ONLINETABLEMODEL_H
#define ONLINETABLEMODEL_H

#include "usertablemodel.h"

class OnlineTableModel : public UserTableModel
{
    Q_OBJECT

public:
    explicit OnlineTableModel(QObject *parent = nullptr)
            : UserTableModel(parent) {}

protected:
    QVector<UserInfo> filterUsers(const QVector<UserInfo> &allUser) const override;

};

#endif // ONLINETABLEMODEL_H
