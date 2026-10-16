#include "markdowneditor.h"

#include "markdownparser.h"
#include "markdownrenderer.h"

MarkdownEditor::MarkdownEditor(QWidget *parent)
    : QTextEdit(parent)
{
    setAcceptRichText(true);
    setLineWrapMode(QTextEdit::WidgetWidth);
    setTabChangesFocus(false);
}

MarkdownEditor::~MarkdownEditor()
{
}

void MarkdownEditor::setMarkdownText(const QString &markdown)
{
    m_source = markdown;

    const QVector<MdBlock> blocks = MarkdownParser::parse(markdown);
    MarkdownRenderer::render(blocks, document());

    document()->setModified(false);
}

QString MarkdownEditor::markdownText() const
{
    // TODO(第 3 阶段)：改为从富文本文档反向序列化，而非回传原始副本
    return m_source;
}
