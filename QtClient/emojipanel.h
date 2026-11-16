#ifndef EMOJIPANEL_H
#define EMOJIPANEL_H

#include <QWidget>

class QGridLayout;

namespace Ui {
class emojipanel;
}

class EmojiPanel : public QWidget
{
    Q_OBJECT

public:
    explicit EmojiPanel(QWidget *parent = nullptr);
    ~EmojiPanel();

signals:
    void emojiSelected(const QString &code);

private:
    void setupGrid(QGridLayout *grid, const QString &prefix);

private:
    Ui::emojipanel *ui;
};

#endif // EMOJIPANEL_H
