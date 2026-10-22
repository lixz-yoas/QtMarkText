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
#include <QStandardPaths>
#include <QStringList>
#include <QTextCodec>
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
    query.addQueryItem(QStringLiteral("tbs"), prefStr("titleBarStyle", "custom"));

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

    // ④ （可选）页面内编辑链路自检：设 QTMT_SELFTEST=1 才注入
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
                              emit rendererReady();
                              // 补发就绪前排队待打开的文件
                              const QStringList listFiles = m_listPendingFiles;
                              m_listPendingFiles.clear();
                              for (const QString &sFile : listFiles) {
                                  emit logMessage(QStringLiteral("info"),
                                                  QStringLiteral("补发待打开文件：%1").arg(sFile));
                                  openFileInRenderer(sFile);
                              }
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

    // ---- 布局状态（渲染层自绘侧边栏/标签栏）----
    // 渲染层 SET_LAYOUT/TOGGLE_LAYOUT_ENTRY 后会 DISPATCH_LAYOUT_MENU_ITEMS()：
    //   send("mt::view-layout-changed", windowId, { showTabBar, showSideBar })
    // 外壳收到后同步「视图」菜单的勾选状态，否则点一次菜单项，界面变了、勾却不动。
    m_hashSend.insert(QStringLiteral("mt::view-layout-changed"), [this](const QJsonArray &arrArgs) {
        const QJsonObject obj = arrArgs.at(1).toObject();
        emit layoutChanged(obj.value(QStringLiteral("showSideBar")).toBool(),
                           obj.value(QStringLiteral("showTabBar")).toBool());
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

    // ---- 命令类（qt 菜单已直接路由，这里兜底）----
    const char *const arrCmdChannels[] = {
        "mt::cmd-open-file", "mt::cmd-open-folder", "mt::cmd-new-editor-window",
        "mt::cmd-import-file", "mt::cmd-close-window", "mt::cmd-toggle-autosave",
        "mt::close-window", "mt::close-window-confirm", "mt::app-try-quit",
        "mt::open-file-by-window-id", "mt::make-screenshot", "mt::check-for-update",
        "mt::NEED_UPDATE", "mt::handle-renderer-error", "mt::format-link-click",
        "mt::save-tabs", "mt::window-tab-closed", "mt::view-layout-changed",
        "mt::set-view-layout", "mt::show-command-palette", "mt::show-export-dialog",
        "mt::print-service-clearup", "mt::pandoc-not-exists", "mt::invalidate-image-cache",
        "mt::screenshot-captured", "mt::menu::closed", "mt::menu::popup",
        "mt::menu::popup-application", "mt::window::drop", "mt::window-active-status",
        "mt::editor-selection-changed", "mt::editor-format-action", "mt::editor-paragraph-action",
        "mt::editor-edit-action", "mt::editor-ask-file-save", "mt::editor-ask-file-save-as",
        "mt::editor-close-tab", "mt::editor-move-file", "mt::editor-rename-file",
        "mt::spellchecker-set-enabled", "mt::spellchecker-switch-language",
        "mt::spellchecker-remove-word", "mt::spellchecker-get-custom-dictionary-words",
        "mt::spellchecker-get-available-dictionaries", "mt::keybinding-debug-dump-keyboard-info",
        "mt::ask-for-open-project-in-sidebar", "mt::ask-for-modify-image-folder-path",
        "mt::ask-for-image-path", "mt::ask-for-image-auto-path", "mt::update-file",
        "mt::update-sidebar-menu", "mt::update-object-tree", "mt::update-format-menu",
        "mt::update-line-ending-menu", "mt::set-file-encoding", "mt::set-final-newline",
        "mt::tab-saved", "mt::tab-save-failure", "mt::export-success", "mt::response-export",
        "mt::response-print", "mt::response-file-save", "mt::response-file-save-as",
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
    objConfig.insert(QStringLiteral("addBlankTab"), true);
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
        emitToRenderer(QStringLiteral("mt::show-export-dialog"), QJsonArray{ QStringLiteral("styledHtml") });
        return true;
    }
    if (sMenuId == QLatin1String("exportPdfMenuItem")) {
        emitToRenderer(QStringLiteral("mt::show-export-dialog"), QJsonArray{ QStringLiteral("pdf") });
        return true;
    }
    if (sMenuId == QLatin1String("printMenuItem")) {
        emitToRenderer(QStringLiteral("mt::show-export-dialog"), QJsonArray{ QStringLiteral("print") });
        return true;
    }

    return false;
}
