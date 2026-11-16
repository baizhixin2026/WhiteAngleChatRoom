#ifndef AVATARDISPLAY_H
#define AVATARDISPLAY_H

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPixmapCache>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QtMath>

/*
 * ① 缩小到显示尺寸 + 缓存（只负责"尺寸和清晰度"，不管圆角）
 *
 * 为什么不能直接 painter->drawPixmap(rect, pixmap)：
 *   那是把 256×256 原图交给 painter 现缩，采样算法由 QPainter::SmoothPixmapTransform 决定，默认关闭（退化成最近邻）。
 *   QIcon 内部是 SmoothTransformation，所以同一个 pixmap：按钮清晰、自绘列表发糊。
 */
inline QPixmap displayScaledPixmap(const QPixmap &src, int avatarSize, qreal pixelRatio)
{
    if (src.isNull() || avatarSize <= 0)
        return src;

    const qreal ratio = (pixelRatio > 0.0) ? pixelRatio : 1.0;
    const int px = qRound(avatarSize * ratio);
    const QString key = QStringLiteral("display-pixmap:%1:%2").arg(src.cacheKey()).arg(px);

    QPixmap out;
    if (QPixmapCache::find(key, &out))
        return out;

    out = src.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    out.setDevicePixelRatio(ratio);
    QPixmapCache::insert(key, out);
    return out;
}

/*
 * ② 以圆角矩形绘制（几何裁剪，不依赖任何 alpha 通道）
 *
 * 为什么不预先裁好带透明的 pixmap：
 *   实测（本机 Qt 5.15 + xcb）：QImage(ARGB32_Premultiplied) 存盘是 RGBA，
 *   但 QPixmap::fromImage() 之后 alpha 通道消失，再 save 出来是 RGB。
 *   没有 alpha，DestinationIn 蒙版静默失效 —— 画面还是方的。
 * 所以圆角交给 QPainter 的裁剪路径：纯几何，和像素格式无关，任何平台都对。
 *
 * 代价：每次绘制光栅化一次圆角路径（40×40 区域，几十微秒）。
 */
inline void paintRoundedPixmap(QPainter *painter, const QRect &target, const QPixmap &pm, qreal radius)
{
    if (!painter || pm.isNull() || target.isEmpty())
        return;

    painter->save();
    if (radius > 0.0) {
        QPainterPath path;
        path.addRoundedRect(QRectF(target), radius, radius);
        painter->setClipPath(path);
    }
    painter->drawPixmap(target, pm);
    painter->restore();
}

#endif // AVATARDISPLAY_H
