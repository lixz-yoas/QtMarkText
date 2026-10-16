#ifndef MARKDOWNPARSER_H
#define MARKDOWNPARSER_H

#include <QString>
#include <QStringList>
#include <QVector>

/// 块级元素类型
enum class MdBlockType
{
    Paragraph,       ///< 段落
    Heading,         ///< ATX 标题 #
    Quote,           ///< 引用 >
    CodeFence,       ///< 围栏代码块 ``` / ~~~
    ListItem,        ///< 列表项（有序 / 无序 / 任务）
    HorizontalRule   ///< 分隔线
};

/// 行内元素类型
enum class MdInlineType
{
    Text,            ///< 普通文本
    Bold,            ///< 粗体 **x** / __x__
    Italic,          ///< 斜体 *x* / _x_
    BoldItalic,      ///< 粗斜体 ***x***
    Strike,          ///< 删除线 ~~x~~
    Code,            ///< 行内代码 `x`
    Link,            ///< 链接 [text](url)
    Image            ///< 图片 ![alt](src)
};

/// 行内片段
struct MdInline
{
    MdInlineType type = MdInlineType::Text;
    QString      text;    ///< 显示文本
    QString      url;     ///< Link / Image 的目标地址
};

/// 块级元素
struct MdBlock
{
    MdBlockType       type = MdBlockType::Paragraph;
    int               level = 0;              ///< 标题级别 1..6
    QString           rawText;                ///< 去掉块前缀后的原始文本
    QString           info;                   ///< 代码块的语言标识
    bool              ordered = false;        ///< 有序列表
    int               order = 0;              ///< 有序列表序号
    bool              task = false;           ///< 是否为任务列表项
    bool              checked = false;        ///< 任务是否已勾选
    QVector<MdInline> inlines;                ///< 行内解析结果
};

/**
 * @brief Markdown 解析器（纯逻辑，不依赖 UI）
 *
 * 仅覆盖 MarkText 常用子集：标题、段落、引用、围栏代码块、列表
 * （含任务列表）、分隔线，以及粗体 / 斜体 / 粗斜体 / 删除线 /
 * 行内代码 / 链接 / 图片。
 * 数学公式（KaTeX）与图表（Mermaid）按 A3 路线不纳入本阶段。
 */
class MarkdownParser
{
public:
    MarkdownParser() = delete;

    /// 解析整篇 Markdown 文本为块序列
    static QVector<MdBlock> parse(const QString &markdown);

    /// 解析单行文本为行内片段序列
    static QVector<MdInline> parseInline(const QString &text);

    /// 判断一行是否为分隔线
    static bool isHorizontalRule(const QString &line);

    /// 判断一行是否以无序列表标记开头
    static bool isUnorderedMarker(const QString &line);
};

#endif // MARKDOWNPARSER_H
