#include "messagedelegate.h"
#include "messagemodel.h"
#include "protocoltypes.h"
#include "emojitext.h"
#include "avatardisplay.h"

#include <QPainter>
#include <QPen>
#include <QPixmapCache>
#include <QDateTime>
#include <QFontMetrics>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QTextCharFormat>
#include <QAbstractTextDocumentLayout>
#include <QPalette>
#include <QUrl>
#include <QtMath>

namespace {
    constexpr int kBodyFontSize = Proto::kMsgSize; // 消息正文字体大小
    constexpr int avatarSize = 32;               // 头像 32×32
    constexpr int avatarMargin = 8;              // 头像距离屏幕边缘
    constexpr int avatarRectTop = 6;             // 头像和矩形顶部之间的垂直间距
    constexpr int avatarRectBottom = 6;
    constexpr int avatarBubbleGap = 8;           // 头像和气泡之间的水平间距
    constexpr int nameHeight = 16;               // 名字区域高度
    constexpr int nameBubbleGap = 2;             // 名字和气泡之间的垂直间距
    constexpr int nameAvatarGap = 8;             // 名字和头像之间的水平间距
    constexpr int bubbleTopPadding = 8;          // 气泡内上边距
    constexpr int bubbleBottomPadding = 8;       // 气泡内下边距
    constexpr int bubbleLeftPadding = 8;         // 气泡内左边距
    constexpr int bubbleRightPadding = 8;        // 气泡内右边距
    constexpr int bubbleRectTop = 8;             // 气泡和矩形顶部之间的垂直间距
    constexpr double kMaxBubbleWidthRatio = 0.55;   // 气泡最大宽度占视图宽度的比例
    constexpr int kEmojiShowPx = Proto::kEmojiUiShowSize; // 气泡内表情 24px

    // ---------------- 附件气泡（1c 新增） ----------------
    constexpr int kFileCardWidth  = 240;   // 文件卡片宽度
    constexpr int kFileCardHeight = 62;    // 文件卡片高度
    constexpr int kFileCardPad    = 10;    // 卡片内边距
    constexpr int kFileIconSize   = 40;    // 卡片左侧图标边长
    constexpr int kFileCardNameH  = 18;    // 文件名那一行的高度
    constexpr int kFileCardSizeH  = 14;    // 文件大小那一行的高度
    constexpr int kFileCardLineGap = 2;    // 两行文字之间的间距
    constexpr int kImagePlaceholderW = 200; // 缩略图还没就绪时的占位尺寸
    constexpr int kImagePlaceholderH = 120;
    constexpr int kImageRadius    = 4;     // 图片圆角半径
    constexpr int kImageHeaderH   = 20;    // 图片气泡顶部：文件名 + 类型/大小
    constexpr int kImageHeaderGap = 6;     // 标题行和缩略图之间的间距
    constexpr int kImageHeaderMinW = 160;  // 竖图/窄图时给标题行保留的最小内容宽度

    // 消息文本字体
    QFont bodyFont(const QFont &base = QFont())
    {
        QFont f = base;
        f.setPixelSize(kBodyFontSize);
        return f;
    }

    // 纯文本测量, 计算显示消息文本的宽高
    QRect plainTextBounding(const QString &body, const QFont &font, int textMaxWidth)
    {
        const QFontMetrics fm(font);
        const QRect textRect(0, 0, textMaxWidth, 10000);
        // 在 textRect 这个矩形里折行排版 body，返回文字实际占用的矩形（位置+宽+高）
        return fm.boundingRect(textRect, Qt::AlignLeft | Qt::TextWordWrap | Qt::TextWrapAnywhere, body);
    }

    // 返回文档在 textMaxWidth 限制下的布局尺寸, 含表情，QTextDocument 图文混排测量/绘制
    QSizeF buildEmojiDocument(QTextDocument &doc, const QString &body, const QFont &font,
                                                  const QBrush &textColor,int textMaxWidth)
    {
        doc.setDefaultFont(font);
        doc.setDocumentMargin(0);       // 去掉文档内边距，测量内容本身尺寸
        doc.setUndoRedoEnabled(false);  // 关闭撤销栈，避免测量期间的无用开销

        QTextCharFormat cf;
        cf.setForeground(textColor);
        QTextCursor cur(&doc);
        const auto segs = EmojiText::parse(body);
        for(const EmojiSegment &s : segs) {
            if(!s.isEmoji) {
                cur.insertText(s.text, cf);
                continue;
            }
            const QPixmap map = EmojiText::pixmapCache(s.code, kEmojiShowPx);
            if(map.isNull()) {
                cur.insertText(QStringLiteral("[表情]"), cf); // 资源缺失
                continue;
            }
            const QString src = QStringLiteral("emoji:%1").arg(s.code);
            doc.addResource(QTextDocument::ImageResource, QUrl(src), map);
            QTextImageFormat fmt;
            fmt.setName(src);
            fmt.setWidth(map.width());
            fmt.setHeight(map.height());
            fmt.setVerticalAlignment(QTextCharFormat::AlignBottom);
            cur.insertImage(fmt);
        }

        const qreal idealWidth = doc.documentLayout()->documentSize().width(); // 自然宽度（不折行）
        const qreal finaWidth = qMin(idealWidth, qreal(textMaxWidth));
        doc.setTextWidth(qMax(qreal(1), finaWidth)); // 保护空文档
        return doc.documentLayout()->documentSize();
    }

    // 字节数 → "1.2 MB"（1024 进制，与文件管理器口径一致）
    QString formatFileSize(qint64 bytes)
    {
        if (bytes < 0)
            return QString();
        constexpr qint64 kb = 1024;
        constexpr qint64 mb = kb * 1024;
        constexpr qint64 gb = mb * 1024;
        if (bytes < kb)
            return QStringLiteral("%1 B").arg(bytes);
        if (bytes < mb)
            return QStringLiteral("%1 KB").arg(double(bytes) / kb, 0, 'f', 1);
        if (bytes < gb)
            return QStringLiteral("%1 MB").arg(double(bytes) / mb, 0, 'f', 1);
        return QStringLiteral("%1 GB").arg(double(bytes) / gb, 0, 'f', 2);
    }

    // 取大写扩展名；没有后缀返回空
    QString fileExtension(const QString &fileName)
    {
        const int dot = fileName.lastIndexOf(QLatin1Char('.'));
        if (dot <= 0 || dot == fileName.size() - 1)
            return QString();
        return fileName.mid(dot + 1).toUpper();
    }

    // 图片气泡标题行右侧的"类型"：用文件名后缀，不额外依赖 mime（接收端消息里也没有 mime 字段）
    QString imageTypeLabel(const QString &fileName)
    {
        const QString ext = fileExtension(fileName);
        return ext.isEmpty() ? QStringLiteral("图片") : ext;
    }

    // 文件卡片左侧的"扩展名"色块：不依赖任何图标资源，按大类上色一眼能分出来
    void paintFileBadge(QPainter *painter, const QRect &rect, const QString &fileName)
    {
        QString ext = fileExtension(fileName);
        if (ext.isEmpty())
            ext = QStringLiteral("FILE");
        if (ext.size() > 4)
            ext = ext.left(4);   // 40px 的方块最多放 4 个字符

        QColor color(0x5B, 0x8F, 0xF9);                        // 默认蓝
        if (ext == QStringLiteral("PDF"))                     color = QColor(0xE0, 0x50, 0x55);
        else if (ext.startsWith(QStringLiteral("DOC")))       color = QColor(0x3B, 0x7D, 0xD8);
        else if (ext.startsWith(QStringLiteral("XLS")))       color = QColor(0x2E, 0xA0, 0x6A);
        else if (ext.startsWith(QStringLiteral("PPT")))       color = QColor(0xE0, 0x7B, 0x2F);
        else if (ext == QStringLiteral("ZIP") || ext == QStringLiteral("RAR")
              || ext == QStringLiteral("7Z")  || ext == QStringLiteral("TAR"))
                                                             color = QColor(0xC8, 0x92, 0x2B);

        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawRoundedRect(rect, 6, 6);   // 先铺底色

        QFont f = painter->font();
        f.setPixelSize(ext.size() > 3 ? 9 : 11);   // 4 个字符缩小一号才放得下
        f.setBold(true);
        painter->setFont(f);
        painter->setPen(Qt::white);                 // 再压白字
        painter->drawText(rect, Qt::AlignCenter, ext);
        painter->restore();
    }

    /*
     * 缩略图 → 按目标尺寸绘制前先缩到"设备像素"，与 displayScaledPixmap 同一思路，区别是支持非正方形。
     * 为什么不能直接 painter->drawPixmap(rect, thumb)：
     *   drawPixmap(QRect, QPixmap) 的采样算法由 SmoothPixmapTransform 决定，默认关闭（退化成最近邻），
     *   2x 屏上还会只用一半的物理像素去画 220 逻辑宽度，糊得比 1x 更明显。
     */
    QPixmap scaledForDisplay(const QPixmap &src, const QSize &logicalSize, qreal pixelRatio)
    {
        if (src.isNull() || logicalSize.isEmpty())
            return QPixmap();

        const qreal ratio = (pixelRatio > 0.0) ? pixelRatio : 1.0;
        const QSize px(qRound(logicalSize.width() * ratio), qRound(logicalSize.height() * ratio));
        if (px.isEmpty())
            return src;

        const QString key = QStringLiteral("msg-image:%1:%2x%3")
                                .arg(src.cacheKey()).arg(px.width()).arg(px.height());
        QPixmap out;
        if (QPixmapCache::find(key, &out))
            return out;

        out = src.scaled(px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        out.setDevicePixelRatio(ratio);
        QPixmapCache::insert(key, out);
        return out;
    }

    /*
     * 一条消息的气泡外框尺寸 —— paint() 和 sizeHint() 必须走这一个函数。
     * 为什么必须同源：测量偏小 → 文字被下一行盖住；偏大 → 行间空出一截。
     * 以前两处各写一遍，加图片/文件类型时只要有一处漏改就错位，而且错得很隐蔽。
     * doc 由调用方传进来：paint 里测量完还要用它画表情，不必重建第二个文档。
     */
    struct BubbleMetrics {
        int  width    = 0;      // 气泡外框宽
        int  height   = 0;      // 气泡外框高
        bool hasEmoji = false;  // 文本里有没有表情（决定用 drawText 还是 QTextDocument）
        QSize contentSize;      // 图片在气泡内的显示尺寸（只有图片消息用）
    };

    // 共用同一个: 测量入口
    BubbleMetrics measureBubble(const QModelIndex &index, const QFont &font, int maxBubbleWidth, QTextDocument *doc)
    {
        BubbleMetrics m;
        const int msgType      = index.data(MessageModel::MsgTypeRole).toInt();
        const int textMaxWidth = qMax(1, maxBubbleWidth - bubbleLeftPadding - bubbleRightPadding);

        // ① 图片：顶部一行"文件名 + 类型/大小"，下面缩略图
        if (msgType == Proto::MsgTypeImage) {
            const QPixmap thumb = index.data(MessageModel::ThumbRole).value<QPixmap>();
            const QSize src = thumb.isNull() ? QSize(kImagePlaceholderW, kImagePlaceholderH)
                                             : thumb.size();
            const int contentMaxW = qMax(1, qMin(MessageBubble::kImageMaxW, textMaxWidth));
            m.contentSize = src.scaled(contentMaxW, MessageBubble::kImageMaxH, Qt::KeepAspectRatio);
            if (m.contentSize.isEmpty()) // 尺寸为 0 的坏文件，退回占位尺寸
                m.contentSize = QSize(qMax(1, qMin(kImagePlaceholderW, textMaxWidth)),
                                      kImagePlaceholderH);

            // 竖图/小图也要给标题行留出可读宽度；横图则让标题和图片同宽
            const int headerW = qMax(m.contentSize.width(), qMin(kImageHeaderMinW, contentMaxW));
            m.width  = headerW + bubbleLeftPadding + bubbleRightPadding;
            m.height = kImageHeaderH + kImageHeaderGap
                     + m.contentSize.height() + bubbleTopPadding + bubbleBottomPadding;
            return m;
        }

        // ② 文件：固定尺寸卡片，只受"气泡最大宽度"限制
        if (msgType == Proto::MsgTypeFile) {
            m.width  = qMin(kFileCardWidth, maxBubbleWidth);
            m.height = kFileCardHeight;
            return m;
        }

        // ③ 文本（语音/视频暂时也按文本渲染，等它们有界面了再分支）
        const QString body = index.data(MessageModel::BodyRole).toString();
        m.hasEmoji = body.contains(Proto::kEmojiBegin);
        if (!m.hasEmoji) {
            const QRect r = plainTextBounding(body, font, textMaxWidth);
            m.width  = qMin(maxBubbleWidth, r.width()  + bubbleLeftPadding + bubbleRightPadding);
            m.height = r.height() + bubbleTopPadding  + bubbleBottomPadding;
        } else {
            const QSizeF sz = buildEmojiDocument(*doc, body, font, QBrush(Qt::black), textMaxWidth);
            m.width  = qMin(maxBubbleWidth,
                            qCeil(sz.width())  + 2 + bubbleLeftPadding + bubbleRightPadding);
            m.height = qCeil(sz.height()) + 2 + bubbleTopPadding  + bubbleBottomPadding;
        }
        return m;
    }

    // 气泡在 viewport 坐标系里的矩形。paint/命中测试共用，避免两边各写一套坐标公式后漂移。
    QRect bubbleRectFromMetrics(const QRect &itemRect, bool isSelf, bool showName,
                                const BubbleMetrics &m)
    {
        const int avatarX = isSelf ? itemRect.right() - avatarMargin - avatarSize
                                   : itemRect.left() + avatarMargin;
        int bubbleY = itemRect.top() + bubbleRectTop;
        if (showName)
            bubbleY += nameHeight + nameBubbleGap;

        const int bubbleX = isSelf ? avatarX - avatarBubbleGap - m.width
                                   : avatarX + avatarBubbleGap + avatarSize;
        return QRect(bubbleX, bubbleY, m.width, m.height);
    }
}

MessageDelegate::MessageDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void MessageDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                            const QModelIndex &index) const
{
    // 把 painter 当前的状态（画笔、字体、笔刷等）压栈存档, 不污染 View 之后的其他绘制
    painter->save();
    // 开启抗锯齿：让圆、斜线的边缘平滑，否则头像圆形占位图、文字边缘会有毛刺感。
    painter->setRenderHint(QPainter::Antialiasing);

    // 获取数据
    const QString sender   = index.data(MessageModel::SenderRole).toString();
    const bool    isSelf   = index.data(MessageModel::IsSelfRole).toBool();
    const bool    showName = index.data(MessageModel::ShowNameRole).toBool();
    const quint8  msgType  = quint8(index.data(MessageModel::MsgTypeRole).toInt());
    const qint64  timestamp = index.data(MessageModel::TimestampRole).toLongLong();

    const qreal pixelRatio = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    const QPixmap avatar = displayScaledPixmap(index.data(MessageModel::AvatarRole).value<QPixmap>(),
                                               avatarSize, pixelRatio);

    const QFont font = bodyFont(painter->font());
    painter->setFont(font);

    // 气泡最大宽度 + 气泡实际宽高（sizeHint 走的是同一个 measureBubble，两边不会漂）
    const int maxBubbleWidth = int(option.rect.width() * kMaxBubbleWidthRatio);
    QTextDocument doc;
    const BubbleMetrics metrics = measureBubble(index, font, maxBubbleWidth, &doc);

    // 计算头像位置
    const int avatarX = isSelf ? option.rect.right() - avatarMargin - avatarSize
                               : option.rect.left() + avatarMargin;
    const int avatarY = option.rect.top() + avatarRectTop;

    // 气泡位置（和鼠标命中测试共用同一套几何，避免"看到的位置"和"点到才触发的位置"不一致）
    const QRect bubbleRect = bubbleRectFromMetrics(option.rect, isSelf, showName, metrics);

    // 画头像
    if (!avatar.isNull()) {
        paintRoundedPixmap(painter, QRect(avatarX, avatarY, avatarSize, avatarSize), avatar, 5);
    }

    // 群聊显示名字
    if (showName && !sender.isEmpty()) {
        QFont smallFont = font;
        smallFont.setPixelSize(11);
        painter->setFont(smallFont);
        painter->setPen(QColor(150, 150, 150));
        QFontMetrics nameFm(smallFont);
        int nameWidth = nameFm.horizontalAdvance(sender) + 8;  // 加一点留白
        QRect nameRect;
        if (isSelf) {
            // 自己消息：名字在头像左边，右对齐
            nameRect = QRect(avatarX - nameWidth - nameAvatarGap, avatarY , nameWidth, nameHeight);
            painter->drawText(nameRect, Qt::AlignRight | Qt::AlignVCenter, sender);
        } else {
            // 对方消息：名字在头像右边，左对齐
            nameRect = QRect(avatarX + nameAvatarGap + avatarSize, avatarY, nameWidth, nameHeight);
            painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, sender);
        }
        painter->setFont(font);
        painter->setPen(Qt::black);
    }

    // 画气泡（文件卡片兼作气泡，需要一圈浅边框，否则白底白框看不见边界）
    const QColor selfBubbleColor(0xCC, 0xEB, 0xFF);   // 浅蓝色
    const QColor otherBubbleColor(0xFF, 0xFF, 0xFF);
    painter->setBrush(isSelf ? selfBubbleColor : otherBubbleColor);
    if (msgType == Proto::MsgTypeFile) {
        painter->setPen(QPen(QColor(0xDD, 0xDD, 0xDD), 1));
        painter->drawRoundedRect(bubbleRect.adjusted(0, 0, -1, -1), 6, 6); // 减 1 让描边落在内部
    } else {
        painter->setPen(Qt::NoPen); // 去掉边框线，让气泡只有填充色，没有轮廓
        painter->drawRoundedRect(bubbleRect, 6, 6); // 绘制圆角矩形，圆角半径为 6 像素
    }

    if (msgType == Proto::MsgTypeImage) {
        const QString fileName = index.data(MessageModel::FileNameRole).toString();
        const qint64  fileSize = index.data(MessageModel::FileSizeRole).toLongLong();

        // 顶部一行：文件名（左，省略号）+ 类型/大小（右）
        const QRect headerRect(bubbleRect.left() + bubbleLeftPadding,
                               bubbleRect.top() + bubbleTopPadding,
                               bubbleRect.width() - bubbleLeftPadding - bubbleRightPadding,
                               kImageHeaderH);

        QString meta = imageTypeLabel(fileName);
        if (fileSize > 0)
            meta += QStringLiteral(" · ") + formatFileSize(fileSize);

        QFont metaFont = font;
        metaFont.setPixelSize(11);
        QFont nameFont = font;
        nameFont.setPixelSize(12);

        const QFontMetrics metaFm(metaFont);
        const int metaW = metaFm.horizontalAdvance(meta);
        painter->setFont(metaFont);
        painter->setPen(QColor(0x88, 0x88, 0x88));
        painter->drawText(headerRect, Qt::AlignRight | Qt::AlignVCenter, meta);

        const QString shownName = fileName.isEmpty() ? QStringLiteral("图片") : fileName;
        const int nameW = qMax(0, headerRect.width() - metaW - 8);
        if (nameW > 0) {
            const QFontMetrics nameFm(nameFont);
            painter->setFont(nameFont);
            painter->setPen(QColor(0x22, 0x22, 0x22));
            painter->drawText(QRect(headerRect.left(), headerRect.top(), nameW, headerRect.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              nameFm.elidedText(shownName, Qt::ElideMiddle, nameW));
        }

        // 缩略图在标题行下方；竖图/窄图在标题宽度内居中
        const int imageX = bubbleRect.left() + bubbleLeftPadding
                         + (headerRect.width() - metrics.contentSize.width()) / 2;
        const int imageY = bubbleRect.top() + bubbleTopPadding + kImageHeaderH + kImageHeaderGap;
        const QRect imageRect(imageX, imageY,
                              metrics.contentSize.width(), metrics.contentSize.height());

        const QPixmap thumb = index.data(MessageModel::ThumbRole).value<QPixmap>();
        if (!thumb.isNull()) {
            paintRoundedPixmap(painter, imageRect,
                               scaledForDisplay(thumb, metrics.contentSize, pixelRatio), kImageRadius);
        } else {
            // 缩略图还没生成/还没下载：画占位框，行高就不会跳变
            painter->setPen(QPen(QColor(0xDF, 0xDF, 0xDF), 1));
            painter->setBrush(QColor(0xF2, 0xF2, 0xF2));
            painter->drawRoundedRect(imageRect.adjusted(0, 0, -1, -1), kImageRadius, kImageRadius);
            painter->setPen(QColor(0x9A, 0x9A, 0x9A));
            painter->drawText(imageRect, Qt::AlignCenter, QStringLiteral("点击下载"));
        }
    } else if (msgType == Proto::MsgTypeFile) {
        // 文件卡片：左边扩展名色块 + 右边两行（文件名 / 大小）
        const QString fileName = index.data(MessageModel::FileNameRole).toString();
        const qint64  fileSize = index.data(MessageModel::FileSizeRole).toLongLong();

        const QRect iconRect(bubbleRect.left() + kFileCardPad,
                             bubbleRect.top() + (bubbleRect.height() - kFileIconSize) / 2,
                             kFileIconSize, kFileIconSize);
        paintFileBadge(painter, iconRect, fileName);

        const int textLeft  = iconRect.right() + 1 + kFileCardPad;
        const int textWidth = qMax(1, bubbleRect.right() - kFileCardPad - textLeft + 1);
        const int textBlockH = kFileCardNameH + kFileCardLineGap + kFileCardSizeH;
        const int textTop    = bubbleRect.top() + qMax(0, (bubbleRect.height() - textBlockH) / 2);

        QFont nameFont = font;
        nameFont.setPixelSize(13);
        painter->setFont(nameFont);
        painter->setPen(QColor(0x22, 0x22, 0x22));
        const QFontMetrics nameFm(nameFont);
        const QString shownName = fileName.isEmpty() ? QStringLiteral("文件") : fileName;
        painter->drawText(QRect(textLeft, textTop, textWidth, kFileCardNameH),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          nameFm.elidedText(shownName, Qt::ElideMiddle, textWidth)); // 中间省略，保住扩展名

        QFont sizeFont = font;
        sizeFont.setPixelSize(11);
        painter->setFont(sizeFont);
        painter->setPen(QColor(0x88, 0x88, 0x88));
        painter->drawText(QRect(textLeft, textTop + kFileCardNameH + kFileCardLineGap,
                                textWidth, kFileCardSizeH),
                          Qt::AlignLeft | Qt::AlignVCenter, formatFileSize(fileSize));
    } else {
        // 文本：画文字
        const QRect innerTextRect = bubbleRect.adjusted(bubbleLeftPadding, bubbleTopPadding,
                                                        -bubbleRightPadding, -bubbleBottomPadding); //将气泡矩形缩小8像素
        if (!metrics.hasEmoji) {
            const QString body = index.data(MessageModel::BodyRole).toString();
            painter->setFont(font);
            painter->setPen(Qt::black);
            painter->drawText(innerTextRect, Qt::AlignLeft | Qt::AlignVCenter |
                                        Qt::TextWordWrap | Qt::TextWrapAnywhere, body);
        } else { // 图文混排渲染
            painter->save();
            painter->setClipRect(innerTextRect);
            painter->translate(innerTextRect.topLeft());
            QAbstractTextDocumentLayout::PaintContext ctx;
            ctx.palette.setColor(QPalette::Text, Qt::black);
            doc.documentLayout()->draw(painter, ctx);
            painter->restore();
        }
    }

    // 画时间, 悬停时显示
    Q_UNUSED(timestamp);

    painter->restore(); // 恢复 save 存档的状态
}

QSize MessageDelegate::sizeHint(const QStyleOptionViewItem &option,
                                const QModelIndex &index) const
{
    const bool showName = index.data(MessageModel::ShowNameRole).toBool();

    const int maxBubbleWidth = int(option.rect.width() * kMaxBubbleWidthRatio);
    const QFont font = bodyFont();

    // 测量与 paint 走到同一个 measureBubble：否则行高不一致导致滚动错位
    QTextDocument doc;
    const BubbleMetrics metrics = measureBubble(index, font, maxBubbleWidth, &doc);

    const int avatarBottom = avatarRectTop + avatarSize;
    int bubbleBottom = bubbleRectTop + metrics.height;
    if (showName)
        bubbleBottom += nameHeight + nameBubbleGap;

    const int contentHeight = qMax(avatarBottom, bubbleBottom);
    return QSize(option.rect.width(), contentHeight + avatarRectBottom);
}

// QListView 点击只响应气泡区域
QRect MessageDelegate::bubbleRect(const QRect &itemRect, const QModelIndex &index) const
{
    if (!index.isValid() || itemRect.isEmpty())
        return QRect();

    const bool isSelf   = index.data(MessageModel::IsSelfRole).toBool();
    const bool showName = index.data(MessageModel::ShowNameRole).toBool();

    const int maxBubbleWidth = int(itemRect.width() * kMaxBubbleWidthRatio);
    const QFont font = bodyFont();

    QTextDocument doc;
    const BubbleMetrics metrics = measureBubble(index, font, maxBubbleWidth, &doc);
    return bubbleRectFromMetrics(itemRect, isSelf, showName, metrics);
}
