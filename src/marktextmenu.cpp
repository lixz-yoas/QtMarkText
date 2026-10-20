#include "marktextmenu.h"

#include "marktextthemes.h"

#include <QAction>
#include <QActionGroup>
#include <QDesktopServices>
#include <QMenu>
#include <QMenuBar>
#include <QUrl>

using MarkTextThemes::Entry;
using MarkTextThemes::kDarkThemeCount;
using MarkTextThemes::kDarkThemes;
using MarkTextThemes::kLightThemeCount;
using MarkTextThemes::kLightThemes;

namespace {

/**
 * MarkText 的键位写法 -> Qt 的键位写法
 *
 * 实测（Qt 5.12.8 / mingw73_64）：两者的键名几乎完全一致，唯一例外是加号——
 * QKeySequence 不认 "Plus" 这个键名，MarkText 的 'Ctrl+Plus'（paragraph.upgrade-heading）
 * 若原样传入会得到一个空键序列，表现形式是"菜单项后面不显示快捷键、按了也没反应"。
 * 故此处只对这一个特例做改写，其余一律原样透传。
 */
QString toQtAccelerator(const QString &sAccel)
{
    if (sAccel.compare(QLatin1String("Ctrl+Plus"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("Ctrl++");
    }
    return sAccel;
}

} // namespace

//=====================================================================
//  构造 / 析构
//=====================================================================
MarkTextMenu::MarkTextMenu(QMenuBar *pMenuBar, QObject *parent)
    : QObject(parent)
    , m_pMenuBar(pMenuBar)
    , m_pThemeGroup(nullptr)
    , m_pFollowSystemAction(nullptr)
    , m_pThemeDisabledHint(nullptr)
    , m_pRecentMenu(nullptr)
    , m_pClearRecentAction(nullptr)
{
    if (pMenuBar == nullptr) {
        return;
    }

    // 菜单顺序严格照抄 templates/index.ts 的非 macOS 分支：
    // 文件 / 编辑 / 段落 / 格式 / 窗口 / 主题 / 视图 / 帮助
    buildFileMenu(pMenuBar);
    buildEditMenu(pMenuBar);
    buildParagraphMenu(pMenuBar);
    buildFormatMenu(pMenuBar);
    buildWindowMenu(pMenuBar);
    buildThemeMenu(pMenuBar);
    buildViewMenu(pMenuBar);
    buildHelpMenu(pMenuBar);
}

MarkTextMenu::~MarkTextMenu()
{
}

//=====================================================================
//  通用的小工具
//=====================================================================
QAction *MarkTextMenu::addCommand(QMenu *pMenu, const QString &sId, const QString &sText,
                                  const QString &sAccel, bool bCheckable)
{
    QAction *pAction = pMenu->addAction(sText);
    pAction->setObjectName(sId);
    if (bCheckable) {
        pAction->setCheckable(true);
    }
    if (!sAccel.isEmpty()) {
        // MarkText 的键位写法与 Qt 的写法基本一致（Ctrl/Shift/Alt + 键名），
        // 个别差异由 toQtAccelerator() 兜住（目前只有 Ctrl+Plus 一例）。
        pAction->setShortcut(QKeySequence(toQtAccelerator(sAccel)));
    }
    m_hashActions.insert(sId, pAction);

    connect(pAction, &QAction::triggered, this, [this, sId]() {
        emit commandTriggered(sId);
    });
    return pAction;
}

QAction *MarkTextMenu::addHintItem(QMenu *pMenu, const QString &sId, const QString &sText)
{
    QAction *pAction = pMenu->addAction(sText);
    pAction->setEnabled(false);
    if (!sId.isEmpty()) {
        pAction->setObjectName(sId);
        m_hashActions.insert(sId, pAction);
    }
    return pAction;
}

QAction *MarkTextMenu::addRadioItem(QMenu *pMenu, QActionGroup *pGroup, const QString &sId,
                                    const QString &sText, bool bEnabled)
{
    QAction *pAction = pMenu->addAction(sText);
    pAction->setObjectName(sId);
    pAction->setCheckable(true);
    pAction->setEnabled(bEnabled);
    if (pGroup != nullptr) {
        pGroup->addAction(pAction);
    }
    m_hashActions.insert(sId, pAction);

    connect(pAction, &QAction::triggered, this, [this, sId]() {
        emit themeSelected(sId);
    });
    return pAction;
}

QAction *MarkTextMenu::addLinkItem(QMenu *pMenu, const QString &sId, const QString &sText,
                                   const QString &sUrl)
{
    QAction *pAction = addCommand(pMenu, sId, sText);
    // 帮助类菜单是纯外壳职责（无跨层语义），直接在此打开外链，
    // 同时照常发 commandTriggered 便于日志与后续接管。
    connect(pAction, &QAction::triggered, this, [sUrl]() {
        if (!sUrl.isEmpty()) {
            QDesktopServices::openUrl(QUrl(sUrl));
        }
    });
    return pAction;
}

void MarkTextMenu::addSeparator(QMenu *pMenu)
{
    pMenu->addSeparator();
}

//=====================================================================
//  按 id 取用 / 状态设置
//=====================================================================
QAction *MarkTextMenu::actionById(const QString &sId) const
{
    return m_hashActions.value(sId, nullptr);
}

int MarkTextMenu::actionCount() const
{
    return m_hashActions.size();
}

QList<QPair<QString, QString> > MarkTextMenu::shortcutList() const
{
    QList<QPair<QString, QString> > listResult;
    if (m_pMenuBar == nullptr) {
        return listResult;
    }

    const QList<QAction *> listTop = m_pMenuBar->actions();
    for (QAction *pTop : listTop) {
        QMenu *pMenu = pTop->menu();
        if (pMenu == nullptr) {
            continue;
        }
        // 去掉助记符（如 "主题(&T)" -> "主题(T)"）
        const QString sMenuName = QString(pTop->text()).remove(QLatin1Char('&'));
        const QList<QAction *> listItems = pMenu->actions();
        for (QAction *pItem : listItems) {
            if (pItem->isSeparator() || pItem->menu() != nullptr) {
                continue;   // 分隔符与子菜单不入表
            }
            const QString sShortcut = pItem->shortcut().toString(QKeySequence::NativeText);
            if (sShortcut.isEmpty()) {
                continue;   // 无默认键位的项不入表
            }
            listResult.append(qMakePair(sMenuName + QStringLiteral(" › ")
                                            + QString(pItem->text()).remove(QLatin1Char('&')),
                                        sShortcut));
        }
    }
    return listResult;
}

void MarkTextMenu::setCheckedById(const QString &sId, bool bChecked)
{
    QAction *pAction = actionById(sId);
    if (pAction != nullptr) {
        pAction->setChecked(bChecked);
    }
}

void MarkTextMenu::setEnabledById(const QString &sId, bool bEnabled)
{
    QAction *pAction = actionById(sId);
    if (pAction != nullptr) {
        pAction->setEnabled(bEnabled);
    }
}

void MarkTextMenu::setVisibleById(const QString &sId, bool bVisible)
{
    QAction *pAction = actionById(sId);
    if (pAction != nullptr) {
        pAction->setVisible(bVisible);
    }
}

//=====================================================================
//  文件菜单
//  对照：templates/file.ts
//=====================================================================
void MarkTextMenu::buildFileMenu(QMenuBar *pBar)
{
    QMenu *pMenu = pBar->addMenu(QStringLiteral("文件"));           // menu.file.file

    addCommand(pMenu, QStringLiteral("newTabMenuItem"),   QStringLiteral("新建标签页"),   QStringLiteral("Ctrl+T"));       // file.new-tab
    addCommand(pMenu, QStringLiteral("newWindowMenuItem"),QStringLiteral("新建窗口"),     QStringLiteral("Ctrl+N"));       // file.new-window
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("openFileMenuItem"), QStringLiteral("打开文件…"),   QStringLiteral("Ctrl+O"));       // file.open-file
    addCommand(pMenu, QStringLiteral("openFolderMenuItem"), QStringLiteral("打开文件夹…"), QStringLiteral("Ctrl+Shift+O")); // file.open-folder

    // 「打开最近」：内容动态重建（MarkText 最多 12 条）
    m_pRecentMenu = pMenu->addMenu(QStringLiteral("打开最近"));      // menu.file.openRecent
    m_pRecentMenu->setObjectName(QStringLiteral("openRecentMenu"));

    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("saveMenuItem"),   QStringLiteral("保存"),     QStringLiteral("Ctrl+S"));            // file.save
    addCommand(pMenu, QStringLiteral("saveAsMenuItem"), QStringLiteral("另存为…"),  QStringLiteral("Ctrl+Shift+S"));      // file.save-as
    addCommand(pMenu, QStringLiteral("autoSaveMenuItem"), QStringLiteral("自动保存"), QString(), true);                    // file.autoSave（checkbox）

    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("moveToMenuItem"), QStringLiteral("移动到"));                                          // file.move-file（无默认键位）
    addCommand(pMenu, QStringLiteral("renameMenuItem"), QStringLiteral("重命名"));                                          // file.rename-file（无默认键位）

    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("importMenuItem"), QStringLiteral("导入"));                                            // file.import

    QMenu *pExportMenu = pMenu->addMenu(QStringLiteral("导出"));     // menu.file.export
    pExportMenu->setObjectName(QStringLiteral("exportMenu"));
    addCommand(pExportMenu, QStringLiteral("exportHtmlMenuItem"), QStringLiteral("导出为 HTML"));                             // file.export-html（无默认键位）
    addCommand(pExportMenu, QStringLiteral("exportPdfMenuItem"),  QStringLiteral("导出为 PDF"), QStringLiteral("Ctrl+Alt+E")); // file.export-file.pdf

    addCommand(pMenu, QStringLiteral("printMenuItem"), QStringLiteral("打印"), QStringLiteral("Ctrl+P"));                    // file.print

    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("preferencesMenuItem"), QStringLiteral("偏好设置"), QStringLiteral("Ctrl+,"));          // file.preferences

    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("closeTabMenuItem"),    QStringLiteral("关闭标签页"), QStringLiteral("Ctrl+W"));       // file.close-tab
    addCommand(pMenu, QStringLiteral("closeWindowMenuItem"), QStringLiteral("关闭窗口"),   QStringLiteral("Ctrl+Shift+W")); // file.close-window

    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("quitMenuItem"), QStringLiteral("退出"), QStringLiteral("Ctrl+Q"));                    // file.quit

    setRecentFiles(QStringList());
}

void MarkTextMenu::setRecentFiles(const QStringList &listFiles)
{
    if (m_pRecentMenu == nullptr) {
        return;
    }

    m_pRecentMenu->clear();

    for (const QString &sPath : listFiles) {
        QAction *pAction = m_pRecentMenu->addAction(sPath);
        connect(pAction, &QAction::triggered, this, [this, sPath]() {
            emit recentFileActivated(sPath);
        });
    }

    // MarkText：分隔线只在有历史记录时可见
    QAction *pSeparator = m_pRecentMenu->addSeparator();
    pSeparator->setVisible(!listFiles.isEmpty());

    m_pClearRecentAction = m_pRecentMenu->addAction(QStringLiteral("清除最近使用"));  // menu.file.clearRecentlyUsed
    m_pClearRecentAction->setObjectName(QStringLiteral("clearRecentlyUsedMenuItem"));
    m_pClearRecentAction->setEnabled(!listFiles.isEmpty());
    connect(m_pClearRecentAction, &QAction::triggered, this, [this]() {
        emit commandTriggered(QStringLiteral("menu.file.clearRecentlyUsed"));
    });
}

//=====================================================================
//  编辑菜单
//  对照：templates/edit.ts
//=====================================================================
void MarkTextMenu::buildEditMenu(QMenuBar *pBar)
{
    QMenu *pMenu = pBar->addMenu(QStringLiteral("编辑"));            // menu.edit.edit

    addCommand(pMenu, QStringLiteral("undoMenuItem"), QStringLiteral("撤销"), QStringLiteral("Ctrl+Z"));       // edit.undo
    addCommand(pMenu, QStringLiteral("redoMenuItem"), QStringLiteral("重做"), QStringLiteral("Ctrl+Shift+Z")); // edit.redo
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("cutMenuItem"),   QStringLiteral("剪切"), QStringLiteral("Ctrl+X"));      // edit.cut
    addCommand(pMenu, QStringLiteral("copyMenuItem"),  QStringLiteral("复制"), QStringLiteral("Ctrl+C"));      // edit.copy
    addCommand(pMenu, QStringLiteral("pasteMenuItem"), QStringLiteral("粘贴"), QStringLiteral("Ctrl+V"));      // edit.paste
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("copyAsRichMenuItem"),   QStringLiteral("复制为富文本"), QStringLiteral("Ctrl+Shift+C")); // edit.copy-as-rich
    addCommand(pMenu, QStringLiteral("copyAsHtmlMenuItem"),   QStringLiteral("复制为 HTML"));                                  // edit.copy-as-html（无默认键位）
    addCommand(pMenu, QStringLiteral("pasteAsPlainTextMenuItem"), QStringLiteral("粘贴为纯文本"), QStringLiteral("Ctrl+Shift+V")); // edit.paste-as-plaintext
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("selectAllMenuItem"), QStringLiteral("全选"), QStringLiteral("Ctrl+A"));  // edit.select-all
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("duplicateMenuItem"),       QStringLiteral("创建副本"), QStringLiteral("Ctrl+Alt+D"));  // edit.duplicate
    addCommand(pMenu, QStringLiteral("createParagraphMenuItem"), QStringLiteral("创建段落"), QStringLiteral("Ctrl+Shift+N"));// edit.create-paragraph
    addCommand(pMenu, QStringLiteral("deleteParagraphMenuItem"), QStringLiteral("删除段落"), QStringLiteral("Ctrl+Shift+D"));// edit.delete-paragraph
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("findMenuItem"),         QStringLiteral("查找"),       QStringLiteral("Ctrl+F"));      // edit.find
    addCommand(pMenu, QStringLiteral("findNextMenuItem"),     QStringLiteral("查找下一个"), QStringLiteral("F3"));          // edit.find-next
    addCommand(pMenu, QStringLiteral("findPreviousMenuItem"), QStringLiteral("查找上一个"), QStringLiteral("Shift+F3"));   // edit.find-previous
    addCommand(pMenu, QStringLiteral("replaceMenuItem"),      QStringLiteral("替换"),       QStringLiteral("Ctrl+R"));      // edit.replace
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("findInFolderMenuItem"), QStringLiteral("在文件夹中查找"), QStringLiteral("Ctrl+Shift+F")); // edit.find-in-folder
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("screenshot"), QStringLiteral("截图"));                                            // edit.screenshot（macOS 专有，无键位）
    addSeparator(pMenu);

    QMenu *pLineEnding = pMenu->addMenu(QStringLiteral("行结束符"));  // menu.edit.lineEnding
    pLineEnding->setObjectName(QStringLiteral("lineEndingMenu"));
    QActionGroup *pGroup = new QActionGroup(this);
    QAction *pCrlf = addCommand(pLineEnding, QStringLiteral("crlfLineEndingMenuEntry"),
                                QStringLiteral("CRLF (Windows)"), QString(), true);
    QAction *pLf = addCommand(pLineEnding, QStringLiteral("lfLineEndingMenuEntry"),
                              QStringLiteral("LF (Unix/macOS)"), QString(), true);
    pGroup->addAction(pCrlf);
    pGroup->addAction(pLf);
}

//=====================================================================
//  段落菜单
//  对照：templates/paragraph.ts
//=====================================================================
void MarkTextMenu::buildParagraphMenu(QMenuBar *pBar)
{
    QMenu *pMenu = pBar->addMenu(QStringLiteral("段落"));            // menu.paragraph.title
    pMenu->setObjectName(QStringLiteral("paragraphMenuEntry"));

    // 标题 1~5：MarkText 在 Windows 上不给默认键位（Ctrl+Alt 是 AltGr 别名）。
    // 只保留 1~5：六级标题在 Markdown 里几乎不用，菜单里留着只是噪音。
    addCommand(pMenu, QStringLiteral("heading1MenuItem"), QStringLiteral("标题 1"), QString(), true);
    addCommand(pMenu, QStringLiteral("heading2MenuItem"), QStringLiteral("标题 2"), QString(), true);
    addCommand(pMenu, QStringLiteral("heading3MenuItem"), QStringLiteral("标题 3"), QString(), true);
    addCommand(pMenu, QStringLiteral("heading4MenuItem"), QStringLiteral("标题 4"), QString(), true);
    addCommand(pMenu, QStringLiteral("heading5MenuItem"), QStringLiteral("标题 5"), QString(), true);
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("upgradeHeadingMenuItem"), QStringLiteral("提升标题级别"), QStringLiteral("Ctrl+Plus"), true);  // paragraph.upgrade-heading
    addCommand(pMenu, QStringLiteral("degradeHeadingMenuItem"), QStringLiteral("降低标题级别"), QStringLiteral("Ctrl+-"), true);     // paragraph.degrade-heading
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("tableMenuItem"),      QStringLiteral("表格"),     QStringLiteral("Ctrl+Shift+T"), true);  // paragraph.table
    addCommand(pMenu, QStringLiteral("codeFencesMenuItem"), QStringLiteral("代码围栏"), QStringLiteral("Ctrl+Shift+K"), true);  // paragraph.code-fence
    addCommand(pMenu, QStringLiteral("quoteBlockMenuItem"), QStringLiteral("引用块"),   QStringLiteral("Ctrl+Shift+Q"), true);  // paragraph.quote-block
    addCommand(pMenu, QStringLiteral("mathBlockMenuItem"),  QStringLiteral("数学块"),   QStringLiteral("Ctrl+Alt+N"), true);    // paragraph.math-formula
    addCommand(pMenu, QStringLiteral("htmlBlockMenuItem"),  QStringLiteral("HTML 块"),  QStringLiteral("Ctrl+Alt+H"), true);    // paragraph.html-block
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("orderListMenuItem"),  QStringLiteral("有序列表"), QStringLiteral("Ctrl+G"), true);        // paragraph.order-list
    addCommand(pMenu, QStringLiteral("bulletListMenuItem"), QStringLiteral("无序列表"), QStringLiteral("Ctrl+H"), true);        // paragraph.bullet-list
    addCommand(pMenu, QStringLiteral("taskListMenuItem"),   QStringLiteral("任务列表"), QStringLiteral("Ctrl+Alt+X"), true);    // paragraph.task-list
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("looseListItemMenuItem"), QStringLiteral("宽松列表项"), QStringLiteral("Ctrl+Alt+L"), true); // paragraph.loose-list-item
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("paragraphMenuItem"),      QStringLiteral("段落"),       QStringLiteral("Ctrl+Shift+0"), true); // paragraph.paragraph
    addCommand(pMenu, QStringLiteral("horizontalLineMenuItem"), QStringLiteral("水平分割线"), QStringLiteral("Ctrl+Shift+U"), true); // paragraph.horizontal-line
    addCommand(pMenu, QStringLiteral("frontMatterMenuItem"),    QStringLiteral("前置元数据"), QStringLiteral("Ctrl+Alt+Y"), true);   // paragraph.front-matter
}

//=====================================================================
//  格式菜单
//  对照：templates/format.ts
//=====================================================================
void MarkTextMenu::buildFormatMenu(QMenuBar *pBar)
{
    QMenu *pMenu = pBar->addMenu(QStringLiteral("格式"));            // menu.format.format
    pMenu->setObjectName(QStringLiteral("formatMenuItem"));

    addCommand(pMenu, QStringLiteral("strongMenuItem"),    QStringLiteral("粗体"),   QStringLiteral("Ctrl+B"), true);   // format.strong
    addCommand(pMenu, QStringLiteral("emphasisMenuItem"),  QStringLiteral("斜体"),   QStringLiteral("Ctrl+I"), true);   // format.emphasis
    addCommand(pMenu, QStringLiteral("underlineMenuItem"), QStringLiteral("下划线"), QStringLiteral("Ctrl+U"), true);   // format.underline
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("superscriptMenuItem"), QStringLiteral("上标"), QString(), true);                  // format.superscript（无默认键位）
    addCommand(pMenu, QStringLiteral("subscriptMenuItem"),   QStringLiteral("下标"), QString(), true);                  // format.subscript（无默认键位）
    addCommand(pMenu, QStringLiteral("highlightMenuItem"),   QStringLiteral("高亮"), QStringLiteral("Ctrl+Shift+H"), true); // format.highlight
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("inlineCodeMenuItem"), QStringLiteral("行内代码"), QStringLiteral("Ctrl+`"), true);      // format.inline-code
    addCommand(pMenu, QStringLiteral("inlineMathMenuItem"), QStringLiteral("行内数学"), QStringLiteral("Ctrl+Shift+M"), true); // format.inline-math
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("strikeMenuItem"),    QStringLiteral("删除线"), QStringLiteral("Ctrl+D"), true);        // format.strike
    addCommand(pMenu, QStringLiteral("hyperlinkMenuItem"), QStringLiteral("超链接"), QStringLiteral("Ctrl+L"), true);        // format.hyperlink
    addCommand(pMenu, QStringLiteral("imageMenuItem"),     QStringLiteral("图片"),   QStringLiteral("Ctrl+Shift+I"), true);  // format.image
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("clearFormatMenuItem"), QStringLiteral("清除格式"), QStringLiteral("Ctrl+Shift+R"));    // format.clear-format
}

//=====================================================================
//  窗口菜单
//  对照：templates/window.ts（非 macOS 分支）
//=====================================================================
void MarkTextMenu::buildWindowMenu(QMenuBar *pBar)
{
    QMenu *pMenu = pBar->addMenu(QStringLiteral("窗口"));            // menu.window.title

    addCommand(pMenu, QStringLiteral("minimizeMenuItem"), QStringLiteral("最小化"), QStringLiteral("Ctrl+M"));             // window.minimize
    addCommand(pMenu, QStringLiteral("alwaysOnTopMenuItem"), QStringLiteral("总是在最前面"), QString(), true);              // window.toggle-always-on-top（无默认键位）
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("zoomInMenuItem"),  QStringLiteral("放大"));                                            // window.zoomIn（无默认键位）
    addCommand(pMenu, QStringLiteral("zoomOutMenuItem"), QStringLiteral("缩小"));                                            // window.zoomOut（无默认键位）
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("fullScreenMenuItem"), QStringLiteral("全屏"), QStringLiteral("F11"));                 // window.toggle-full-screen
}

//=====================================================================
//  主题菜单
//  对照：templates/theme.ts
//=====================================================================
void MarkTextMenu::buildThemeMenu(QMenuBar *pBar)
{
    QMenu *pMenu = pBar->addMenu(QStringLiteral("主题(&T)"));        // menu.theme.theme（MarkText 原文就带 &T 助记符）
    pMenu->setObjectName(QStringLiteral("themeMenu"));

    m_pThemeGroup = new QActionGroup(this);

    // 跟随系统主题（checkbox，不在互斥组内）
    m_pFollowSystemAction = addCommand(pMenu, QStringLiteral("follow-system-theme"),
                                       QStringLiteral("跟随系统主题"), QString(), true);   // preferences.theme.followSystemTheme
    connect(m_pFollowSystemAction, &QAction::triggered, this, [this](bool bChecked) {
        emit followSystemThemeToggled(bChecked);
    });

    // 「跟随系统主题时无法选择主题」——MarkText 仅在跟随系统时插入该项
    m_pThemeDisabledHint = addHintItem(pMenu, QStringLiteral("follow-system-theme-hint"),
                                       QStringLiteral("跟随系统主题时无法选择主题"));        // menu.theme.followThemDisabled
    m_pThemeDisabledHint->setVisible(false);

    // — 浅色主题 —
    addHintItem(pMenu, QStringLiteral("lightThemesHeader"), QStringLiteral("— 浅色主题 —"));  // menu.theme.lightThemes
    for (int i = 0; i < kLightThemeCount; ++i) {
        const Entry &entry = kLightThemes[i];
        addRadioItem(pMenu, m_pThemeGroup,
                     QString::fromLatin1(entry.pszId),
                     QString::fromUtf8(entry.pszText), true);
    }

    addSeparator(pMenu);

    // — 深色主题 —
    addHintItem(pMenu, QStringLiteral("darkThemesHeader"), QStringLiteral("— 深色主题 —"));   // menu.theme.darkThemes
    for (int i = 0; i < kDarkThemeCount; ++i) {
        const Entry &entry = kDarkThemes[i];
        addRadioItem(pMenu, m_pThemeGroup,
                     QString::fromLatin1(entry.pszId),
                     QString::fromUtf8(entry.pszText), true);
    }

    // 初始状态：MarkText 默认 followSystemTheme = true
    applyThemeState(QStringLiteral("light"), true);
}

void MarkTextMenu::applyThemeState(const QString &sThemeId, bool bFollowSystemTheme)
{
    if (m_pFollowSystemAction != nullptr) {
        m_pFollowSystemAction->setChecked(bFollowSystemTheme);
    }
    if (m_pThemeDisabledHint != nullptr) {
        m_pThemeDisabledHint->setVisible(bFollowSystemTheme);
    }

    const QList<QAction *> listThemes = (m_pThemeGroup != nullptr)
                                            ? m_pThemeGroup->actions()
                                            : QList<QAction *>();
    for (QAction *pAction : listThemes) {
        pAction->setEnabled(!bFollowSystemTheme);
        pAction->setChecked(pAction->objectName() == sThemeId);
    }
}

//=====================================================================
//  视图菜单
//  对照：templates/view.ts（非 macOS 分支）
//=====================================================================
void MarkTextMenu::buildViewMenu(QMenuBar *pBar)
{
    QMenu *pMenu = pBar->addMenu(QStringLiteral("视图"));            // menu.view.view

    addCommand(pMenu, QStringLiteral("commandPaletteMenuItem"), QStringLiteral("命令面板"), QStringLiteral("Ctrl+Shift+P")); // view.command-palette
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("sourceCodeModeMenuItem"), QStringLiteral("源码模式"),   QStringLiteral("Ctrl+E"), true);       // view.source-code-mode
    addCommand(pMenu, QStringLiteral("typewriterModeMenuItem"), QStringLiteral("打字机模式"), QStringLiteral("Ctrl+Shift+G"), true); // view.typewriter-mode
    addCommand(pMenu, QStringLiteral("focusModeMenuItem"),      QStringLiteral("专注模式"),   QStringLiteral("Ctrl+Shift+J"), true); // view.focus-mode
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("sideBarMenuItem"), QStringLiteral("切换侧边栏"), QStringLiteral("Ctrl+J"), true);            // view.toggle-sidebar
    addCommand(pMenu, QStringLiteral("tabBarMenuItem"),  QStringLiteral("切换标签栏"), QStringLiteral("Ctrl+Shift+B"), true);      // view.toggle-tabbar
    addCommand(pMenu, QStringLiteral("tocMenuItem"),     QStringLiteral("切换目录"),   QStringLiteral("Ctrl+K"));                  // view.toggle-toc
    addCommand(pMenu, QStringLiteral("reloadImagesMenuItem"), QStringLiteral("重新加载图片"), QStringLiteral("F5"));               // view.reload-images
    addSeparator(pMenu);

    addCommand(pMenu, QStringLiteral("showDeveloperToolsMenuItem"), QStringLiteral("显示开发者工具"), QStringLiteral("Ctrl+Alt+I")); // view.toggle-dev-tools
    addCommand(pMenu, QStringLiteral("reloadWindowMenuItem"),       QStringLiteral("重新加载窗口"),   QStringLiteral("Ctrl+F5"));    // view.dev-reload
}

//=====================================================================
//  帮助菜单
//  对照：templates/help.ts（URL 原样照抄）
//=====================================================================
void MarkTextMenu::buildHelpMenu(QMenuBar *pBar)
{
    QMenu *pMenu = pBar->addMenu(QStringLiteral("帮助"));            // menu.help.help

    addLinkItem(pMenu, QStringLiteral("markdownReferenceMenuItem"), QStringLiteral("Markdown 参考"),
                QStringLiteral("https://marktext.me/docs/markdown-syntax"));
    addLinkItem(pMenu, QStringLiteral("changelogMenuItem"), QStringLiteral("更新日志"),
                QStringLiteral("https://github.com/marktext/marktext/releases"));
    addSeparator(pMenu);

    addLinkItem(pMenu, QStringLiteral("followUsMenuItem"), QStringLiteral("关注我们"),
                QStringLiteral("https://twitter.com/marktextapp"));
    addLinkItem(pMenu, QStringLiteral("supportMenuItem"), QStringLiteral("支持 MarkText"),
                QStringLiteral("https://github.com/sponsors/marktext"));
    addSeparator(pMenu);

    addLinkItem(pMenu, QStringLiteral("askQuestionMenuItem"), QStringLiteral("提问"),
                QStringLiteral("https://github.com/marktext/marktext/discussions"));
    addLinkItem(pMenu, QStringLiteral("reportBugMenuItem"), QStringLiteral("报告错误"),
                QStringLiteral("https://github.com/marktext/marktext/issues"));
    addLinkItem(pMenu, QStringLiteral("viewSourceMenuItem"), QStringLiteral("查看源码"),
                QStringLiteral("https://github.com/marktext/marktext"));
    addSeparator(pMenu);

    addLinkItem(pMenu, QStringLiteral("licenseMenuItem"), QStringLiteral("许可证"),
                QStringLiteral("https://github.com/marktext/marktext/blob/develop/LICENSE"));

    // MarkText：仅当 resources/app-update.yml 存在（安装版）时追加「检查更新」
    addSeparator(pMenu);
    addCommand(pMenu, QStringLiteral("checkUpdatesMenuItem"), QStringLiteral("检查更新"));

    // MarkText：非 macOS 追加「关于」
    addSeparator(pMenu);
    addCommand(pMenu, QStringLiteral("aboutMenuItem"), QStringLiteral("关于"));      // menu.help.about
}
