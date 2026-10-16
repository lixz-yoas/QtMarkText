#include "markdownparser.h"

//-------------------------------------------------
// 块级辅助判断
//-------------------------------------------------

bool MarkdownParser::isHorizontalRule(const QString &line)
{
    if (line.size() < 3) {
        return false;
    }
    const QChar c = line.at(0);
    if (c != QLatin1Char('-') && c != QLatin1Char('*') && c != QLatin1Char('_')) {
        return false;
    }
    int nCount = 0;
    for (const QChar ch : line) {
        if (ch == c) {
            ++nCount;
        } else if (!ch.isSpace()) {
            return false;
        }
    }
    return nCount >= 3;
}

bool MarkdownParser::isUnorderedMarker(const QString &line)
{
    if (line.size() < 2) {
        return false;
    }
    const QChar c = line.at(0);
    if (c != QLatin1Char('-') && c != QLatin1Char('*') && c != QLatin1Char('+')) {
        return false;
    }
    return line.at(1).isSpace();
}

//-------------------------------------------------
// 行内解析
//-------------------------------------------------

QVector<MdInline> MarkdownParser::parseInline(const QString &text)
{
    QVector<MdInline> out;
    QString           buf;
    const int         n = text.size();
    int               i = 0;

    auto flushText = [&out, &buf]() {
        if (!buf.isEmpty()) {
            MdInline in;
            in.type = MdInlineType::Text;
            in.text = buf;
            out.append(in);
            buf.clear();
        }
    };

    while (i < n) {
        const QChar c = text.at(i);

        // 反斜杠转义
        if (c == QLatin1Char('\\') && (i + 1) < n) {
            buf.append(text.at(i + 1));
            i += 2;
            continue;
        }

        // 行内代码
        if (c == QLatin1Char('`')) {
            const int j = text.indexOf(QLatin1Char('`'), i + 1);
            if (j > i) {
                flushText();
                MdInline in;
                in.type = MdInlineType::Code;
                in.text = text.mid(i + 1, j - i - 1);
                out.append(in);
                i = j + 1;
                continue;
            }
        }

        // 图片 ![alt](src)
        if (c == QLatin1Char('!') && (i + 1) < n && text.at(i + 1) == QLatin1Char('[')) {
            const int close = text.indexOf(QStringLiteral("]("), i + 2);
            if (close > 0) {
                const int end = text.indexOf(QLatin1Char(')'), close + 2);
                if (end > 0) {
                    flushText();
                    MdInline in;
                    in.type = MdInlineType::Image;
                    in.text = text.mid(i + 2, close - i - 2);
                    in.url = text.mid(close + 2, end - close - 2);
                    out.append(in);
                    i = end + 1;
                    continue;
                }
            }
        }

        // 链接 [text](url)
        if (c == QLatin1Char('[')) {
            const int close = text.indexOf(QStringLiteral("]("), i + 1);
            if (close > 0) {
                const int end = text.indexOf(QLatin1Char(')'), close + 2);
                if (end > 0) {
                    flushText();
                    MdInline in;
                    in.type = MdInlineType::Link;
                    in.text = text.mid(i + 1, close - i - 1);
                    in.url = text.mid(close + 2, end - close - 2);
                    out.append(in);
                    i = end + 1;
                    continue;
                }
            }
        }

        // 粗斜体 ***x***
        if (c == QLatin1Char('*') && (i + 2) < n
            && text.at(i + 1) == QLatin1Char('*') && text.at(i + 2) == QLatin1Char('*')) {
            const int j = text.indexOf(QStringLiteral("***"), i + 3);
            if (j > i) {
                flushText();
                MdInline in;
                in.type = MdInlineType::BoldItalic;
                in.text = text.mid(i + 3, j - i - 3);
                out.append(in);
                i = j + 3;
                continue;
            }
        }

        // 粗体 **x** / __x__
        if ((c == QLatin1Char('*') || c == QLatin1Char('_')) && (i + 1) < n && text.at(i + 1) == c) {
            const QString mark = QString(2, c);
            const int     j = text.indexOf(mark, i + 2);
            if (j > i) {
                flushText();
                MdInline in;
                in.type = MdInlineType::Bold;
                in.text = text.mid(i + 2, j - i - 2);
                out.append(in);
                i = j + 2;
                continue;
            }
        }

        // 删除线 ~~x~~
        if (c == QLatin1Char('~') && (i + 1) < n && text.at(i + 1) == QLatin1Char('~')) {
            const int j = text.indexOf(QStringLiteral("~~"), i + 2);
            if (j > i) {
                flushText();
                MdInline in;
                in.type = MdInlineType::Strike;
                in.text = text.mid(i + 2, j - i - 2);
                out.append(in);
                i = j + 2;
                continue;
            }
        }

        // 斜体 *x* / _x_
        if (c == QLatin1Char('*') || c == QLatin1Char('_')) {
            const int j = text.indexOf(c, i + 1);
            if (j > i) {
                flushText();
                MdInline in;
                in.type = MdInlineType::Italic;
                in.text = text.mid(i + 1, j - i - 1);
                out.append(in);
                i = j + 1;
                continue;
            }
        }

        buf.append(c);
        ++i;
    }

    flushText();
    return out;
}

//-------------------------------------------------
// 块级解析
//-------------------------------------------------

QVector<MdBlock> MarkdownParser::parse(const QString &markdown)
{
    QVector<MdBlock> blocks;
    QStringList      lines = markdown.split(QLatin1Char('\n'));

    bool        bInFence = false;
    MdBlock     fenceBlock;
    QStringList fenceLines;

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines.at(i);
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }

        // ---- 围栏代码块内部 ----
        if (bInFence) {
            const QString t = line.trimmed();
            if (t.startsWith(QStringLiteral("```")) || t.startsWith(QStringLiteral("~~~"))) {
                fenceBlock.rawText = fenceLines.join(QLatin1Char('\n'));
                blocks.append(fenceBlock);
                fenceLines.clear();
                fenceBlock = MdBlock();
                bInFence = false;
            } else {
                fenceLines.append(line);
            }
            continue;
        }

        const QString sLine = line.trimmed();
        if (sLine.isEmpty()) {
            continue;
        }

        // ---- 围栏代码块开始 ----
        if (sLine.startsWith(QStringLiteral("```")) || sLine.startsWith(QStringLiteral("~~~"))) {
            bInFence = true;
            fenceBlock = MdBlock();
            fenceBlock.type = MdBlockType::CodeFence;
            fenceBlock.info = sLine.mid(3).trimmed();
            fenceLines.clear();
            continue;
        }

        // ---- 分隔线 ----
        if (isHorizontalRule(sLine)) {
            MdBlock b;
            b.type = MdBlockType::HorizontalRule;
            blocks.append(b);
            continue;
        }

        // ---- ATX 标题 ----
        if (sLine.startsWith(QLatin1Char('#'))) {
            int n = 0;
            while (n < sLine.size() && sLine.at(n) == QLatin1Char('#')) {
                ++n;
            }
            if (n <= 6 && n < sLine.size() && sLine.at(n) == QLatin1Char(' ')) {
                QString content = sLine.mid(n + 1).trimmed();
                while (content.endsWith(QLatin1Char('#'))) {
                    content.chop(1);
                }
                content = content.trimmed();

                MdBlock b;
                b.type = MdBlockType::Heading;
                b.level = n;
                b.rawText = content;
                b.inlines = parseInline(content);
                blocks.append(b);
                continue;
            }
        }

        // ---- 引用 ----
        if (sLine.startsWith(QLatin1Char('>'))) {
            QString content = sLine.mid(1);
            if (content.startsWith(QLatin1Char(' '))) {
                content = content.mid(1);
            }
            MdBlock b;
            b.type = MdBlockType::Quote;
            b.rawText = content;
            b.inlines = parseInline(content);
            blocks.append(b);
            continue;
        }

        // ---- 无序列表 / 任务列表 ----
        if (isUnorderedMarker(sLine)) {
            QString content = sLine.mid(1).trimmed();

            MdBlock b;
            b.type = MdBlockType::ListItem;
            b.ordered = false;

            if (content.startsWith(QStringLiteral("[ ]"))) {
                b.task = true;
                b.checked = false;
                content = content.mid(3).trimmed();
            } else if (content.startsWith(QStringLiteral("[x]"))
                       || content.startsWith(QStringLiteral("[X]"))) {
                b.task = true;
                b.checked = true;
                content = content.mid(3).trimmed();
            }

            b.rawText = content;
            b.inlines = parseInline(content);
            blocks.append(b);
            continue;
        }

        // ---- 有序列表 ----
        {
            int nDot = -1;
            for (int k = 0; k < sLine.size(); ++k) {
                const QChar ch = sLine.at(k);
                if (ch.isDigit()) {
                    continue;
                }
                if (ch == QLatin1Char('.') && k > 0) {
                    nDot = k;
                }
                break;
            }
            if (nDot > 0 && (nDot + 1) < sLine.size() && sLine.at(nDot + 1) == QLatin1Char(' ')) {
                MdBlock b;
                b.type = MdBlockType::ListItem;
                b.ordered = true;
                b.order = sLine.left(nDot).toInt();

                const QString content = sLine.mid(nDot + 1).trimmed();
                b.rawText = content;
                b.inlines = parseInline(content);
                blocks.append(b);
                continue;
            }
        }

        // ---- 普通段落 ----
        {
            MdBlock b;
            b.type = MdBlockType::Paragraph;
            b.rawText = sLine;
            b.inlines = parseInline(sLine);
            blocks.append(b);
        }
    }

    // 未闭合的代码块也要落盘
    if (bInFence) {
        fenceBlock.rawText = fenceLines.join(QLatin1Char('\n'));
        blocks.append(fenceBlock);
    }

    return blocks;
}
