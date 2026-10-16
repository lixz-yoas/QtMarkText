#ifndef MARKDOWNRENDERER_H
#define MARKDOWNRENDERER_H

#include <QVector>

#include "markdownparser.h"

class QTextDocument;

/**
 * @brief 把解析结果渲染进 QTextDocument
 *
 * 采用「全量重建」策略：清空文档后按块顺序重建。
 * 块级差异（标题字号、引用缩进、代码块底色、列表符号）通过
 * QTextBlockFormat / QTextListFormat 表达；行内差异通过 QTextCharFormat 表达。
 */
class MarkdownRenderer
{
public:
    MarkdownRenderer() = delete;

    /// 全量渲染：清空 doc 并按 blocks 重建
    static void render(const QVector<MdBlock> &blocks, QTextDocument *doc);
};

#endif // MARKDOWNRENDERER_H
