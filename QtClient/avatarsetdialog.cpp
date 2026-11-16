#include "avatarsetdialog.h"
#include "ui_avatarsetdialog.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QtMath>

//  AvatarCanvas 头像裁剪画布, 显示图片 + 拖动 + 滚轮缩放，输出正方形裁剪结果
class AvatarCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit AvatarCanvas(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(320, 320);
        setMouseTracking(true);
        setCursor(Qt::OpenHandCursor);  // 鼠标移到画布上变成张开的手掌图标
        setAttribute(Qt::WA_OpaquePaintEvent, true); // 声明填满整个区域, 省一次重绘, Qt 的合成器看到这个属性，会跳过背景擦除
    }

    void setImage(const QImage &image)
    {
        m_image = image;
        resetView();
        update();
        emit imageChanged();
    }

    bool hasImage() const { return !m_image.isNull(); }

    // 原图裁减
    QImage croppedImage(int outputSize) const
    {
        if (m_image.isNull() || m_scale <= 0.0)
            return QImage();

        // viewport 覆盖的原图区域
        const double side = double(width()) / m_scale;  // 视口覆盖的原图边长: 例如放大 2 倍时只覆盖 160 原图像素
        const int x0 = qRound(-m_offset.x() / m_scale); // 除以 scale 得到原图上的起点
        const int y0 = qRound(-m_offset.y() / m_scale);
        const int sideInt = qRound(side);

        QRect src(x0, y0, sideInt, sideInt); // 正方形区域
        src = src.intersected(QRect(0, 0, m_image.width(), m_image.height()));
        if (src.width() <= 0 || src.height() <= 0)
            return QImage();

        // 从原图切出那块正方形, 缩放到目标尺寸（256×256）, 双线性/双三次插值
        return m_image.copy(src).scaled(outputSize, outputSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

signals:
    void imageChanged();

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform); // 图片缩放时双线性插值
        p.fillRect(rect(), QColor(0xFA, 0xFA, 0xFA)); // 清屏

        if (m_image.isNull()) {
            p.setPen(QColor(180, 180, 180));
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("请选择图片"));
            return;
        }

        const QRectF target(m_offset, QSizeF(m_image.width() * m_scale, m_image.height() * m_scale));
        p.drawImage(target, m_image);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || m_image.isNull()) // 只响应鼠标左键
            return;
        m_dragging = true;
        m_dragStart = e->pos();
        m_offsetAtPress = m_offset;
        setCursor(Qt::ClosedHandCursor); // 鼠标左键按住时 显示手掌合上
    }

    // 鼠标左键按住时 计算 offset
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!m_dragging)
            return;
        m_offset = m_offsetAtPress + QPointF(e->pos() - m_dragStart);
        clampOffset();
        update();
    }

    // 清 m_dragging、恢复光标
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
    }

    // 鼠标滚轮: 缩放图片  +- 10% , 计算 m_scale/m_offset， 重绘
    void wheelEvent(QWheelEvent *e) override
    {
        if (m_image.isNull())
            return;
        const double factor = (e->angleDelta().y() > 0) ? 1.1 : (1.0 / 1.1);
        // 缩放范围钳制, 最大放大 4 倍
        const double lo = minScale();
        const double hi = lo * 4.0;
        const double newScale = qBound(lo, m_scale * factor, hi);
        if (qFuzzyCompare(newScale, m_scale))
            return; // 已经到达边界（比如已经到 4× 还继续滚鼠标），直接返回

        // 以光标为锚点缩放：光标下的原图像素保持不动（Qt5: posF()）
        const QPointF pos = e->position();
        const QPointF imgPt((pos.x() - m_offset.x()) / m_scale, (pos.y() - m_offset.y()) / m_scale);
        m_scale = newScale;
        m_offset = QPointF(pos.x() - imgPt.x() * m_scale, pos.y() - imgPt.y() * m_scale);
        clampOffset();
        update();
        e->accept();
    }

private:
    double minScale() const
    {
        if (m_image.isNull())
            return 1.0;
        const double V = width();
        return qMax(V / double(m_image.width()), V / double(m_image.height()));
    }

    void resetView()
    {
        if (m_image.isNull())
            return;
        m_scale = minScale();
        const double V = width();
        const double dw = m_image.width() * m_scale;
        const double dh = m_image.height() * m_scale;
        m_offset = QPointF((V - dw) / 2.0, (V - dh) / 2.0);   // 居中
        clampOffset();
    }

    void clampOffset()
    {
        if (m_image.isNull())
            return;
        const double V = width();
        const double dw = m_image.width() * m_scale;
        const double dh = m_image.height() * m_scale;
        // 图片必须完全覆盖 viewport：offset ∈ [V - dw, 0]
        m_offset.setX(qBound(V - dw, m_offset.x(), 0.0));
        m_offset.setY(qBound(V - dh, m_offset.y(), 0.0));
    }

    QImage  m_image;
    double  m_scale = 1.0;
    QPointF m_offset;          // 图片左上角在画布里的位置（<=0）
    QPoint  m_dragStart;       // 按下瞬间 鼠标在控件内的位置
    QPointF m_offsetAtPress;   // 按下瞬间图片的偏移
    bool    m_dragging = false;
};


// ========================== AvatarSetDialog ==================================

namespace {
    constexpr qint64 kMaxSourceBytes  = 10LL * 1024 * 1024;
    constexpr qint64 kMaxSourcePixels = 50000000LL; // 源图总像素数的上限：5000 万像素， 覆盖了主流手机和相机的最高像素（4800 万）
    constexpr int    kOutputSize      = 256;
}

AvatarSetDialog::AvatarSetDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::AvatarSetDialog)
{
    ui->setupUi(this);

    setWindowTitle("设置头像");
    setModal(false);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);  // window 去掉对话框标题栏上的"?"帮助按钮

    // 创建画布，插到布局顶部
    m_canvas = new AvatarCanvas(this);
    ui->verticalLayout->insertWidget(0, m_canvas, 0, Qt::AlignCenter);

    ui->okBtn->setEnabled(false);

    connect(ui->chooseImageBtn, &QPushButton::clicked, this, &AvatarSetDialog::onChooseImage);
    connect(ui->cancelBtn,      &QPushButton::clicked, this, &QDialog::close);
    connect(ui->okBtn,          &QPushButton::clicked, this, &AvatarSetDialog::onOkBtnClicked);

    connect(m_canvas, &AvatarCanvas::imageChanged, this, [this]() {
        const bool has = m_canvas->hasImage();
        ui->okBtn->setEnabled(has);
        ui->chooseImageBtn->setText(has ? QStringLiteral("重新选择") : QStringLiteral("选择图片"));
    });

    adjustSize();
}

AvatarSetDialog::~AvatarSetDialog()
{
    delete ui;   // m_canvas 的父对象是 this，随 Dialog 一起销毁
}

void AvatarSetDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    // 单例复用：每次显示都从"未选图"开始
    m_canvas->setImage(QImage());
    ui->okBtn->setEnabled(false);
    ui->chooseImageBtn->setText(QStringLiteral("选择图片"));
}

void AvatarSetDialog::onChooseImage()
{
    const QString path = QFileDialog::getOpenFileName( this, QStringLiteral("选择图片"), QString(),
                            QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (path.isEmpty())
        return;

    if (QFileInfo(path).size() > kMaxSourceBytes) {
        QMessageBox::warning(this, "提示", "图片过大，请选择 10MB 以内的图片");
        return;
    }

    QImageReader reader(path);

    reader.setAutoTransform(true);   // 头像很多来自手机自拍、导出照片，手机拍照时，传感器方向固定，竖拍的照片实际存的是横着的像素
                                     // 读取 EXIF Orientation 并在解码后自动旋转/镜像， 避免手机拍的照片"横躺"
    const QImage src = reader.read();
    if (src.isNull()) {
        QMessageBox::warning(this, "提示", "图片无法加载或格式不支持");
        return;
    }
    if (qint64(src.width()) * src.height() > kMaxSourcePixels) {
        QMessageBox::warning(this, "提示", "图片像素过大，请先压缩后再试");
        return;
    }

    m_canvas->setImage(src);
}

void AvatarSetDialog::onOkBtnClicked()
{
    const QImage out = m_canvas->croppedImage(kOutputSize);
    if (out.isNull()) {
        QMessageBox::warning(this, "提示", "头像裁剪失败");
        return;
    }
    emit avatarReady(out);
    close();
}

// .cpp 内有 Q_OBJECT 类，必须有这一行让 moc 收尾
#include "avatarsetdialog.moc"
