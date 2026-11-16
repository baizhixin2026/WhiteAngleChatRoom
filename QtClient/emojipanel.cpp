#include "emojipanel.h"
#include "ui_emojipanel.h"
#include "protocoltypes.h"
#include "emojitext.h"

#include <QGridLayout>

EmojiPanel::EmojiPanel(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::emojipanel)
{
    ui->setupUi(this);
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint); // 弹出式窗口，点击窗口外部时，窗口会自动关闭, 无边框
    setFixedSize(300, 180);

    ui->stackedWidget->setCurrentWidget(ui->facePage);
    ui->faceBtn->setCursor(Qt::PointingHandCursor);
    ui->handBtn->setCursor(Qt::PointingHandCursor);
    ui->otherBtn->setCursor(Qt::PointingHandCursor);

    for (QScrollArea *area : { ui->facePageScrollArea, ui->handPageScrollArea, ui->otherPageScrollArea }) {
        area->viewport()->setAutoFillBackground(false);
        area->viewport()->setStyleSheet("background: #FAFAFA;");
    }

    setupGrid(ui->gridLayout_face, Proto::kEmojiFace);
    setupGrid(ui->gridLayout_hand, Proto::kEmojiHand);
    setupGrid(ui->gridLayout_other, Proto::kEmojiOther);

    connect(ui->faceBtn, &QToolButton::clicked, this, [this]{ ui->stackedWidget->setCurrentWidget(ui->facePage); });
    connect(ui->handBtn, &QToolButton::clicked, this, [this]{ ui->stackedWidget->setCurrentWidget(ui->handPage); });
    connect(ui->otherBtn, &QToolButton::clicked, this, [this]{ ui->stackedWidget->setCurrentWidget(ui->otherPage); });
}

EmojiPanel::~EmojiPanel()
{
    delete ui;
}

// 资源图片 命名前缀(必须是连续的数字, 100000.png 方便后期头部插入新表情 9999.png)  举例 face-100001.png
// 命令：ls -v 自然排序, 批量改前缀 for n in {1..10}; do mv -- "face-$n.png" "face-$((100000 + n)).png"; done
void EmojiPanel::setupGrid(QGridLayout *grid, const QString &prefix)
{
    if (!grid)
        return;

    int idx = 100001;
    for (int r = 0; r < grid->rowCount(); ++r) {
        for (int c = 0; c < grid->columnCount(); ++c) {
            QLayoutItem *item = grid->itemAtPosition(r, c);
            if (!item || !item->widget())
                continue;
            auto *btn = qobject_cast<QToolButton *>(item->widget());
            if (!btn)
                continue;

            const QString code = QStringLiteral("%1-%2").arg(prefix).arg(idx++);
            const QString path = EmojiText::emojiResourcePath(code);

            btn->setIcon(QIcon(path));
            btn->setIconSize(QSize(24, 24));
            btn->setAutoRaise(true);
            btn->setCursor(Qt::PointingHandCursor);

            connect(btn, &QToolButton::clicked, this, [this, code]() {
                emit emojiSelected(code);
                hide();
            });
        }
    }
}
