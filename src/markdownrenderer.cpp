#include "markdownrenderer.h"

#include <QColor>
#include <QFont>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextListFormat>

namespace {

const QColor kColorQuote(0x60, 0x60, 0x60);
const QColor kColorCodeBg(0xF0, 0xF0, 0xF0);
const QColor kColorCodeBlockBg(0xF5, 0xF5, 0xF5);
const QColor kColorLink(0x1E, 0x66, 0xC0);
const QColor kColorRule(0xC0, 0xC0, 0xC0);

const char *const kMonoFamily = "Consolas";

/// 标题字号：一级 24pt，逐级递减 3pt
qreal headingPointSize(int level)
{
    return 24.0 - (level - 1) * 3.0;
}

/// 该块类型作用到每个字符上的基础字符格式
QTextCharFormat baseCharFormat(const MdBlock &b)
{
    QTextCharFormat fmt;

    if (b.type == MdBlockType::Heading) {
        fmt.setFontWeight(QFont::Bold);
        fmt.setFontPointSize(headingPointSize(b.level));
    } else if (b.type == MdBlockType::Quote) {
        fmt.setForeground(kColorQuote);
        fmt.setFontItalic(true);
    }

    return fmt;
}

/// 叠加行内元素自身的字符格式
void mergeInlineFormat(const MdInline &in, QTextCharFormat &fmt)
{
    switch (in.type) {
    case MdInlineType::Text:
        break;
    case MdInlineType::Bold:
        fmt.setFontWeight(QFont::Bold);
        break;
    case MdInlineType::Italic:
        fmt.setFontItalic(true);
        break;
    case MdInlineType::BoldItalic:
        fmt.setFontWeight(QFont::Bold);
        fmt.setFontItalic(true);
        break;
    case MdInlineType::Strike:
        fmt.setFontStrikeOut(true);
        break;
    case MdInlineType::Code:
        fmt.setFontFamily(QString::fromLatin1(kMonoFamily));
        fmt.setBackground(kColorCodeBg);
        break;
    case MdInlineType::Link:
    case MdInlineType::Image:
        fmt.setForeground(kColorLink);
        fmt.setFontUnderline(true);
        break;
    }
}

/// 块级格式
QTextBlockFormat blockFormat(const MdBlock &b)
{
    QTextBlockFormat bf;

    switch (b.type) {
    case MdBlockType::Heading:
        bf.setTopMargin(12);
        bf.setBottomMargin(8);
        break;
    case MdBlockType::Quote:
        bf.setLeftMargin(24);
        bf.setTopMargin(4);
        bf.setBottomMargin(4);
        break;
    case MdBlockType::CodeFence:
        bf.setTopMargin(6);
        bf.setBottomMargin(6);
        break;
    case MdBlockType::ListItem:
        bf.setTopMargin(2);
        bf.setBottomMargin(2);
        break;
    case MdBlockType::HorizontalRule:
    case MdBlockType::Paragraph:
    default:
        bf.setTopMargin(2);
        bf.setBottomMargin(6);
        break;
    }

    return bf;
}

} // namespace

void MarkdownRenderer::render(const QVector<MdBlock> &blocks, QTextDocument *doc)
{
    if (doc == nullptr) {
        return;
    }

    doc->clear();

    QTextCursor cur(doc);
    cur.movePosition(QTextCursor::End);

    QTextList *pList = nullptr;
    bool       bListOrdered = false;

    for (int i = 0; i < blocks.size(); ++i) {
        const MdBlock &b = blocks.at(i);

        if (i > 0) {
            cur.insertBlock();
        }

        // 非列表项一律断开当前列表
        if (b.type != MdBlockType::ListItem) {
            pList = nullptr;
        }

        // --- 列表容器 ---
        if (b.type == MdBlockType::ListItem) {
            if (pList == nullptr || bListOrdered != b.ordered) {
                QTextListFormat lf;
                lf.setStyle(b.ordered ? QTextListFormat::ListDecimal
                                      : QTextListFormat::ListDisc);
                lf.setIndent(1);
                pList = cur.createList(lf);
                bListOrdered = b.ordered;
            } else {
                pList->add(cur.block());
            }
        } else {
            cur.setBlockFormat(blockFormat(b));
        }

        // --- 块内容 ---
        if (b.type == MdBlockType::HorizontalRule) {
            QTextCharFormat fmt;
            fmt.setForeground(kColorRule);
            cur.insertText(QString(40, QChar(0x2014)), fmt);   // ————
            continue;
        }

        if (b.type == MdBlockType::CodeFence) {
            QTextCharFormat fmt;
            fmt.setFontFamily(QString::fromLatin1(kMonoFamily));
            fmt.setFontPointSize(10.5);
            fmt.setBackground(kColorCodeBlockBg);
            cur.insertText(b.rawText, fmt);
            continue;
        }

        // 任务列表的勾选框
        if (b.task) {
            QTextCharFormat fmt;
            fmt.setFontFamily(QStringLiteral("Segoe UI Symbol"));
            cur.insertText(b.checked ? QStringLiteral("\u2611 ")
                                     : QStringLiteral("\u2610 "), fmt);
        }

        const QTextCharFormat base = baseCharFormat(b);
        for (const MdInline &in : b.inlines) {
            QTextCharFormat fmt = base;
            mergeInlineFormat(in, fmt);
            cur.insertText(in.text, fmt);
        }
    }

    cur.movePosition(QTextCursor::Start);
}
