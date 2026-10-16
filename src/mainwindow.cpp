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
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPdfWriter>
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
    // MarkText 的菜单栏/标签栏/侧栏/状态栏全部由渲染层自绘，外壳只提供窗口。
    // 这里把 Qt 外壳自绘的这几块全部收起（功能保留）：
    //   - 菜单栏：默认隐藏，按 Alt 呼出（与 MarkText 一致）
    //   - 标签栏：渲染层有自己的 tab 栏
    //   - 文件 dock：渲染层有内置侧栏（openFolder）
    //   - 状态栏：渲染层有状态栏
    menuBar()->setVisible(false);
    m_pTabWidget->tabBar()->setVisible(false);
    m_pFileDock->setVisible(false);
    statusBar()->setVisible(false);

    addEditorTab();
    refreshStatus();
}

MainWindow::~MainWindow()
{
}

//=====================================================================
//  外壳搭建
//=====================================================================
void MainWindow::onAltPressed()
{
    // 对齐 MarkText：按 Alt 呼出/收起菜单栏；点完命令自动收起（见 commandTriggered）
    menuBar()->setVisible(!menuBar()->isVisible());
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
    connect(m_pMenu, &MarkTextMenu::commandTriggered, this, [this](const QString &sId) {
        onMenuCommand(sId);
        // 点完命令自动收起菜单栏（对齐 MarkText 菜单行为）
        if (menuBar()->isVisible()) {
            menuBar()->hide();
        }
    });
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
void MainWindow::onMenuCommand(const QString &sId)
{
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
            m_pFileTree->setRootIndex(m_pFileModel->index(sDir));
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
