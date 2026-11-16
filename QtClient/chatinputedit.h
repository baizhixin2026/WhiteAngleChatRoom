#ifndef CHATINPUTEDIT_H
#define CHATINPUTEDIT_H

#include "emojitext.h"
#include "protocoltypes.h"

#include <QTextEdit>
#include <QMimeData>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTextImageFormat>
#include <QTextDocument>
#include <QUrl>
#include <QPixmap>
#include <QFontMetrics>

/*
 * ChatInputEdit — 聊天输入框
 * 唯一附加行为：粘贴 "kEmojiBegin+code+kEmojiEnd" 标记时，
 * 自动还原成输入框内的 18px 表情图（与 insertEmojiToUiInput 行为一致）。
 * 普通文本 / HTML / 文件粘贴行为与 QTextEdit 完全一致。
 */
class ChatInputEdit : public QTextEdit
{
    Q_OBJECT
public:
    explicit ChatInputEdit(QWidget *parent = nullptr) : QTextEdit(parent) {}

protected:
    void insertFromMimeData(const QMimeData *source) override
    {
        // 无纯文本（如粘贴文件）→ 交给基类
        if (!source || !source->hasText()) {
            QTextEdit::insertFromMimeData(source);
            return;
        }
        const QString text = source->text();
        // 不含表情标记 → 交给基类，保留原本的段落/换行处理
        if (!text.contains(Proto::kEmojiBegin)) {
            QTextEdit::insertFromMimeData(source);
            return;
        }

        // 含表情标记：逐段插入 文本 / 18px 图片
        const int emojiSize = qMax(Proto::kEmojiUiInputSize, fontMetrics().height());
        QTextCursor cur = textCursor();
        cur.beginEditBlock();
        for (const EmojiSegment &s : EmojiText::parse(text)) {
            if (!s.isEmoji) {
                cur.insertText(s.text);
                continue;
            }
            if (!EmojiText::insertTo(cur, document(), s.code, emojiSize))
                cur.insertText(QStringLiteral("[表情]"));
        }
        cur.endEditBlock();
        setTextCursor(cur);
    }
};

#endif // CHATINPUTEDIT_H
