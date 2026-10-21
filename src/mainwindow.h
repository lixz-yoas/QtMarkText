#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>
#include <QStringList>

class QCloseEvent;
class QAction;
class QDockWidget;
class QFileSystemModel;
class QColor;
class QLabel;
class QTabWidget;
class QTimer;
class QTreeView;

class IEditorEngine;
class MarkTextMenu;
class PreferencesDialog;
class PreferencesStore;

/**
 * @brief 主窗口 —— MarkText 外壳层的 Qt 实现
 *
 * 重构方向（2026-10-12 起，见 readme.md）：
 *   「Qt 外壳 + 调用 MarkText 自己的渲染层」。
 *   —— 外壳（本类）替换掉 MarkText 的 Electron 主进程：窗口、菜单、标签页、
 *      文件树、状态栏、文件读写、偏好设置；
 *   —— 「里面」不动：编辑与渲染由 MarkText 的渲染层承担，经 IEditorEngine
 *      接口挂载（当前为占位引擎，WebView2 接入后原样替换）。
 *
 * 职责边界：
 *   本类**不**直接操作编辑器控件，只通过 IEditorEngine 接口；
 *   菜单一律由 MarkTextMenu 构建，本类只负责"点了做什么"。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *pParent = nullptr);
    ~MainWindow() override;

    /// 打开一个 Markdown 文件（命令行参数 / 文件树双击 / 菜单共用）
    bool openFile(const QString &sPath);

protected:
    /// 去掉系统标题栏但保留边框（对齐 MarkText titleBarStyle=custom 的自绘标题栏）。
    /// 客户区扩展到整个窗口矩形；最大化时按系统边框内缩，防止窗口超出屏幕。
    bool nativeEvent(const QByteArray &eventType, void *pMessage, long *pnResult) override;

    /// 关闭窗口时的退出时序处理（WebView2 必须在消息循环尚在时关闭，否则进程退出不干净）
    void closeEvent(QCloseEvent *pEvent) override;

private slots:
    /// WebView2 内按下了 Alt → 呼出/收起菜单栏（对齐 MarkText）
    void onAltPressed();
    /// 渲染层标题栏窗口按钮（mt::win::*）→ 外壳执行
    void onWindowAction(const QString &sAction);
    /// 菜单命令总入口：sId 即 MarkText 的菜单项 id
    void onMenuCommand(const QString &sId);
    /// 主题菜单里选中了某个主题
    void onThemeSelected(const QString &sThemeId);
    /// 勾选/取消「跟随系统主题」
    void onFollowSystemThemeToggled(bool bFollow);
    /// 点击「打开最近」里的文件
    void onRecentFileActivated(const QString &sPath);
    void onTabCloseRequested(int nIndex);
    void onCurrentTabChanged(int nIndex);
    /// 偏好设置窗口改了主题/语言
    void onPreferencesThemeChanged(const QString &sThemeId, bool bFollowSystemTheme);
    void onPreferencesLanguageChanged(const QString &sLanguage);

private:
    void createCentral();
    void createDocks();
    void createStatusBar();
    void createMenus();

    // ---- 引擎（渲染层挂载点）----
    IEditorEngine *addEditorTab();
    IEditorEngine *currentEngine() const;
    IEditorEngine *engineAt(int nIndex) const;
    int            indexOfEngine(const IEditorEngine *pEngine) const;

    /// 菜单快捷键接管：焦点在 WebView2 里时按键进不了 Qt，由 WebView2 的
    /// AcceleratorKeyPressed 回调到这里。与菜单里各 QAction 的快捷键匹配，
    /// 命中就"认领"（返回 true → 宿主导致 put_Handled 吞掉按键）并延后执行命令。
    /// nQtKey / nQtModifiers 均为 Qt 值（Qt::Key_* / Qt::*Modifier）。
    bool tryShortcut(int nQtKey, int nQtModifiers);

    // ---- 文件 ----
    bool saveCurrentFile(bool bSaveAs);
    /// 剪切/复制/粘贴：渲染层只监听 mt::editor-edit-action 的 undo/redo/… 这些，
    /// cut/copy/paste 在原版 MarkText 里走 Electron 原生 webContents.cut/copy/paste，
    /// 渲染层没有任何对应监听。故 WebView2 引擎下由外壳注入 Ctrl+X/C/V。
    /// 返回 true 表示本条命令已被本函数消化。
    bool dispatchClipboardCommand(const QString &sId);
    void exportCurrentAsHtml();
    void exportCurrentAsPdf();
    void moveCurrentFile();
    void renameCurrentFile();
    void importIntoCurrent();

    // ---- 最近打开（MarkText 最多保留 12 条）----
    void loadRecentFiles();
    void addRecentFile(const QString &sPath);
    void saveRecentFiles();
    void clearRecentFiles();

    // ---- 界面同步 ----
    void refreshStatus();
    void refreshTitle();
    void syncAutoSaveMenu();
    void syncViewMenu();
    void syncThemeMenu();
    void applyShellTheme();
    /// 实际生效的主题 id（开启「跟随系统主题」时解析成 light/dark 里对应的那个）。
    /// 渲染层要的是"生效值"，直接给它 preference.theme 会漏掉跟随系统这一层。
    QString effectiveThemeId() const;
    /// 把"实际生效的主题"广播给渲染层。
    /// 渲染层的主题是 Vuex 的 theme 状态（watch 到变化才重新 addThemeStyle），
    /// 而它只在启动时从 URL 的 theme= 参数读一次；外壳换主题若不广播，
    /// 编辑区就得重启才换肤 —— 用户反馈的"改了回不去"正是这个原因。
    void pushThemeToRenderer();
    /// 深色主题下给菜单栏与弹出菜单上色（黑底白字）；
    /// 浅色主题清空 QSS，还原系统默认观感。
    void applyMenuBarStyle(bool bDark);
    /// 把当前主题的编辑器背景色同步到各 WebView2 引擎宿主
    /// （view palette + WebView2 默认背景色，消除加载闪白与色差边）
    void applyEngineHostBackground(const QColor &color);
    void showPendingMessage(const QString &sWhat);

    /// 字数/字符数/段落数（对齐 MarkText 状态栏的 counters）
    static QString counterText(const QString &sMarkdown);

    MarkTextMenu      *m_pMenu;
    QTabWidget        *m_pTabWidget;
    QTreeView         *m_pFileTree;
    QFileSystemModel  *m_pFileModel;
    QDockWidget       *m_pFileDock;
    QLabel            *m_pStatusEngine;
    QLabel            *m_pStatusCounter;
    QLabel            *m_pStatusPath;
    PreferencesDialog *m_pPreferences;
    PreferencesStore  *m_pStore;
    QTimer            *m_pAutoSaveTimer;
    QStringList        m_listRecentFiles;
    QString            m_sLastDir;
};

#endif // MAINWINDOW_H
