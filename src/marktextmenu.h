#ifndef MARKTEXTMENU_H
#define MARKTEXTMENU_H

#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

class QAction;
class QActionGroup;
class QMenu;
class QMenuBar;

/**
 * @brief MarkText 菜单模板的 Qt 实现（数据驱动、一一对照）
 *
 * 对照来源（MarkText v0.19.1）：
 *   - 结构：packages/desktop/src/main/menu/templates 下的各菜单模板
 *   - 文案：packages/desktop/static/locales/zh-CN.json -> menu.*
 *   - 快捷键：packages/desktop/src/main/keyboard/keybindingsWindows.ts
 *   - 顺序：packages/desktop/src/main/menu/templates/index.ts（非 macOS 分支）
 *
 * 设计原则：
 *   1. 只负责「菜单长什么样」，不负责「点了做什么」：一律以
 *      commandTriggered(id) 抛给 MainWindow，MainWindow 再决定是自己处理
 *      （文件/窗口类）还是转交渲染层（格式/段落/视图类）。
 *   2. 每个 QAction 的 objectName 直接沿用 MarkText 的菜单项 id
 *      （如 paragraphMenuItem / sourceCodeModeMenuItem / autoSaveMenuItem），
 *      后续 WebView2 桥接层按同一套 id 下发命令，不需要第二张映射表。
 *   3. 文案与快捷键全部来自 MarkText 官方中文语言包与 Windows 版键位表，
 *      不做"优化改写"。
 *
 * ⚠️ 本轮为「外壳层」改造：菜单项齐全，但只有文件/窗口/帮助类命令有真实动作，
 *    格式/段落/视图类命令一律透传给渲染层接口（当前为占位引擎）。
 */
class MarkTextMenu : public QObject
{
    Q_OBJECT

public:
    explicit MarkTextMenu(QMenuBar *pMenuBar, QObject *parent = nullptr);
    ~MarkTextMenu() override;

    /// 按 MarkText 菜单项 id 取回 QAction（找不到返回 nullptr）
    QAction *actionById(const QString &sId) const;
    /// 已登记的菜单项总数（不含分隔符）
    int      actionCount() const;

    void setCheckedById(const QString &sId, bool bChecked);
    void setEnabledById(const QString &sId, bool bEnabled);
    void setVisibleById(const QString &sId, bool bVisible);

    // ---- 渲染层状态回传驱动的菜单刷新（对照主进程 UpdateApplicationMenu）----
    /// 应用光标/选区状态（载荷即官方 createApplicationMenuState 的结构）。
    /// 负责：段落菜单勾选 + 段落/格式两组的可用态。
    /// objState 的键：affiliation / isTable / isLooseListItem / isTaskList /
    ///                isDisabled / isMultiline / isCodeFences / isCodeContent
    void applySelectionState(const QJsonObject &objState);
    /// 应用行内格式状态：{"<格式 type>": true, ...}
    void applyFormatState(const QJsonObject &objFormats);
    /// 应用行结束符状态："crlf" / "lf"
    void applyLineEndingState(const QString &sLineEnding);

    /// 重建「打开最近」子菜单（MarkText 最多保留 12 条）
    void setRecentFiles(const QStringList &listFiles);

    /// 按当前主题状态刷新主题菜单（含"跟随系统"禁用逻辑）
    void applyThemeState(const QString &sThemeId, bool bFollowSystemTheme);

    /// 汇总「菜单路径 -> 快捷键」清单，供偏好设置的「快捷键」页直接使用
    /// （零重复：与菜单同源，菜单改了这里自动跟着变）
    QList<QPair<QString, QString> > shortcutList() const;

signals:
    /// 通用命令：id 即 MarkText 菜单项 id
    void commandTriggered(const QString &sId);
    /// 主题菜单里选中了某个主题 id
    void themeSelected(const QString &sThemeId);
    /// 勾选/取消「跟随系统主题」
    void followSystemThemeToggled(bool bFollow);
    /// 点击「打开最近」里的某个文件
    void recentFileActivated(const QString &sPath);

private:
    /// 追加一个命令项；sAccel 直接用 MarkText 的键位写法（如 "Ctrl+Shift+T"）
    QAction *addCommand(QMenu *pMenu, const QString &sId, const QString &sText,
                        const QString &sAccel = QString(), bool bCheckable = false);
    /// 追加一个只用于说明/分节的不可点项（enabled=false）
    QAction *addHintItem(QMenu *pMenu, const QString &sId, const QString &sText);
    /// 追加一个互斥项（主题菜单用）
    QAction *addRadioItem(QMenu *pMenu, QActionGroup *pGroup, const QString &sId,
                          const QString &sText, bool bEnabled);
    /// 追加一个外链项（帮助菜单用），点击即用系统浏览器打开
    QAction *addLinkItem(QMenu *pMenu, const QString &sId, const QString &sText,
                         const QString &sUrl);
    /// 追加分隔线
    void addSeparator(QMenu *pMenu);

    void buildFileMenu(QMenuBar *pBar);
    void buildEditMenu(QMenuBar *pBar);
    void buildParagraphMenu(QMenuBar *pBar);
    void buildFormatMenu(QMenuBar *pBar);
    void buildWindowMenu(QMenuBar *pBar);
    void buildThemeMenu(QMenuBar *pBar);
    void buildViewMenu(QMenuBar *pBar);
    void buildHelpMenu(QMenuBar *pBar);

    QHash<QString, QAction *> m_hashActions;      ///< id -> action
    QMenuBar     *m_pMenuBar;                     ///< 宿主菜单栏（用于快捷键清单）
    QActionGroup *m_pThemeGroup;                  ///< 主题项互斥组
    QAction      *m_pFollowSystemAction;          ///< 跟随系统主题
    QAction      *m_pThemeDisabledHint;           ///< "跟随系统时无法选择主题"
    QMenu        *m_pRecentMenu;                  ///< 「打开最近」子菜单
    QAction      *m_pClearRecentAction;           ///< 「清除最近使用」
};

#endif // MARKTEXTMENU_H
