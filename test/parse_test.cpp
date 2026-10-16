#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVector>

#include "markdownparser.h"

//-------------------------------------------------
// 无 GUI 自测：验证 Markdown 解析器的块级 / 行内解析结果
// 结果写入同目录 parse_result.txt（不依赖 qDebug 输出）
//-------------------------------------------------

namespace {

int         g_pass = 0;
int         g_fail = 0;
QStringList g_log;

void check(bool cond, const QString &what)
{
    if (cond) {
        ++g_pass;
        g_log << QStringLiteral("  [PASS] ") + what;
    } else {
        ++g_fail;
        g_log << QStringLiteral("  [FAIL] ") + what;
    }
}

QString blockName(MdBlockType t)
{
    switch (t) {
    case MdBlockType::Paragraph:      return QStringLiteral("Paragraph");
    case MdBlockType::Heading:        return QStringLiteral("Heading");
    case MdBlockType::Quote:          return QStringLiteral("Quote");
    case MdBlockType::CodeFence:      return QStringLiteral("CodeFence");
    case MdBlockType::ListItem:       return QStringLiteral("ListItem");
    case MdBlockType::HorizontalRule: return QStringLiteral("HorizontalRule");
    }
    return QStringLiteral("?");
}

QString inlineName(MdInlineType t)
{
    switch (t) {
    case MdInlineType::Text:       return QStringLiteral("Text");
    case MdInlineType::Bold:       return QStringLiteral("Bold");
    case MdInlineType::Italic:     return QStringLiteral("Italic");
    case MdInlineType::BoldItalic: return QStringLiteral("BoldItalic");
    case MdInlineType::Strike:     return QStringLiteral("Strike");
    case MdInlineType::Code:       return QStringLiteral("Code");
    case MdInlineType::Link:       return QStringLiteral("Link");
    case MdInlineType::Image:      return QStringLiteral("Image");
    }
    return QStringLiteral("?");
}

int countBlock(const QVector<MdBlock> &blocks, MdBlockType t)
{
    int n = 0;
    for (const MdBlock &b : blocks) {
        if (b.type == t) {
            ++n;
        }
    }
    return n;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString md = QStringLiteral(
        "# 一级标题\n"
        "\n"
        "这是一个普通段落，含 **粗体**、*斜体* 与 `行内代码`。\n"
        "\n"
        "## 二级标题\n"
        "\n"
        "- 无序项一\n"
        "- [ ] 待办事项\n"
        "- [x] 已完成事项\n"
        "\n"
        "1. 有序一\n"
        "2. 有序二\n"
        "\n"
        "> 引用文本\n"
        "\n"
        "---\n"
        "\n"
        "```cpp\n"
        "int main() { return 0; }\n"
        "```\n"
        "\n"
        "[链接](https://example.com) 与 ![图片](a.png) 以及 ~~删除~~ 和 ***粗斜***。\n");

    const QVector<MdBlock> blocks = MarkdownParser::parse(md);

    g_log << QStringLiteral("块总数 = %1").arg(blocks.size());
    for (int i = 0; i < blocks.size(); ++i) {
        const MdBlock &b = blocks.at(i);
        g_log << QStringLiteral("  #%1 %2 level=%3 task=%4 checked=%5 raw=\"%6\"")
                 .arg(i)
                 .arg(blockName(b.type))
                 .arg(b.level)
                 .arg(b.task ? 1 : 0)
                 .arg(b.checked ? 1 : 0)
                 .arg(b.rawText.left(28));
    }
    g_log << QStringLiteral("---");

    check(countBlock(blocks, MdBlockType::Heading) == 2, QStringLiteral("标题块数 == 2"));
    check(countBlock(blocks, MdBlockType::Paragraph) == 2, QStringLiteral("段落块数 == 2"));
    check(countBlock(blocks, MdBlockType::ListItem) == 5, QStringLiteral("列表项数 == 5"));
    check(countBlock(blocks, MdBlockType::Quote) == 1, QStringLiteral("引用块数 == 1"));
    check(countBlock(blocks, MdBlockType::CodeFence) == 1, QStringLiteral("代码块数 == 1"));
    check(countBlock(blocks, MdBlockType::HorizontalRule) == 1, QStringLiteral("分隔线数 == 1"));

    int nTask = 0;
    int nChecked = 0;
    for (const MdBlock &b : blocks) {
        if (b.task) {
            ++nTask;
            if (b.checked) {
                ++nChecked;
            }
        }
    }
    check(nTask == 2, QStringLiteral("任务项数 == 2"));
    check(nChecked == 1, QStringLiteral("已勾选任务数 == 1"));

    // 代码块内容
    bool bCodeOk = false;
    for (const MdBlock &b : blocks) {
        if (b.type == MdBlockType::CodeFence) {
            bCodeOk = (b.info == QStringLiteral("cpp"))
                      && b.rawText.contains(QStringLiteral("int main()"));
        }
    }
    check(bCodeOk, QStringLiteral("代码块语言标识与内容正确"));

    // 标题级别
    bool bLevelOk = false;
    for (const MdBlock &b : blocks) {
        if (b.type == MdBlockType::Heading && b.level == 2) {
            bLevelOk = true;
        }
    }
    check(bLevelOk, QStringLiteral("二级标题 level == 2"));

    // ---- 行内解析 ----
    {
        const QVector<MdInline> ins = MarkdownParser::parseInline(
            QStringLiteral("A **粗** B *斜* C `码` D [链](u) E ![图](p) F ~~删~~ G ***粗斜***"));

        g_log << QStringLiteral("---");
        g_log << QStringLiteral("行内片段数 = %1").arg(ins.size());
        for (const MdInline &in : ins) {
            g_log << QStringLiteral("    %1 text=\"%2\" url=\"%3\"")
                     .arg(inlineName(in.type))
                     .arg(in.text)
                     .arg(in.url);
        }

        int nBold = 0;
        int nItalic = 0;
        int nCode = 0;
        int nLink = 0;
        int nImg = 0;
        int nStrike = 0;
        int nBI = 0;
        for (const MdInline &in : ins) {
            switch (in.type) {
            case MdInlineType::Bold:       ++nBold;   break;
            case MdInlineType::Italic:     ++nItalic; break;
            case MdInlineType::Code:       ++nCode;   break;
            case MdInlineType::Link:       ++nLink;   break;
            case MdInlineType::Image:      ++nImg;    break;
            case MdInlineType::Strike:     ++nStrike; break;
            case MdInlineType::BoldItalic: ++nBI;     break;
            case MdInlineType::Text:                  break;
            }
        }
        check(nBold == 1,   QStringLiteral("行内粗体 == 1"));
        check(nItalic == 1, QStringLiteral("行内斜体 == 1"));
        check(nCode == 1,   QStringLiteral("行内代码 == 1"));
        check(nLink == 1,   QStringLiteral("链接 == 1"));
        check(nImg == 1,    QStringLiteral("图片 == 1"));
        check(nStrike == 1, QStringLiteral("删除线 == 1"));
        check(nBI == 1,     QStringLiteral("粗斜体 == 1"));

        // 链接目标
        bool bUrlOk = false;
        for (const MdInline &in : ins) {
            if (in.type == MdInlineType::Link) {
                bUrlOk = (in.text == QStringLiteral("链")) && (in.url == QStringLiteral("u"));
            }
        }
        check(bUrlOk, QStringLiteral("链接文本与地址解析正确"));
    }

    // ---- 边界判断 ----
    check(MarkdownParser::isHorizontalRule(QStringLiteral("---")),  QStringLiteral("--- 是分隔线"));
    check(MarkdownParser::isHorizontalRule(QStringLiteral("***")),  QStringLiteral("*** 是分隔线"));
    check(!MarkdownParser::isHorizontalRule(QStringLiteral("--")),  QStringLiteral("--  不是分隔线"));
    check(MarkdownParser::isUnorderedMarker(QStringLiteral("- x")), QStringLiteral("- x 是无序标记"));
    check(!MarkdownParser::isUnorderedMarker(QStringLiteral("-x")), QStringLiteral("-x  不是无序标记"));

    QString report;
    for (const QString &s : g_log) {
        report += s + QLatin1Char('\n');
    }
    report += QStringLiteral("\n==== PASS=%1  FAIL=%2 ====\n").arg(g_pass).arg(g_fail);

    QFile f(QStringLiteral("parse_result.txt"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream ts(&f);
        ts.setCodec("UTF-8");
        ts << report;
        f.close();
    }

    return (g_fail == 0) ? 0 : 1;
}
