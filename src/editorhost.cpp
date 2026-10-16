#include "editorhost.h"

#include "markdowneditor.h"
#include "marktextbridge.h"
#include "preferencesstore.h"
#include "webview2host.h"

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPalette>
#include <QVBoxLayout>
#include <QWidget>

//=====================================================================
//  IEditorEngine
//=====================================================================
IEditorEngine::IEditorEngine(QObject *pParent)
    : QObject(pParent)
    , m_sFilePath()
    , m_bModified(false)
{
}

IEditorEngine::~IEditorEngine()
{
}

QString IEditorEngine::filePath() const
{
    return m_sFilePath;
}

bool IEditorEngine::isModified() const
{
    return m_bModified;
}

QString IEditorEngine::displayName() const
{
    if (m_sFilePath.isEmpty()) {
        return QStringLiteral("未命名.md");
    }
    return QFileInfo(m_sFilePath).fileName();
}

void IEditorEngine::setFilePath(const QString &sPath)
{
    if (m_sFilePath == sPath) {
        return;
    }
    m_sFilePath = sPath;
    emit titleChanged(displayName());
}

void IEditorEngine::setModified(bool bModified)
{
    if (m_bModified == bModified) {
        return;
    }
    m_bModified = bModified;
    emit modifiedChanged(bModified);
}

MarkTextBridge *IEditorEngine::bridge() const
{
    return nullptr;   // 仅 WebView2 引擎有桥接层
}

QString IEditorEngine::exportHtml() const
{
    // 默认不提供；由外壳兜底（把 Markdown 源码包进 <pre>）
    return QString();
}

bool IEditorEngine::loadFromFile(const QString &sPath)
{
    QFile file(sPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    // TODO(桥接阶段)：编码由 preference.defaultEncoding / autoGuessEncoding 决定，
    //                 MarkText 用原生模块 ced 做探测；当前占位引擎统一按 UTF-8。
    const QByteArray data = file.readAll();
    file.close();

    setMarkdownText(QString::fromUtf8(data));
    setFilePath(sPath);
    setModified(false);
    return true;
}

void IEditorEngine::openFile(const QString &sPath)
{
    // 默认实现：等价于 loadFromFile（占位引擎够用）
    loadFromFile(sPath);
}

bool IEditorEngine::saveToFile(const QString &sPath)
{
    QFile file(sPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return false;
    }
    // TODO(桥接阶段)：行尾由 preference.endOfLine（default/crlf/lf）决定
    file.write(markdownText().toUtf8());
    file.close();

    setFilePath(sPath);
    setModified(false);
    return true;
}

//=====================================================================
//  占位引擎：包装第 1/2 阶段的纯 Qt MarkdownEditor
//=====================================================================
PlaceholderEditorEngine::PlaceholderEditorEngine(QObject *pOwner, QWidget *pViewParent)
    : IEditorEngine(pOwner)
    , m_pEditor(nullptr)
{
    m_pEditor = new MarkdownEditor(pViewParent);
    m_pEditor->setPlaceholderText(
        QStringLiteral("在此输入 Markdown：# 标题、**粗体**、- 列表、`代码`"));

    // 正文变化即置脏，供外壳的自动保存与标题星号使用
    connect(m_pEditor, &QTextEdit::textChanged, this, [this]() {
        setModified(true);
    });
}

PlaceholderEditorEngine::~PlaceholderEditorEngine()
{
    // m_pEditor 的父子关系挂在调用方（标签页）上，此处只清引用
    m_pEditor = nullptr;
}

QString PlaceholderEditorEngine::engineName() const
{
    return QStringLiteral("纯 Qt 占位引擎（MarkdownEditor）");
}

QWidget *PlaceholderEditorEngine::view() const
{
    return m_pEditor;
}

void PlaceholderEditorEngine::setMarkdownText(const QString &sText)
{
    if (m_pEditor != nullptr) {
        m_pEditor->setMarkdownText(sText);
    }
}

QString PlaceholderEditorEngine::markdownText() const
{
    return (m_pEditor != nullptr) ? m_pEditor->markdownText() : QString();
}

QString PlaceholderEditorEngine::exportHtml() const
{
    // 占位引擎的富文本视图本身就能吐 HTML；
    // 但它只覆盖第 2 阶段支持的语法，正式导出应等渲染层接管。
    return (m_pEditor != nullptr) ? m_pEditor->toHtml() : QString();
}

//  占位引擎能真实响应的一部分命令：只做 Qt 编辑器本身支持的那几条，
//  其余（格式/段落/视图类）返回 false，由外壳记录"未处理"。
bool PlaceholderEditorEngine::execCommand(const QString &sCommandId)
{
    if (m_pEditor == nullptr) {
        return false;
    }

    if (sCommandId == QLatin1String("edit.undoMenuItem")) {
        m_pEditor->undo();
    } else if (sCommandId == QLatin1String("edit.redoMenuItem")) {
        m_pEditor->redo();
    } else if (sCommandId == QLatin1String("edit.cutMenuItem")) {
        m_pEditor->cut();
    } else if (sCommandId == QLatin1String("edit.copyMenuItem")) {
        m_pEditor->copy();
    } else if (sCommandId == QLatin1String("edit.pasteMenuItem")) {
        m_pEditor->paste();
    } else if (sCommandId == QLatin1String("edit.selectAllMenuItem")) {
        m_pEditor->selectAll();
    } else {
        return false;
    }
    return true;
}

void PlaceholderEditorEngine::setPreference(const QString &sKey, const QVariant &valueValue)
{
    // 占位引擎只认得字体/字号/行距这三项（其余等渲染层接管）
    if (m_pEditor == nullptr) {
        return;
    }
    if (sKey == QLatin1String("editorFontFamily")) {
        QFont font = m_pEditor->font();
        font.setFamily(valueValue.toString());
        m_pEditor->setFont(font);
    } else if (sKey == QLatin1String("fontSize")) {
        QFont font = m_pEditor->font();
        font.setPointSize(valueValue.toInt());
        m_pEditor->setFont(font);
    }
}

void PlaceholderEditorEngine::setPreferenceAll(const QVariantMap &mapPreferences)
{
    for (auto it = mapPreferences.constBegin(); it != mapPreferences.constEnd(); ++it) {
        setPreference(it.key(), it.value());
    }
}

//=====================================================================
//  WebView2 引擎：把 MarkText 官方渲染层嵌进 Qt 外壳
//  链路：
//    WebView2EditorEngine
//      ├─ WebView2Host    —— WebView2 运行时（导航/注入/消息/尺寸）
//      ├─ MarkTextBridge  —— 顶替 Electron 主进程（启动时序/偏好/文件/命令路由）
//      └─ webui/          —— 从成品 app.asar 抽出的渲染层 + 原版 preload + env.js 垫片
//=====================================================================
WebView2EditorEngine::WebView2EditorEngine(QObject *pOwner, QWidget *pViewParent)
    : IEditorEngine(pOwner)
    , m_pView(nullptr)
    , m_pHost(nullptr)
    , m_pBridge(nullptr)
    , m_bAttachRequested(false)
{
    m_pView = new QWidget(pViewParent);
    m_pView->setObjectName(QStringLiteral("markTextSurface"));
    m_pView->setAutoFillBackground(true);
    m_pView->setFocusPolicy(Qt::StrongFocus);
    m_pView->installEventFilter(this);

    m_pBridge = new MarkTextBridge(PreferencesStore::instance(), this);
    m_pHost   = new WebView2Host(this);

    // 桥接层 → 页面
    m_pBridge->setPostJson([this](const QString &sJson) {
        if (m_pHost != nullptr) {
            m_pHost->postJson(sJson);
        }
    });

    // 页面 → 桥接层
    connect(m_pHost, &WebView2Host::webMessageReceived,
            m_pBridge, &MarkTextBridge::onWebMessage);

    // 渲染层诊断日志 → 调试输出（排查握手链路时打开 Qt 日志即可看到）
    connect(m_pBridge, &MarkTextBridge::logMessage, this,
            [](const QString &sLevel, const QString &sText) {
                qInfo("MarkText[%s]: %s", qPrintable(sLevel), qPrintable(sText));
            });

    // 控制器就绪 → 做资源映射 + 注入脚本 + 导航
    connect(m_pHost, &WebView2Host::controllerReady, this, [this]() {
        qInfo("WebView2 控制器就绪，开始注入 MarkText 渲染层");
        m_pBridge->setShellWindow(m_pView->window());
        m_pBridge->prepareInjection(m_pHost);
    });

    // 页面加载完成 → 下发启动配置（对照 Electron 的 did-finish-load）
    connect(m_pHost, &WebView2Host::navigationCompleted, this,
            [this](bool bSuccess, int nErrorCode) {
                qInfo("WebView2 导航完成: success=%d err=%d", bSuccess ? 1 : 0, nErrorCode);
                if (bSuccess) {
                    m_pBridge->setShellWindow(m_pView->window());
                    m_pBridge->sendBootstrapEditor();
                }
            });

    connect(m_pHost, &WebView2Host::documentTitleChanged, this,
            [this](const QString &sTitle) {
                emit titleChanged(sTitle);
            });

    // Alt 键 → 外壳（呼出菜单栏，对齐 MarkText 行为）
    connect(m_pHost, &WebView2Host::acceleratorKeyPressed, this,
            [this](int, int nVirtualKey) {
                if (nVirtualKey == 0x12 /* VK_MENU */) {
                    emit altKeyPressed();
                }
            });

    connect(m_pHost, &WebView2Host::processFailed, this,
            [](int nKind, const QString &sDesc) { qWarning("%s (kind=%d)", qPrintable(sDesc), nKind); });

    connect(m_pHost, &WebView2Host::newWindowRequested, this,
            [this](const QString &sUrl) {
                emit m_pBridge->openExternalRequested(sUrl);
            });
}

WebView2EditorEngine::~WebView2EditorEngine()
{
    // 控件与宿主的父子关系挂在调用方（标签页）与本对象上，此处只清引用
    m_pView = nullptr;
    m_pHost = nullptr;
    m_pBridge = nullptr;
}

QString WebView2EditorEngine::engineName() const
{
    return QStringLiteral("MarkText 渲染层（WebView2）");
}

QWidget *WebView2EditorEngine::view() const
{
    return m_pView;
}

MarkTextBridge *WebView2EditorEngine::bridge() const
{
    return m_pBridge;
}

void WebView2EditorEngine::setMarkdownText(const QString &sText)
{
    // 渲染层自己持有文档模型（Muya），外壳不再直接灌文本；
    // 需要打开内容时走 openFileInRenderer()（对应 mt::open-new-tab）。
    Q_UNUSED(sText)
}

QString WebView2EditorEngine::markdownText() const
{
    // 文档内容由渲染层持有，需要时通过桥接层异步取回（见 readme 待办）
    return QString();
}

QString WebView2EditorEngine::exportHtml() const
{
    // 导出交由渲染层的 mt::show-export-dialog 处理
    return QString();
}

bool WebView2EditorEngine::execCommand(const QString &sCommandId)
{
    if (m_pBridge == nullptr) {
        return false;
    }
    // 命令 id 与菜单项 id 同源，直接交给桥接层路由
    return m_pBridge->routeMenuCommand(sCommandId);
}

void WebView2EditorEngine::setPreference(const QString &sKey, const QVariant &valueValue)
{
    Q_UNUSED(sKey)
    Q_UNUSED(valueValue)
    // 渲染层会自己向桥接层要偏好（mt::ask-for-user-preference）；
    // 外壳侧改动由 MainWindow 通过 bridge()->emitToRenderer("mt::user-preference", ...) 广播。
}

void WebView2EditorEngine::setPreferenceAll(const QVariantMap &mapPreferences)
{
    Q_UNUSED(mapPreferences)
}

void WebView2EditorEngine::openFile(const QString &sPath)
{
    if (m_pBridge != nullptr) {
        m_pBridge->openFileInRenderer(sPath);
    }
}

bool WebView2EditorEngine::isRendererReady() const
{
    return m_pBridge != nullptr && m_pBridge->isRendererReady();
}

void WebView2EditorEngine::setViewBackground(const QColor &color)
{
    if (!color.isValid()) {
        return;
    }
    // view 控件本身：attach 前 / 引擎未覆盖到的缝隙都铺成编辑器背景色
    if (m_pView != nullptr) {
        QPalette pal = m_pView->palette();
        pal.setColor(QPalette::Window, color);
        pal.setColor(QPalette::Base, color);
        m_pView->setPalette(pal);
        m_pView->setAutoFillBackground(true);
    }
    // WebView2 默认背景色（加载完成前浏览器进程自己刷的底色）
    if (m_pHost != nullptr) {
        m_pHost->setDefaultBackgroundColor(color);
    }
}

bool WebView2EditorEngine::eventFilter(QObject *pWatched, QEvent *pEvent)
{
    if (pWatched == m_pView) {
        if (pEvent->type() == QEvent::Show || pEvent->type() == QEvent::Resize) {
            // 首次显示时才挂载：此时控件已 reparent 到标签页，HWND 是最终的那个
            ensureAttached();
            syncBounds();
        }
    }
    return IEditorEngine::eventFilter(pWatched, pEvent);
}

void WebView2EditorEngine::ensureAttached()
{
    if (m_bAttachRequested || m_pHost == nullptr || m_pView == nullptr) {
        return;
    }
    m_bAttachRequested = true;

    const QString sUserData = QCoreApplication::applicationDirPath()
            + QStringLiteral("/config/webview2");
    if (!m_pHost->attach(m_pView, sUserData)) {
        qWarning("WebView2 挂载失败（Loader 或运行时缺失），编辑区将保持空白");
        m_bAttachRequested = false;
    }
}

void WebView2EditorEngine::syncBounds()
{
    if (m_pHost == nullptr || m_pView == nullptr || !m_pHost->isControllerReady()) {
        return;
    }
    m_pHost->setBounds(0, 0, m_pView->width(), m_pView->height());
    m_pHost->setVisible(m_pView->isVisible());
}

//=====================================================================
//  引擎工厂
//=====================================================================
bool EditorHost::isWebView2RuntimeInstalled()
{
    // 用桥接层同款探测：要求 WebView2Loader.dll 可加载 + 已装 WebView2 运行时
    return WebView2Host::runtimeAvailable(nullptr);
}

EditorHost::EngineKind EditorHost::availableEngine()
{
    // 运行时就绪即默认使用 MarkText 渲染层；否则退回纯 Qt 占位引擎
    return isWebView2RuntimeInstalled() ? EngineWebView2 : EnginePlaceholder;
}

EditorHost::EngineKind EditorHost::requestedEngine()
{
    const QByteArray baEngine = qgetenv("QTMT_ENGINE").trimmed().toLower();
    if (baEngine == QByteArray("webview2")) {
        return EngineWebView2;      // 挂载点占位：可用来验收"接口替换"这件事本身
    }
    if (baEngine == QByteArray("placeholder")) {
        return EnginePlaceholder;
    }
    return availableEngine();
}

QString EditorHost::engineTitle(EngineKind kind)
{
    return (kind == EngineWebView2)
               ? QStringLiteral("MarkText 渲染层（WebView2）")
               : QStringLiteral("纯 Qt 占位引擎");
}

QString EditorHost::engineDescription(EngineKind kind)
{
    return (kind == EngineWebView2)
               ? QStringLiteral("MarkText 官方渲染层，经 WebView2 嵌入（本轮未接通）")
               : QStringLiteral("第 1/2 阶段的纯 Qt Markdown 编辑器，作为外壳挂载点的占位实现");
}

IEditorEngine *EditorHost::create(EngineKind kind, QObject *pOwner, QWidget *pViewParent)
{
    if (kind == EngineWebView2) {
        return new WebView2EditorEngine(pOwner, pViewParent);
    }
    return new PlaceholderEditorEngine(pOwner, pViewParent);
}
