#ifndef AVATARSETDIALOG_H
#define AVATARSETDIALOG_H

#include <QDialog>
#include <QImage>

class AvatarCanvas;      // 前置声明，定义在 .cpp 里

namespace Ui {
class AvatarSetDialog;
}

class AvatarSetDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AvatarSetDialog(QWidget *parent = nullptr);
    ~AvatarSetDialog();

signals:
    // 用户点"确定"且裁剪成功，携带正方形 QImage
    void avatarReady(const QImage &cropped);

protected:
    void showEvent(QShowEvent *e) override;   // 每次显示重置状态

private slots:
    void onChooseImage();
    void onOkBtnClicked();

private:
    Ui::AvatarSetDialog *ui;
    AvatarCanvas *m_canvas = nullptr;
};

#endif // AVATARSETDIALOG_H
