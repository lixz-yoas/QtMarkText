/*
 * marktextbridge.cpp —— Qt 外壳 ⇄ MarkText 渲染层 桥接层实现
 * ===========================================================================
 * 通道实现分三类：
 *   [A] 完整实现 —— 显示/编辑这条主线必需的：bootInfo、偏好、i18n、文件读写、
 *       剪贴板、shell、窗口控制、启动时序、菜单命令路由。
 *   [B] 降级实现 —— 依赖外部能力（ripgrep 全文搜索、picgo 图床、拼写检查
 *       词典、PDF 导出、自动更新）暂不可用，返回安全值并记日志，不假装成功。
 *   [C] 兜底 —— 未登记通道统一走 fallbackInvoke()，按命名给安全默认值并记日志，
 *       保证渲染层不会因 undefined 崩掉。日志会打到 statusTip / qWarning。
 */

#include "marktextbridge.h"

#include <QJsonDocument>
#include <QStandardPaths>
#include <QTextDocument>

#include "marktextthemes.h"

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegExp>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTextCodec>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <QVariantMap>
#include <QWidget>

// shellapi.h 依赖 windef/windows 的基本类型，必须先引入 windows.h
#include <windows.h>
#include <shellapi.h>
#include <string>

#include "preferencesstore.h"
#include "webview2host.h"

namespace {

/// MarkText 的 md 扩展名（common/filesystem/paths.ts）
const char *const kMarkdownExtensions[] = {
    "markdown", "mdown", "mkdn", "md", "mkd", "mdwn", "mdtxt", "mdtext", "mdx", "text", "txt"
};

/// MarkText 的图片扩展名
const char *const kImageExtensions[] = { "jpeg", "jpg", "png", "gif", "svg", "webp" };

bool hasImageExtension(const QString &sPath)
{
    const QString sLower = sPath.toLower();
    for (const char *pszExt : kImageExtensions) {
        if (sLower.endsWith(QLatin1String(".") + QLatin1String(pszExt))) {
            return true;
        }
    }
    return false;
}

/// 用 Node 的 BufferEncoding 名称解码
QString decodeBytes(const QByteArray &data, const QString &sEncoding, bool *pbHandled)
{
    *pbHandled = true;
    const QString sEnc = sEncoding.toLower();
    if (sEnc.isEmpty() || sEnc == QLatin1String("utf8") || sEnc == QLatin1String("utf-8")) {
        QTextCodec *pCodec = QTextCodec::codecForName("UTF-8");
        return pCodec->toUnicode(data);
    }
    if (sEnc == QLatin1String("utf16le") || sEnc == QLatin1String("ucs2") || sEnc == QLatin1String("ucs-2")) {
        QTextCodec *pCodec = QTextCodec::codecForName("UTF-16LE");
        return pCodec->toUnicode(data);
    }
    if (sEnc == QLatin1String("latin1") || sEnc == QLatin1String("binary")) {
        return QString::fromLatin1(data);
    }
    if (sEnc == QLatin1String("ascii")) {
        return QString::fromLatin1(data);
    }
    if (sEnc == QLatin1String("base64")) {
        return QString::fromLatin1(data.toBase64());
    }
    if (sEnc == QLatin1String("hex")) {
        return QString::fromLatin1(data.toHex());
    }
    // MarkText 也支持 gbk / big5 等（iconv 名称），交给 Qt 的 codecForName 兜底
    QTextCodec *pCodec = QTextCodec::codecForName(sEncoding.toLatin1());
    if (pCodec != nullptr) {
        return pCodec->toUnicode(data);
    }
    *pbHandled = false;
    return QString();
}

/// 二进制回传用的标记对象（webui/env.js 会还原成 Uint8Array）
QJsonValue makeBinaryValue(const QByteArray &data)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("__mtBinary"), QString::fromLatin1(data.toBase64()));
    return obj;
}

/// shell 打开「所在文件夹并选中该文件」
void revealInExplorer(const QString &sPath)
{
    const QString sNative = QDir::toNativeSeparators(QFileInfo(sPath).absoluteFilePath());
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            QStringList() << QStringLiteral("/select,") + sNative);
}

/// 移入回收站
/// Qt 5.12 尚未提供 QFile::moveToTrash（5.15 才有），故走 Win32 SHFileOperation。
/// 安全约束：绝不直接 DeleteFile，永远带 FOF_ALLOWUNDO。
bool moveToRecycleBin(const QString &sPath)
{
    const QString sNative = QDir::toNativeSeparators(QFileInfo(sPath).absoluteFilePath());

    // SHFileOperationW 的 pFrom 要求以「双 \0」结尾
    std::wstring wsFrom(sNative.utf16(), sNative.utf16() + sNative.length());
    wsFrom.push_back(L'\0');
    wsFrom.push_back(L'\0');

    SHFILEOPSTRUCTW op;
    ZeroMemory(&op, sizeof(op));
    op.wFunc  = FO_DELETE;
    op.pFrom  = wsFrom.c_str();
    op.fFlags = static_cast<FILEOP_FLAGS>(FOF_ALLOWUNDO | FOF_NOCONFIRMATION
                                         | FOF_NOERRORUI | FOF_SILENT);

    const int nRet = SHFileOperationW(&op);
    return nRet == 0 && op.fAnyOperationsAborted == FALSE;
}

/// 页面内编辑链路自检脚本（仅 QTMT_SELFTEST=1 时注入）
/// 用途：无头验证「显示 + 编辑」是否真的可用。
///   阶段 1：定位 Muya 编辑容器（唯一 contenteditable），报告其状态；
///   阶段 2：尝试 execCommand('insertText')（Chromium 下可能被拒，失败不影响）；
///   阶段 3：周期回报 DOM 文本 —— 配合外部 SendInput 真实键击，
///           可从日志看到文本是否真的进了 Muya。
const char *const kSelfTestScript =
    "(function(){\n"
    "  function log(t, v){\n"
    "    try{ window.chrome.webview.postMessage({__mt:1, kind:'log', level:'info',\n"
    "      text:'SELFTEST-' + t + ': ' + v}); }catch(e){}\n"
    "  }\n"
    "  var nTries = 0;\n"
    "  function step(){\n"
    "    var el = document.querySelector('[contenteditable=\"true\"]');\n"
    "    if(!el && nTries++ < 100){ setTimeout(step, 100); return; }\n"
    "    if(!el){ log('1-editor-found','false'); return; }\n"
    "    log('1-editor-found','true class=' + el.className);\n"
    "    log('1b-info', 'id=' + el.id + ' editable=' + el.isContentEditable +\n"
    "        ' children=' + el.childElementCount + ' htmlLen=' + (el.innerHTML||'').length);\n"
    "    try{\n"
    "      el.focus();\n"
    "      var sel = window.getSelection();\n"
    "      var range = document.createRange();\n"
    "      range.selectNodeContents(el); range.collapse(true);\n"
    "      sel.removeAllRanges(); sel.addRange(range);\n"
    "      log('2-activeElement', document.activeElement.className);\n"
    "      var ok = document.execCommand('insertText', false,\n"
    "                 'Hello MarkText from Qt shell');\n"
    "      log('2-insertText', ok);\n"
    "    }catch(e){ log('2-insertText','EXC ' + e.message); }\n"
    "    var n = 0;\n"
    "    var timer = setInterval(function(){\n"
    "      var t = (el.innerText || el.textContent || '').replace(/\\s+/g,' ').trim();\n"
    "      log('3-tick' + n, 'len=' + t.length + ' text=' + t.slice(0,80));\n"
    "      if (n === 1) {\n"
    "        var codes = document.querySelectorAll('code');\n"
    "        log('5-code-count', codes.length);\n"
    "        var c0 = codes[0];\n"
    "        if (c0) {\n"
    "          var cs = getComputedStyle(c0);\n"
    "          log('5-code-text', c0.textContent.slice(0, 80));\n"
    "          log('5-code-style', 'display=' + cs.display + ' visibility=' + cs.visibility\n"
    "            + ' color=' + cs.color + ' fontSize=' + cs.fontSize\n"
    "            + ' font=' + cs.fontFamily.slice(0, 60)\n"
    "            + ' opacity=' + cs.opacity + ' rect=' + JSON.stringify(c0.getBoundingClientRect()));\n"
    "          var par = c0.parentElement;\n"
    "          log('5-parent-html', (par ? par.outerHTML : 'null').replace(/\\s+/g,' ').slice(0, 220));\n"
    "          var pstyle = par ? getComputedStyle(par) : null;\n"
    "          if (pstyle) log('5-parent-style', 'color=' + pstyle.color + ' fontSize=' + pstyle.fontSize\n"
    "            + ' fontFamily=' + pstyle.fontFamily.slice(0, 60));\n"
    "        }\n"
    "      }\n"
    "      if (++n >= 4) clearInterval(timer);\n"
    "    }, 1500);\n"
    "  }\n"
    "  setTimeout(step, 2000);\n"
    "})();\n";

} // namespace

//=====================================================================
//  构造
//=====================================================================
MarkTextBridge::MarkTextBridge(PreferencesStore *pStore, QObject *parent)
    : QObject(parent)
    , m_pStore(pStore)
    , m_pShellWindow(nullptr)
    , m_sWebUiDir(QString())
    , m_bBridgeReady(false)
    , m_bRendererReady(false)
    , m_bBootstrapSent(false)
{
    // 资源根查找：环境变量 > exe/webui > exe/../webui > exe/../../webui
    const QString sEnv = qEnvironmentVariable("QTMT_WEBUI_DIR");
    const QString sAppDir = QCoreApplication::applicationDirPath();
    const QStringList listCandidates = {
        sEnv,
        sAppDir + QStringLiteral("/webui"),
        sAppDir + QStringLiteral("/../webui"),
        sAppDir + QStringLiteral("/../../webui")
    };
    for (const QString &sCandidate : listCandidates) {
        if (sCandidate.isEmpty()) {
            continue;
        }
        if (QFileInfo::exists(sCandidate + QStringLiteral("/renderer/index.html"))) {
            m_sWebUiDir = QDir(sCandidate).absolutePath();
            break;
        }
    }
    if (m_sWebUiDir.isEmpty()) {
        qWarning("MarkTextBridge: 未找到 webui 资源目录（可设 QTMT_WEBUI_DIR 指定）");
    }

    registerInvokeHandlers();
    registerSendHandlers();
}

/// 「段落前悬浮工具栏」功能自检脚本（仅 QTMT_CHECK_FRONTBAR=1 时执行）
///
/// 用途：本机沙箱抓不到 WebView2 画面（GPU 合成层读不到），UI 改动无法用
///       截图验证。于是**按官方 FrontMenu 的真实结构造一份 DOM**，读回
///       getComputedStyle，用数值判断补丁是否生效。
///       之所以必须造一份而不是直接查真实元素：官方的 FrontMenu 只在
///       「点开」时才 render() 出 ul，启动后容器是空的。
const char *const kFrontBarCheckScript =
    "(function(){\n"
    "  try{\n"
    "    var hasStyle = !!document.getElementById('qtmt-frontmenu-bar-style');\n"
    "    var wrap = document.createElement('div');\n"
    "    wrap.className = 'ag-float-wrapper';\n"
    "    var menu = document.createElement('div');\n"
    "    menu.className = 'ag-front-menu ag-float-container';\n"
    "    var inner = document.createElement('div');\n"
    "    var ul = document.createElement('ul');\n"
    "    function mk(cls, text, cut){\n"
    "      var li = document.createElement('li');\n"
    "      li.className = 'item ' + cls;\n"
    "      var iw = document.createElement('div'); iw.className = 'icon-wrapper';\n"
    "      li.appendChild(iw);\n"
    "      var sp = document.createElement('span'); sp.textContent = text; li.appendChild(sp);\n"
    "      var sc = document.createElement('div'); sc.className = 'short-cut';\n"
    "      var s2 = document.createElement('span'); s2.textContent = cut || ''; sc.appendChild(s2);\n"
    "      li.appendChild(sc);\n"
    "      return li;\n"
    "    }\n"
    "    ul.appendChild(mk('duplicate', 'chongfu', 'S+C'));\n"
    "    var ti = mk('turnInto', 'zhuanhuan', '');\n"
    "    var sm = document.createElement('div'); sm.className = 'submenu';\n"
    "    var sul = document.createElement('ul');\n"
    "    sul.appendChild(mk('paragraph', 'duanluo', ''));\n"
    "    sul.appendChild(mk('heading-1', 'biaoti1', ''));\n"
    "    sul.appendChild(mk('quote-block', 'yinyong', ''));\n"
    "    sm.appendChild(sul); ti.appendChild(sm);\n"
    "    ul.appendChild(ti);\n"
    "    ul.appendChild(mk('new', 'xinjian', 'S+N'));\n"
    "    ul.appendChild(mk('delete', 'shanchu', 'S+D'));\n"
    "    inner.appendChild(ul); menu.appendChild(inner); wrap.appendChild(menu);\n"
    "    document.body.appendChild(wrap);\n"
    "    function q(sel){ var e = menu.querySelector(sel); return e ? getComputedStyle(e) : null; }\n"
    "    var cs = getComputedStyle(ul);\n"
    "    var r = {};\n"
    "    r.styleInjected   = hasStyle;\n"
    "    r.ulFlexDir       = cs.flexDirection;\n"
    "    r.turnIntoDisplay = q('li.turnInto') ? q('li.turnInto').display : 'null';\n"
    "    r.submenuDisplay  = q('.submenu') ? q('.submenu').display : 'null';\n"
    "    r.submenuUlDisplay= q('.submenu ul') ? q('.submenu ul').display : 'null';\n"
    "    r.headingDisplay  = q('li.heading-1') ? q('li.heading-1').display : 'null';\n"
    "    r.headingOrder    = q('li.heading-1') ? q('li.heading-1').order : 'null';\n"
    "    r.dupOrder        = q('li.duplicate') ? q('li.duplicate').order : 'null';\n"
    "    r.newOrder        = q('li.new') ? q('li.new').order : 'null';\n"
    "    r.delOrder        = q('li.delete') ? q('li.delete').order : 'null';\n"
    "    r.spanDisplay     = q('li.duplicate > span') ? q('li.duplicate > span').display : 'null';\n"
    "    r.delBorderRight  = q('li.delete') ? q('li.delete').borderRightWidth : 'null';\n"
    "    r.menuWidth       = getComputedStyle(menu).width;\n"
    "    r.menuMaxWidth    = getComputedStyle(menu).maxWidth;\n"
    "    r.boxTransparent  = getComputedStyle(wrap).backgroundColor;\n"
    "    wrap.remove();\n"
    "    return JSON.stringify(r);\n"
    "  }catch(e){ return 'FRONTBAR-ERR: ' + e.message; }\n"
    "})()"
;


MarkTextBridge::~MarkTextBridge() = default;

void MarkTextBridge::setPostJson(const PostJsonFn &fnPost)
{
    m_fnPost = fnPost;
}

void MarkTextBridge::setShellWindow(QWidget *pWindow)
{
    m_pShellWindow = pWindow;
}

QString MarkTextBridge::webUiDir() const
{
    return m_sWebUiDir;
}

QString MarkTextBridge::startUrl() const
{
    // 对照 main/windows/base.ts 的 _buildUrlWithSettings：
    // 渲染层 bootstrapRenderer()（renderer/src/bootstrap.ts）从 URL 取
    //   udp / debug / wid / type + 设置项 cff/cfs/hsb/theme/tbs，
    // 其中 udp 缺失会直接 throw "No user data path is given."，导致白屏。
    // 因此虚拟主机 URL **必须**带齐这串查询参数。
    QUrl url(QStringLiteral("https://marktext.local/renderer/index.html"));
    QUrlQuery query;

    // 取偏好；本工程偏好表暂缺的键按 MarkText preference.json 的默认值兜底
    auto prefStr = [this](const char *pszKey, const char *pszDefault) -> QString {
        const QString s = m_pStore != nullptr ? m_pStore->stringValue(QLatin1String(pszKey))
                                              : QString();
        return s.isEmpty() ? QLatin1String(pszDefault) : s;
    };
    auto prefBool = [this](const char *pszKey, bool bDefault) -> bool {
        return m_pStore != nullptr ? m_pStore->boolValue(QLatin1String(pszKey)) : bDefault;
    };
    auto prefInt = [this](const char *pszKey, int nDefault) -> int {
        return m_pStore != nullptr ? m_pStore->intValue(QLatin1String(pszKey)) : nDefault;
    };

    const QString sUserData = QCoreApplication::applicationDirPath() + QStringLiteral("/config");
    query.addQueryItem(QStringLiteral("udp"), QDir::toNativeSeparators(sUserData));
    query.addQueryItem(QStringLiteral("debug"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("wid"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("editor"));

    // 设置项（与 base.ts 的 searchParams.set 一一对应）
    query.addQueryItem(QStringLiteral("cff"),
                       prefStr("codeFontFamily", "DejaVu Sans Mono"));
    query.addQueryItem(QStringLiteral("cfs"),
                       QString::number(prefInt("codeFontSize", 14)));
    query.addQueryItem(QStringLiteral("hsb"),
                       prefBool("hideScrollbar", false) ? QStringLiteral("1") : QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("theme"),
                       MarkTextThemes::effectiveThemeId(
                           prefStr("theme", "light"),
                           prefBool("followSystemTheme", true),
                           prefStr("lightModeTheme", "light"),
                           prefStr("darkModeTheme", "dark")));
    // ---- tbs（titleBarStyle）：固定 native，不读偏好 ----
    //
    // 渲染层在 titleBarStyle === "custom" 时会在自己的标题栏右侧画一套
    // 窗口按钮（frameless-titlebar-close / -toggle / -minimize，
    // 见 index-B-PE276Q.js 的 showCustomTitleBar 与三个分支）。
    // 而外壳在菜单栏右上角已经有WindowControlBar 那套了
    // （nativeEvent 去掉了系统标题栏，见 MainWindow::nativeEvent）——
    // 两者同时存在就是"两套窗口按钮"。
    //
    // 故这里一律下发 "native"：渲染层不再画自绘按钮、也不给标题栏加
    // frameless 类（那会让标签栏右侧留出一块空白）。
    // 拖动窗口改由外壳菜单栏承担（MainWindow::isMenuBarCaptionArea +
    // eventFilter 已实现拖动与双击最大化），功能不丢。
    //
    // 之所以不读偏好：偏好里的 titleBarStyle 是官方为 Electron 准备的
    // （原生标题栏 or 自绘），在 Qt 外壳下"原生标题栏"不存在，
    // 两个取值都会导致按钮缺失或重复，没有第三种有意义的状态。
    query.addQueryItem(QStringLiteral("tbs"), QStringLiteral("native"));

    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

//=====================================================================
//  bootInfo（对照 main/ipc/bootInfo.ts 的 buildBootInfo）
//=====================================================================
QJsonObject MarkTextBridge::bootInfo() const
{
    QJsonObject objPaths;
    objPaths.insert(QStringLiteral("ripgrepBinary"), QString());  // 未随包分发，见 readme 待办
    objPaths.insert(QStringLiteral("resources"), m_sWebUiDir);
    objPaths.insert(QStringLiteral("userData"),
                    QCoreApplication::applicationDirPath() + QStringLiteral("/config"));
    objPaths.insert(QStringLiteral("cwd"), QDir::currentPath());

    QJsonObject objVersions;
    objVersions.insert(QStringLiteral("node"), QString());
    objVersions.insert(QStringLiteral("chrome"), QString());
    objVersions.insert(QStringLiteral("electron"), QString());

    QJsonArray arrInclusions;
    for (const char *pszExt : kMarkdownExtensions) {
        arrInclusions.append(QLatin1String("*.") + QLatin1String(pszExt));
    }

    QJsonObject obj;
    obj.insert(QStringLiteral("platform"), QStringLiteral("win32"));
    obj.insert(QStringLiteral("arch"), QStringLiteral("x64"));
    obj.insert(QStringLiteral("versions"), objVersions);
    obj.insert(QStringLiteral("env"), QJsonObject());
    obj.insert(QStringLiteral("paths"), objPaths);
    obj.insert(QStringLiteral("isUpdatable"), false);
    obj.insert(QStringLiteral("MARKDOWN_INCLUSIONS"), arrInclusions);
    return obj;
}

QJsonObject MarkTextBridge::preferenceObjectForRenderer() const
{
    QJsonObject obj;
    if (m_pStore != nullptr) {
        obj = QJsonObject::fromVariantMap(m_pStore->all());
        // theme 必须是"生效主题"：开启跟随系统后，preference.theme 只是选中的值，
        // 真正要加载的是 lightModeTheme / darkModeTheme 之一（原版由 Electron
        // 主进程的 nativeTheme 解析，本工程在 MarkTextThemes 里做同一件事）。
        obj.insert(QStringLiteral("theme"),
                   MarkTextThemes::effectiveThemeId(
                       m_pStore->stringValue(QStringLiteral("theme")),
                       m_pStore->boolValue(QStringLiteral("followSystemTheme")),
                       m_pStore->stringValue(QStringLiteral("lightModeTheme")),
                       m_pStore->stringValue(QStringLiteral("darkModeTheme"))));

        // titleBarStyle 同理固定native：渲染层 ASK_FOR_USER_PREFERENCE 会把这份
        // 全量偏好灌回 store，若这里透出 ini 里的历史值（如 custom），
        // 就会把 URL 参数刚设好的 native 覆盖回去、按钮又冒出来。
        // 详见 startUrl() 里 tbs 参数处的说明。
        obj.insert(QStringLiteral("titleBarStyle"), QStringLiteral("native"));
    }
    return obj;
}

QString MarkTextBridge::syncResponsesJson() const
{
    QJsonObject objSync;
    objSync.insert(QStringLiteral("mt::boot-info"), bootInfo());
    // 大小写不同名的同一路径：Windows 下视为同一文件（marktext 原逻辑会再问主进程核对）
    objSync.insert(QStringLiteral("mt::paths::is-same-sync"), true);

    const QString sJson = QString::fromUtf8(QJsonDocument(objSync).toJson(QJsonDocument::Compact));
    return QStringLiteral("window.__mtBridgeConfig = { bootInfo: %1, syncResponses: %2 };")
            .arg(QString::fromUtf8(QJsonDocument(bootInfo()).toJson(QJsonDocument::Compact)),
                 sJson);
}

//=====================================================================
//  注入 + 导航
//=====================================================================
void MarkTextBridge::prepareInjection(WebView2Host *pHost)
{
    if (pHost == nullptr || m_sWebUiDir.isEmpty()) {
        return;
    }
    // 记住宿主：导出/打印/自动保存要用它 executeScript 取页面内容
    m_pHost = pHost;
    if (!connect(pHost, &WebView2Host::executeScriptFinished,
                 this, &MarkTextBridge::onScriptResult)) {
        qWarning("MarkTextBridge: executeScriptFinished 连接失败");
    }
    // PDF 由 WebView2 直接写入目标文件（PrintToPdf），
    // 回执只带「成功与否 + 哪个文件」，不再有 base64 数据。
    connect(pHost, &WebView2Host::printToPdfFinished,
            this, [this](bool bOk, const QString &sOutPath) {
                onPdfRendered(bOk, sOutPath);
            });

    // 虚拟主机映射：把 webui 目录挂到 https://marktext.local
    pHost->mapVirtualHost(QStringLiteral("marktext.local"), m_sWebUiDir);

    // ① 注入 bootInfo 与同步应答表
    pHost->addInitScript(syncResponsesJson());

    // ② 注入环境垫片（提供 require/electron/传输层）
    QFile fileEnv(m_sWebUiDir + QStringLiteral("/env.js"));
    if (fileEnv.open(QIODevice::ReadOnly)) {
        pHost->addInitScript(QString::fromUtf8(fileEnv.readAll()));
        fileEnv.close();
    } else {
        qWarning("MarkTextBridge: 读取 env.js 失败");
    }

    // ③ 注入 MarkText 原版 preload（**文件内容零改动**）
    QFile filePreload(m_sWebUiDir + QStringLiteral("/preload/index.js"));
    if (filePreload.open(QIODevice::ReadOnly)) {
        pHost->addInitScript(QString::fromUtf8(filePreload.readAll()));
        filePreload.close();
    } else {
        qWarning("MarkTextBridge: 读取 preload/index.js 失败");
    }

    // ④ 注入本工程的 UI 调整补丁
    //    段落前那个按钮点开的竖向弹出菜单 -> 横向悬浮工具栏。
    //    走注入而不是改官方 bundle：与 env.js 同一机制，
    //    官方文件（index-B-PE276Q.js / index-Bc-qQhaX.css）保持零改动。
    QFile fileUiTweaks(m_sWebUiDir + QStringLiteral("/ui-frontmenu-bar.js"));
    if (fileUiTweaks.open(QIODevice::ReadOnly)) {
        pHost->addInitScript(QString::fromUtf8(fileUiTweaks.readAll()));
        fileUiTweaks.close();
    } else {
        qWarning("MarkTextBridge: 读取 ui-frontmenu-bar.js 失败");
    }

    // ⑤ （可选）段落前悬浮工具栏的功能自检：QTMT_CHECK_FRONTBAR=1
    //    沙箱抓不到 WebView2 画面，故用「造 DOM + 读计算样式」代替截图。
    if (!qEnvironmentVariable("QTMT_CHECK_FRONTBAR").isEmpty()) {
        QTimer::singleShot(9000, this, [this]() {
            if (m_pHost == nullptr) {
                qInfo("[前栏自检] 宿主未就绪");
                return;
            }
            m_fnScriptDone = [](const QString &sRet) {
                qInfo("[前栏自检] %s", qPrintable(sRet));
            };
            m_pHost->executeScript(QLatin1String(kFrontBarCheckScript));
        });
    }

    // ⑤ （可选）页面内编辑链路自检：设 QTMT_SELFTEST=1 才注入
    if (!qEnvironmentVariable("QTMT_SELFTEST").isEmpty()) {
        pHost->addInitScript(QLatin1String(kSelfTestScript));
        qInfo("MarkTextBridge: 已注入页面自检脚本（QTMT_SELFTEST）");
    }

    pHost->navigate(startUrl());
}

//=====================================================================
//  收发
//=====================================================================
void MarkTextBridge::emitToRenderer(const QString &sChannel, const QJsonArray &args)
{
    if (!m_fnPost) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("__mt"), 1);
    obj.insert(QStringLiteral("kind"), QStringLiteral("emit"));
    obj.insert(QStringLiteral("channel"), sChannel);
    obj.insert(QStringLiteral("args"), args);
    m_fnPost(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void MarkTextBridge::sendReply(int nId, bool bOk, const QJsonValue &value, const QString &sError)
{
    if (!m_fnPost) {
        return;
    }
    QJsonObject obj;
    obj.insert(QStringLiteral("__mt"), 1);
    obj.insert(QStringLiteral("kind"), QStringLiteral("reply"));
    obj.insert(QStringLiteral("id"), nId);
    obj.insert(QStringLiteral("ok"), bOk);
    if (bOk) {
        obj.insert(QStringLiteral("value"), value);
    } else {
        obj.insert(QStringLiteral("error"), sError);
    }
    m_fnPost(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void MarkTextBridge::onWebMessage(const QString &sJson)
{
    const QJsonDocument doc = QJsonDocument::fromJson(sJson.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject objMsg = doc.object();
    if (objMsg.value(QStringLiteral("__mt")).toInt() != 1) {
        return;
    }
    const QString sKind = objMsg.value(QStringLiteral("kind")).toString();
    if (sKind == QLatin1String("invoke")) {
        handleInvoke(objMsg);
    } else if (sKind == QLatin1String("send")) {
        handleSend(objMsg);
    } else if (sKind == QLatin1String("log")) {
        const QString sLevel = objMsg.value(QStringLiteral("level")).toString();
        const QString sText = objMsg.value(QStringLiteral("text")).toString();
        emit logMessage(sLevel, sText);
        if (sLevel == QLatin1String("error")) {
            qWarning("MarkText 渲染层: %s", qPrintable(sText));
        }
    }
}

void MarkTextBridge::handleInvoke(const QJsonObject &objMsg)
{
    const int nId = objMsg.value(QStringLiteral("id")).toInt();
    const QString sChannel = objMsg.value(QStringLiteral("channel")).toString();
    const QJsonArray arrArgs = objMsg.value(QStringLiteral("args")).toArray();

    auto it = m_hashInvoke.find(sChannel);
    if (it == m_hashInvoke.end()) {
        const QJsonValue value = fallbackInvoke(sChannel, arrArgs);
        emit logMessage(QStringLiteral("warn"),
                        QStringLiteral("[未实现 invoke] %1 -> %2")
                                .arg(sChannel,
                                     QString::fromUtf8(QJsonDocument(QJsonArray{ value })
                                                               .toJson(QJsonDocument::Compact))));
        sendReply(nId, true, value);
        return;
    }
    // 通道实现里如果抛错（Qt 里一般为空返回），统一按成功回包
    sendReply(nId, true, it.value()(arrArgs));
}

void MarkTextBridge::handleSend(const QJsonObject &objMsg)
{
    const QString sChannel = objMsg.value(QStringLiteral("channel")).toString();
    const QJsonArray arrArgs = objMsg.value(QStringLiteral("args")).toArray();

    // 自建的握手通道
    if (sChannel == QLatin1String("mt::bridge-env-ready")) {
        m_bBridgeReady = true;
        if (m_fnPost) {
            m_fnPost(QStringLiteral("{\"__mt\":1,\"kind\":\"ready-check\"}"));
        }
        emit logMessage(QStringLiteral("info"), QStringLiteral("渲染层垫片握手完成，已放行消息队列"));
        return;
    }

    auto it = m_hashSend.find(sChannel);
    if (it == m_hashSend.end()) {
        emit logMessage(QStringLiteral("warn"), QStringLiteral("[未实现 send] %1").arg(sChannel));
        return;
    }
    it.value()(arrArgs);
}

QJsonValue MarkTextBridge::fallbackInvoke(const QString &sChannel, const QJsonArray &arrArgs)
{
    Q_UNUSED(arrArgs);
    // 按命名约定给安全默认值，避免渲染层对 undefined 做属性访问时崩掉
    if (sChannel.endsWith(QLatin1String("::list")) || sChannel.endsWith(QLatin1String("readdir"))
        || sChannel.endsWith(QLatin1String("ask-for-image-path"))) {
        return QJsonArray();
    }
    if (sChannel.contains(QLatin1String("::is-")) || sChannel.endsWith(QLatin1String("exists"))) {
        return false;
    }
    if (sChannel.startsWith(QLatin1String("mt::fs::")) || sChannel.startsWith(QLatin1String("mt::path"))) {
        return false;
    }
    return QJsonValue(QJsonValue::Null);
}

//=====================================================================
//  会话恢复：update-buffer-state（收）/ mt::load-state（发）
//=====================================================================
//  快照形如（BUFFERED_STATE_VERSION = 1）：
//    {
//      "version": 1,
//      "currentFileId": "<id>",              // editor 段
//      "tabs": [ { id, pathname, filename, markdown, isSaved,
//                  encoding, lineEnding, trimTrailingNewline,
//                  adjustLineEndingOnSave, cursor, wordCount,
//                  muyaIndexCursor, scrollTop, history, ... } ],
//      "project": { "rootDirectory": "<绝对路径|空串>" },
//      "layout":  { "rightColumn", "showSideBar", "showTabBar",
//                   "sideBarWidth" }
//    }
//
//  注意 layout 段里也含 sideBarWidth —— 若原样回灌会覆盖 r155 刚统一
//  的"默认 2 倍宽"口径。故下发前把 layout.sideBarWidth 去掉，
//  宽度仍由 localStorage / DEFAULT_SIDE_BAR_WIDTH 那套说了算
//  （渲染层 createBufferedLayoutState 对 undefined 是跳过不赋值的）。

QString MarkTextBridge::sessionIniValueKey()
{
    return QStringLiteral("session/bufferedState");
}

bool MarkTextBridge::hasRestorableSession() const
{
    const QJsonObject objState = loadSessionFromIni();
    return !objState.isEmpty() && !objState.value(QStringLiteral("tabs")).toArray().isEmpty();
}

void MarkTextBridge::storeBufferedState(const QJsonObject &objState)
{
    if (objState.isEmpty()) {
        return;
    }

    // ---- 1) 只保留"已落盘"的标签页 ----
    //   判定：pathname 非空且 isSaved 为真。用户口径是"关闭时没有
    //   打开的文件就什么都不恢复"，untitled 页（pathname 为空）
    //   即使 isSaved 也留不住内容，不参与恢复。
    const QJsonArray arrTabsIn = objState.value(QStringLiteral("tabs")).toArray();
    QJsonArray arrTabsKeep;
    QString    sCurrentFileId = objState.value(QStringLiteral("currentFileId")).toString();
    bool       bCurrentStillOpen = false;

    for (const QJsonValue &vTab : arrTabsIn) {
        if (!vTab.isObject()) {
            continue;
        }
        const QJsonObject objTab = vTab.toObject();
        const QString sPathname = objTab.value(QStringLiteral("pathname")).toString();
        if (sPathname.isEmpty()) {
            continue;
        }
        // isSaved 缺失时按"已保存"处理（老快照/边界情况），
        // 因为 pathname 非空本身就意味着它对应磁盘上的一个文件。
        if (objTab.contains(QStringLiteral("isSaved"))
                && !objTab.value(QStringLiteral("isSaved")).toBool()) {
            continue;
        }
        if (objTab.value(QStringLiteral("id")).toString() == sCurrentFileId) {
            bCurrentStillOpen = true;
        }
        arrTabsKeep.append(objTab);
    }

    PreferencesStore *pStore = m_pStore;
    Q_UNUSED(pStore);

    QSettings settings(PreferencesStore::iniFilePath(), QSettings::IniFormat);
    settings.setIniCodec("UTF-8");

    // ---- 2) 过滤后一个不剩：清掉记录，下次走空白首页 ----
    if (arrTabsKeep.isEmpty()) {
        settings.remove(sessionIniValueKey());
        settings.sync();
        emit logMessage(QStringLiteral("info"),
                        QStringLiteral("[会话] 退出时无已保存的打开文件，清除会话记录"));
        return;
    }

    // 当前页若被过滤掉了，退化为第一页，免得恢复后 currentFile 悬空
    if (!bCurrentStillOpen) {
        sCurrentFileId = arrTabsKeep.at(0).toObject().value(QStringLiteral("id")).toString();
    }

    QJsonObject objKeep = objState;
    objKeep.insert(QStringLiteral("currentFileId"), sCurrentFileId);
    objKeep.insert(QStringLiteral("tabs"), arrTabsKeep);

    // ---- 3) 抹掉 layout.sideBarWidth，保住 r155 的默认宽度口径 ----
    if (objKeep.contains(QStringLiteral("layout"))) {
        QJsonObject objLayout = objKeep.value(QStringLiteral("layout")).toObject();
        objLayout.remove(QStringLiteral("sideBarWidth"));
        objKeep.insert(QStringLiteral("layout"), objLayout);
    }

    const QString sJson = QString::fromUtf8(
                QJsonDocument(objKeep).toJson(QJsonDocument::Compact));
    settings.setValue(sessionIniValueKey(), sJson);
    settings.sync();

    qInfo("[会话] 已记录 %d 个打开文件（当前页 id=%s）",
          arrTabsKeep.size(), qPrintable(sCurrentFileId));
}

QJsonObject MarkTextBridge::loadSessionFromIni() const
{
    QSettings settings(PreferencesStore::iniFilePath(), QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    const QString sJson = settings.value(sessionIniValueKey()).toString();
    if (sJson.isEmpty()) {
        return QJsonObject();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(sJson.toUtf8());
    if (!doc.isObject()) {
        return QJsonObject();
    }
    QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("tabs")).toArray().isEmpty()) {
        return QJsonObject();
    }
    return obj;
}

bool MarkTextBridge::restoreBufferedState()
{
    const QJsonObject objState = loadSessionFromIni();
    if (objState.isEmpty()) {
        return false;
    }

    // 二次校验：路径在两次启动之间可能已被删除/改名（U 盘拔了、文件被移走）。
    // 剔除后仍为空则整体放弃，交回外壳走"空白首页 + 上次目录"。
    const QJsonArray arrTabsIn = objState.value(QStringLiteral("tabs")).toArray();
    QJsonArray arrTabsKeep;
    for (const QJsonValue &vTab : arrTabsIn) {
        const QJsonObject objTab = vTab.toObject();
        const QString sPathname = objTab.value(QStringLiteral("pathname")).toString();
        if (sPathname.isEmpty() || !QFileInfo::exists(sPathname)) {
            qInfo("[会话] 跳过已不存在的文件：%s", qPrintable(sPathname));
            continue;
        }
        arrTabsKeep.append(objTab);
    }
    if (arrTabsKeep.isEmpty()) {
        qInfo("[会话] 快照里的文件全部不存在，放弃恢复");
        return false;
    }

    QJsonObject objSend = objState;
    objSend.insert(QStringLiteral("tabs"), arrTabsKeep);
    // currentFileId 若指向被剔除的页，退化为第一页
    const QString sCur = objSend.value(QStringLiteral("currentFileId")).toString();
    bool bCurAlive = false;
    for (const QJsonValue &vTab : arrTabsKeep) {
        if (vTab.toObject().value(QStringLiteral("id")).toString() == sCur) {
            bCurAlive = true;
            break;
        }
    }
    if (!bCurAlive) {
        objSend.insert(QStringLiteral("currentFileId"),
                       arrTabsKeep.at(0).toObject().value(QStringLiteral("id")).toString());
    }

    emitToRenderer(QStringLiteral("mt::load-state"), QJsonArray{ objSend });
    qInfo("[会话] 已下发会话快照：%d 个文件", arrTabsKeep.size());
    return true;
}

//=====================================================================
//  invoke 通道登记
//=====================================================================
void MarkTextBridge::registerInvokeHandlers()
{
    // ---- boot ----
    m_hashInvoke.insert(QStringLiteral("mt::boot-info-async"),
                        [this](const QJsonArray &) -> QJsonValue { return bootInfo(); });

    // ---- 偏好设置 ----
    m_hashInvoke.insert(QStringLiteral("mt::keybinding-get-pref-keybindings"),
                        [](const QJsonArray &) -> QJsonValue {
                            QJsonObject obj;
                            obj.insert(QStringLiteral("defaultKeybindings"), QJsonObject());
                            obj.insert(QStringLiteral("userKeybindings"), QJsonObject());
                            return obj;
                        });
    m_hashInvoke.insert(QStringLiteral("mt::keybinding-get-keyboard-info"),
                        [](const QJsonArray &) -> QJsonValue {
                            QJsonObject obj;
                            obj.insert(QStringLiteral("platform"), QStringLiteral("win32"));
                            obj.insert(QStringLiteral("isMac"), false);
                            return obj;
                        });
    m_hashInvoke.insert(QStringLiteral("mt::keybinding-save-user-keybindings"),
                        [](const QJsonArray &) -> QJsonValue { return true; });

    // ---- i18n ----
    m_hashInvoke.insert(QStringLiteral("mt::i18n::load"),
                        [this](const QJsonArray &arrArgs) -> QJsonValue {
                            const QString sLang = arrArgs.isEmpty() ? QStringLiteral("en")
                                                                   : arrArgs.at(0).toString();
                            const QString sJson = readLocaleJson(sLang);
                            if (sJson.isEmpty()) {
                                return QJsonObject();
                            }
                            return QJsonDocument::fromJson(sJson.toUtf8()).object();
                        });

    // ---- 字体 ----
    m_hashInvoke.insert(QStringLiteral("mt::fonts::list"),
                        [](const QJsonArray &) -> QJsonValue {
                            QJsonArray arr;
                            const QStringList list = QFontDatabase().families();
                            for (const QString &s : list) {
                                arr.append(s);
                            }
                            return arr;
                        });

    // ---- 剪贴板 ----
    m_hashInvoke.insert(QStringLiteral("mt::clipboard::read-text"),
                        [](const QJsonArray &) -> QJsonValue {
                            return QApplication::clipboard()->text();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::clipboard::guess-file-path"),
                        [](const QJsonArray &) -> QJsonValue {
                            const QString sText = QApplication::clipboard()->text().trimmed();
                            if (sText.isEmpty() || sText.contains(QLatin1Char('\n'))) {
                                return QJsonValue(QJsonValue::Null);
                            }
                            if (QFileInfo::exists(sText)) {
                                return sText;
                            }
                            return QJsonValue(QJsonValue::Null);
                        });

    // ---- shell ----
    m_hashInvoke.insert(QStringLiteral("mt::shell::open-external"),
                        [this](const QJsonArray &arrArgs) -> QJsonValue {
                            const QString sUrl = arrArgs.isEmpty() ? QString() : arrArgs.at(0).toString();
                            if (!sUrl.isEmpty()) {
                                emit openExternalRequested(sUrl);
                                QDesktopServices::openUrl(QUrl(sUrl));
                            }
                            return QJsonValue();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::shell::open-path"),
                        [this](const QJsonArray &arrArgs) -> QJsonValue {
                            const QString sPath = arrArgs.isEmpty() ? QString() : arrArgs.at(0).toString();
                            if (!sPath.isEmpty()) {
                                QDesktopServices::openUrl(QUrl::fromLocalFile(sPath));
                            }
                            return QJsonValue();
                        });

    // ---- 窗口控制（异步查询）----
    m_hashInvoke.insert(QStringLiteral("mt::win::is-maximized"),
                        [this](const QJsonArray &) -> QJsonValue {
                            return m_pShellWindow != nullptr && m_pShellWindow->isMaximized();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::win::is-fullscreen"),
                        [this](const QJsonArray &) -> QJsonValue {
                            return m_pShellWindow != nullptr && m_pShellWindow->isFullScreen();
                        });

    // ---- 外部能力：明确降级并记日志（不做假的"已生效"）----
    const char *const arrUnsupported[] = {
        "mt::rg::start", "mt::uploader::upload", "mt::i18n::supported", "mt::i18n::is-supported"
    };
    for (const char *pszChannel : arrUnsupported) {
        const QString sChannel = QLatin1String(pszChannel);
        m_hashInvoke.insert(sChannel, [this, sChannel](const QJsonArray &) -> QJsonValue {
            emit logMessage(QStringLiteral("warn"),
                            QStringLiteral("[能力未接入] %1（返回空值）").arg(sChannel));
            return QJsonObject();
        });
    }

    // ---- 文件系统（对照 main/ipc/fs.ts）----
    m_hashInvoke.insert(QStringLiteral("mt::fs::read-file"),
                        [this](const QJsonArray &arrArgs) -> QJsonValue {
                            const QString sPath = arrArgs.at(0).toString();
                            const QString sEnc = arrArgs.size() > 1 ? arrArgs.at(1).toString() : QString();
                            QFile file(sPath);
                            if (!file.open(QIODevice::ReadOnly)) {
                                emit logMessage(QStringLiteral("error"),
                                                QStringLiteral("读取失败: %1").arg(sPath));
                                return QJsonValue(QJsonValue::Null);
                            }
                            const QByteArray data = file.readAll();
                            file.close();
                            if (sEnc.isEmpty()) {
                                return makeBinaryValue(data);
                            }
                            bool bHandled = false;
                            const QString sText = decodeBytes(data, sEnc, &bHandled);
                            if (!bHandled) {
                                emit logMessage(QStringLiteral("warn"),
                                                QStringLiteral("不支持的编码 %1，按 UTF-8 处理").arg(sEnc));
                                QTextCodec *pCodec = QTextCodec::codecForName("UTF-8");
                                return pCodec->toUnicode(data);
                            }
                            return sText;
                        });

    m_hashInvoke.insert(QStringLiteral("mt::fs::write-file"),
                        [this](const QJsonArray &arrArgs) -> QJsonValue {
                            return QJsonValue(writeFileFromArgs(arrArgs));
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::output-file"),
                        [this](const QJsonArray &arrArgs) -> QJsonValue {
                            // output-file 会自建目录
                            const QString sPath = arrArgs.at(0).toString();
                            QDir().mkpath(QFileInfo(sPath).absolutePath());
                            return QJsonValue(writeFileFromArgs(arrArgs));
                        });

    m_hashInvoke.insert(QStringLiteral("mt::fs::path-exists"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            return QFileInfo::exists(arrArgs.at(0).toString());
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::is-file"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            return QFileInfo(arrArgs.at(0).toString()).isFile();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::is-directory"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            return QFileInfo(arrArgs.at(0).toString()).isDir();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::is-executable"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            // Windows 下等价于「是文件」
                            return QFileInfo(arrArgs.at(0).toString()).isFile();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::stat"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            const QFileInfo fi(arrArgs.at(0).toString());
                            if (!fi.exists()) {
                                return QJsonValue(QJsonValue::Null);
                            }
                            QJsonObject obj;
                            obj.insert(QStringLiteral("size"), static_cast<double>(fi.size()));
                            obj.insert(QStringLiteral("mtimeMs"),
                                       static_cast<double>(fi.lastModified().toMSecsSinceEpoch()));
                            obj.insert(QStringLiteral("ctimeMs"),
                                       static_cast<double>(fi.birthTime().toMSecsSinceEpoch()));
                            obj.insert(QStringLiteral("isFile"), fi.isFile());
                            obj.insert(QStringLiteral("isDirectory"), fi.isDir());
                            obj.insert(QStringLiteral("isSymbolicLink"), fi.isSymLink());
                            return obj;
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::readdir"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            QDir dir(arrArgs.at(0).toString());
                            QJsonArray arr;
                            const QStringList list = dir.entryList(
                                        QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot
                                        | QDir::Hidden | QDir::System,
                                        QDir::Name | QDir::DirsFirst);
                            for (const QString &s : list) {
                                arr.append(s);
                            }
                            return arr;
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::ensure-dir"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            QDir().mkpath(arrArgs.at(0).toString());
                            return QJsonValue();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::empty-dir"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            QDir dir(arrArgs.at(0).toString());
                            if (dir.exists()) {
                                dir.removeRecursively();
                                QDir().mkpath(dir.absolutePath());
                            }
                            return QJsonValue();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::copy"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            const QString sSrc = arrArgs.at(0).toString();
                            const QString sDst = arrArgs.at(1).toString();
                            if (QFileInfo(sSrc).isDir()) {
                                // 递归复制目录
                                QDir().mkpath(sDst);
                                QDirIterator it(sSrc, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot,
                                                QDirIterator::Subdirectories);
                                while (it.hasNext()) {
                                    const QString sPath = it.next();
                                    const QString sRel = QDir(sSrc).relativeFilePath(sPath);
                                    const QString sTarget = sDst + QLatin1Char('/') + sRel;
                                    if (QFileInfo(sPath).isDir()) {
                                        QDir().mkpath(sTarget);
                                    } else {
                                        QFile::copy(sPath, sTarget);
                                    }
                                }
                                return QJsonValue();
                            }
                            QFile::copy(sSrc, sDst);
                            return QJsonValue();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::move"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            QFile::rename(arrArgs.at(0).toString(), arrArgs.at(1).toString());
                            return QJsonValue();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs::unlink"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            QFile::remove(arrArgs.at(0).toString());
                            return QJsonValue();
                        });
    m_hashInvoke.insert(QStringLiteral("mt::fs-trash-item"),
                        [this](const QJsonArray &arrArgs) -> QJsonValue {
                            // 走回收站而非直接删除（安全优先）
                            const QString sPath = arrArgs.at(0).toString();
                            emit logMessage(QStringLiteral("info"),
                                            QStringLiteral("移入回收站: %1").arg(sPath));
                            moveToRecycleBin(sPath);
                            return QJsonValue();
                        });

    // ---- 路径谓词 ----
    m_hashInvoke.insert(QStringLiteral("mt::paths::is-image"),
                        [](const QJsonArray &arrArgs) -> QJsonValue {
                            return hasImageExtension(arrArgs.at(0).toString());
                        });
    m_hashInvoke.insert(QStringLiteral("mt::paths::is-same-sync"),
                        [](const QJsonArray &) -> QJsonValue { return true; });

    // ---- 外部命令是否存在（pandoc 等）：本工程不随包分发 ----
    m_hashInvoke.insert(QStringLiteral("mt::cmd::exists"),
                        [](const QJsonArray &) -> QJsonValue { return false; });

    // ---- 会话快照（渲染层每次改动 debounce 1s 上报；对照 main/ipc/state.ts）----
    //  通道名没有 mt:: 前缀（渲染层就是这么写的），照抄。
    m_hashInvoke.insert(QStringLiteral("update-buffer-state"),
                        [this](const QJsonArray &arrArgs) -> QJsonValue {
                            if (!arrArgs.isEmpty() && arrArgs.at(0).isObject()) {
                                storeBufferedState(arrArgs.at(0).toObject());
                                return true;
                            }
                            return false;
                        });
}

//=====================================================================
//  保存落盘：渲染层 mt::response-file-save / -save-as 的实现
// ---------------------------------------------------------------------
//  为什么必须由本层落盘：
//    渲染层的文档模型（Muya）持有正文，外壳的 markdownText() 是空串。
//    保存链路必须是：
//      [外壳菜单/快捷键] → mt::editor-ask-file-save
//        → 渲染层 FILE_SAVE() 把自己那份 markdown 发回来
//        → mt::response-file-save(id, filename, pathname, markdown, options, defaultPath)
//        → 本函数写盘 → 回 mt::set-pathname（新文件）或 mt::tab-saved（已存在）
//    若绕过这条链路直接让外壳写文件，会把用户文件清空（历史隐患，已修）。
//  对照：packages/desktop/src/main/menu/actions/file.ts
//        - handleResponseForSave / ipcMain.on('mt::response-file-save-as')
//        - filesystem/markdown.ts: writeMarkdownFile
//=====================================================================

namespace {

/// 从正文推荐文件名：取层级最高的标题（对照 main/utils/index.ts）
QString recommendTitleFromMarkdown(const QString &sMarkdown)
{
    // 匹配行首 1~6 个 '#' + 空格 + 标题内容（与上游正则等价，逐行更直观）
    QString sBest;
    int nBestLevel = 99;
    const QStringList listLines = sMarkdown.split(QLatin1Char('\n'));
    for (const QString &sLine : listLines) {
        const QString sTrim = sLine.trimmed();
        if (sTrim.isEmpty() || sTrim.at(0) != QLatin1Char('#')) {
            continue;
        }
        int nLevel = 0;
        while (nLevel < sTrim.length() && sTrim.at(nLevel) == QLatin1Char('#')) {
            ++nLevel;
        }
        if (nLevel < 1 || nLevel > 6 || nLevel >= sTrim.length()
                || sTrim.at(nLevel) != QLatin1Char(' ')) {
            continue;
        }
        const QString sContent = sTrim.mid(nLevel + 1).trimmed();
        if (sContent.isEmpty()) {
            continue;
        }
        if (nLevel < nBestLevel) {
            nBestLevel = nLevel;
            sBest = sContent;
        }
    }
    return sBest;
}

/// 去掉文件名里 Windows 不允许的字符
QString sanitizeFileName(const QString &sName)
{
    QString sOut = sName;
    static const char *const kBad = "\\/:*?\"<>|";
    for (const char *p = kBad; *p != '\0'; ++p) {
        sOut.remove(QLatin1Char(*p));
    }
    sOut = sOut.trimmed();
    while (sOut.endsWith(QLatin1Char('.'))) {
        sOut.chop(1);
    }
    return sOut;
}

/// iconv-lite 的编码名 -> QTextCodec 名（渲染层 options.encoding.encoding 来自此处命名）
QTextCodec *codecForMarkTextEncoding(const QString &sEncoding)
{
    static const struct { const char *pszMarkText; const char *pszQt; } kMap[] = {
        { "utf8",      "UTF-8" },
        { "utf16le",   "UTF-16LE" },
        { "utf16be",   "UTF-16BE" },
        { "gbk",       "GBK" },
        { "gb18030",   "GB18030" },
        { "big5",      "Big5" },
        { "big5hkscs", "Big5-HKSCS" },
        { "shiftjis",  "Shift-JIS" },
        { "eucjp",     "EUC-JP" },
        { "euckr",     "EUC-KR" },
        { "latin1",    "ISO-8859-1" },
        { "latin2",    "ISO-8859-2" },
        { "latin5",    "ISO-8859-9" },
        { "koi8r",     "KOI8-R" },
        { "cp1251",    "Windows-1251" },
        { "cp850",     "IBM850" },
        { "cp866",     "IBM866" }
    };
    const QString sLower = sEncoding.toLower();
    for (const auto &kv : kMap) {
        if (sLower == QLatin1String(kv.pszMarkText)) {
            return QTextCodec::codecForName(kv.pszQt);
        }
    }
    return QTextCodec::codecForName(sEncoding.toLatin1());
}

/// 按渲染层给的 options 写 Markdown 文件（对照 filesystem/markdown.ts: writeMarkdownFile）
bool writeMarkdownFileWithOptions(const QString &sPathname, const QString &sMarkdown,
                                  const QJsonObject &objOptions, QString *pszError)
{
    QString sContent = sMarkdown;

    // 行尾：只有"保存时调整行尾"打开才统一
    const QString sLineEnding = objOptions.value(QStringLiteral("lineEnding")).toString();
    if (objOptions.value(QStringLiteral("adjustLineEndingOnSave")).toBool()) {
        if (sLineEnding == QLatin1String("crlf")) {
            sContent.replace(QRegExp(QStringLiteral("\r\n|\r|\n")), QStringLiteral("\r\n"));
        } else {
            sContent.replace(QRegExp(QStringLiteral("\r\n|\r|\n")), QStringLiteral("\n"));
        }
    }
    // 尾部空行：MarkText 的 trimTrailingNewline 语义（>=1 表示保留 N 个换行）
    const int nTrim = objOptions.value(QStringLiteral("trimTrailingNewline")).toInt();
    if (nTrim >= 0) {
        const QString sEol = (sLineEnding == QLatin1String("crlf"))
                                 ? QStringLiteral("\r\n") : QStringLiteral("\n");
        while (sContent.endsWith(sEol + sEol)) {
            sContent.chop(sEol.length());
        }
        while (sContent.endsWith(QStringLiteral("\n")) || sContent.endsWith(QStringLiteral("\r"))) {
            sContent.chop(1);
        }
        sContent += sEol;
    }

    // 编码
    const QJsonObject objEncoding = objOptions.value(QStringLiteral("encoding")).toObject();
    const QString sEncoding = objEncoding.value(QStringLiteral("encoding"))
                                  .toString(QStringLiteral("utf8"));
    const bool bBom = objEncoding.value(QStringLiteral("isBom")).toBool();
    QTextCodec *pCodec = codecForMarkTextEncoding(sEncoding);
    if (pCodec == nullptr) {
        pCodec = QTextCodec::codecForName("UTF-8");
    }
    QByteArray bytes = pCodec != nullptr ? pCodec->fromUnicode(sContent)
                                         : sContent.toUtf8();
    if (bBom && !bytes.startsWith("\xEF\xBB\xBF")) {
        bytes.prepend("\xEF\xBB\xBF");
    }

    QFile file(sPathname);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (pszError != nullptr) {
            *pszError = file.errorString();
        }
        return false;
    }
    const qint64 nWritten = file.write(bytes);
    file.close();
    if (nWritten != bytes.size()) {
        if (pszError != nullptr) {
            *pszError = QStringLiteral("写入不完整");
        }
        return false;
    }
    return true;
}

} // namespace

//=====================================================================
//  send 通道登记
//=====================================================================
void MarkTextBridge::registerSendHandlers()
{
    // ---- 启动完成 ----
    m_hashSend.insert(QStringLiteral("mt::window-initialized"),
                      [this](const QJsonArray &) {
                          if (!m_bRendererReady) {
                              m_bRendererReady = true;
                              emit logMessage(QStringLiteral("info"),
                                              QStringLiteral("渲染层已初始化（mt::window-initialized）"));
                              // ---- 会话恢复：必须延后一拍下发 ----
                              //  为什么不能在这里直接发 mt::load-state：
                              //    mt::window-initialized 是渲染层在 bootstrap-editor
                              //    回调里**第一行**发的（在解构 config 之后立刻 send），
                              //    此时同文件里的 LISTEN_FOR_STATE_REPLACE（它才是
                              //    mt::load-state 的监听者）还没执行到。
                              //    实测：同一毫秒下发，快照被丢弃，界面停在空白页。
                              //  故推到事件循环下一拍，让 preload 剩下的 init 链先跑完。
                              //  （emit 是 postMessage 异步投递，这一拍足够。）
                              QTimer::singleShot(0, this, [this]() {
                                  const bool bRestored = restoreBufferedState();
                                  if (bRestored) {
                                      emit sessionRestored();
                                  }
                                  // 命令行文件是用户的显式意图，排在会话恢复之后打开
                                  const QStringList listFiles = m_listPendingFiles;
                                  m_listPendingFiles.clear();
                                  // 启动时没让渲染层开空白页（addBlankTab=false，见
                                  // sendBootstrapEditor），而会话又没恢复成（快照里的
                                  // 文件已被删除等）且也没有文件要开：渲染层会停在欢迎
                                  // 页，这里补开一个空白编辑页兜底。
                                  if (!bRestored && listFiles.isEmpty() && !m_bBlankTabByBootstrap) {
                                      emitToRenderer(QStringLiteral("mt::new-untitled-tab"),
                                                     QJsonArray{ true, QString() });
                                      emit logMessage(QStringLiteral("info"),
                                                      QStringLiteral("[会话] 会话未恢复成，补开空白编辑页"));
                                  }
                                  for (const QString &sFile : listFiles) {
                                      emit logMessage(QStringLiteral("info"),
                                                      QStringLiteral("补发待打开文件：%1").arg(sFile));
                                      openFileInRenderer(sFile);
                                  }
                              });
                              emit rendererReady();
                          }
                      });

    // ---- 偏好设置 ----
    m_hashSend.insert(QStringLiteral("mt::ask-for-user-preference"), [this](const QJsonArray &) {
        emitToRenderer(QStringLiteral("mt::user-preference"),
                       QJsonArray{ preferenceObjectForRenderer() });
    });
    m_hashSend.insert(QStringLiteral("mt::ask-for-user-data"), [this](const QJsonArray &) {
        // dataCenter 与 preferences 同名回包，这里给同一份（本工程无独立 dataCenter）
        emitToRenderer(QStringLiteral("mt::user-preference"),
                       QJsonArray{ preferenceObjectForRenderer() });
    });
    m_hashSend.insert(QStringLiteral("mt::set-user-preference"), [this](const QJsonArray &arrArgs) {
        if (m_pStore == nullptr || arrArgs.isEmpty()) {
            return;
        }
        const QJsonObject obj = arrArgs.at(0).toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            m_pStore->setValue(it.key(), it.value().toVariant());
            emit preferenceChangedByRenderer(it.key(), it.value());
        }
    });
    m_hashSend.insert(QStringLiteral("mt::set-user-data"), [this](const QJsonArray &arrArgs) {
        if (m_pStore == nullptr || arrArgs.isEmpty()) {
            return;
        }
        const QJsonObject obj = arrArgs.at(0).toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            m_pStore->setValue(it.key(), it.value().toVariant());
            emit preferenceChangedByRenderer(it.key(), it.value());
        }
    });

    // ---- 语言 ----
    m_hashSend.insert(QStringLiteral("mt::get-current-language"), [this](const QJsonArray &) {
        const QString sLang = m_pStore != nullptr ? m_pStore->stringValue(QStringLiteral("language"))
                                                  : QString();
        emitToRenderer(QStringLiteral("mt::current-language"),
                       QJsonArray{ sLang.isEmpty() ? QStringLiteral("en") : sLang });
    });

    // ---- 快捷键 ----
    m_hashSend.insert(QStringLiteral("mt::request-keybindings"), [this](const QJsonArray &) {
        QJsonObject obj;
        for (const QPair<QString, QString> &pair : m_listKeybindings) {
            if (!pair.first.isEmpty() && !pair.second.isEmpty()) {
                obj.insert(pair.first, pair.second);
            }
        }
        emitToRenderer(QStringLiteral("mt::keybindings-response"), QJsonArray{ obj });
    });

    // ---- 窗口控制 → 外壳 ----
    const char *const arrWinCmds[] = {
        "mt::win::minimize", "mt::win::maximize", "mt::win::unmaximize", "mt::win::toggle-maximize",
        "mt::win::close", "mt::win::toggle-fullscreen"
    };
    for (const char *pszChannel : arrWinCmds) {
        const QString sChannel = QLatin1String(pszChannel);
        m_hashSend.insert(sChannel, [this, sChannel](const QJsonArray &) {
            emit windowActionRequested(sChannel);
        });
    }
    m_hashSend.insert(QStringLiteral("mt::win::set-fullscreen"), [this](const QJsonArray &arrArgs) {
        emit windowActionRequested(QStringLiteral("mt::win::set-fullscreen:%1")
                                           .arg(arrArgs.at(0).toBool() ? 1 : 0));
    });

    // ---- 缩放 ----
    m_hashSend.insert(QStringLiteral("mt::window-zoom"), [this](const QJsonArray &arrArgs) {
        const QJsonObject obj = arrArgs.at(0).toObject();
        if (obj.contains(QStringLiteral("zoomFactor"))) {
            emit windowActionRequested(QStringLiteral("mt::win::zoom:%1")
                                               .arg(obj.value(QStringLiteral("zoomFactor")).toDouble()));
        }
    });

    // ---- 在资源管理器中定位 ----
    m_hashSend.insert(QStringLiteral("mt::shell::show-item"), [this](const QJsonArray &arrArgs) {
        const QString sPath = arrArgs.at(0).toString();
        if (!sPath.isEmpty()) {
            revealInExplorer(sPath);
        }
    });

    // ---- 剪贴板写入 ----
    m_hashSend.insert(QStringLiteral("mt::clipboard::write-text"), [](const QJsonArray &arrArgs) {
        QApplication::clipboard()->setText(arrArgs.at(0).toString());
    });

    // ---- 需要外壳动作的 ----
    m_hashSend.insert(QStringLiteral("mt::open-setting-window"), [this](const QJsonArray &) {
        emit shellActionRequested(QStringLiteral("settings"));
    });
    m_hashSend.insert(QStringLiteral("mt::show-notification"), [this](const QJsonArray &arrArgs) {
        const QJsonObject obj = arrArgs.at(0).toObject();
        emit notificationRequested(obj.value(QStringLiteral("type")).toString(),
                                   obj.value(QStringLiteral("title")).toString(),
                                   obj.value(QStringLiteral("message")).toString());
    });
    m_hashSend.insert(QStringLiteral("mt::open-file"), [this](const QJsonArray &) {
        emit shellActionRequested(QStringLiteral("open-file"));
    });
    m_hashSend.insert(QStringLiteral("mt::open-directory"), [this](const QJsonArray &) {
        emit shellActionRequested(QStringLiteral("open-directory"));
    });
    m_hashSend.insert(QStringLiteral("mt::select-default-directory-to-open"),
                      [this](const QJsonArray &) {
                          emit shellActionRequested(QStringLiteral("open-directory"));
                      });

    // ---- 导出回执：渲染层把 styledHtml 与元信息发回来，外壳弹框落盘 ----
    // 载荷：{ type, title, content, filename, pathname, pageOptions }
    // （官方主进程 handleResponseForExport 做的正是这件事）
    m_hashSend.insert(QStringLiteral("mt::response-export"), [this](const QJsonArray &arrArgs) {
        handleResponseForExport(arrArgs.at(0).toObject());
    });
    // ---- 打印回执：官方只发信号不带内容（它直接 webContents.print()）。
    //      WebView2 无等价 API，故取整页 HTML 后用 QPrinter 打印。
    m_hashSend.insert(QStringLiteral("mt::response-print"), [this](const QJsonArray &) {
        // 与 PDF 同理：走 Chromium 出PDF 字节流，再送QPrinter ——
        // 官方是 webContents.print()（也是 Chromium 出品），样式才保真。
        printViaNative();
    });

    // ---- 布局状态（渲染层自绘侧边栏/标签栏）----
    // 渲染层 SET_LAYOUT/TOGGLE_LAYOUT_ENTRY 后会 DISPATCH_LAYOUT_MENU_ITEMS()：
    //   send("mt::view-layout-changed", windowId, { showTabBar, showSideBar })
    // 外壳收到后同步「视图」菜单的勾选状态，否则点一次菜单项，界面变了、勾却不动。
    m_hashSend.insert(QStringLiteral("mt::view-layout-changed"), [this](const QJsonArray &arrArgs) {
        const QJsonObject obj = arrArgs.at(1).toObject();
        emit layoutChanged(obj.value(QStringLiteral("showSideBar")).toBool(),
                           obj.value(QStringLiteral("showTabBar")).toBool());
    });

    // ---- 菜单勾选 / 可用态回传（对照主进程 _listenForIpcMain 的四个通道）----
    // 这几条以前全落进下面的"未处理 send"兜底，结果是菜单永远不反映
    // 光标处真实状态：段落/格式打不了勾，行结束符永远不选中。
    // 载荷的第 0 项都是 windowId，第 1 项才是状态本身。
    m_hashSend.insert(QStringLiteral("mt::editor-selection-changed"),
                      [this](const QJsonArray &arrArgs) {
                          emit selectionStateChanged(arrArgs.at(1).toObject());
                      });
    m_hashSend.insert(QStringLiteral("mt::update-format-menu"), [this](const QJsonArray &arrArgs) {
        emit formatMenuStateChanged(arrArgs.at(1).toObject());
    });
    m_hashSend.insert(QStringLiteral("mt::update-line-ending-menu"),
                      [this](const QJsonArray &arrArgs) {
                          emit lineEndingMenuStateChanged(arrArgs.at(1).toString());
                      });
    // 侧边栏勾选（layoutChanged 已覆盖布局，这里同步的是 sideBarMenuItem 本身）
    m_hashSend.insert(QStringLiteral("mt::update-sidebar-menu"), [this](const QJsonArray &arrArgs) {
        const bool bShow = arrArgs.at(1).toBool();
        m_pStore->setValue(QStringLiteral("sideBarVisibility"), bShow);
        emit layoutChanged(bShow, m_pStore->boolValue(QStringLiteral("tabBarVisibility")));
    });

    // ---- 保存落盘（渲染层 FILE_SAVE / FILE_SAVE_AS 的回执）----
    // 渲染层把自己持有的 markdown 发回来，这里落盘并回报结果。
    m_hashSend.insert(QStringLiteral("mt::response-file-save"),
                      [this](const QJsonArray &arrArgs) {
                          handleFileSaveRequest(arrArgs, false);
                      });
    m_hashSend.insert(QStringLiteral("mt::response-file-save-as"),
                      [this](const QJsonArray &arrArgs) {
                          handleFileSaveRequest(arrArgs, true);
                      });

    // ---- 退出握手 ----
    //  外壳 closeEvent 发 mt::ask-for-close，渲染层会先同步刷一次会话快照
    //  （sendBufferedState）再回 mt::close-window；本层把它转成信号，
    //  外壳据此放行关闭。注意必须注册在下面 arrCmdChannels 兜底循环之前，
    //  否则会被"统一忽略"占位覆盖（循环里有 contains 保护，但先注册更直观）。
    m_hashSend.insert(QStringLiteral("mt::close-window"),
                      [this](const QJsonArray &) {
                          emit logMessage(QStringLiteral("info"),
                                          QStringLiteral("[退出] 渲染层已回 mt::close-window（快照已刷出）"));
                          emit closeWindowRequested();
                      });
    // 有未保存内容时渲染层会回这条（本工程未实现保存确认对话框，
    // 直接按"放行关闭"处理：未保存内容由渲染层自己的恢复机制兜底）
    m_hashSend.insert(QStringLiteral("mt::close-window-confirm"),
                      [this](const QJsonArray &) {
                          emit logMessage(QStringLiteral("info"),
                                          QStringLiteral("[退出] 渲染层回 mt::close-window-confirm，直接放行"));
                          emit closeWindowRequested();
                      });

    // ---- 命令类（qt 菜单已直接路由，这里兜底）----
    const char *const arrCmdChannels[] = {
        "mt::cmd-open-file", "mt::cmd-open-folder", "mt::cmd-new-editor-window",
        "mt::cmd-import-file", "mt::cmd-close-window", "mt::cmd-toggle-autosave",
        "mt::app-try-quit",
        "mt::open-file-by-window-id", "mt::make-screenshot", "mt::check-for-update",
        "mt::NEED_UPDATE", "mt::handle-renderer-error", "mt::format-link-click",
        "mt::save-tabs", "mt::window-tab-closed", "mt::view-layout-changed",
        "mt::set-view-layout", "mt::show-command-palette", "mt::show-export-dialog",
        "mt::print-service-clearup", "mt::pandoc-not-exists", "mt::invalidate-image-cache",
        "mt::screenshot-captured", "mt::menu::closed", "mt::menu::popup",
        "mt::menu::popup-application", "mt::window::drop", "mt::window-active-status",
        "mt::editor-format-action", "mt::editor-paragraph-action",
        "mt::editor-edit-action", "mt::editor-ask-file-save", "mt::editor-ask-file-save-as",
        "mt::editor-close-tab", "mt::editor-move-file", "mt::editor-rename-file",
        "mt::spellchecker-set-enabled", "mt::spellchecker-switch-language",
        "mt::spellchecker-remove-word", "mt::spellchecker-get-custom-dictionary-words",
        "mt::spellchecker-get-available-dictionaries", "mt::keybinding-debug-dump-keyboard-info",
        "mt::ask-for-open-project-in-sidebar", "mt::ask-for-modify-image-folder-path",
        "mt::ask-for-image-path", "mt::ask-for-image-auto-path", "mt::update-file",
        "mt::update-object-tree",
        // 以下四条已在上面注册了真实处理（菜单状态回传），从兜底列表移除：
        // "mt::update-sidebar-menu", "mt::update-format-menu", "mt::update-line-ending-menu"
        // "mt::editor-selection-changed"
        "mt::set-file-encoding", "mt::set-final-newline",
        "mt::tab-saved", "mt::tab-save-failure", "mt::export-success",
        // "mt::response-export" / "mt::response-print" 已在上面注册真实处理
        "mt::response-file-save", "mt::response-file-save-as",
        "mt::response-file-move-to", "mt::rename", "mt::set-pathname", "mt::set-user-data",
        "mt::spelling-replace-misspelling", "mt::spelling-show-switch-language",
        "mt::force-close-tabs-by-id", "mt::switch-tab-by-index", "mt::switch-tab-by-file_path",
        "mt::tabs-cycle-left", "mt::tabs-cycle-right", "mt::new-untitled-tab",
        "mt::open-new-tab", "mt::load-state", "mt::bootstrap-editor"
    };
    for (const char *pszChannel : arrCmdChannels) {
        const QString sChannel = QLatin1String(pszChannel);
        if (m_hashSend.contains(sChannel)) {
            continue;
        }
        m_hashSend.insert(sChannel, [this, sChannel](const QJsonArray &) {
            emit logMessage(QStringLiteral("info"),
                            QStringLiteral("[外壳未处理 send] %1").arg(sChannel));
        });
    }
}

//=====================================================================
//  工具
//=====================================================================
QString MarkTextBridge::readLocaleJson(const QString &sLanguage) const
{
    if (m_sWebUiDir.isEmpty()) {
        return QString();
    }
    QStringList listCandidates;
    if (!sLanguage.isEmpty()) {
        listCandidates << sLanguage;
    }
    // 语言代码归一：zh-cn -> zh-CN
    if (sLanguage.contains(QLatin1Char('-')) || sLanguage.contains(QLatin1Char('_'))) {
        QString sNorm = sLanguage;
        sNorm.replace(QLatin1Char('_'), QLatin1Char('-'));
        const int nPos = sNorm.indexOf(QLatin1Char('-'));
        sNorm = sNorm.left(nPos + 1) + sNorm.mid(nPos + 1).toUpper();
        listCandidates << sNorm;
    }
    listCandidates << QStringLiteral("en");

    for (const QString &sName : listCandidates) {
        QFile file(m_sWebUiDir + QStringLiteral("/locales/") + sName + QStringLiteral(".json"));
        if (file.open(QIODevice::ReadOnly)) {
            const QString sJson = QString::fromUtf8(file.readAll());
            file.close();
            return sJson;
        }
    }
    return QString();
}

void MarkTextBridge::setKeybindings(const QList<QPair<QString, QString> > &listKeybindings)
{
    m_listKeybindings = listKeybindings;
}

void MarkTextBridge::openFileInRenderer(const QString &sPath)
{
    // 渲染层尚未初始化：先入队，等 mt::window-initialized 后补发
    // （命令行参数在首帧就到，早于 WebView2 完成 注入+导航+bootstrap 的链路）
    if (!m_bRendererReady) {
        m_listPendingFiles.append(QDir::toNativeSeparators(sPath));
        emit logMessage(QStringLiteral("info"),
                        QStringLiteral("渲染层未就绪，文件先入队待发：%1").arg(sPath));
        return;
    }

    QFile file(sPath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit logMessage(QStringLiteral("error"), QStringLiteral("无法打开文件: %1").arg(sPath));
        return;
    }
    const QByteArray data = file.readAll();
    file.close();

    const QFileInfo fi(sPath);
    QJsonObject objDoc;
    objDoc.insert(QStringLiteral("markdown"), QString::fromUtf8(data));
    objDoc.insert(QStringLiteral("filename"), fi.fileName());
    objDoc.insert(QStringLiteral("pathname"), QDir::toNativeSeparators(fi.absoluteFilePath()));
    objDoc.insert(QStringLiteral("encoding"), QStringLiteral("utf8"));
    objDoc.insert(QStringLiteral("lineEnding"),
                  data.contains("\r\n") ? QStringLiteral("crlf") : QStringLiteral("lf"));
    objDoc.insert(QStringLiteral("adjustLineEndingOnSave"), false);
    objDoc.insert(QStringLiteral("trimTrailingNewline"), 1);

    QJsonObject objOptions;
    objOptions.insert(QStringLiteral("selected"), true);

    emitToRenderer(QStringLiteral("mt::open-new-tab"),
                   QJsonArray{ objDoc, objOptions, true });
}

//=====================================================================
//  write-file / output-file 的公共实现
//=====================================================================
bool MarkTextBridge::writeFileFromArgs(const QJsonArray &arrArgs)
{
    const QString sPath = arrArgs.at(0).toString();
    const QJsonValue valueData = arrArgs.at(1);

    QByteArray bytes;
    if (valueData.isObject() && valueData.toObject().contains(QStringLiteral("__mtBinary"))) {
        bytes = QByteArray::fromBase64(
                    valueData.toObject().value(QStringLiteral("__mtBinary")).toString().toLatin1());
    } else if (valueData.isString()) {
        bytes = valueData.toString().toUtf8();
    } else if (valueData.isArray()) {
        // Node Buffer 的 JSON 形态 {type:'Buffer',data:[...]} 已被 env.js 归一为二进制；
        // 这里的数组兜底用于纯数字数组
        const QJsonArray arr = valueData.toArray();
        bytes.reserve(arr.size());
        for (const QJsonValue &v : arr) {
            bytes.append(static_cast<char>(v.toInt()));
        }
    } else if (!valueData.isNull() && !valueData.isUndefined()) {
        bytes = QJsonDocument(QJsonArray{ valueData }).toJson(QJsonDocument::Compact);
    }

    QFile file(sPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit logMessage(QStringLiteral("error"), QStringLiteral("写入失败: %1").arg(sPath));
        return false;
    }
    file.write(bytes);
    file.close();
    return true;
}

//=====================================================================
//  启动时序：bootstrap-editor（对照 main/windows/editor.ts）
//=====================================================================
void MarkTextBridge::sendBootstrapEditor()
{
    if (m_bBootstrapSent) {
        return;
    }
    m_bBootstrapSent = true;

    const QString sEol = m_pStore != nullptr ? m_pStore->stringValue(QStringLiteral("endOfLine"))
                                            : QString();
    QString sLineEnding = QStringLiteral("crlf");   // Windows 默认
    if (sEol == QLatin1String("lf")) {
        sLineEnding = QStringLiteral("lf");
    } else if (sEol == QLatin1String("crlf")) {
        sLineEnding = QStringLiteral("crlf");
    }

    // 对照 main/windows/editor.ts:
    //   const resolvedSideBarVisibility = restoreLayoutState ? !!sideBarVisibility : false
    const bool bRestoreLayout = m_pStore != nullptr
            && m_pStore->boolValue(QStringLiteral("restoreLayoutState"));
    const bool bSideBar = bRestoreLayout
            && m_pStore != nullptr && m_pStore->boolValue(QStringLiteral("sideBarVisibility"));
    const bool bTabBar = m_pStore != nullptr && m_pStore->boolValue(QStringLiteral("tabBarVisibility"));
    const bool bSourceCode = m_pStore != nullptr
            && m_pStore->boolValue(QStringLiteral("sourceCodeModeEnabled"));

    QJsonObject objConfig;
    // ---- addBlankTab 不能恒为 true ----
    //  原版口径（main/windows/editor.ts:79）：
    //    const addBlankTab = !bufferStoreInfo && !rootDirectory && ...
    //  即"有会话要恢复时不开空白页"。若恒为 true，渲染层在 bootstrap 回调里
    //  先 NEW_UNTITLED_TAB 建空白页 → app.vue 的 <editor-with-tabs> 随即以
    //  空 markdown 挂载（Muya 在 onMounted 里一次性吃 props.markdown，此后
    //  只认 file-loaded / file-changed 两个事件）；紧接着下发的 mt::load-state
    //  (RESTORE_BUFFERED_STATE) 只换 store 状态、不 emit 上述任一事件 ——
    //  于是恢复出来的"当前页"永远空白，要切一次标签才出内容（20261014 实测）。
    const bool bAddBlankTab = !hasRestorableSession();
    m_bBlankTabByBootstrap = bAddBlankTab;
    objConfig.insert(QStringLiteral("addBlankTab"), bAddBlankTab);
    objConfig.insert(QStringLiteral("markdownList"), QJsonArray());
    objConfig.insert(QStringLiteral("lineEnding"), sLineEnding);
    objConfig.insert(QStringLiteral("sideBarVisibility"), bSideBar);
    objConfig.insert(QStringLiteral("tabBarVisibility"), bTabBar);
    objConfig.insert(QStringLiteral("sourceCodeModeEnabled"), bSourceCode);

    emitToRenderer(QStringLiteral("mt::bootstrap-editor"), QJsonArray{ objConfig });
    emit logMessage(QStringLiteral("info"), QStringLiteral("已下发 mt::bootstrap-editor"));
}

//=====================================================================
//  保存落盘：渲染层 mt::response-file-save / -save-as 的处理
//  参数：id, filename, pathname, markdown, options, defaultPath
//=====================================================================
void MarkTextBridge::handleFileSaveRequest(const QJsonArray &arrArgs, bool bSaveAs)
{
    const QString sId       = arrArgs.at(0).toString();
    const QString sFilename = arrArgs.at(1).toString();
    const QString sPathname = arrArgs.at(2).toString();
    const QString sMarkdown = arrArgs.at(3).toString();
    const QJsonObject objOptions = arrArgs.at(4).toObject();
    const QString sDefaultPath   = arrArgs.at(5).toString();

    if (sId.isEmpty()) {
        emit logMessage(QStringLiteral("warn"),
                        QStringLiteral("保存请求缺少 tab id，已忽略"));
        return;
    }

    // 推荐文件名：正文里的最高级标题 → 文件名 → Untitled
    QString sRecommend = sanitizeFileName(recommendTitleFromMarkdown(sMarkdown));
    if (sRecommend.isEmpty()) {
        sRecommend = sanitizeFileName(sFilename);
    }
    if (sRecommend.isEmpty()) {
        sRecommend = QStringLiteral("Untitled");
    }

    // 文件是否已经在磁盘上（有 pathname 即已存在）
    const bool bAlreadyOnDisk = !sPathname.isEmpty();

    QString sFilePath = sPathname;
    if (bSaveAs || sFilePath.isEmpty()) {
        // 「另存为」永远弹框；普通「保存」只在还没有路径时弹框
        QString sDefault;
        if (bSaveAs && bAlreadyOnDisk) {
            sDefault = sPathname;
        } else {
            QString sDir = sDefaultPath;
            if (sDir.isEmpty()) {
                sDir = QDir::homePath();
            }
            sDefault = QDir(sDir).filePath(sRecommend + QStringLiteral(".md"));
        }

        sFilePath = QFileDialog::getSaveFileName(
            m_pShellWindow,
            bSaveAs ? QStringLiteral("另存为 Markdown 文件")
                    : QStringLiteral("保存 Markdown 文件"),
            sDefault,
            QStringLiteral("Markdown 文件 (*.md *.markdown);;所有文件 (*.*)"));
        if (sFilePath.isEmpty()) {
            // 用户取消：与上游一致，静默返回（不发失败通知）
            emit logMessage(QStringLiteral("info"), QStringLiteral("保存对话框被取消"));
            return;
        }
    }

    sFilePath = QDir::fromNativeSeparators(sFilePath);
    if (QFileInfo(sFilePath).suffix().isEmpty()) {
        sFilePath += QStringLiteral(".md");
    }

    QString sError;
    if (!writeMarkdownFileWithOptions(sFilePath, sMarkdown, objOptions, &sError)) {
        emit logMessage(QStringLiteral("error"),
                        QStringLiteral("保存失败 %1：%2").arg(sFilePath, sError));
        emitToRenderer(QStringLiteral("mt::tab-save-failure"),
                       QJsonArray{ sId, sError });
        return;
    }

    const QString sNativePath = QDir::toNativeSeparators(QFileInfo(sFilePath).absoluteFilePath());
    const QString sNewName = QFileInfo(sFilePath).fileName();

    if (!bAlreadyOnDisk || QDir::fromNativeSeparators(sPathname) != sFilePath) {
        // 新文件（或另存到新位置）：告诉渲染层换路径/文件名
        QJsonObject objPath;
        objPath.insert(QStringLiteral("id"), sId);
        objPath.insert(QStringLiteral("pathname"), sNativePath);
        objPath.insert(QStringLiteral("filename"), sNewName);
        emitToRenderer(QStringLiteral("mt::set-pathname"), QJsonArray{ objPath });
    } else {
        // 原路径覆盖保存：只回报"已保存"
        emitToRenderer(QStringLiteral("mt::tab-saved"), QJsonArray{ sId });
    }

    emit logMessage(QStringLiteral("info"),
                    QStringLiteral("已保存：%1").arg(sNativePath));
}

//=====================================================================
//  菜单命令路由（对照 main/menu/actions/*.ts + templates/*.ts）
//=====================================================================
namespace {

struct MenuRoute
{
    const char *pszMenuId;
    const char *pszChannel;
    const char *pszType;     ///< 载荷 type 字段（无则空串）
};

/// 段落菜单：菜单项 id -> mt::editor-paragraph-action 的 type
/// 逐条对照 templates/paragraph.ts 的 click 与 actions/paragraph.ts 的实现
const MenuRoute kParagraphRoutes[] = {
    { "heading1MenuItem",        "mt::editor-paragraph-action", "heading 1" },
    { "heading2MenuItem",        "mt::editor-paragraph-action", "heading 2" },
    { "heading3MenuItem",        "mt::editor-paragraph-action", "heading 3" },
    { "heading4MenuItem",        "mt::editor-paragraph-action", "heading 4" },
    { "heading5MenuItem",        "mt::editor-paragraph-action", "heading 5" },
    { "heading6MenuItem",        "mt::editor-paragraph-action", "heading 6" },
    { "upgradeHeadingMenuItem",  "mt::editor-paragraph-action", "upgrade heading" },
    { "degradeHeadingMenuItem",  "mt::editor-paragraph-action", "degrade heading" },
    { "tableMenuItem",           "mt::editor-paragraph-action", "table" },
    { "codeFencesMenuItem",      "mt::editor-paragraph-action", "pre" },
    { "quoteBlockMenuItem",      "mt::editor-paragraph-action", "blockquote" },
    { "mathBlockMenuItem",       "mt::editor-paragraph-action", "mathblock" },
    { "htmlBlockMenuItem",       "mt::editor-paragraph-action", "html" },
    { "orderListMenuItem",       "mt::editor-paragraph-action", "ol-order" },
    { "bulletListMenuItem",      "mt::editor-paragraph-action", "ul-bullet" },
    { "taskListMenuItem",        "mt::editor-paragraph-action", "ul-task" },
    { "looseListItemMenuItem",   "mt::editor-paragraph-action", "loose-list-item" },
    { "paragraphMenuItem",       "mt::editor-paragraph-action", "paragraph" },
    { "horizontalLineMenuItem",  "mt::editor-paragraph-action", "hr" },
    { "frontMatterMenuItem",     "mt::editor-paragraph-action", "front-matter" }
};

/// 格式菜单：菜单项 id -> mt::editor-format-action 的 type
const MenuRoute kFormatRoutes[] = {
    { "strongMenuItem",     "mt::editor-format-action", "strong" },
    { "emphasisMenuItem",   "mt::editor-format-action", "em" },
    { "underlineMenuItem",  "mt::editor-format-action", "u" },
    { "superscriptMenuItem","mt::editor-format-action", "sup" },
    { "subscriptMenuItem",  "mt::editor-format-action", "sub" },
    { "highlightMenuItem",  "mt::editor-format-action", "mark" },
    { "inlineCodeMenuItem", "mt::editor-format-action", "inline_code" },
    { "inlineMathMenuItem", "mt::editor-format-action", "inline_math" },
    { "strikeMenuItem",     "mt::editor-format-action", "del" },
    { "hyperlinkMenuItem",  "mt::editor-format-action", "link" },
    { "imageMenuItem",      "mt::editor-format-action", "image" },
    { "clearFormatMenuItem","mt::editor-format-action", "clear" }
};

/// 编辑菜单：菜单项 id -> mt::editor-edit-action 的 type
const MenuRoute kEditRoutes[] = {
    { "undoMenuItem",          "mt::editor-edit-action", "undo" },
    { "redoMenuItem",          "mt::editor-edit-action", "redo" },
    { "copyAsRichMenuItem",    "mt::editor-edit-action", "copyAsRich" },
    { "copyAsHtmlMenuItem",    "mt::editor-edit-action", "copyAsHtml" },
    { "pasteAsPlainTextMenuItem", "mt::editor-edit-action", "pasteAsPlainText" },
    { "selectAllMenuItem",     "mt::editor-edit-action", "selectAll" },
    { "duplicateMenuItem",     "mt::editor-edit-action", "duplicate" },
    { "createParagraphMenuItem","mt::editor-edit-action", "createParagraph" },
    { "deleteParagraphMenuItem","mt::editor-edit-action", "deleteParagraph" },
    { "findMenuItem",          "mt::editor-edit-action", "find" },
    { "findNextMenuItem",      "mt::editor-edit-action", "findNext" },
    { "findPreviousMenuItem",  "mt::editor-edit-action", "findPrev" },
    { "findInFolderMenuItem",  "mt::editor-edit-action", "findInFolder" },
    // 替换：渲染层命令表里 delete "edit.replace" 就是 emitter.emit("replace","replace")，
    // 而 EDITOR_EDIT_ACTION(type) = emitter.emit(type, type)，所以这里同一条通路即可。
    // （上游 edit.ts 的 editorReplace 误写成 edit(win,'undo')，此处按渲染层真实语义修正。）
    { "replaceMenuItem",       "mt::editor-edit-action", "replace" }
};

} // namespace

bool MarkTextBridge::routeMenuCommand(const QString &sMenuId)
{
    // 1a) 段落 / 格式两类：id -> { type: ... } 对象
    //     渲染层侧：on("mt::editor-format-action", (_e, { type }) => emitter.emit("format", type))
    //               on("mt::editor-paragraph-action", (_e, { type }) => emitter.emit("paragraph", type))
    //     解构取 type，故**必须是对象**。
    const struct { const MenuRoute *pArray; int nCount; } arrObjTables[] = {
        { kParagraphRoutes, int(sizeof(kParagraphRoutes) / sizeof(kParagraphRoutes[0])) },
        { kFormatRoutes,    int(sizeof(kFormatRoutes) / sizeof(kFormatRoutes[0])) }
    };
    for (const auto &table : arrObjTables) {
        for (int i = 0; i < table.nCount; ++i) {
            if (sMenuId == QLatin1String(table.pArray[i].pszMenuId)) {
                QJsonObject objPayload;
                objPayload.insert(QStringLiteral("type"),
                                  QLatin1String(table.pArray[i].pszType));
                emitToRenderer(QLatin1String(table.pArray[i].pszChannel),
                               QJsonArray{ objPayload });
                return true;
            }
        }
    }

    // 1b) 编辑类：id -> 裸字符串
    //     渲染层侧：on("mt::editor-edit-action", (_e, type) => EDITOR_EDIT_ACTION(type))
    //     直接取第二个参数当字符串用；若包成 { type: ... } 会变成 "[object Object]"，
    //     命令全部落空（实测现象：点了"撤销"毫无反应且无报错）。
    //     MarkText 主进程自己也是这么发的：webContents.send('mt::editor-edit-action', type)。
    for (int i = 0; i < int(sizeof(kEditRoutes) / sizeof(kEditRoutes[0])); ++i) {
        if (sMenuId != QLatin1String(kEditRoutes[i].pszMenuId)) {
            continue;
        }
        emitToRenderer(QLatin1String(kEditRoutes[i].pszChannel),
                       QJsonArray{ QLatin1String(kEditRoutes[i].pszType) });
        return true;
    }

    // 2) 视图类（对照 actions/view.ts）
    if (sMenuId == QLatin1String("sourceCodeModeMenuItem")) {
        emitToRenderer(QStringLiteral("mt::toggle-view-mode-entry"),
                       QJsonArray{ QStringLiteral("sourceCode") });
        return true;
    }
    if (sMenuId == QLatin1String("typewriterModeMenuItem")) {
        emitToRenderer(QStringLiteral("mt::toggle-view-mode-entry"),
                       QJsonArray{ QStringLiteral("typewriter") });
        return true;
    }
    if (sMenuId == QLatin1String("focusModeMenuItem")) {
        emitToRenderer(QStringLiteral("mt::toggle-view-mode-entry"),
                       QJsonArray{ QStringLiteral("focus") });
        return true;
    }
    if (sMenuId == QLatin1String("sideBarMenuItem")) {
        emitToRenderer(QStringLiteral("mt::toggle-view-layout-entry"),
                       QJsonArray{ QStringLiteral("showSideBar") });
        return true;
    }
    if (sMenuId == QLatin1String("tabBarMenuItem")) {
        emitToRenderer(QStringLiteral("mt::toggle-view-layout-entry"),
                       QJsonArray{ QStringLiteral("showTabBar") });
        return true;
    }
    if (sMenuId == QLatin1String("tocMenuItem")) {
        QJsonObject objLayout;
        objLayout.insert(QStringLiteral("rightColumn"), QStringLiteral("toc"));
        emitToRenderer(QStringLiteral("mt::set-view-layout"), QJsonArray{ objLayout });
        return true;
    }
    if (sMenuId == QLatin1String("commandPaletteMenuItem")) {
        emitToRenderer(QStringLiteral("mt::show-command-palette"));
        return true;
    }
    if (sMenuId == QLatin1String("reloadImagesMenuItem")) {
        emitToRenderer(QStringLiteral("mt::invalidate-image-cache"));
        return true;
    }

    // 3) 行结束符（对照 actions/edit.ts: lineEnding）
    if (sMenuId == QLatin1String("crlfLineEndingMenuEntry")) {
        emitToRenderer(QStringLiteral("mt::set-line-ending"), QJsonArray{ QStringLiteral("crlf") });
        return true;
    }
    if (sMenuId == QLatin1String("lfLineEndingMenuEntry")) {
        emitToRenderer(QStringLiteral("mt::set-line-ending"), QJsonArray{ QStringLiteral("lf") });
        return true;
    }

    // 4) 文件类（对照 actions/file.ts）
    if (sMenuId == QLatin1String("newFileMenuItem") || sMenuId == QLatin1String("newTabMenuItem")) {
        emitToRenderer(QStringLiteral("mt::new-untitled-tab"));
        return true;
    }
    if (sMenuId == QLatin1String("saveMenuItem")) {
        emitToRenderer(QStringLiteral("mt::editor-ask-file-save"));
        return true;
    }
    if (sMenuId == QLatin1String("saveAsMenuItem")) {
        emitToRenderer(QStringLiteral("mt::editor-ask-file-save-as"));
        return true;
    }
    if (sMenuId == QLatin1String("closeTabMenuItem")) {
        emitToRenderer(QStringLiteral("mt::editor-close-tab"));
        return true;
    }
    if (sMenuId == QLatin1String("moveToMenuItem")) {
        emitToRenderer(QStringLiteral("mt::editor-move-file"));
        return true;
    }
    if (sMenuId == QLatin1String("renameMenuItem")) {
        emitToRenderer(QStringLiteral("mt::editor-rename-file"));
        return true;
    }
    if (sMenuId == QLatin1String("exportHtmlMenuItem")) {
        exportHtml();
        return true;
    }
    if (sMenuId == QLatin1String("exportPdfMenuItem")) {
        exportPdf();
        return true;
    }
    if (sMenuId == QLatin1String("printMenuItem")) {
        print();
        return true;
    }

    // 5) 缩放（对照 actions/window.ts 的 zoomIn / zoomOut）
    //    注意官方走的是 mt::window-zoom 而**不是**本地改 preference：
    //    偏好只是记录，真正的缩放动作必须经渲染层 EDIT_ZOOM 才生效。
    if (sMenuId == QLatin1String("zoomInMenuItem")) {
        zoomBy(true);
        return true;
    }
    if (sMenuId == QLatin1String("zoomOutMenuItem")) {
        zoomBy(false);
        return true;
    }

    return false;
}

//=====================================================================
//  导出 / 打印 / 取内容
//
//  背景：WebView2EditorEngine::markdownText() 返回空串（内容由渲染层持有），
//        所以外壳侧的导出/打印/自动保存全都拿不到内容 —— 此前的实现是
//        空转。本节改为对齐官方机制：让渲染层把内容发回来，外壳再落盘。
//=====================================================================
void MarkTextBridge::exportHtml()
{
    emitToRenderer(QStringLiteral("mt::show-export-dialog"),
                   QJsonArray{ QStringLiteral("styledHtml") });
}

void MarkTextBridge::exportPdf()
{
    emitToRenderer(QStringLiteral("mt::show-export-dialog"),
                   QJsonArray{ QStringLiteral("pdf") });
}

void MarkTextBridge::print()
{
    // 官方 printDocument 直接发 mt::show-export-dialog "print"，
    // 渲染层 PRINT_RESPONSE() 回 mt::response-print（**不带内容**），
    // 主进程靠 webContents.print() 直接打页面。
    // WebView2 无 print() 等价 API，故收到该信号后取整页 HTML 再打印。
    emitToRenderer(QStringLiteral("mt::show-export-dialog"),
                   QJsonArray{ QStringLiteral("print") });
}

void MarkTextBridge::requestCurrentMarkdown()
{
    grabCurrentMarkdown([this](const QString &sMd) {
        if (sMd.isEmpty()) {
            emit logMessage(QStringLiteral("warn"),
                            QStringLiteral("自动保存：未能取到当前文档内容"));
        }
        emit markdownReady(sMd);
    });
}

void MarkTextBridge::onScriptResult(bool bSuccess, const QString &sResultJson)
{
    if (!bSuccess || m_fnScriptDone == nullptr) {
        if (m_fnScriptDone != nullptr) {
            m_fnScriptDone(QString());
            m_fnScriptDone = nullptr;
        }
        return;
    }
    // executeScript 的结果是 JSON：字符串值、null，或对象/数组
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(sResultJson.toUtf8(), &err);
    QString sValue;
    if (err.error == QJsonParseError::NoError) {
        if (doc.isNull()) {
            sValue.clear();
        } else if (doc.isObject() || doc.isArray()) {
            // 返回的是对象/数组（不是我们要的字符串）——原样交回让调用方判断
            sValue = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
        } else {
            // 字符串 / 数字 / 布尔：取其字面量
            const QJsonValue v = doc.object();
            sValue = v.toVariant().toString();
            if (sValue.isEmpty() && !doc.isObject()) {
                // QJsonDocument 顶层非对象时用 QJsonDocument::fromJson 的变体：
                // 直接把原始 JSON 字面量剥掉引号
                QString s = sResultJson.trimmed();
                if (s.size() >= 2 && s.startsWith(QLatin1Char('"'))
                        && s.endsWith(QLatin1Char('"'))) {
                    sValue = s.mid(1, s.size() - 2);
                }
            }
        }
    } else {
        // 不是合法 JSON（例如页面返回了裸文本）：按原文处理
        sValue = sResultJson;
    }

    std::function<void(const QString &)> fnDone = m_fnScriptDone;
    m_fnScriptDone = nullptr;
    if (fnDone) {
        fnDone(sValue);
    }
}

void MarkTextBridge::grabPageHtml(const std::function<void(const QString &)> &fnDone)
{
    if (m_pHost == nullptr) {
        if (fnDone) {
            fnDone(QString());
        }
        return;
    }
    m_fnScriptDone = fnDone;
    m_pHost->executeScript(QStringLiteral(
        "(function(){try{return document.documentElement.outerHTML || '';}"
        "catch(e){return '';}})()"));
}

void MarkTextBridge::grabCurrentMarkdown(const std::function<void(const QString &)> &fnDone)
{
    if (m_pHost == nullptr) {
        if (fnDone) {
            fnDone(QString());
        }
        return;
    }
    // 取编辑器当前内容。渲染层正文在 CodeMirror/Muya 的编辑器实例里，
    // 拿不到时退回 DOM 文本（内容仍可能不完整，故取不到就报"未能取到内容"，
    // 由调用方决定不落盘 —— 绝不能把空串写进文件）。
    m_fnScriptDone = fnDone;
    m_pHost->executeScript(QStringLiteral(
        "(function(){try{"
        "  var root = typeof window !== 'undefined' ? window : {};"
        "  // 渲染层把 Vuex store 挂在 window.marktext 上"
        "  if (root.marktext && root.marktext.store) {"
        "    var st = root.marktext.store.state;"
        "    if (st && st.editor && st.editor.currentFile"
        "        && typeof st.editor.currentFile.markdown === 'string') {"
        "      return st.editor.currentFile.markdown;"
        "    }"
        "  }"
        "  var el = document.querySelector('.ag-render-buffer')"
        "         || document.querySelector('#editor')"
        "         || document.querySelector('.CodeMirror');"
        "  return el ? (el.innerText || el.textContent || '') : '';"
        "}catch(e){return '';}})()"));
}

void MarkTextBridge::handleResponseForExport(const QJsonObject &objPayload)
{
    const QString sType     = objPayload.value(QStringLiteral("type")).toString();
    const QString sContent  = objPayload.value(QStringLiteral("content")).toString();
    const QString sTitle    = objPayload.value(QStringLiteral("title")).toString();
    const QString sPathname = objPayload.value(QStringLiteral("pathname")).toString();
    const QString sFilename = objPayload.value(QStringLiteral("filename")).toString();

    // ---- 诊断：每一次导出回执都记下来（PDF 排查用）----
    qInfo("[导出] 收到 mt::response-export: type=%s content=%d 字符 "
          "title=%s pathname=%s pageOptions=%s",
          qPrintable(sType), static_cast<int>(sContent.size()),
          qPrintable(sTitle), qPrintable(sPathname),
          qPrintable(QString::fromUtf8(QJsonDocument(
              objPayload.value(QStringLiteral("pageOptions")).toObject())
              .toJson(QJsonDocument::Compact))));

    const bool bPdf = (sType == QLatin1String("pdf"));

    // ⚠ content 为空的判断**必须区分类型**（r183 的 bug，这里曾无条件 return，
    //   导致 PDF 导出永远走不到后面的 CDP 分支）：
    //   · html 分支靠 content 落盘 → 空即失败；
    //   · pdf  分支**本来就没有 content** —— 官方 handleResponseForExport 里
    //     pdf 走 printToPDF()、只有 else 分支才检查 content，
    //     渲染层 EXPORT({type:"pdf"}) 也没传 content。PDF 的内容由
    //     Chromium 直接从当前页面抓，故此处不能拦。
    if (!bPdf && sContent.isEmpty()) {
        emit logMessage(QStringLiteral("warn"),
                        QStringLiteral("导出失败：渲染层未返回内容"));
        return;
    }

    // 默认文件名：正文标题 → 源文件名 → Untitled（对齐官方 handleResponseForExport）
    QString sNaked = sPathname.isEmpty()
        ? sTitle
        : QFileInfo(sPathname).completeBaseName();
    if (sNaked.isEmpty()) {
        sNaked = sFilename.isEmpty() ? QStringLiteral("Untitled")
                                     : QFileInfo(sFilename).completeBaseName();
    }
    sNaked = sanitizeFileName(sNaked.isEmpty() ? QStringLiteral("Untitled") : sNaked);

    const QString sExt = bPdf ? QStringLiteral(".pdf") : QStringLiteral(".html");

    QString sDir = sPathname.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        : QFileInfo(sPathname).absolutePath();
    if (sDir.isEmpty()) {
        sDir = QDir::homePath();
    }

    // 自测模式（PDF）：路径已指定，跳过对话框
    if (bPdf && !m_sPdfSelfTestPath.isEmpty()) {
        qInfo("[自测] PDF 走免对话框路径：%s", qPrintable(m_sPdfSelfTestPath));
        const QString sTarget3 = m_sPdfSelfTestPath;
        m_sPdfSelfTestPath.clear();
        exportPdfNative(objPayload.value(QStringLiteral("pageOptions")).toObject(),
                        sTarget3);
        return;
    }

    // 自测模式：路径已指定，跳过对话框
    if (!m_sSelfTestHtmlPath.isEmpty()) {
        const QString sType2 = sType;
        const QString sContent2 = sContent;
        const QString sTarget2 = m_sSelfTestHtmlPath;
        qInfo("[自测] 收到 mt::response-export type=%s content长度=%d",
              qPrintable(sType2), sContent2.size());
        QFile f2(sTarget2);
        if (!f2.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qInfo("[自测] 无法写入 %s", qPrintable(sTarget2));
        } else {
            f2.write(sContent2.toUtf8());
            f2.close();
            qInfo("[自测] 导出成功：%s（%d 字节）",
                  qPrintable(sTarget2), static_cast<int>(QFileInfo(sTarget2).size()));
        }
        m_sSelfTestHtmlPath.clear();
        return;
    }

    const QString sTarget = QFileDialog::getSaveFileName(
        m_pShellWindow,
        bPdf ? QStringLiteral("导出为 PDF") : QStringLiteral("导出为 HTML"),
        QDir(sDir).filePath(sNaked + sExt),
        bPdf ? QStringLiteral("PDF 文件 (*.pdf)")
             : QStringLiteral("HTML 文件 (*.html)"));
    if (sTarget.isEmpty()) {
        emit logMessage(QStringLiteral("info"), QStringLiteral("导出对话框被取消"));
        return;
    }

    if (bPdf) {
        // pdf 类型时渲染层**不传 content**（官方主进程也不看它，直接调
        // printToPDF）。所以走 CDP：请渲染层把它用 exportStyledHTML +
        // printer.renderMarkdown 渲染出的那一页留在页面上，
        // 再由 Chromium 内核 Page.printToPDF 抓取 —— 与官方同路径。
        qInfo("[导出] PDF 分支：目标=%s", qPrintable(sTarget));
        exportPdfNative(objPayload.value(QStringLiteral("pageOptions")).toObject(),
                         sTarget);
        return;   // 落盘由 onCdpPdfReady 异步完成
    } else {
        QFile file(sTarget);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit logMessage(QStringLiteral("warn"),
                            QStringLiteral("导出失败：无法写入 ") + sTarget);
            return;
        }
        file.write(sContent.toUtf8());
        file.close();
    }

    emit logMessage(QStringLiteral("info"), QStringLiteral("已导出：") + sTarget);
    // 回报渲染层，好让它弹「导出成功」并定位文件（对齐官方 mt::export-success）
    emitToRenderer(QStringLiteral("mt::export-success"),
                   QJsonArray{ QJsonObject{
                       { QStringLiteral("type"), sType },
                       { QStringLiteral("filePath"), sTarget } } });
}

void MarkTextBridge::exportHtmlForSelfTest(const QString &sPath)
{
    if (m_pHost == nullptr) {
        qInfo("[自测] 导出失败：宿主未就绪");
        return;
    }
    m_sSelfTestHtmlPath = sPath;
    qInfo("[自测] 已发 mt::show-export-dialog styledHtml -> %s",
          qPrintable(sPath));
    emitToRenderer(QStringLiteral("mt::show-export-dialog"),
                   QJsonArray{ QStringLiteral("styledHtml") });
    clickExportDialogLater();
}

void MarkTextBridge::printToPdfForSelfTest(const QString &sPath)
{
    // ★ 必须走 mt::show-export-dialog，与真实菜单一致 ★
    //   官方的 PDF 流程是：渲染层 exportStyledHTML() -> printer.renderMarkdown()
    //   把 .print-container 渲染进 DOM -> 主进程才 printToPDF。
    //   若这里直接调 CDP，就跳过了「渲染层准备打印页」这一步，
    //   CDP 抓到的会是普通编辑页而非打印页。
    //   故本自测只负责"记住目标路径 + 请求渲染层出内容"，
    //   真正落盘在 handleResponseForExport 里（跳过对话框）。
    m_sPdfSelfTestPath = sPath;
    qInfo("[自测] 请求渲染层出 PDF 内容（目标 %s）", qPrintable(sPath));
    emitToRenderer(QStringLiteral("mt::show-export-dialog"),
                   QJsonArray{ QStringLiteral("pdf") });
    clickExportDialogLater();
}

void MarkTextBridge::clickExportDialogLater()
{
    // 渲染层的导出流程要用户点一下设置对话框里的「导出」按钮才会继续。
    // 沙箱拦了 OS 级输入注入，但 executeScript 能在页面里跑 JS ——
    // 用 DOM 的 .click() 直接触发，绕开输入注入。
    QTimer::singleShot(2500, this, [this]() {
        if (m_pHost == nullptr) {
            qInfo("[自测] 无法点击导出按钮：宿主未就绪");
            return;
        }
        m_fnScriptDone = [this](const QString &sRet) {
            qInfo("[自测] 点击导出按钮结果：%s", qPrintable(sRet));
        };
        m_pHost->executeScript(QStringLiteral(
            "(function(){try{"
            "  var all = document.querySelectorAll("
            "    '.el-overlay button, .el-dialog button, button.button-primary');"
            "  for (var i=0;i<all.length;i++){"
            "    var b = all[i];"
            "    var t = (b.innerText || b.textContent || '').trim();"
            "    if (/导出|Export/i.test(t)) { b.click(); return 'clicked:' + t; }"
            "  }"
            "  return 'notfound:' + all.length;"
            "}catch(e){ return 'err:' + e.message; }})()"));
    });
}

void MarkTextBridge::pdfPageSizeInches(const QJsonObject &objPageOptions,
                                      double *pdWidth, double *pdHeight,
                                      bool *pbLandscape)
{
    // 对齐官方 main/ipc 的 getPdfPageOptions —— 但换成 WebView2 原生 API 的单位。
    // 官方那边给 Electron printToPDF 的 pageSize 是 {width,height} 微米，
    // 而 WebView2 的 ICoreWebView2PrintSettings::put_PageWidth/PageHeight
    // 单位是**英寸**，故这里统一换算成英寸。
    //
    // 渲染层的纸张下拉共七种（A3/A4/A5/Legal/Letter/Tabloid/custom），
    // 尺寸以**毫米**给出（默认 A4 = 210 × 297）。
    *pdWidth = 0.0;
    *pdHeight = 0.0;
    *pbLandscape = objPageOptions.value(QStringLiteral("isLandscape")).toBool();

    const QString sSize = objPageOptions.value(QStringLiteral("pageSize")).toString();
    if (sSize.compare(QLatin1String("A3"), Qt::CaseInsensitive) == 0) {
        *pdWidth = 11.69; *pdHeight = 16.54;
    } else if (sSize.compare(QLatin1String("A4"), Qt::CaseInsensitive) == 0) {
        *pdWidth = 8.27;  *pdHeight = 11.69;
    } else if (sSize.compare(QLatin1String("A5"), Qt::CaseInsensitive) == 0) {
        *pdWidth = 5.83;  *pdHeight = 8.27;
    } else if (sSize.compare(QLatin1String("Letter"), Qt::CaseInsensitive) == 0) {
        *pdWidth = 8.5;   *pdHeight = 11.0;
    } else if (sSize.compare(QLatin1String("Legal"), Qt::CaseInsensitive) == 0) {
        *pdWidth = 8.5;   *pdHeight = 14.0;
    } else if (sSize.compare(QLatin1String("Tabloid"), Qt::CaseInsensitive) == 0) {
        *pdWidth = 11.0;  *pdHeight = 17.0;
    } else {
        // custom：渲染层给毫米，转英寸
        const double dWmm = objPageOptions.value(QStringLiteral("pageSizeWidth")).toDouble(0.0);
        const double dHmm = objPageOptions.value(QStringLiteral("pageSizeHeight")).toDouble(0.0);
        if (dWmm > 0.0 && dHmm > 0.0) {
            *pdWidth = dWmm / 25.4;
            *pdHeight = dHmm / 25.4;
        }
    }
}

void MarkTextBridge::exportPdfNative(const QJsonObject &objPageOptions,
                                    const QString &sOutPath)
{
    if (m_pHost == nullptr) {
        emit logMessage(QStringLiteral("warn"),
                        QStringLiteral("导出 PDF 失败：渲染层尚未就绪"));
        return;
    }
    double dW = 0.0;
    double dH = 0.0;
    bool bLandscape = false;
    pdfPageSizeInches(objPageOptions, &dW, &dH, &bLandscape);

    qInfo("[导出] 调 WebView2 原生 PrintToPdf -> %s （%.2f x %.2f 英寸, %s）",
          qPrintable(sOutPath), dW, dH, bLandscape ? "横向" : "纵向");
    if (!m_pHost->printToPdf(sOutPath, dW, dH, bLandscape)) {
        qInfo("[导出] printToPdf 返回 false —— WebView2 运行时不支持或宿主未就绪");
    }
}

void MarkTextBridge::onPdfRendered(bool bOk, const QString &sOutPath)
{
    if (sOutPath.isEmpty()) {
        qInfo("[导出] printToPdf 回执无路径，忽略");
        return;
    }
    qInfo("[导出] PrintToPdf 回执: ok=%d path=%s", bOk ? 1 : 0, qPrintable(sOutPath));

    // 无论成败都要清理渲染层的打印容器 ——
    // printer.renderMarkdown() 把 .print-container 插进 document.body，
    // 其注释写明「容器之外的一切都会被 display:none 隐藏」。
    // 官方在 printToPDF 后发 mt::print-service-clearup 让渲染层 printer.clearup()。
    // 不清理的话，导出完编辑器会一直停在打印态（看起来是一片空白）。
    emitToRenderer(QStringLiteral("mt::print-service-clearup"), QJsonArray());

    if (!bOk || !QFileInfo::exists(sOutPath)) {
        qInfo("[导出] PDF 失败：WebView2 未生成文件");
        emit logMessage(QStringLiteral("warn"), QStringLiteral("导出 PDF 失败"));
        return;
    }

    qInfo("[导出] PDF 已生成：%s（%d 字节）",
          qPrintable(sOutPath), static_cast<int>(QFileInfo(sOutPath).size()));
    emit logMessage(QStringLiteral("info"), QStringLiteral("已导出：") + sOutPath);

    // 回报渲染层，让它弹「导出成功」并定位文件（对齐官方 mt::export-success）
    emitToRenderer(QStringLiteral("mt::export-success"),
                   QJsonArray{ QJsonObject{
                       { QStringLiteral("type"), QStringLiteral("pdf") },
                       { QStringLiteral("filePath"), sOutPath } } });
}

void MarkTextBridge::printViaNative()
{
    if (m_pHost == nullptr) {
        emit logMessage(QStringLiteral("warn"),
                        QStringLiteral("打印失败：渲染层尚未就绪"));
        return;
    }
    // 官方是 webContents.print()（弹系统打印对话框）。
    // WebView2 的等价物是 ICoreWebView2_16::ShowPrintUI（同样弹系统对话框），
    // 由 Chromium 内核直接出打印数据，样式与屏幕一致 ——
    // 不再需要 QPrinter 中转，也不会丢主题样式与 KaTeX 公式。
    if (!m_pHost->showPrintUI()) {
        emit logMessage(QStringLiteral("warn"),
                        QStringLiteral("打印失败：当前 WebView2 运行时不支持系统打印对话框"));
    }
}


double MarkTextBridge::zoomBy(bool bZoomIn)
{
    // 当前值取 preference.zoom（渲染层每次 EDIT_ZOOM 都会把它写回，
    // 所以这里读到的一定是"上次生效的缩放"，不是初始默认值）。
    double dZoom = m_pStore->value(QStringLiteral("zoom")).toDouble();
    if (dZoom <= 0.0) {
        dZoom = 1.0;
    }
    dZoom += (bZoomIn ? 0.125 : -0.125);
    dZoom = qBound(0.5, dZoom, 2.0);

    if (!isBridgeReady()) {
        // 页面还没起来就先记下偏好，等渲染层就绪后自己读表应用
        m_pStore->setValue(QStringLiteral("zoom"), dZoom);
        return dZoom;
    }

    // 载荷是**裸数字**（渲染层 EDIT_ZOOM 里直接 parseFloat + toFixed(3)，
    // 包成对象会变 NaN）。
    emitToRenderer(QStringLiteral("mt::window-zoom"), QJsonArray{ dZoom });
    // 本地同步一份，便于外壳侧显示与下次启动的初值
    m_pStore->setValue(QStringLiteral("zoom"), dZoom);
    qInfo("[缩放] %s -> %.3f", bZoomIn ? "放大" : "缩小", dZoom);
    return dZoom;
}
