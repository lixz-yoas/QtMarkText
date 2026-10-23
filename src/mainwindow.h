#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>
#include <QStringList>

class QCloseEvent;
class QEvent;
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

    /// 首次显示时按上次的"正常大小"落位。**不能放在构造函数里**：Qt 的 show()
    /// 还会用它自己记的 geometry 再 SetWindowPos 一次，把构造函数里设的值冲掉
    /// （实测尺寸被还原成 1200x800 + Qt 假定的边框 16x39 = 1216x839）。
    void showEvent(QShowEvent *pEvent) override;

    /// 窗口状态变化（最大化↔还原）→ 刷新右上角"最大化/还原"按钮的图标
    void changeEvent(QEvent *pEvent) override;

    /// 跟踪"正常大小"。无系统标题栏 + WM_NCCALCSIZE 抹平非客户区之后，
    /// 系统/Qt 记的还原位置会被污染（实测"还原"后窗口仍是最大化尺寸 2576x1408），
    /// 所以正常态下的每次移动/缩放都自己记一份，还原时显式用回去。
    void resizeEvent(QResizeEvent *pEvent) override;
    void moveEvent(QMoveEvent *pEvent) override;

    /// 菜单栏空白处：左键拖动窗口、双击最大化/还原（Windows 标题栏手感）
    bool eventFilter(QObject *pWatched, QEvent *pEvent) override;

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

    // ---- 无系统标题栏的自绘顶栏：菜单栏拖动 + 右上角窗口按钮 ----
    /// 焦点（屏幕坐标）是否落在"菜单栏空白处"。菜单项本身、右上角窗口按钮不算，
    /// 只有空白处才当作标题栏（可拖动窗口 / 双击最大化还原）。
    bool isMenuBarCaptionArea(const QPoint &ptGlobal) const;
    /// 刷新右上角"最大化/还原"按钮的图标与描边色（跟随外壳主题）
    void updateWindowButtons();
    /// 把三颗窗口按钮的**稳定后**真实屏幕坐标打进日志（延后一拍调用，
    /// 供外部自动化脚本精确对齐点击位置；窗口状态切换时布局当拍还没稳）。
    void logWindowButtonCenters();
    /// 安排一次（防抖）按钮坐标诊断
    void scheduleButtonDiag();
    /// 最大化 ↔ 还原。还原时显式回到自己记着的"正常大小"（不用系统的还原位置，
    /// 那玩意儿在本窗口下实测是被污染的）。
    void toggleMaximizeRestore();
    /// 记下当前"正常大小"（仅在非最大化/最小化/全屏时有效）
    void rememberNormalGeometry();
    /// 退出时把"正常大小"（最大化/全屏时取 normalGeometry）落盘；
    /// 下次启动按它还原 —— 即"保留正常大小作为下次启动时的大小"。
    void saveWindowGeometry();
    /// 启动时按上次的"正常大小"还原窗口位置与尺寸
    void restoreWindowGeometry();

    // ---- 侧边栏与目录（默认打开视图）----
    /// 把目录交给渲染层的项目树（对照 Electron 主进程的 mt::open-directory），
    /// 并把它记为"上次打开的文件夹"
    void openFolderInShell(const QString &sDir);
    /// 渲染层就绪后，若上次的文件夹仍在则自动重新打开（只做一次）
    void restoreLastOpenedFolder();
    /// "上次打开的文件夹"存 ini 的 [ui] 节 —— 属外壳自有界面状态，
    /// 不进偏好表（偏好表是全量下发给渲染层、也会被它回传覆盖的）。
    QString lastOpenedFolder() const;
    void    setLastOpenedFolder(const QString &sDir);
    /// 传给 Windows 原生文件对话框（打开/保存）的起始目录：
    /// 必须是反斜杠且真实存在，见 dialogStartDir() 实现处的说明
    QString dialogStartDir() const;

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
    QWidget           *m_pWindowButtons;      // 菜单栏右上角的 最小化/最大化/关闭（自定义绘制）
    QTimer            *m_pButtonsDiagTimer;   // 按钮坐标诊断的"稳态"延时器
    QStringList        m_listRecentFiles;
    QString            m_sLastDir;
    bool               m_bLastFolderRestored; // 上次文件夹只自动恢复一次
    bool               m_bGeometryRestored;   // 上次窗口大小只还原一次（showEvent 里做）
    QRect              m_rcNormalGeometry;    // 自记的"正常大小"（非最大化时）
    bool               m_bNormalGeometryValid;// m_rcNormalGeometry 是否已可用
    QString            m_sLastCommandId;      // 命令去重（菜单快捷键有两条到达路径）
    qint64             m_nLastCommandMs;
    bool               m_bDraggingMenuBar;    // 菜单栏空白处按住拖动中
    QPoint             m_ptDragOrigin;        // 拖动起点（全局）
    QPoint             m_ptDragWindowOrigin;  // 拖动开始时的窗口位置
};

#endif // MAINWINDOW_H
