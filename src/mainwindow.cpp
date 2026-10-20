#include "mainwindow.h"

#include "editorhost.h"
#include "marktextbridge.h"
#include "marktextmenu.h"
#include "marktextthemes.h"
#include "preferencesdialog.h"
#include "preferencesstore.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPdfWriter>
#include <QPointer>
#include <QSettings>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QTreeView>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef SM_CXPADDEDBORDER
#define SM_CXPADDEDBORDER 92    // 老 MinGW winuser.h 未定义（XP+ 才有）
#endif
#endif

namespace {

/// 该 MarkText 主题 id 是否属于深色系（用于外壳自身的调色板）
bool isDarkThemeId(const QString &sThemeId)
{
    for (int i = 0; i < MarkTextThemes::kDarkThemeCount; ++i) {
        if (sThemeId == QLatin1String(MarkTextThemes::kDarkThemes[i].pszId)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 外壳调色板用的每主题配色（对照 MarkText 渲染层主题 CSS）
 *
 * 色值来源：packages/desktop/src/renderer/src/assets/themes/<id>.theme.css
 *   - cWindow   ：--sideBarBgColor / floatBgColor（外壳菜单、浮层底色）
 *   - cBase     ：--itemBgColor / --floatBgColor（输入框、QMenu 弹层底色）
 *   - cText     ：--editorColor / --floatFontColor（正文与菜单文字）
 *   - cHighlight：--themeColor（强调/选中）
 *   - cEditorBg ：--editorBgColor（编辑器区背景，同步给 WebView2 宿主做加载底色）
 * 未列出的主题：深色系走通用深色，浅色系走 Qt 默认。
 */
struct ShellThemeColors
{
    const char *pszTheme;
    QColor cWindow;
    QColor cBase;
    QColor cText;
    QColor cHighlight;
    QColor cHighlightText;
    QColor cDisabledText;
    QColor cEditorBg;
};

// ayu-mirage：取自 ayu-mirage.theme.css（本机 MarkText 正在使用的主题）
//   sideBarBg #1b1f2a / itemBg #272d38 / editorBg #1f2430
//   editorColor #cbccc6 / themeColor #ffcc66 / linkColor #73d0ff
const ShellThemeColors kAyuMirageColors = {
    "ayu-mirage",
    QColor(0x27, 0x2d, 0x38),   // Window：floatBgColor #272d38（QMenu 弹层与菜单同源）
    QColor(0x27, 0x2d, 0x38),   // Base：itemBgColor #272d38
    QColor(0xcb, 0xcc, 0xc6),   // Text：editorColor #cbccc6
    QColor(0xff, 0xcc, 0x66),   // Highlight：themeColor #ffcc66
    QColor(0x1f, 0x24, 0x30),   // HighlightedText：buttonPrimaryFontColor #1f2430
    QColor(0x69, 0x6d, 0x71),   // Disabled：editorColor 40% 叠在 #272d38 上
    QColor(0x1f, 0x24, 0x30)    // EditorBg：editorBgColor #1f2430
};

// 通用深色（此前版本的兜底配色，未单独定义的深色主题沿用）
const ShellThemeColors kGenericDarkColors = {
    "",
    QColor(53, 53, 53),
    QColor(35, 35, 35),
    QColor(220, 220, 220),
    QColor(68, 114, 196),
    QColor(255, 255, 255),
    QColor(127, 127, 127),
    QColor(35, 35, 35)
};

/// 查某深色主题的外壳配色；未命中返回通用深色
const ShellThemeColors &shellColorsForTheme(const QString &sThemeId)
{
    if (sThemeId == QLatin1String(kAyuMirageColors.pszTheme)) {
        return kAyuMirageColors;
    }
    return kGenericDarkColors;
}

/// 浅色主题的编辑器背景（light.theme.css：--editorBgColor: #ffffff）
QColor lightEditorBackground()
{
    return QColor(0xff, 0xff, 0xff);
}

} // namespace

//=====================================================================
MainWindow::MainWindow(QWidget *pParent)
    : QMainWindow(pParent)
    , m_pMenu(nullptr)
    , m_pTabWidget(nullptr)
    , m_pFileTree(nullptr)
    , m_pFileModel(nullptr)
    , m_pFileDock(nullptr)
    , m_pStatusEngine(nullptr)
    , m_pStatusCounter(nullptr)
    , m_pStatusPath(nullptr)
    , m_pPreferences(nullptr)
    , m_pStore(PreferencesStore::instance())
    , m_pAutoSaveTimer(nullptr)
    , m_sLastDir(QDir::homePath())
{
    setWindowTitle(QStringLiteral("QtMarkText"));
    resize(1200, 800);

    createMenus();
    createCentral();
    createDocks();
    createStatusBar();

    loadRecentFiles();
    syncAutoSaveMenu();
    syncViewMenu();
    syncThemeMenu();

    // 自动保存：preference.autoSave / autoSaveDelay
    m_pAutoSaveTimer = new QTimer(this);
    m_pAutoSaveTimer->setSingleShot(true);
    connect(m_pAutoSaveTimer, &QTimer::timeout, this, [this]() {
        if (!m_pStore->boolValue(QStringLiteral("autoSave"))) {
            return;
        }
        IEditorEngine *pEngine = currentEngine();
        if (pEngine == nullptr || pEngine->filePath().isEmpty() || !pEngine->isModified()) {
            return;
        }
        if (pEngine->saveToFile(pEngine->filePath())) {
            statusBar()->showMessage(
                QStringLiteral("已自动保存：%1").arg(pEngine->filePath()), 3000);
            refreshTitle();
        }
    });

    applyShellTheme();

    // ---- 外壳视觉对齐 MarkText（v0.19.1 Windows）----
    // MarkText 的标签栏/侧栏/状态栏全部由渲染层自绘，外壳只提供窗口：
    //   - 标签栏：渲染层有自己的 tab 栏
    //   - 文件 dock：渲染层有内置侧栏（openFolder）
    //   - 状态栏：渲染层有状态栏
    // 菜单栏则改为**常驻显示**（2026-10-13 起）：渲染层的自绘菜单只在页面内，
    // 外壳菜单栏放在窗口最上一行，常驻可见、按业界习惯可直接点。
    menuBar()->setVisible(true);
    m_pTabWidget->tabBar()->setVisible(false);
    m_pFileDock->setVisible(false);
    statusBar()->setVisible(false);

    addEditorTab();
    refreshStatus();
}

MainWindow::~MainWindow()
{
    qInfo("[退出] ~MainWindow 进入（剩余顶层窗口 %d 个）",
          QApplication::topLevelWidgets().size());
    qInfo("[退出] ~MainWindow 退出");
}

void MainWindow::closeEvent(QCloseEvent *pEvent)
{
    // 退出诊断：Qt 只在「没有可见的、无父的顶层窗口」时才自动退出事件循环。
    // 把当前顶层窗口全部列出来，谁挡着退出就一目了然（详见 readme 退出分析）。
    const QList<QWidget *> listTops = QApplication::topLevelWidgets();
    qInfo("[退出] closeEvent：顶层窗口 %d 个", listTops.size());
    for (QWidget *pW : listTops) {
        if (pW == nullptr) {
            continue;
        }
        qInfo("[退出]   - %s visible=%d parent=%p type=%d",
              pW->metaObject()->className(), pW->isVisible() ? 1 : 0,
              reinterpret_cast<void *>(pW->parentWidget()),
              static_cast<int>(pW->windowType()));
    }

    // ---- 关键：在消息循环还活着的时候关掉 WebView2 ----
    // WebView2 的 Close()/环境释放是"发 IPC 让浏览器进程收尾"的异步流程，
    // 要靠消息泵把回调派完。若拖到 main() 返回之后（事件循环已停摆）才做，
    // 清理只能等内核超时，实测进程要卡 18 秒才真正消失（用户观感=退不干净）。
    // 这里提前关，并把随之产生的异步消息当场消化掉。
    const QList<IEditorEngine *> listEngines = findChildren<IEditorEngine *>();
    for (IEditorEngine *pEngine : listEngines) {
        if (pEngine != nullptr) {
            pEngine->shutdown();
        }
    }
    if (!listEngines.isEmpty()) {
        for (int i = 0; i < 10; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        }
        qInfo("[退出] 渲染层已关闭并消化完异步消息");
    }

    // ---- 偏好与最近文件落盘 ----
    // PreferencesStore 是永不析构的堆单例，其 QSettings 只在显式 sync() 时才写盘；
    // 不补这一刀，用户刚改的偏好会随进程一起消失（退出流程见 readme 退出分析）。
    if (m_pStore != nullptr) {
        m_pStore->sync();
        qInfo("[退出] 偏好设置已落盘");
    }
    saveRecentFiles();

    QMainWindow::closeEvent(pEvent);
}

//=====================================================================
//  外壳搭建
//=====================================================================
void MainWindow::onAltPressed()
{
    // 菜单栏已改为常驻显示（见构造函数），Alt 不再用于收起；
    // 这里只保证它处于可见状态，避免渲染层 Alt 事件把菜单栏关掉。
    if (!menuBar()->isVisible()) {
        menuBar()->setVisible(true);
    }
}

#ifdef Q_OS_WIN
//=====================================================================
//  无边框窗口（titleBarStyle=custom 对齐）
//=====================================================================
bool MainWindow::nativeEvent(const QByteArray &eventType, void *pMessage, long *pnResult)
{
    Q_UNUSED(eventType)
    MSG *pMsg = static_cast<MSG *>(pMessage);
    if (pMsg != nullptr && pMsg->message == WM_NCCALCSIZE && pMsg->wParam == TRUE) {
        // 客户区 = 整个窗口矩形（吞掉系统标题栏）；保留边框以获得
        // 系统边缘缩放 / Aero Snap / 最小化动画，与 Electron frameless 一致。
        // 最大化时系统边框被推到屏幕外，按 SM_CXSIZEFRAME+SM_CXPADDEDBORDER 内缩。
        RECT *pRect = &reinterpret_cast<NCCALCSIZE_PARAMS *>(pMsg->lParam)->rgrc[0];
        if (IsZoomed(pMsg->hwnd)) {
            const int nFrame = GetSystemMetrics(SM_CXSIZEFRAME)
                             + GetSystemMetrics(SM_CXPADDEDBORDER);
            pRect->left   += nFrame;
            pRect->top    += nFrame;
            pRect->right  -= nFrame;
            pRect->bottom -= nFrame;
        }
        *pnResult = 0;
        return true;
    }
    return QMainWindow::nativeEvent(eventType, pMessage, pnResult);
}
#endif

void MainWindow::onWindowAction(const QString &sAction)
{
    // 渲染层标题栏窗口按钮 → 外壳窗口动作（对照 Electron 的 win.send('mt::win::*')）
    if (sAction == QLatin1String("mt::win::minimize")) {
        showMinimized();
    } else if (sAction == QLatin1String("mt::win::maximize")) {
        showMaximized();
    } else if (sAction == QLatin1String("mt::win::unmaximize")) {
        showNormal();
    } else if (sAction == QLatin1String("mt::win::toggle-maximize")) {
        isMaximized() ? showNormal() : showMaximized();
    } else if (sAction == QLatin1String("mt::win::close")) {
        close();
    } else if (sAction == QLatin1String("mt::win::toggle-fullscreen")) {
        isFullScreen() ? showNormal() : showFullScreen();
    } else if (sAction.startsWith(QLatin1String("mt::win::set-fullscreen:"))) {
        if (sAction.endsWith(QLatin1Char('1'))) {
            showFullScreen();
        } else {
            showNormal();
        }
    } else if (sAction.startsWith(QLatin1String("mt::win::zoom:"))) {
        // 页面缩放由渲染层自己处理（EDIT_ZOOM Vuex action），外壳无需跟随
        qDebug("窗口缩放请求（渲染层自处理）: %s", qPrintable(sAction));
    } else {
        qDebug("未处理的窗口动作: %s", qPrintable(sAction));
    }
}

void MainWindow::createMenus()
{
    // 菜单结构、文案、快捷键全部由 MarkTextMenu 按 MarkText 原样构建
    m_pMenu = new MarkTextMenu(menuBar(), this);

    // 状态栏要跟着配置走
    connect(m_pMenu, &MarkTextMenu::commandTriggered, this, &MainWindow::onMenuCommand);
    connect(m_pMenu, &MarkTextMenu::themeSelected, this, &MainWindow::onThemeSelected);
    connect(m_pMenu, &MarkTextMenu::followSystemThemeToggled,
            this, &MainWindow::onFollowSystemThemeToggled);
    connect(m_pMenu, &MarkTextMenu::recentFileActivated,
            this, &MainWindow::onRecentFileActivated);
}

void MainWindow::createCentral()
{
    m_pTabWidget = new QTabWidget(this);
    m_pTabWidget->setTabsClosable(true);
    m_pTabWidget->setMovable(true);
    m_pTabWidget->setDocumentMode(true);

    connect(m_pTabWidget, &QTabWidget::tabCloseRequested,
            this, &MainWindow::onTabCloseRequested);
    connect(m_pTabWidget, &QTabWidget::currentChanged,
            this, &MainWindow::onCurrentTabChanged);

    setCentralWidget(m_pTabWidget);
}

void MainWindow::createDocks()
{
    m_pFileModel = new QFileSystemModel(this);
    m_pFileModel->setRootPath(QDir::homePath());

    m_pFileTree = new QTreeView(this);
    m_pFileTree->setModel(m_pFileModel);
    m_pFileTree->setRootIndex(m_pFileModel->index(QDir::homePath()));
    m_pFileTree->setHeaderHidden(true);
    for (int i = 1; i < m_pFileModel->columnCount(); ++i) {
        m_pFileTree->hideColumn(i);
    }
    connect(m_pFileTree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        const QString sPath = m_pFileModel->filePath(index);
        if (QFileInfo(sPath).isFile()) {
            openFile(sPath);
        }
    });

    m_pFileDock = new QDockWidget(QStringLiteral("文件"), this);
    m_pFileDock->setObjectName(QStringLiteral("fileDock"));
    m_pFileDock->setWidget(m_pFileTree);
    addDockWidget(Qt::LeftDockWidgetArea, m_pFileDock);
}

void MainWindow::createStatusBar()
{
    // 对齐 MarkText 状态栏：左侧路径 / 右侧 字数·字符数·段落数
    m_pStatusPath = new QLabel(this);
    statusBar()->addWidget(m_pStatusPath, 1);

    m_pStatusCounter = new QLabel(this);
    statusBar()->addPermanentWidget(m_pStatusCounter);

    m_pStatusEngine = new QLabel(this);
    statusBar()->addPermanentWidget(m_pStatusEngine);
}

//=====================================================================
//  引擎（渲染层挂载点）
//=====================================================================
IEditorEngine *MainWindow::addEditorTab()
{
    const EditorHost::EngineKind kind = EditorHost::requestedEngine();
    IEditorEngine *pEngine = EditorHost::create(kind, this, m_pTabWidget);
    pEngine->setPreferenceAll(m_pStore->all());

    // 引擎创建晚于 applyShellTheme()，这里补一次当前主题的宿主底色
    {
        const QString sTheme = m_pStore->stringValue(QStringLiteral("theme"));
        const bool bFollow = m_pStore->boolValue(QStringLiteral("followSystemTheme"));
        QColor cEditorBg = lightEditorBackground();
        if (!bFollow && isDarkThemeId(sTheme)) {
            cEditorBg = shellColorsForTheme(sTheme).cEditorBg;
        }
        if (WebView2EditorEngine *pWv2 = qobject_cast<WebView2EditorEngine *>(pEngine)) {
            pWv2->setViewBackground(cEditorBg);
        }
    }

    QWidget *pView = pEngine->view();
    const int nIndex = m_pTabWidget->addTab(pView, pEngine->displayName());
    m_pTabWidget->setCurrentIndex(nIndex);

    // Alt 呼出菜单栏（WebView2 拿走焦点后 Qt 收不到，由引擎转发）
    if (WebView2EditorEngine *pWv2 = qobject_cast<WebView2EditorEngine *>(pEngine)) {
        connect(pWv2, &WebView2EditorEngine::altKeyPressed,
                this, &MainWindow::onAltPressed);
        // 菜单快捷键接管：焦点在渲染层里时按键进不了 Qt（直接进了 WebView2 的
        // 原生窗口），QAction 的快捷键全失效。由 WebView2 的 AcceleratorKeyPressed
        // 回调进来，与菜单匹配后由外壳执行 —— 对齐 Electron 的菜单 accelerator。
        pWv2->setShortcutHandler([this](int nQtKey, int nQtModifiers) {
            return tryShortcut(nQtKey, nQtModifiers);
        });
    }

    // 渲染层标题栏的窗口按钮（最小化/最大化/关闭/全屏）→ 外壳执行
    if (pEngine->bridge() != nullptr) {
        connect(pEngine->bridge(), &MarkTextBridge::windowActionRequested,
                this, &MainWindow::onWindowAction);
    }

    connect(pEngine, &IEditorEngine::titleChanged, this, [this, pEngine]() {
        const int n = indexOfEngine(pEngine);
        if (n >= 0) {
            m_pTabWidget->setTabText(n, pEngine->displayName());
        }
        if (pEngine == currentEngine()) {
            refreshTitle();
        }
    });
    connect(pEngine, &IEditorEngine::modifiedChanged, this, [this, pEngine](bool) {
        const int n = indexOfEngine(pEngine);
        if (n >= 0) {
            m_pTabWidget->setTabText(n, pEngine->displayName());
        }
        if (pEngine == currentEngine()) {
            refreshTitle();
        }
        if (m_pAutoSaveTimer != nullptr) {
            m_pAutoSaveTimer->start(m_pStore->intValue(QStringLiteral("autoSaveDelay")));
        }
    });

    return pEngine;
}

IEditorEngine *MainWindow::engineAt(int nIndex) const
{
    if (m_pTabWidget == nullptr || nIndex < 0 || nIndex >= m_pTabWidget->count()) {
        return nullptr;
    }
    QWidget *pView = m_pTabWidget->widget(nIndex);
    const QList<IEditorEngine *> listEngines = findChildren<IEditorEngine *>();
    for (IEditorEngine *pEngine : listEngines) {
        if (pEngine != nullptr && pEngine->view() == pView) {
            return pEngine;
        }
    }
    return nullptr;
}

IEditorEngine *MainWindow::currentEngine() const
{
    return engineAt((m_pTabWidget != nullptr) ? m_pTabWidget->currentIndex() : -1);
}

int MainWindow::indexOfEngine(const IEditorEngine *pEngine) const
{
    if (m_pTabWidget == nullptr || pEngine == nullptr) {
        return -1;
    }
    for (int i = 0; i < m_pTabWidget->count(); ++i) {
        if (m_pTabWidget->widget(i) == pEngine->view()) {
            return i;
        }
    }
    return -1;
}

//=====================================================================
//  标签页
//=====================================================================
void MainWindow::onTabCloseRequested(int nIndex)
{
    IEditorEngine *pEngine = engineAt(nIndex);
    QWidget *pView = m_pTabWidget->widget(nIndex);

    m_pTabWidget->removeTab(nIndex);
    if (pEngine != nullptr) {
        pEngine->deleteLater();
    }
    if (pView != nullptr) {
        pView->deleteLater();
    }

    // 关掉最后一个就补一个空白页（MarkText 的行为）
    if (m_pTabWidget->count() == 0) {
        addEditorTab();
    }
    refreshStatus();
}

void MainWindow::onCurrentTabChanged(int nIndex)
{
    Q_UNUSED(nIndex)
    refreshStatus();
    refreshTitle();
}

//=====================================================================
//  文件操作
//=====================================================================
bool MainWindow::openFile(const QString &sPath)
{
    // 渲染层引擎：文件由渲染层开新标签承载（对应 mt::open-new-tab），
    // 外壳不再另开 QTab —— MarkText 本尊就是这样管理多文件的。
    IEditorEngine *pEngine = currentEngine();
    if (pEngine == nullptr) {
        pEngine = addEditorTab();
    }

    if (pEngine->bridge() != nullptr) {
        pEngine->openFile(sPath);
    } else if (!pEngine->loadFromFile(sPath)) {
        QMessageBox::warning(this, QStringLiteral("QtMarkText"),
                             QStringLiteral("无法打开文件：\n%1").arg(sPath));
        return false;
    }

    m_sLastDir = QFileInfo(sPath).absolutePath();
    addRecentFile(sPath);
    m_pFileTree->setRootIndex(m_pFileModel->index(m_sLastDir));

    statusBar()->showMessage(QStringLiteral("已打开：%1").arg(sPath), 5000);
    refreshStatus();
    return true;
}

bool MainWindow::saveCurrentFile(bool bSaveAs)
{
    IEditorEngine *pEngine = currentEngine();
    if (pEngine == nullptr) {
        return false;
    }

    QString sPath = pEngine->filePath();
    if (bSaveAs || sPath.isEmpty()) {
        const QString sDefault = sPath.isEmpty()
                                     ? m_sLastDir + QStringLiteral("/未命名.md")
                                     : sPath;
        sPath = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("保存 Markdown 文件"),
            sDefault,
            QStringLiteral("Markdown 文件 (*.md *.markdown);;所有文件 (*.*)"));
        if (sPath.isEmpty()) {
            return false;
        }
    }

    if (!pEngine->saveToFile(sPath)) {
        QMessageBox::warning(this, QStringLiteral("QtMarkText"),
                             QStringLiteral("无法写入文件：\n%1").arg(sPath));
        return false;
    }

    m_sLastDir = QFileInfo(sPath).absolutePath();
    addRecentFile(sPath);
    statusBar()->showMessage(QStringLiteral("已保存：%1").arg(sPath), 5000);
    refreshStatus();
    refreshTitle();
    return true;
}

/**
 * @brief 剪切 / 复制 / 粘贴 —— 渲染层引擎下改为原生按键注入
 *
 * 为什么不能走 mt::editor-edit-action：
 *   原版 MarkText 的这三项由 Electron 主进程直接调 win.webContents.cut/copy/paste()，
 *   渲染层压根没有对应的 emitter 监听（实测 emitter 上只有 undo/redo/selectAll/…）。
 *   而 WebView2 的 ICoreWebView2 没有 SendKeyEvent（本机 SDK 头 22 版都没有），
 *   所以只能：先把键盘焦点交给页面 → 再用 Win32 SendInput 注入 Ctrl+X/C/V，
 *   让 Chromium 自己的默认编辑行为去操作选区与剪贴板。
 *
 * 占位引擎（QTextEdit）不需要这么绕，返回 false 让它走 execCommand 分支。
 */
bool MainWindow::dispatchClipboardCommand(const QString &sId)
{
    QString sAction;
    if (sId == QLatin1String("cutMenuItem")) {
        sAction = QStringLiteral("cut");
    } else if (sId == QLatin1String("copyMenuItem")) {
        sAction = QStringLiteral("copy");
    } else if (sId == QLatin1String("pasteMenuItem")) {
        sAction = QStringLiteral("paste");
    } else {
        return false;
    }

    IEditorEngine *pEngine = currentEngine();
    WebView2EditorEngine *pWv2 = qobject_cast<WebView2EditorEngine *>(pEngine);
    if (pWv2 == nullptr) {
        return false;   // 占位引擎：交回 execCommand 走 Qt 剪贴板
    }

    if (!pWv2->nativeEdit(sAction)) {
        statusBar()->showMessage(
            QStringLiteral("剪贴板操作不可用：WebView2 尚未就绪"), 3000);
    }
    return true;
}

void MainWindow::exportCurrentAsHtml()
{
    IEditorEngine *pEngine = currentEngine();
    if (pEngine == nullptr) {
        return;
    }

    const QString sDefault = m_sLastDir + QStringLiteral("/")
                             + QFileInfo(pEngine->displayName()).completeBaseName()
                             + QStringLiteral(".html");
    const QString sPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出为 HTML"), sDefault,
        QStringLiteral("HTML 文件 (*.html)"));
    if (sPath.isEmpty()) {
        return;
    }

    QString sHtml = pEngine->exportHtml();
    if (sHtml.isEmpty()) {
        // 便宜兜底：把 Markdown 源码包进 <pre>，等渲染层接管后换上有样式版
        QString sBody = pEngine->markdownText().toHtmlEscaped();
        sHtml = QStringLiteral(
                    "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n"
                    "<meta charset=\"utf-8\">\n<title>%1</title>\n"
                    "<style>body{font-family:system-ui,'Microsoft YaHei',sans-serif;"
                    "max-width:52em;margin:2em auto;padding:0 1em;line-height:1.7}"
                    "pre{white-space:pre-wrap}</style>\n</head>\n<body>\n<pre>%2</pre>\n</body>\n</html>\n")
                    .arg(pEngine->displayName().toHtmlEscaped(), sBody);
    }

    QFile file(sPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("QtMarkText"),
                             QStringLiteral("无法写入：\n%1").arg(sPath));
        return;
    }
    file.write(sHtml.toUtf8());
    file.close();
    statusBar()->showMessage(QStringLiteral("已导出：%1").arg(sPath), 5000);
}

void MainWindow::exportCurrentAsPdf()
{
    IEditorEngine *pEngine = currentEngine();
    if (pEngine == nullptr) {
        return;
    }

    const QString sDefault = m_sLastDir + QStringLiteral("/")
                             + QFileInfo(pEngine->displayName()).completeBaseName()
                             + QStringLiteral(".pdf");
    const QString sPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出为 PDF"), sDefault, QStringLiteral("PDF 文件 (*.pdf)"));
    if (sPath.isEmpty()) {
        return;
    }

    // 纯 Qt 的 PDF 导出（QPdfWriter 在 QtGui 里，不需要 printsupport 模块）
    QPdfWriter writer(sPath);
    writer.setPageSize(QPagedPaintDevice::A4);
    writer.setResolution(96);
    writer.setPageMargins(QMarginsF(20, 20, 20, 20), QPageLayout::Millimeter);

    QTextDocument doc;
    doc.setDefaultFont(font());
    doc.setPageSize(QSizeF(writer.width(), writer.height()));

    const QString sHtml = pEngine->exportHtml();
    if (!sHtml.isEmpty()) {
        doc.setHtml(sHtml);
    } else {
        doc.setPlainText(pEngine->markdownText());
    }
    doc.print(&writer);

    statusBar()->showMessage(QStringLiteral("已导出：%1").arg(sPath), 5000);
}

void MainWindow::moveCurrentFile()
{
    IEditorEngine *pEngine = currentEngine();
    if (pEngine == nullptr || pEngine->filePath().isEmpty()) {
        showPendingMessage(QStringLiteral("移动到（请先保存文件）"));
        return;
    }

    const QString sDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("移动到"), QFileInfo(pEngine->filePath()).absolutePath());
    if (sDir.isEmpty()) {
        return;
    }

    const QString sFrom = pEngine->filePath();
    const QString sTo = sDir + QStringLiteral("/") + QFileInfo(sFrom).fileName();
    if (sFrom == sTo || !QFile::rename(sFrom, sTo)) {
        QMessageBox::warning(this, QStringLiteral("QtMarkText"),
                             QStringLiteral("移动失败：\n%1").arg(sFrom));
        return;
    }
    pEngine->setFilePath(sTo);
    statusBar()->showMessage(QStringLiteral("已移动到：%1").arg(sTo), 5000);
    refreshStatus();
    refreshTitle();
}

void MainWindow::renameCurrentFile()
{
    IEditorEngine *pEngine = currentEngine();
    if (pEngine == nullptr || pEngine->filePath().isEmpty()) {
        showPendingMessage(QStringLiteral("重命名（请先保存文件）"));
        return;
    }

    const QFileInfo fi(pEngine->filePath());
    bool bOk = false;
    const QString sName = QInputDialog::getText(
        this, QStringLiteral("重命名"), QStringLiteral("新文件名："),
        QLineEdit::Normal, fi.fileName(), &bOk);
    if (!bOk || sName.trimmed().isEmpty()) {
        return;
    }

    const QString sFrom = pEngine->filePath();
    const QString sTo = fi.absolutePath() + QStringLiteral("/") + sName.trimmed();
    if (sFrom == sTo) {
        return;
    }
    if (!QFile::rename(sFrom, sTo)) {
        QMessageBox::warning(this, QStringLiteral("QtMarkText"),
                             QStringLiteral("重命名失败：\n%1").arg(sFrom));
        return;
    }
    pEngine->setFilePath(sTo);
    statusBar()->showMessage(QStringLiteral("已重命名：%1").arg(sTo), 5000);
    refreshStatus();
    refreshTitle();
}

void MainWindow::importIntoCurrent()
{
    // MarkText 的「导入」支持 docx/odt/epub 等，依赖渲染层与 Electron 侧的转换器，
    // 外壳本轮只把入口接出来。
    showPendingMessage(QStringLiteral("导入"));
}

//=====================================================================
//  最近打开
//=====================================================================
void MainWindow::loadRecentFiles()
{
    QSettings settings(PreferencesStore::iniFilePath(), QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    m_listRecentFiles = settings.value(QStringLiteral("ui/recentlyUsedFiles")).toStringList();

    // 过滤已不存在的路径（对齐 MarkText 的 getRecentlyUsedDocuments）
    QStringList listValid;
    for (const QString &sPath : m_listRecentFiles) {
        if (QFileInfo::exists(sPath)) {
            listValid.append(sPath);
        }
    }
    m_listRecentFiles = listValid.mid(0, 12);

    m_pMenu->setRecentFiles(m_listRecentFiles);
}

void MainWindow::addRecentFile(const QString &sPath)
{
    m_listRecentFiles.removeAll(sPath);
    m_listRecentFiles.prepend(sPath);
    while (m_listRecentFiles.size() > 12) {
        m_listRecentFiles.removeLast();
    }
    m_pMenu->setRecentFiles(m_listRecentFiles);
    saveRecentFiles();
}

void MainWindow::saveRecentFiles()
{
    QSettings settings(PreferencesStore::iniFilePath(), QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    settings.setValue(QStringLiteral("ui/recentlyUsedFiles"), m_listRecentFiles);
    settings.sync();
}

void MainWindow::clearRecentFiles()
{
    m_listRecentFiles.clear();
    m_pMenu->setRecentFiles(m_listRecentFiles);
    saveRecentFiles();
    statusBar()->showMessage(QStringLiteral("已清除最近使用记录"), 3000);
}

void MainWindow::onRecentFileActivated(const QString &sPath)
{
    if (!QFileInfo::exists(sPath)) {
        QMessageBox::information(this, QStringLiteral("QtMarkText"),
                                 QStringLiteral("文件已不存在：\n%1").arg(sPath));
        m_listRecentFiles.removeAll(sPath);
        m_pMenu->setRecentFiles(m_listRecentFiles);
        saveRecentFiles();
        return;
    }
    openFile(sPath);
}

//=====================================================================
//  菜单命令分发
//=====================================================================
bool MainWindow::tryShortcut(int nQtKey, int nQtModifiers)
{
    if (menuBar() == nullptr || nQtKey == 0) {
        return false;
    }

    const QKeySequence seq(nQtModifiers | nQtKey);

    // 菜单项都在 menuBar 的子孙里（QMenuBar → QMenu → QAction），递归找一次即可。
    // 按一次键遍历百来个 action 做整数比较，代价在微秒级，不值得再加缓存。
    const QList<QAction *> listActions = menuBar()->findChildren<QAction *>();
    for (QAction *pAction : listActions) {
        if (pAction == nullptr || !pAction->isEnabled()) {
            continue;
        }
        const QKeySequence seqAction = pAction->shortcut();
        if (seqAction.isEmpty()
                || seqAction.matches(seq) != QKeySequence::ExactMatch) {
            continue;
        }

        const QString sId = pAction->objectName();

        // 剪切/复制/粘贴放行给 Chromium 原生处理（它本来就能用）。
        // 若这里吞掉再注入 Ctrl+X/C/V，注入的按键会再次触发本函数 —— 死循环。
        if (sId == QLatin1String("cutMenuItem")
                || sId == QLatin1String("copyMenuItem")
                || sId == QLatin1String("pasteMenuItem")) {
            return false;
        }

        // 这里还处在 WebView2 的按键回调里，不要同步执行重命令
        //（可能弹模态对话框、发 IPC、注入按键）；只"认领"这个组合键，
        // 真正执行推到事件循环下一拍。
        QPointer<QAction> pGuard(pAction);
        QTimer::singleShot(0, this, [pGuard]() {
            if (pGuard != nullptr) {
                pGuard->trigger();
            }
        });
        return true;
    }
    return false;
}

void MainWindow::onMenuCommand(const QString &sId)
{
    // ---------------- 渲染层优先（外壳与渲染层都实现同一件事时）----------------
    // 文档内容、标签页、导出/打印对话框都在渲染层手里；外壳若抢着做，
    // 轻则做了没用（外壳侧的文件 dock 已隐藏），重则出错——
    // 例如"保存"：外壳的 markdownText() 对渲染层引擎返回空串，
    // 直接落盘会把用户文件清空（MarkTextBridge 已接管 mt::response-file-save，
    // 真正的落盘在那边完成）。渲染层没有对应路由时 routeMenuCommand 返回 false，
    // 继续往下走外壳自己的实现（占位引擎场景）。
    static const char *const kRendererFirst[] = {
        "newTabMenuItem", "saveMenuItem", "saveAsMenuItem",
        "moveToMenuItem", "renameMenuItem", "importMenuItem",
        "exportHtmlMenuItem", "exportPdfMenuItem", "printMenuItem",
        "closeTabMenuItem"
    };
    for (const char *pszId : kRendererFirst) {
        if (sId != QLatin1String(pszId)) {
            continue;
        }
        IEditorEngine *pEngine = currentEngine();
        MarkTextBridge *pBridge = (pEngine != nullptr) ? pEngine->bridge() : nullptr;
        if (pBridge != nullptr && pBridge->routeMenuCommand(sId)) {
            return;   // 已转交渲染层，外壳不再插手
        }
        break;
    }

    // ---------------- 文件 ----------------
    if (sId == QLatin1String("newTabMenuItem")) {
        addEditorTab();
        return;
    }
    if (sId == QLatin1String("newWindowMenuItem")) {
        MainWindow *pWindow = new MainWindow;
        pWindow->setAttribute(Qt::WA_DeleteOnClose);
        pWindow->show();
        return;
    }
    if (sId == QLatin1String("openFileMenuItem")) {
        const QString sPath = QFileDialog::getOpenFileName(
            this, QStringLiteral("打开 Markdown 文件"), m_sLastDir,
            QStringLiteral("Markdown 文件 (*.md *.markdown *.txt);;所有文件 (*.*)"));
        if (!sPath.isEmpty()) {
            openFile(sPath);
        }
        return;
    }
    if (sId == QLatin1String("openFolderMenuItem")) {
        const QString sDir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("打开文件夹"), m_sLastDir);
        if (!sDir.isEmpty()) {
            m_sLastDir = sDir;
            // 渲染层引擎：把目录交给渲染层的项目树（对照主进程的
            //   ipcMain → win.webContents.send('mt::open-directory', pathname)
            // 渲染层收到后 OPEN_PROJECT(pathname)，侧栏与项目树由它自己管）。
            IEditorEngine *pEngine = currentEngine();
            MarkTextBridge *pBridge = (pEngine != nullptr) ? pEngine->bridge() : nullptr;
            if (pBridge != nullptr) {
                pBridge->emitToRenderer(QStringLiteral("mt::open-directory"),
                                        QJsonArray{ QDir::toNativeSeparators(sDir) });
            } else {
                m_pFileTree->setRootIndex(m_pFileModel->index(sDir));
            }
            statusBar()->showMessage(QStringLiteral("已打开文件夹：%1").arg(sDir), 5000);
        }
        return;
    }
    if (sId == QLatin1String("saveMenuItem")) {
        saveCurrentFile(false);
        return;
    }
    if (sId == QLatin1String("saveAsMenuItem")) {
        saveCurrentFile(true);
        return;
    }
    if (sId == QLatin1String("autoSaveMenuItem")) {
        QAction *pAction = m_pMenu->actionById(sId);
        m_pStore->setValue(QStringLiteral("autoSave"),
                           (pAction != nullptr) && pAction->isChecked());
        return;
    }
    if (sId == QLatin1String("moveToMenuItem")) {
        moveCurrentFile();
        return;
    }
    if (sId == QLatin1String("renameMenuItem")) {
        renameCurrentFile();
        return;
    }
    if (sId == QLatin1String("importMenuItem")) {
        importIntoCurrent();
        return;
    }
    if (sId == QLatin1String("exportHtmlMenuItem")) {
        exportCurrentAsHtml();
        return;
    }
    if (sId == QLatin1String("exportPdfMenuItem")) {
        exportCurrentAsPdf();
        return;
    }
    if (sId == QLatin1String("printMenuItem")) {
        showPendingMessage(QStringLiteral("打印"));
        return;
    }
    if (sId == QLatin1String("preferencesMenuItem")) {
        if (m_pPreferences == nullptr) {
            m_pPreferences = new PreferencesDialog(m_pStore, this);
            m_pPreferences->setKeybindings(m_pMenu->shortcutList());
            connect(m_pPreferences, &PreferencesDialog::themeChanged,
                    this, &MainWindow::onPreferencesThemeChanged);
            connect(m_pPreferences, &PreferencesDialog::languageChanged,
                    this, &MainWindow::onPreferencesLanguageChanged);
        }
        m_pPreferences->show();
        m_pPreferences->raise();
        m_pPreferences->activateWindow();
        return;
    }
    if (sId == QLatin1String("closeTabMenuItem")) {
        onTabCloseRequested(m_pTabWidget->currentIndex());
        return;
    }
    if (sId == QLatin1String("closeWindowMenuItem")) {
        close();
        return;
    }
    if (sId == QLatin1String("quitMenuItem")) {
        qApp->closeAllWindows();
        return;
    }
    if (sId == QLatin1String("menu.file.clearRecentlyUsed")) {
        clearRecentFiles();
        return;
    }

    // ---------------- 编辑：剪切 / 复制 / 粘贴 ----------------
    // 渲染层只监听 mt::editor-edit-action 里的 undo/redo/selectAll/… 这些；
    // cut/copy/paste 在原版 MarkText 里走 Electron 原生
    // win.webContents.cut()/copy()/paste()（见 main/menu/actions/edit.ts），
    // 渲染层没有任何对应监听。于是 WebView2 引擎下由外壳注入 Ctrl+X/C/V，
    // 占位引擎则返回 false、走下面 execCommand 分支的 Qt 剪贴板。
    if (dispatchClipboardCommand(sId)) {
        return;
    }

    // ---------------- 窗口（外壳自己就能做）----------------
    if (sId == QLatin1String("minimizeMenuItem")) {
        showMinimized();
        return;
    }
    if (sId == QLatin1String("fullScreenMenuItem")) {
        if (isFullScreen()) {
            showNormal();
        } else {
            showFullScreen();
        }
        return;
    }
    if (sId == QLatin1String("alwaysOnTopMenuItem")) {
        QAction *pAction = m_pMenu->actionById(sId);
        const bool bOn = (pAction != nullptr) && pAction->isChecked();
        const QPoint pos = frameGeometry().topLeft();
        setWindowFlag(Qt::WindowStaysOnTopHint, bOn);
        move(pos);
        show();
        return;
    }
    if (sId == QLatin1String("zoomInMenuItem") || sId == QLatin1String("zoomOutMenuItem")) {
        // MarkText 的缩放作用于渲染层；外壳侧改的是渲染层下发的 preference.zoom
        const double dStep = (sId == QLatin1String("zoomInMenuItem")) ? 0.1 : -0.1;
        double dZoom = m_pStore->value(QStringLiteral("zoom")).toDouble() + dStep;
        dZoom = qBound(0.5, dZoom, 2.0);
        m_pStore->setValue(QStringLiteral("zoom"), dZoom);
        IEditorEngine *pEngine = currentEngine();
        if (pEngine != nullptr) {
            pEngine->setPreference(QStringLiteral("zoom"), dZoom);
        }
        statusBar()->showMessage(QStringLiteral("缩放：%1").arg(dZoom, 0, 'f', 1), 2000);
        return;
    }

    // ---------------- 视图（外壳可管的先自己管）----------------
    if (sId == QLatin1String("sideBarMenuItem")) {
        QAction *pAction = m_pMenu->actionById(sId);
        const bool bVisible = (pAction != nullptr) && pAction->isChecked();
        m_pFileDock->setVisible(bVisible);
        m_pStore->setValue(QStringLiteral("sideBarVisibility"), bVisible);
        return;
    }
    if (sId == QLatin1String("tabBarMenuItem")) {
        QAction *pAction = m_pMenu->actionById(sId);
        const bool bVisible = (pAction != nullptr) && pAction->isChecked();
        m_pTabWidget->tabBar()->setVisible(bVisible);
        m_pStore->setValue(QStringLiteral("tabBarVisibility"), bVisible);
        return;
    }

    // ---------------- 帮助 ----------------
    if (sId == QLatin1String("checkUpdatesMenuItem")) {
        QMessageBox::information(
            this, QStringLiteral("检查更新"),
            QStringLiteral("<b>QtMarkText —— MarkText 外壳的 Qt 重写版</b><br><br>"
                           "本程序是 MarkText 的 Qt 外壳（2026-10-12 起）的本地改造版，"
                           "不接入 MarkText 官方的自动更新通道。<br><br>"
                           "上游版本：MarkText v0.19.1（MIT License）"));
        return;
    }
    if (sId == QLatin1String("aboutMenuItem")) {
        const IEditorEngine *pEngine = currentEngine();
        QMessageBox::about(
            this, QStringLiteral("关于 QtMarkText"),
            QStringLiteral(
                "<b>QtMarkText —— MarkText 的 Qt 重写版（外壳层）</b><br><br>"
                "外壳：Qt Widgets / C++（Qt %1，%2）<br>"
                "渲染层：%3<br>"
                "落盘配置：%4<br><br>"
                "参考项目：MarkText v0.19.1（MIT License）<br>"
                "上游仓库：https://github.com/marktext/marktext")
                .arg(QLatin1String(qVersion()),
                     QSysInfo::prettyProductName(),
                     EditorHost::engineTitle(
                         EditorHost::requestedEngine()),
                     PreferencesStore::iniFilePath()));
        Q_UNUSED(pEngine)
        return;
    }

    // ---------------- 其余一律转交渲染层 ----------------
    IEditorEngine *pEngine = currentEngine();
    if (pEngine != nullptr && pEngine->execCommand(sId)) {
        return;
    }

    QAction *pAction = m_pMenu->actionById(sId);
    showPendingMessage((pAction != nullptr) ? pAction->text() : sId);
}

void MainWindow::onThemeSelected(const QString &sThemeId)
{
    m_pStore->setValue(QStringLiteral("theme"), sThemeId);
    m_pStore->setValue(QStringLiteral("followSystemTheme"), false);
    syncThemeMenu();
    applyShellTheme();
    statusBar()->showMessage(QStringLiteral("主题已切换为：%1").arg(sThemeId), 3000);
}

void MainWindow::onFollowSystemThemeToggled(bool bFollow)
{
    m_pStore->setValue(QStringLiteral("followSystemTheme"), bFollow);
    syncThemeMenu();
    applyShellTheme();
}

void MainWindow::onPreferencesThemeChanged(const QString &sThemeId, bool bFollowSystemTheme)
{
    m_pMenu->applyThemeState(sThemeId, bFollowSystemTheme);
    applyShellTheme();
}

void MainWindow::onPreferencesLanguageChanged(const QString &sLanguage)
{
    // 外壳文案来自 MarkText 的 zh-CN 语言包，切换语言的真正效果在渲染层；
    // 这里只落盘 + 提示，避免出现"菜单还写着中文但声称已切英文"的错觉。
    statusBar()->showMessage(
        QStringLiteral("语言偏好已记录为 %1；界面文案切换待渲染层接管").arg(sLanguage), 5000);
}

//=====================================================================
//  界面同步
//=====================================================================
void MainWindow::refreshStatus()
{
    IEditorEngine *pEngine = currentEngine();

    if (m_pStatusPath != nullptr) {
        m_pStatusPath->setText((pEngine != nullptr) ? pEngine->filePath() : QString());
    }

    if (m_pStatusCounter != nullptr) {
        const QString sText = (pEngine != nullptr) ? pEngine->markdownText() : QString();
        m_pStatusCounter->setText(counterText(sText));
    }

    if (m_pStatusEngine != nullptr) {
        m_pStatusEngine->setText(
            QStringLiteral("引擎：%1").arg(
                EditorHost::engineTitle(EditorHost::requestedEngine())));
    }
}

void MainWindow::refreshTitle()
{
    IEditorEngine *pEngine = currentEngine();
    const QString sName = (pEngine != nullptr) ? pEngine->displayName()
                                               : QStringLiteral("未命名.md");
    const bool bDirty = (pEngine != nullptr) && pEngine->isModified();
    setWindowTitle(QStringLiteral("%1%2 - MarkText")
                       .arg(sName, bDirty ? QStringLiteral("*") : QString()));
}

void MainWindow::syncAutoSaveMenu()
{
    m_pMenu->setCheckedById(QStringLiteral("autoSaveMenuItem"),
                            m_pStore->boolValue(QStringLiteral("autoSave")));
}

void MainWindow::syncViewMenu()
{
    m_pMenu->setCheckedById(QStringLiteral("sideBarMenuItem"),
                            m_pStore->boolValue(QStringLiteral("sideBarVisibility")));
    m_pMenu->setCheckedById(QStringLiteral("tabBarMenuItem"),
                            m_pStore->boolValue(QStringLiteral("tabBarVisibility")));
}

void MainWindow::syncThemeMenu()
{
    m_pMenu->applyThemeState(m_pStore->stringValue(QStringLiteral("theme")),
                             m_pStore->boolValue(QStringLiteral("followSystemTheme")));
}

void MainWindow::applyShellTheme()
{
    // 只做"外壳自己那层"的明暗切换，真正的配色由渲染层加载主题 CSS 完成。
    // 跟随系统 => 用 Qt 的默认调色板。
    const bool bFollow = m_pStore->boolValue(QStringLiteral("followSystemTheme"));
    const QString sTheme = m_pStore->stringValue(QStringLiteral("theme"));

    if (bFollow || !isDarkThemeId(sTheme)) {
        qApp->setPalette(qApp->style()->standardPalette());
        // 浅色主题（或跟随系统）下编辑器背景为白色
        applyEngineHostBackground(lightEditorBackground());
        // 菜单栏还原系统默认观感（清掉深色 QSS）
        applyMenuBarStyle(false);
        return;
    }

    // 深色主题：按主题精确配色（色值对照渲染层主题 CSS，见 ShellThemeColors 注释）
    const ShellThemeColors &colors = shellColorsForTheme(sTheme);
    QPalette pal;
    pal.setColor(QPalette::Window, colors.cWindow);
    pal.setColor(QPalette::WindowText, colors.cText);
    pal.setColor(QPalette::Base, colors.cBase);
    pal.setColor(QPalette::AlternateBase, colors.cWindow);
    pal.setColor(QPalette::Text, colors.cText);
    pal.setColor(QPalette::Button, colors.cWindow);
    pal.setColor(QPalette::ButtonText, colors.cText);
    pal.setColor(QPalette::ToolTipBase, colors.cBase);
    pal.setColor(QPalette::ToolTipText, colors.cText);
    pal.setColor(QPalette::Highlight, colors.cHighlight);
    pal.setColor(QPalette::HighlightedText, colors.cHighlightText);
    pal.setColor(QPalette::Disabled, QPalette::Text, colors.cDisabledText);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, colors.cDisabledText);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, colors.cDisabledText);
    qApp->setPalette(pal);

    // 同一主题的编辑器背景同步给 WebView2 宿主（加载前底色 / 缝隙色）
    applyEngineHostBackground(colors.cEditorBg);

    // 菜单栏常驻后必须自带配色：Windows 原生样式下 QPalette 管不到
    // QMenuBar::item，只能用 QSS 精确上色（黑底白字）。
    applyMenuBarStyle(true);
}

/**
 * @brief 给菜单栏与它的弹出菜单上色
 *
 * 为什么用 QSS 而不是 QPalette：
 *   Windows（windowsvista 样式）下 QMenuBar 的条目背景由样式引擎自己画，
 *   QPalette::Window/WindowText 对它基本无效；只有 QSS 能稳定改掉。
 *
 * QSS 只挂在 menuBar() 上：MarkTextMenu 里所有 QMenu 都是它的（孙）子对象，
 * Qt 的样式表会沿对象树级联，故弹出菜单、子菜单一并生效，不必动全局样式表。
 */
void MainWindow::applyMenuBarStyle(bool bDark)
{
    QMenuBar *pBar = menuBar();
    if (pBar == nullptr) {
        return;
    }

    if (!bDark) {
        pBar->setStyleSheet(QString());
        return;
    }

    const ShellThemeColors &colors = shellColorsForTheme(
        m_pStore->stringValue(QStringLiteral("theme")));

    const QString sBg     = colors.cWindow.name();            // 菜单底色（黑）
    const QString sText   = colors.cText.name();              // 菜单文字（近白）
    const QString sSel    = colors.cHighlight.name();         // 选中高亮
    const QString sSelTxt = colors.cHighlightText.name();   // 高亮上的文字
    const QString sBorder = colors.cBase.lighter(160).name(); // 弹出菜单描边
    const QString sDis    = colors.cDisabledText.name();      // 禁用项文字

    pBar->setStyleSheet(QStringLiteral(
        "QMenuBar{background:%1;color:%2;border:none;padding:0px;}"
        "QMenuBar::item{background:transparent;color:%2;padding:5px 12px;}"
        "QMenuBar::item:selected{background:%3;color:%4;}"
        "QMenuBar::item:pressed{background:%3;color:%4;}"
        "QMenu{background:%1;color:%2;border:1px solid %5;padding:4px 0px;}"
        "QMenu::item{background:transparent;color:%2;padding:5px 30px 5px 28px;}"
        "QMenu::item:selected{background:%3;color:%4;}"
        "QMenu::item:disabled{color:%6;}"
        "QMenu::separator{height:1px;background:%5;margin:4px 10px;}"
        "QMenu::right-arrow{width:8px;height:8px;}")
        .arg(sBg, sText, sSel, sSelTxt, sBorder, sDis));
}

void MainWindow::applyEngineHostBackground(const QColor &color)
{
    const QList<IEditorEngine *> listEngines = findChildren<IEditorEngine *>();
    for (IEditorEngine *pEngine : listEngines) {
        if (WebView2EditorEngine *pWv2 = qobject_cast<WebView2EditorEngine *>(pEngine)) {
            pWv2->setViewBackground(color);
        }
    }
}

void MainWindow::showPendingMessage(const QString &sWhat)
{
    statusBar()->showMessage(
        QStringLiteral("「%1」由渲染层接管，当前引擎尚未实现").arg(sWhat), 4000);
}

QString MainWindow::counterText(const QString &sMarkdown)
{
    // 对齐 MarkText 状态栏的三项计数（menu.counter.words / characters / paragraphs）
    if (sMarkdown.isEmpty()) {
        return QStringLiteral("字数 0　字符数 0　段落数 0");
    }

    int nCharacters = 0;
    int nWords = 0;
    int nParagraphs = 0;
    bool bInLatinWord = false;
    bool bParagraphHasContent = false;

    for (int i = 0; i < sMarkdown.length(); ++i) {
        const QChar ch = sMarkdown.at(i);
        if (ch == QLatin1Char('\n')) {
            if (bParagraphHasContent) {
                ++nParagraphs;
            }
            bParagraphHasContent = false;
            bInLatinWord = false;
            continue;
        }
        ++nCharacters;

        const ushort u = ch.unicode();
        const bool bCjk = (u >= 0x3040 && u <= 0x30FF)     // 假名
                          || (u >= 0x3400 && u <= 0x4DBF)   // 扩展 A
                          || (u >= 0x4E00 && u <= 0x9FFF)   // 汉字
                          || (u >= 0xF900 && u <= 0xFAFF);  // 兼容汉字
        if (bCjk) {
            ++nWords;
            bParagraphHasContent = true;
            bInLatinWord = false;
        } else if (ch.isLetterOrNumber()) {
            if (!bInLatinWord) {
                ++nWords;
                bInLatinWord = true;
            }
            bParagraphHasContent = true;
        } else {
            bInLatinWord = false;
            if (!ch.isSpace()) {
                bParagraphHasContent = true;
            }
        }
    }
    if (bParagraphHasContent) {
        ++nParagraphs;
    }

    return QStringLiteral("字数 %1　字符数 %2　段落数 %3")
        .arg(nWords)
        .arg(nCharacters)
        .arg(nParagraphs);
}
