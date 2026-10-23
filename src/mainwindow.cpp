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
#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPalette>
#include <QPdfWriter>
#include <QPixmap>
#include <QPointer>
#include <QResizeEvent>
#include <QScreen>
#include <QSettings>
#include <QShowEvent>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QWindowStateChangeEvent>

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

//=====================================================================
//  菜单栏右上角的窗口按钮（最小化 / 最大化·还原 / 关闭）
// ---------------------------------------------------------------------
//  本窗口用 WM_NCCALCSIZE 去掉了系统标题栏（对齐 MarkText titleBarStyle=custom），
//  系统那三颗按钮随之消失，于是"关窗口只能从菜单点"。这里用 QMenuBar 的
//  corner widget 把三颗按钮钉在菜单栏最右端，图标自绘，以便跟随深/浅两套外壳配色。
//
//  图标画成矢量（QPainter 现画，按 DPR 放大）：不依赖任何字体里的符号字形，
//  高分屏也不会糊 —— 用 "─ □ ✕" 这类字符的话，不同 Windows 字体下大小/粗细不一致。
//=====================================================================
class WindowControlBar : public QWidget
{
public:
    enum IconKind { IconMinimize = 0, IconMaximize, IconRestore, IconClose };

    explicit WindowControlBar(QWidget *pParent = nullptr)
        : QWidget(pParent)
        , m_pMin(nullptr)
        , m_pMax(nullptr)
        , m_pClose(nullptr)
        , m_bMaximized(false)
        , m_bDark(false)
        , m_bCloseHover(false)
    {
        m_pMin   = createButton(QStringLiteral("最小化"));
        m_pMax   = createButton(QStringLiteral("最大化 / 还原"));
        m_pClose = createButton(QStringLiteral("关闭"));
        m_pClose->setObjectName(QStringLiteral("qtmtCloseButton"));

        // 关闭键对齐 Windows 习惯：悬停红底、图标转白（底色交给样式表，色由 refreshIcons 给）
        m_pClose->installEventFilter(this);

        // 没有布局三颗按钮会全部叠在 (0,0)（实测只有一颗可见、点不到其余两颗）
        QHBoxLayout *pLayout = new QHBoxLayout(this);
        pLayout->setContentsMargins(0, 0, 0, 0);
        pLayout->setSpacing(0);
        pLayout->addWidget(m_pMin);
        pLayout->addWidget(m_pMax);
        pLayout->addWidget(m_pClose);

        setStyleSheet(QStringLiteral(
            "QToolButton{border:none;background:transparent;padding:0px;}"
            "QToolButton:hover{background:rgba(127,127,127,90);}"
            "QToolButton:pressed{background:rgba(127,127,127,150);}"
            "QToolButton#qtmtCloseButton:hover{background:#e81123;}"
            "QToolButton#qtmtCloseButton:pressed{background:#f1707a;}"));

        refreshIcons();
    }

    QToolButton *minimizeButton() const { return m_pMin; }
    QToolButton *maximizeButton() const { return m_pMax; }
    QToolButton *closeButton()    const { return m_pClose; }

    /// 交给菜单栏之后调用：跟着菜单栏高度走（字体/DPI 变化时菜单栏会变高）
    void attachTo(QWidget *pInMenuBar)
    {
        if (pInMenuBar == nullptr) {
            return;
        }
        pInMenuBar->installEventFilter(this);
        syncHeight(pInMenuBar->height());
    }

    /// 最大化 ↔ 还原：切换"最大化/还原"那颗按钮的图标
    void setMaximized(bool bMaximized)
    {
        if (m_bMaximized == bMaximized) {
            return;
        }
        m_bMaximized = bMaximized;
        refreshIcons();
    }

    /// 跟随外壳主题（深色主题给近白图标，浅色给深灰图标）
    void setDarkTheme(bool bDark)
    {
        if (m_bDark == bDark) {
            return;
        }
        m_bDark = bDark;
        refreshIcons();
    }

    QSize sizeHint() const override
    {
        return QSize(kButtonWidth * 3, kDefaultHeight);
    }

protected:
    bool eventFilter(QObject *pWatched, QEvent *pEvent) override
    {
        if (pWatched == m_pClose) {
            if (pEvent->type() == QEvent::Enter) {
                m_bCloseHover = true;
                refreshIcons();
            } else if (pEvent->type() == QEvent::Leave) {
                m_bCloseHover = false;
                refreshIcons();
            }
        } else if (pEvent->type() == QEvent::Resize) {
            QWidget *pWidget = qobject_cast<QWidget *>(pWatched);
            if (pWidget != nullptr) {
                syncHeight(pWidget->height());
            }
        }
        return QWidget::eventFilter(pWatched, pEvent);
    }

private:
    static const int kButtonWidth   = 45;
    static const int kDefaultHeight = 28;

    QToolButton *createButton(const QString &sTip)
    {
        QToolButton *pButton = new QToolButton(this);
        pButton->setToolTip(sTip);
        pButton->setIconSize(QSize(10, 10));
        pButton->setFocusPolicy(Qt::NoFocus);
        pButton->setFixedWidth(kButtonWidth);
        return pButton;
    }

    void syncHeight(int nHeight)
    {
        // 值相等就不再 setFixedHeight：否则在菜单栏的 Resize 里反复改高度会来回触发布局
        if (nHeight <= 0 || height() == nHeight) {
            return;
        }
        setFixedHeight(nHeight);
    }

    void refreshIcons()
    {
        const QColor cIcon = m_bDark ? QColor(0xdc, 0xdc, 0xdc) : QColor(0x33, 0x33, 0x33);
        const qreal dDpr = (devicePixelRatioF() > 0.0) ? devicePixelRatioF() : 1.0;

        m_pMin->setIcon(makeIcon(IconMinimize, cIcon, dDpr));
        m_pMax->setIcon(makeIcon(m_bMaximized ? IconRestore : IconMaximize, cIcon, dDpr));
        m_pClose->setIcon(makeIcon(IconClose,
                                   m_bCloseHover ? QColor(0xff, 0xff, 0xff) : cIcon, dDpr));
    }

    /// 画一枚 10x10 的窗口按钮图标（乘 DPR 保证高分屏锐利）
    static QIcon makeIcon(int nKind, const QColor &cColor, qreal dDpr)
    {
        const int nSize = 10;
        QPixmap pixmap(QSize(nSize, nSize) * dDpr);
        pixmap.setDevicePixelRatio(dDpr);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(cColor);
        pen.setWidthF(1.0);
        pen.setCapStyle(Qt::FlatCap);
        painter.setPen(pen);

        // 1px 描边要落在半像素上才不糊（逻辑画布 10x10）
        switch (nKind) {
        case IconMinimize:
            painter.drawLine(QPointF(0.5, 5.5), QPointF(9.5, 5.5));
            break;
        case IconMaximize:
            painter.drawRect(QRectF(0.5, 0.5, 9.0, 9.0));
            break;
        case IconRestore:
            // 后框只留"顶边 + 右边"，前框完整 —— 即 Windows 的还原图标
            painter.drawLine(QPointF(3.5, 0.5), QPointF(9.5, 0.5));
            painter.drawLine(QPointF(9.5, 0.5), QPointF(9.5, 6.5));
            painter.drawRect(QRectF(0.5, 3.5, 6.0, 6.0));
            break;
        case IconClose:
        default:
            painter.drawLine(QPointF(0.5, 0.5), QPointF(9.5, 9.5));
            painter.drawLine(QPointF(9.5, 0.5), QPointF(0.5, 9.5));
            break;
        }
        painter.end();
        return QIcon(pixmap);
    }

    QToolButton *m_pMin;
    QToolButton *m_pMax;
    QToolButton *m_pClose;
    bool         m_bMaximized;
    bool         m_bDark;
    bool         m_bCloseHover;
};

/// 某颗窗口按钮在屏幕上的中心点文本（诊断用；外部自动化脚本据此对齐点击坐标）
QString buttonCenterText(QWidget *pButton)
{
    if (pButton == nullptr || !pButton->isVisible()) {
        return QStringLiteral("(不可见)");
    }
    const QPoint pt = pButton->mapToGlobal(pButton->rect().center());
    return QStringLiteral("(%1,%2) %3x%4")
            .arg(pt.x()).arg(pt.y())
            .arg(pButton->width()).arg(pButton->height());
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
    , m_pWindowButtons(nullptr)
    , m_pButtonsDiagTimer(nullptr)
    , m_sLastDir(QDir::homePath())
    , m_bLastFolderRestored(false)
    , m_bGeometryRestored(false)
    , m_bNormalGeometryValid(false)
    , m_nLastCommandMs(0)
    , m_bDraggingMenuBar(false)
{
    setWindowTitle(QStringLiteral("QtMarkText"));
    resize(1200, 800);
    // 注：上次的"正常大小"不在这里还原 —— Qt 的 show() 之后还会再 SetWindowPos
    // 一次，会把这里设的值冲掉（实测尺寸被冲回 1200x800+边框 16x39）。
    // 真正落位在 showEvent() 里做，见 restoreWindowGeometry()。

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
    // 窗口"正常大小"落盘（最大化/全屏时取 normalGeometry）：下次启动按它还原
    saveWindowGeometry();

    QMainWindow::closeEvent(pEvent);
}

void MainWindow::showEvent(QShowEvent *pEvent)
{
    QMainWindow::showEvent(pEvent);

    // ---- 首次显示：按上次的"正常大小"落位 ----
    // 时机很关键 —— Qt 在 show() 内部会按它自己记的 geometry 再 SetWindowPos
    // 一次（并且会按"有边框"的语义给尺寸加上 16x39），所以放在构造函数里做
    // 一定会被冲掉。这里在基类 showEvent 之后执行，才是最终生效的那一次。
    if (!m_bGeometryRestored) {
        m_bGeometryRestored = true;
        restoreWindowGeometry();
    }
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

bool MainWindow::isMenuBarCaptionArea(const QPoint &ptGlobal) const
{
    QMenuBar *pBar = menuBar();
    if (pBar == nullptr || !pBar->isVisible()) {
        return false;
    }

    const QPoint ptInBar = pBar->mapFromGlobal(ptGlobal);
    if (!pBar->rect().contains(ptInBar)) {
        return false;
    }

    // 命中某个菜单项（文件/编辑/…）：是普通客户区，交给 Qt
    if (pBar->actionAt(ptInBar) != nullptr) {
        return false;
    }

    // 右上角窗口按钮：也是普通客户区
    if (m_pWindowButtons != nullptr) {
        const QPoint ptInButtons = m_pWindowButtons->mapFromGlobal(ptGlobal);
        if (m_pWindowButtons->rect().contains(ptInButtons)) {
            return false;
        }
    }
    return true;
}

bool MainWindow::eventFilter(QObject *pWatched, QEvent *pEvent)
{
    // ---- 菜单栏空白处 = 自绘标题栏：拖动窗口 + 双击最大化/还原 ----
    // 试过"mousePress 里 ReleaseCapture + SendMessage(WM_NCLBUTTONDOWN,HTCAPTION)"
    // 这条原生拖动路线：事件确实到了（日志能打印），但本机窗口由 WM_NCCALCSIZE
    // 抹平了非客户区，系统的移动循环拿不到有效的拖动上下文，窗口一动不动。
    // 改为纯 Qt 自己拖：按下记起点，移动时按全局位移 move() —— 行为确定、可验证。
    if (pWatched == menuBar()) {
        switch (pEvent->type()) {
        case QEvent::MouseButtonPress: {
            QMouseEvent *pMe = static_cast<QMouseEvent *>(pEvent);
            if (pMe != nullptr && pMe->button() == Qt::LeftButton
                    && isMenuBarCaptionArea(pMe->globalPos())) {
                m_bDraggingMenuBar = true;
                m_ptDragOrigin = pMe->globalPos();
                m_ptDragWindowOrigin = frameGeometry().topLeft();
                return true;   // 吃掉：别让 QMenuBar 把它当普通点击
            }
            break;
        }
        case QEvent::MouseMove: {
            QMouseEvent *pMe = static_cast<QMouseEvent *>(pEvent);
            if (m_bDraggingMenuBar && pMe != nullptr && !isMaximized() && !isFullScreen()) {
                // 位移用全局坐标算：窗口自己在动，用窗口内坐标会自激振荡
                move(m_ptDragWindowOrigin + (pMe->globalPos() - m_ptDragOrigin));
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            if (m_bDraggingMenuBar) {
                m_bDraggingMenuBar = false;
                return true;
            }
            break;
        }
        case QEvent::MouseButtonDblClick: {
            QMouseEvent *pMe = static_cast<QMouseEvent *>(pEvent);
            if (pMe != nullptr && pMe->button() == Qt::LeftButton
                    && isMenuBarCaptionArea(pMe->globalPos())) {
                toggleMaximizeRestore();
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(pWatched, pEvent);
}

void MainWindow::changeEvent(QEvent *pEvent)
{
    // 最大化 ↔ 还原时，右上角那颗按钮要在"最大化"与"还原"两个图标间切换
    if (pEvent != nullptr && pEvent->type() == QEvent::WindowStateChange) {
        updateWindowButtons();
    }
    QMainWindow::changeEvent(pEvent);
}

//=====================================================================
//  自绘顶栏：窗口按钮状态 / 窗口几何持久化
//=====================================================================
void MainWindow::toggleMaximizeRestore()
{
    // 无系统标题栏 + WM_NCCALCSIZE 抹平非客户区之后，"还原位置"不能信系统/Qt 的：
    // 实测点"还原"会回到 (−8,−8) 2576x1408（即最大化矩形本身），窗口回不到正常大小。
    // 所以正常态下自己记一份，还原时显式用回去。
    if (isMaximized() || isFullScreen()) {
        showNormal();
        if (m_bNormalGeometryValid && m_rcNormalGeometry.isValid()) {
            setGeometry(m_rcNormalGeometry);
            qInfo("[窗口] 还原到自记的正常大小：(%d,%d) %dx%d",
                  m_rcNormalGeometry.x(), m_rcNormalGeometry.y(),
                  m_rcNormalGeometry.width(), m_rcNormalGeometry.height());
        } else {
            qWarning("[窗口] 还原：没有可用的正常大小记录，只能交给系统");
        }
    } else {
        rememberNormalGeometry();
        qInfo("[窗口] 最大化：已记住正常大小 (%d,%d) %dx%d（有效=%d）",
              m_rcNormalGeometry.x(), m_rcNormalGeometry.y(),
              m_rcNormalGeometry.width(), m_rcNormalGeometry.height(),
              m_bNormalGeometryValid ? 1 : 0);
        showMaximized();
    }
    updateWindowButtons();
}

void MainWindow::rememberNormalGeometry()
{
    // 只有正常态才有意义：最大化/最小化/全屏时 geometry() 给的是那个状态的矩形，
    // 记下来会把"正常大小"污染掉。
    if (isMaximized() || isMinimized() || isFullScreen() || !isVisible()) {
        return;
    }
    const QRect rc = geometry();
    if (!rc.isValid() || rc.width() < 320 || rc.height() < 240) {
        return;
    }
    m_rcNormalGeometry = rc;
    m_bNormalGeometryValid = true;
}

void MainWindow::resizeEvent(QResizeEvent *pEvent)
{
    QMainWindow::resizeEvent(pEvent);
    rememberNormalGeometry();
    scheduleButtonDiag();   // 窗口尺寸变了 → 角落按钮位置也变，重打稳态坐标
}

void MainWindow::moveEvent(QMoveEvent *pEvent)
{
    QMainWindow::moveEvent(pEvent);
    rememberNormalGeometry();
    scheduleButtonDiag();   // 窗口挪了 → 按钮的屏幕坐标跟着变
}

void MainWindow::updateWindowButtons()
{
    // 头文件里只留 QWidget*（WindowControlBar 是本文件的私有实现），这里转回去
    WindowControlBar *pButtons = static_cast<WindowControlBar *>(m_pWindowButtons);
    if (pButtons == nullptr) {
        return;
    }
    pButtons->setMaximized(isMaximized() || isFullScreen());
    pButtons->setDarkTheme(isDarkThemeId(effectiveThemeId()));
    scheduleButtonDiag();
}

void MainWindow::scheduleButtonDiag()
{
    // 诊断日志延后一拍再打。窗口状态切换（尤其最大化/还原）与拖动/缩放之后，
    // 菜单栏与角落按钮的布局要等下一个事件循环拍子才稳定 —— 立刻读 mapToGlobal
    // 拿到的是过渡期的旧坐标（实测据此点"还原"会点到隔壁的"最小化"）。
    // 外部自动化脚本按这条日志定位按钮，所以必须给"稳定后"的值。
    if (m_pButtonsDiagTimer == nullptr) {
        m_pButtonsDiagTimer = new QTimer(this);
        m_pButtonsDiagTimer->setSingleShot(true);
        connect(m_pButtonsDiagTimer, &QTimer::timeout,
                this, &MainWindow::logWindowButtonCenters);
    }
    m_pButtonsDiagTimer->start(400);
}

void MainWindow::logWindowButtonCenters()
{
    WindowControlBar *pButtons = static_cast<WindowControlBar *>(m_pWindowButtons);
    if (pButtons == nullptr) {
        return;
    }
    const QRect rcFrame = frameGeometry();
    qInfo("[窗口按钮][诊断] isMaximized=%d 窗口矩形=(%d,%d) %dx%d | 最小化=%s 最大化=%s 关闭=%s",
          isMaximized() ? 1 : 0,
          rcFrame.x(), rcFrame.y(), rcFrame.width(), rcFrame.height(),
          qPrintable(buttonCenterText(pButtons->minimizeButton())),
          qPrintable(buttonCenterText(pButtons->maximizeButton())),
          qPrintable(buttonCenterText(pButtons->closeButton())));
}

void MainWindow::saveWindowGeometry()
{
    // 统一用 Win32 的窗口矩形语义存取：存的是"窗口矩形"，还原也用同一语义。
    // 关于 Qt 的 geometry()：构造函数里 resize(1200,800) 会被 Qt 按"有边框"加上
    // 16x39（本窗口 style 仍带 WS_CAPTION|WS_THICKFRAME 以保留缩放/Aero Snap，
    // 而 nativeEvent(WM_NCCALCSIZE) 又把客户区拉满了，两边语义不一致）；
    // 但在 show() 之后调 setGeometry() 则不换算 —— 实测传什么落什么。
    // 所以存取一律走 Win32 全窗口矩形，避开这层不一致。
    QRect rect;
#ifdef Q_OS_WIN
    // 优先用自己记的"正常大小"：最大化状态下 GetWindowPlacement 的 rcNormalPosition
    // 在本窗口下会被污染（实测还原成最大化矩形 2576x1408），不可信。
    if (m_bNormalGeometryValid && m_rcNormalGeometry.isValid()) {
        rect = m_rcNormalGeometry;
    } else {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        WINDOWPLACEMENT wp;
        memset(&wp, 0, sizeof(wp));
        wp.length = sizeof(wp);
        if (::GetWindowPlacement(hwnd, &wp) != FALSE) {
            const RECT &rc = wp.rcNormalPosition;
            rect = QRect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
        }
    }
#else
    rect = geometry();
#endif
    if (!rect.isValid() || rect.width() <= 0 || rect.height() <= 0) {
        return;
    }

    QSettings settings(PreferencesStore::iniFilePath(), QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    settings.setValue(QStringLiteral("ui/windowGeometry"), rect);
    settings.sync();
    qInfo("[窗口] 正常大小已记录：(%d,%d) %dx%d",
          rect.x(), rect.y(), rect.width(), rect.height());
}

void MainWindow::restoreWindowGeometry()
{
    QSettings settings(PreferencesStore::iniFilePath(), QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    const QRect rect = settings.value(QStringLiteral("ui/windowGeometry")).toRect();
    if (!rect.isValid() || rect.width() < 320 || rect.height() < 240) {
        return;   // 首次运行 / 配置损坏：沿用构造函数里的缺省 1200x800
    }

    // 位置只在"目标点仍落在某块屏幕上"时恢复：拔掉显示器或改过分辨率后
    // 照搬旧坐标，窗口会跑到看不见的地方去。
    if (QGuiApplication::screenAt(rect.center()) == nullptr) {
        qInfo("[窗口] 上次的位置已不在当前屏幕上，跳过还原");
        return;
    }

#ifdef Q_OS_WIN
    // ---- 为什么在 showEvent 里用 Qt 的 setGeometry，而不是 Win32 的 SetWindowPos ----
    // 本窗口的 style 仍带 WS_CAPTION|WS_THICKFRAME（为保留系统边缘缩放 / Aero Snap），
    // 而 nativeEvent(WM_NCCALCSIZE) 又把客户区拉满（客户区 == 窗口矩形），两套语义
    // 不一致，实测踩过两次坑：
    //   1) 构造函数里直接 SetWindowPos(1000x700)：当时生效（日志确证 Win32 窗口
    //      已是 1000x700），但 Qt 随后的 show() 又用它自己账本里的 geometry 覆盖成
    //      1216x839（= 缺省 1200x800 + Qt 假定的边框 16x39），界面尺寸回不来。
    //   2) 构造函数里 resize()+move()：落地的窗口矩形比请求的大 16x39。
    // 而在 showEvent（即 show() 之后）里调 setGeometry，实测 Qt 对这个窗口不做
    // 任何边框换算 —— 传进去的矩形与 GetWindowRect 的结果分毫不差（见下方诊断）。
    // 所以这里直接给目标矩形即可，不要自己再补偿，反向补偿反而会把尺寸带偏。
    setGeometry(rect);
    qInfo("[窗口] 已按上次的正常大小还原：(%d,%d) %dx%d",
          rect.x(), rect.y(), rect.width(), rect.height());
#else
    resize(rect.size());
    move(rect.topLeft());
    qInfo("[窗口] 已按上次的正常大小还原：(%d,%d) %dx%d",
          rect.x(), rect.y(), rect.width(), rect.height());
#endif

#ifdef Q_OS_WIN
    // 诊断：确认落地的 Win32 窗口矩形确实等于目标（存什么就还原成什么）
    RECT rcWin = { 0, 0, 0, 0 };
    ::GetWindowRect(reinterpret_cast<HWND>(winId()), &rcWin);
    const QRect rcQt = geometry();
    qInfo("[窗口][诊断] Win32窗口=(%ld,%ld) %ldx%ld | Qt geometry=(%d,%d) %dx%d | 目标 (%d,%d) %dx%d",
          rcWin.left, rcWin.top, rcWin.right - rcWin.left, rcWin.bottom - rcWin.top,
          rcQt.x(), rcQt.y(), rcQt.width(), rcQt.height(),
          rect.x(), rect.y(), rect.width(), rect.height());
#endif

    // 把还原出来的尺寸认作"正常大小" —— 之后点最大化再还原，回到的就是它
    m_rcNormalGeometry = rect;
    m_bNormalGeometryValid = true;
}

//=====================================================================
//  侧边栏与目录
//=====================================================================
QString MainWindow::lastOpenedFolder() const
{
    QSettings settings(PreferencesStore::iniFilePath(), QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    return settings.value(QStringLiteral("ui/lastOpenedFolder")).toString();
}

void MainWindow::setLastOpenedFolder(const QString &sDir)
{
    QSettings settings(PreferencesStore::iniFilePath(), QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    settings.setValue(QStringLiteral("ui/lastOpenedFolder"), sDir);
    settings.sync();
}

void MainWindow::openFolderInShell(const QString &sDir)
{
    if (sDir.isEmpty()) {
        return;
    }
    m_sLastDir = sDir;
    // 记住它 —— 下次启动自动打开（用户要求的"默认打开视图的目录"）
    setLastOpenedFolder(QDir::toNativeSeparators(sDir));

    IEditorEngine *pEngine = currentEngine();
    MarkTextBridge *pBridge = (pEngine != nullptr) ? pEngine->bridge() : nullptr;
    if (pBridge != nullptr) {
        // 对照 Electron 主进程：win.webContents.send('mt::open-directory', pathname)
        // 渲染层收到后 OPEN_PROJECT(pathname)：建项目树 + 展开侧边栏
        //（rightColumn='files'、showSideBar=true），布局全归它管。
        pBridge->emitToRenderer(QStringLiteral("mt::open-directory"),
                                QJsonArray{ QDir::toNativeSeparators(sDir) });
    } else {
        m_pFileTree->setRootIndex(m_pFileModel->index(sDir));
    }
    statusBar()->showMessage(QStringLiteral("已打开文件夹：%1").arg(sDir), 5000);
    qInfo("[侧边栏] 已打开文件夹：%s", qPrintable(sDir));
}

void MainWindow::restoreLastOpenedFolder()
{
    if (m_bLastFolderRestored) {
        return;
    }
    m_bLastFolderRestored = true;

    const QString sDir = lastOpenedFolder();
    if (sDir.isEmpty()) {
        return;
    }
    const QFileInfo fi(sDir);
    if (!fi.exists() || !fi.isDir()) {
        // 目录没了（U 盘拔了 / 网络盘没挂 / 被删）：清掉记录，
        // 免得每次启动都白试一遍，也别让侧栏停在空目录上。
        qInfo("[侧边栏] 上次的文件夹已不存在，清除记录：%s", qPrintable(sDir));
        setLastOpenedFolder(QString());
        return;
    }

    IEditorEngine *pEngine = currentEngine();
    MarkTextBridge *pBridge = (pEngine != nullptr) ? pEngine->bridge() : nullptr;
    if (pBridge == nullptr) {
        return;   // 占位引擎：没有渲染层项目树可开
    }

    qInfo("[侧边栏] 自动打开上次的文件夹：%s", qPrintable(sDir));
    pBridge->emitToRenderer(QStringLiteral("mt::open-directory"),
                            QJsonArray{ QDir::toNativeSeparators(sDir) });
    m_sLastDir = sDir;
}

QString MainWindow::dialogStartDir() const
{
    // ---- 为什么必须转反斜杠：Windows 原生文件对话框的起始目录只认 '\' ----
    // 实测（tmp_qtmin/shelltest/test_shell.exe，直调 shell 的
    // SHCreateItemFromParsingName）：正斜杠路径一律返回 E_INVALIDARG(0x80070057)，
    // 只有反斜杠能解析。解析失败时对话框弹「路径不存在」，并把可解析前缀
    // 当作当前目录、剩余残段填进"文件名"框 —— 正是 20261014 用户截图的现象。
    //
    // 而 m_sLastDir 的两个来源都产出 Qt 风格正斜杠：
    //   1) openFile 里 QFileInfo::absolutePath()（恒为正斜杠）；
    //   2) ini 读回的 lastOpenedFolder（历史上写入过正斜杠形式）。
    // 所以传给任何 QFileDialog 之前必须统一转成原生分隔符。
    QDir dir(m_sLastDir);
    if (!dir.exists()) {
        // 记录的目录已不在（被删/盘符变化）：退回用户主目录，别让对话框报错
        qInfo("[对话框] 起始目录不存在，退回主目录：%s", qPrintable(m_sLastDir));
        return QDir::toNativeSeparators(QDir::homePath());
    }
    return QDir::toNativeSeparators(dir.absolutePath());
}

void MainWindow::onWindowAction(const QString &sAction)
{
    // 渲染层标题栏窗口按钮 → 外壳窗口动作（对照 Electron 的 win.send('mt::win::*')）
    if (sAction == QLatin1String("mt::win::minimize")) {
        showMinimized();
    } else if (sAction == QLatin1String("mt::win::maximize")) {
        rememberNormalGeometry();
        showMaximized();
    } else if (sAction == QLatin1String("mt::win::unmaximize")) {
        toggleMaximizeRestore();   // 已在最大化态 → 走"还原回自记正常大小"分支
    } else if (sAction == QLatin1String("mt::win::toggle-maximize")) {
        toggleMaximizeRestore();
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

    // ---- 菜单栏右上角的窗口按钮（最小化 / 最大化·还原 / 关闭）----
    // 系统标题栏已由 nativeEvent(WM_NCCALCSIZE) 去掉，这三颗按钮得自己画；
    // 用 QMenuBar 的 corner widget 钉在最右端，菜单栏多长它就跟到多长。
    WindowControlBar *pButtons = new WindowControlBar(this);
    menuBar()->setCornerWidget(pButtons, Qt::TopRightCorner);
    pButtons->attachTo(menuBar());
    m_pWindowButtons = pButtons;

    // 菜单栏空白处按下 = 拖动窗口、双击 = 最大化/还原（见 eventFilter）
    menuBar()->installEventFilter(this);

    connect(pButtons->minimizeButton(), &QToolButton::clicked, this, [this]() {
        qInfo("[窗口按钮] 最小化被点击");
        showMinimized();
    });
    connect(pButtons->maximizeButton(), &QToolButton::clicked, this, [this]() {
        qInfo("[窗口按钮] 最大化/还原被点击（当前 isMaximized=%d）", isMaximized() ? 1 : 0);
        toggleMaximizeRestore();
    });
    connect(pButtons->closeButton(), &QToolButton::clicked, this, [this]() {
        qInfo("[窗口按钮] 关闭被点击");
        close();
    });
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
        const QString sTheme = effectiveThemeId();
        QColor cEditorBg = lightEditorBackground();
        if (isDarkThemeId(sTheme)) {
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
        // 渲染层启动时会用 mt::ask-for-user-preference 拉一遍偏好，其中 theme 是
        // "选中的主题"原值；跟随系统时它不等于生效值，故就绪后再广播一次纠正。
        connect(pEngine->bridge(), &MarkTextBridge::rendererReady,
                this, &MainWindow::pushThemeToRenderer);
        // 同上，就绪后再恢复"上次打开的文件夹"（渲染层的项目树监听此时才注册好）
        connect(pEngine->bridge(), &MarkTextBridge::rendererReady,
                this, &MainWindow::restoreLastOpenedFolder);
        // 侧边栏/标签栏是渲染层自绘的，"切换"由它自己执行；它改完会回一条
        // mt::view-layout-changed，外壳据此同步「视图」菜单的勾选状态。
        connect(pEngine->bridge(), &MarkTextBridge::layoutChanged, this,
                [this](bool bShowSideBar, bool bShowTabBar) {
                    qInfo("[布局] 渲染层布局变化：侧边栏=%d 标签栏=%d",
                          bShowSideBar ? 1 : 0, bShowTabBar ? 1 : 0);
                    m_pMenu->setCheckedById(QStringLiteral("sideBarMenuItem"), bShowSideBar);
                    m_pMenu->setCheckedById(QStringLiteral("tabBarMenuItem"), bShowTabBar);
                });
        // 渲染层里点"打开文件/文件夹/设置"（欢迎页、侧栏按钮等）→ 外壳出对话框
        connect(pEngine->bridge(), &MarkTextBridge::shellActionRequested, this,
                [this](const QString &sAction) {
                    QString sMenuId;
                    if (sAction == QLatin1String("open-directory")) {
                        sMenuId = QStringLiteral("openFolderMenuItem");
                    } else if (sAction == QLatin1String("open-file")) {
                        sMenuId = QStringLiteral("openFileMenuItem");
                    } else if (sAction == QLatin1String("settings")) {
                        sMenuId = QStringLiteral("preferencesMenuItem");
                    }
                    if (sMenuId.isEmpty()) {
                        return;
                    }
                    // 此刻还在 WebView2 的 IPC 回调里，直接开模态对话框会挡住回调
                    // 返回、把渲染层卡住；推到事件循环下一拍再弹。
                    QTimer::singleShot(0, this, [this, sMenuId]() {
                        onMenuCommand(sMenuId);
                    });
                });
        // 反方向：渲染层自己改了主题（命令面板 window.change-theme-*）时，
        // 外壳的菜单栏/窗口配色要跟着换，否则会出现"编辑区变了、菜单栏没变"。
        connect(pEngine->bridge(), &MarkTextBridge::preferenceChangedByRenderer,
                this, [this](const QString &sKey, const QJsonValue &) {
                    if (sKey == QLatin1String("theme")
                            || sKey == QLatin1String("followSystemTheme")
                            || sKey == QLatin1String("lightModeTheme")
                            || sKey == QLatin1String("darkModeTheme")) {
                        syncThemeMenu();
                        applyShellTheme();
                    }
                });
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
        const QString sDefault = QDir::toNativeSeparators(
                    sPath.isEmpty() ? m_sLastDir + QStringLiteral("/未命名.md")
                                    : sPath);
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

    const QString sDefault = QDir::toNativeSeparators(m_sLastDir + QStringLiteral("/")
                             + QFileInfo(pEngine->displayName()).completeBaseName()
                             + QStringLiteral(".html"));
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

    const QString sDefault = QDir::toNativeSeparators(m_sLastDir + QStringLiteral("/")
                             + QFileInfo(pEngine->displayName()).completeBaseName()
                             + QStringLiteral(".pdf"));
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
        qInfo("[快捷键] 外壳认领：%s -> %s",
              qPrintable(seq.toString(QKeySequence::NativeText)), qPrintable(sId));
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
    // ---- 防抖：同一条命令在极短时间内被重复请求 ----
    // 菜单快捷键有两条到达路径：Qt 自己的 QAction 快捷键（焦点在 Qt 侧，例如刚
    // 激活窗口、焦点还在菜单栏上时）与 WebView2 的 AcceleratorKeyPressed（焦点在
    // 渲染层里时，见 tryShortcut）。两条会同时命中 —— 实测按一次 Ctrl+J 会
    // "切了又切回来"（渲染层收到两次 toggle，净效果为 0）。这里做 250ms 去重。
    {
        const qint64 nNow = QDateTime::currentMSecsSinceEpoch();
        if (sId == m_sLastCommandId && (nNow - m_nLastCommandMs) < 250) {
            qInfo("[菜单] 忽略重复请求：%s（距上次 %lld ms）",
                  qPrintable(sId), static_cast<long long>(nNow - m_nLastCommandMs));
            return;
        }
        m_sLastCommandId = sId;
        m_nLastCommandMs = nNow;
    }

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
            this, QStringLiteral("打开 Markdown 文件"), dialogStartDir(),
            QStringLiteral("Markdown 文件 (*.md *.markdown *.txt);;所有文件 (*.*)"));
        if (!sPath.isEmpty()) {
            openFile(sPath);
        }
        return;
    }
    if (sId == QLatin1String("openFolderMenuItem")) {
        const QString sDir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("打开文件夹"), dialogStartDir());
        if (!sDir.isEmpty()) {
            openFolderInShell(sDir);
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

    // ---------------- 视图 ----------------
    // 「切换侧边栏 / 切换标签栏」切的是渲染层自绘的布局 —— 外壳那个 fileDock 与
    // 标签栏自 2026-10-13 起就永久隐藏了（对齐 MarkText：布局由渲染层负责）。
    // 所以必须转交渲染层执行；下面只保留占位引擎（无渲染层）时的兜底分支。
    if (sId == QLatin1String("sideBarMenuItem") || sId == QLatin1String("tabBarMenuItem")) {
        IEditorEngine *pEngine = currentEngine();
        MarkTextBridge *pBridge = (pEngine != nullptr) ? pEngine->bridge() : nullptr;
        if (pBridge != nullptr && pBridge->routeMenuCommand(sId)) {
            qInfo("[侧边栏] 外壳转交渲染层切换：%s", qPrintable(sId));
            return;   // 真正的切换与菜单勾选同步都在渲染层侧闭环
        }

        QAction *pAction = m_pMenu->actionById(sId);
        const bool bVisible = (pAction != nullptr) && pAction->isChecked();
        if (sId == QLatin1String("sideBarMenuItem")) {
            m_pFileDock->setVisible(bVisible);
            m_pStore->setValue(QStringLiteral("sideBarVisibility"), bVisible);
        } else {
            m_pTabWidget->tabBar()->setVisible(bVisible);
            m_pStore->setValue(QStringLiteral("tabBarVisibility"), bVisible);
        }
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
    pushThemeToRenderer();
    statusBar()->showMessage(QStringLiteral("主题已切换为：%1").arg(sThemeId), 3000);
}

void MainWindow::onFollowSystemThemeToggled(bool bFollow)
{
    m_pStore->setValue(QStringLiteral("followSystemTheme"), bFollow);
    syncThemeMenu();
    applyShellTheme();
    pushThemeToRenderer();
}

void MainWindow::onPreferencesThemeChanged(const QString &sThemeId, bool bFollowSystemTheme)
{
    m_pMenu->applyThemeState(sThemeId, bFollowSystemTheme);
    applyShellTheme();
    pushThemeToRenderer();
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
    // 菜单勾选的是"用户选中的主题"（preference.theme），不是跟随系统解析后的生效值，
    // 故这里用原值。
    m_pMenu->applyThemeState(m_pStore->stringValue(QStringLiteral("theme")),
                             m_pStore->boolValue(QStringLiteral("followSystemTheme")));
}

QString MainWindow::effectiveThemeId() const
{
    return MarkTextThemes::effectiveThemeId(
        m_pStore->stringValue(QStringLiteral("theme")),
        m_pStore->boolValue(QStringLiteral("followSystemTheme")),
        m_pStore->stringValue(QStringLiteral("lightModeTheme")),
        m_pStore->stringValue(QStringLiteral("darkModeTheme")));
}

void MainWindow::pushThemeToRenderer()
{
    // 只发主题相关的四项：渲染层的 SET_USER_PREFERENCE 是"逐键赋值"，
    // 发全量会把渲染层自己刚改过、尚未回传的项一并覆盖掉。
    QJsonObject obj;
    obj.insert(QStringLiteral("theme"), effectiveThemeId());
    obj.insert(QStringLiteral("followSystemTheme"),
               m_pStore->boolValue(QStringLiteral("followSystemTheme")));
    obj.insert(QStringLiteral("lightModeTheme"),
               m_pStore->stringValue(QStringLiteral("lightModeTheme")));
    obj.insert(QStringLiteral("darkModeTheme"),
               m_pStore->stringValue(QStringLiteral("darkModeTheme")));

    const QList<IEditorEngine *> listEngines = findChildren<IEditorEngine *>();
    for (IEditorEngine *pEngine : listEngines) {
        if (pEngine != nullptr && pEngine->bridge() != nullptr) {
            pEngine->bridge()->emitToRenderer(QStringLiteral("mt::user-preference"),
                                              QJsonArray{ obj });
        }
    }
    qInfo("[主题] 已广播到渲染层：theme=%s", qPrintable(obj.value(QStringLiteral("theme")).toString()));
}

void MainWindow::applyShellTheme()
{
    // 只做"外壳自己那层"的明暗切换，真正的配色由渲染层加载主题 CSS 完成。
    // 开启「跟随系统主题」时先解析出生效主题（与原版 Electron 主进程的
    // nativeTheme 解析同语义），再按同一套明暗逻辑走。
    const QString sTheme = effectiveThemeId();

    if (!isDarkThemeId(sTheme)) {
        qApp->setPalette(qApp->style()->standardPalette());
        // 浅色主题下编辑器背景为白色
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
        updateWindowButtons();   // 右上角窗口按钮的图标颜色跟着主题走
        return;
    }

    // 取"生效主题"的配色（跟随系统时是解析后的那个），保证菜单栏与编辑区一致
    const ShellThemeColors &colors = shellColorsForTheme(effectiveThemeId());

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

    updateWindowButtons();   // 右上角窗口按钮的图标颜色跟着主题走
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
