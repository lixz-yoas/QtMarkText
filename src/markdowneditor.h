#ifndef MARKDOWNEDITOR_H
#define MARKDOWNEDITOR_H

#include <QTextEdit>

/**
 * @brief Markdown 编辑区（所见即所得）
 *
 * 当前阶段（第 2 阶段）实现「载入 -> 解析 -> 渲染」单向链路，
 * 编辑区呈现富文本视图。
 *
 * 第 3 阶段将补齐：
 *   1. 光标所在块回退为「源码态」（显示 ** 等标记），其余块保持渲染态
 *   2. 富文本文档 -> Markdown 的反向序列化（markdownText 不再回传原始副本）
 */
class MarkdownEditor : public QTextEdit
{
    Q_OBJECT

public:
    explicit MarkdownEditor(QWidget *parent = nullptr);
    ~MarkdownEditor() override;

    /// 载入 Markdown 文本并渲染为富文本视图
    void setMarkdownText(const QString &markdown);

    /// 取回 Markdown 文本
    QString markdownText() const;

private:
    QString m_source;   ///< 原始 Markdown 源码（第 3 阶段前作为回写来源）
};

#endif // MARKDOWNEDITOR_H
