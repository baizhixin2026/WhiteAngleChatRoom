#ifndef EMOJITEXT_H
#define EMOJITEXT_H

/*
 * EmojiText — 表情标记文本的解析/校验/取图
 * 协议格式: 消息正文中表情为  kEmojiBegin(0xE000) + code + kEmojiEnd(0xE001)
 * code 示例 "face-100001"，资源路径 :/image/emoji/<前缀>/<code>.png
 * 消费方:
 *   - MessageDelegate      气泡渲染(24px) + 测量
 *   - MainWindow           输入框插入(18px)
 *   - 后续: 最近会话预览/通知栏 摘要文本（用 parse() 拼 "[表情]"）
 * 线程纪律: pixmap() 内部用 QPixmapCache，只允许在 GUI 线程调用。
 */

#include <QString>
#include <QStringList>
#include <QVector>
#include <QPixmap>
#include <QPixmapCache>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QTextCharFormat>
#include "protocoltypes.h"

struct EmojiSegment {
    bool isEmoji = false;
    QString text;   // isEmoji == false 时有效（原始文本，未转义）
    QString code;   // isEmoji == true  时有效，如 "face-100001"
};

namespace EmojiText {

    // 表情分组前缀唯一来源
    inline const QStringList &emojiPrefix() {
        static const QStringList k = { Proto::kEmojiFace, Proto::kEmojiHand, Proto::kEmojiOther };
        return k;
    }

    // 白名单校验: 前缀必须在 allowedPrefixes 内，且编号为正整数
    inline bool isValidEmojiCode(const QString &code)
    {
        const int dash = code.indexOf(QLatin1Char('-'));
        if(dash <= 0 || dash + 1 >= code.size())
            return false;
        if(!emojiPrefix().contains(code.left(dash)))
            return false;
        bool ok = false;
        const int num = code.mid(dash + 1).toInt(&ok);
        return ok && num > 0;
    }

    inline QString emojiResourcePath(const QString &code)
    {
        const QString prefix = code.section(QLatin1Char('-'), 0, 0);
        return QStringLiteral(":/image/emoji/%1/%2.png").arg(prefix, code);
    }

    // 带 QPixmapCache 的取图: key 含 size，输入框 18pix / 气泡 24pix 两档共存。
    // 加载失败不进缓存，避免 null 记录累积
    // QPixmapCache 是全局池，同一 code 的 18/24 两档必须用不同 key，否则互相污染
    inline QPixmap pixmapCache(const QString &code, int size)
    {
        if(!isValidEmojiCode(code))
            return QPixmap();
        const QString key = QStringLiteral("emoji:%1:%2").arg(code).arg(size);
        QPixmap map;
        if(QPixmapCache::find(key, &map))
            return map;
        map.load(emojiResourcePath(code));
        if(map.isNull())
            return map;
        map = map.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmapCache::insert(key, map);
        return map;
    }

    // 解析: kEmojiBegin + code + kEmojiEnd → 表情段；其余 → 文本段。
    // 未配对的 begin 降级为普通字符（用户手敲的私有区字符，协议无损）
    // 配对完整的 code（含非法）一律作为表情段，合法性交给 pixmap() 兜底 [表情]
    inline QVector<EmojiSegment> parse(const QString &rawText)
    {
        QVector<EmojiSegment> segs;
        QString buf; // 文本段
        auto flushText = [&segs, &buf]() {
            if(!buf.isEmpty()) {
                EmojiSegment s;
                s.text = buf;
                segs.append(s);
                buf.clear();
            }
        };
        for(int i = 0; i < rawText.size(); ) {
            if(rawText.at(i) == Proto::kEmojiBegin) {
                const int end = rawText.indexOf(Proto::kEmojiEnd, i+1);
                if(end > i) { // 配对存在
                    const QString code = rawText.mid(i + 1, end -i - 1);
//                    if(isValidEmojiCode(code)) {
                        flushText();
                        EmojiSegment s;
                        s.isEmoji = true;
                        s.code = code;
                        segs.append(s);
                        i = end + 1;
                        continue;
//                    }
                }
                // end == -1（没有找到 end）或者 end > i 但是 code 无效
                buf += rawText.at(i);
                ++i;
            } else {
                buf += rawText.at(i);
                ++i;
            }
        }
        flushText();
        return segs;
    }

    inline bool insertTo(QTextCursor &cur, QTextDocument *doc, const QString &code, int size)
    {
        const QPixmap pm = pixmapCache(code, size); // 走缓存
        if (pm.isNull())
            return false;
        const QString src = QStringLiteral("emoji:%1").arg(code);
        // 注册到文档资源表 ， 映射：资源键src，资源pm ， QTextEdit 通过 URL 引用文档资源表
        doc->addResource(QTextDocument::ImageResource, QUrl(src), pm);
        QTextImageFormat fmt;
        fmt.setName(src);
        fmt.setWidth(pm.width());
        fmt.setHeight(pm.height());
        fmt.setVerticalAlignment(QTextCharFormat::AlignMiddle);
        cur.insertImage(fmt);
        return true;
    }

    // 摘要文本: 表情字段 → [表情]，其余原样。 复制为纯文本 / 最近会话预览 使用
    inline QString toPlainSummary(const QString &raw)
    {
        QString out;
        const auto segs = parse(raw);
        out.reserve(raw.size());
        for (const EmojiSegment &s : segs)
            out += s.isEmoji ? QStringLiteral("[表情]") : s.text;
        return out;
    }
}

#endif // EMOJITEXT_H
