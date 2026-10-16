/*
 * marktextbridge.h —— Qt 外壳 ⇄ MarkText 渲染层 的桥接层
 * ===========================================================================
 * 定位：**顶替 MarkText 的 Electron 主进程**。
 *
 * MarkText 原本的结构是：
 *     [Electron 主进程]  ──IPC──▶ [preload] ──window.electron.*──▶ [渲染层 Vue+Muya]
 * 本工程把左边整块换成 Qt：
 *     [Qt 外壳] ──marktextbridge──▶ [webui/env.js 垫片] ──window.electron.*──▶ [渲染层]
 *
 * 职责（对照 MarkText 主进程的模块）：
 *   - bootInfo 构造            ← 对应 main/ipc/bootInfo.ts
 *   - 偏好设置读写             ← 对应 main/preferences、main/dataCenter
 *   - i18n 语言包              ← 对应 main/ipc/i18n.ts
 *   - 文件系统 / 剪贴板 / shell ← 对应 main/ipc/fs.ts 与 main/ipc 目录下的全部通道
 *   - 键盘映射下发             ← 对应 main/app/index.ts: mt::request-keybindings
 *   - 启动时序（bootstrap-editor）← 对应 main/windows/editor.ts
 *   - 菜单点击 → 渲染层命令     ← 对应 main/menu/actions 目录下的全部 .ts
 *
 * 协议：见 webui/env.js 顶部注释（invoke / send / reply / emit / log）。
 */

#ifndef MARKTEXTBRIDGE_H
#define MARKTEXTBRIDGE_H

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>

class PreferencesStore;
class QWidget;
class WebView2Host;

class MarkTextBridge : public QObject
{
    Q_OBJECT

public:
    /// 往渲染层投递 JSON（由 WebView2 宿主注入）
    typedef std::function<void(const QString &sJson)> PostJsonFn;

    explicit MarkTextBridge(PreferencesStore *pStore, QObject *parent = nullptr);
    ~MarkTextBridge() override;

    /// 设置投递函数（引擎挂载完成后调用）
    void setPostJson(const PostJsonFn &fnPost);

    /// 静态资源根目录（含 renderer/ preload/ locales/ env.js）
    QString webUiDir() const;
    /// 起始页地址（虚拟主机）
    QString startUrl() const;
    /// 供注入页面的同步应答表（含 mt::boot-info）
    QString syncResponsesJson() const;

    /// 由引擎在「控制器就绪」后调用：映射虚拟主机 + 注入脚本 + 导航
    void prepareInjection(WebView2Host *pHost);

    /// 页面消息入口
    void onWebMessage(const QString &sJson);

    /// 页面加载完成 → 下发 mt::bootstrap-editor
    void sendBootstrapEditor();

    /// 外壳 → 渲染层
    void emitToRenderer(const QString &sChannel, const QJsonArray &args = QJsonArray());

    /// 菜单项 id → 渲染层命令；返回 true 表示已转交渲染层
    bool routeMenuCommand(const QString &sMenuId);

    /// 缩放编辑区（对照官方 main/menu/actions/window.ts 的 zoomIn / zoomOut）。
    ///
    /// 官方做法是读当前 zoomFactor ±0.125 后发 `mt::window-zoom <数字>`，
    /// 渲染层 LISTEN_WINDOW_ZOOM 收到即 EDIT_ZOOM()：
    /// 既改页面 zoom，又把新值写回 preference.zoom。
    /// bZoomIn=true 放大，false 缩小。返回实际生效的缩放值（未就绪时返回 0）。
    double zoomBy(bool bZoomIn);

    //-----------------------------------------------------------------
    //  导出 / 打印（对照官方 main/menu/actions/file.ts 与主进程
    //  handleResponseForExport / handleResponseForPrint）
    //
    //  机制与「保存」同构：渲染层先把内容发回来（mt::response-export /
    //  mt::response-print），外壳再弹框落盘。
    //  官方主进程用 webContents.printToPDF / print() 出 PDF 与打印，
    //  WebView2 无等价 API，故改用「渲染层交回带样式的 HTML →
    //  外壳用 QTextDocument 渲染成 PDF / 送打印机」。
    //-----------------------------------------------------------------
    /// 导出为 HTML（type = "styledHtml"）
    //-----------------------------------------------------------------
    //  免对话框版本（仅供环境变量触发的自测用，见 MainWindow 里的自测钩子）
    //-----------------------------------------------------------------
    /// 让渲染层出导出内容，结果直接落 sPath（不弹保存对话框）
    void exportHtmlForSelfTest(const QString &sPath);
    /// 让渲染层出打印内容并走 CDP 出 PDF，直接落 sPath（不弹任何对话框）
    void printToPdfForSelfTest(const QString &sPath);

    /// 自测用：延时用 JS 点掉渲染层导出对话框里的「导出」按钮
    ///（executeScript 走页面内 JS，不受沙箱的输入注入限制）
    void clickExportDialogLater();

    void exportHtml();

    /// 开关「段落工具面板」——段落前那个按钮点开后显示的面板。
    /// 由「视图 → 段落工具面板」菜单调用；补丁在渲染层里惰性加载，故可实时开关。
    /// 渲染层未就绪时返回 false。
    bool setFrontMenuPanelEnabled(bool bOn);
    /// 导出为 PDF（type = "pdf"）
    void exportPdf();
    /// 打印（type = "print"）
    void print();

    /// 向渲染层要当前文档的 markdown 原文（供自动保存落盘）。
    /// 渲染层回传见 markdownReady 信号。
    void requestCurrentMarkdown();

    /// 记住宿主：executeScript 要用它，且要接 executeScriptFinished 信号
    void setHost(WebView2Host *pHost) { m_pHost = pHost; }

    /// 把「菜单项 -> 快捷键」灌进来（供渲染层命令面板显示）
    void setKeybindings(const QList<QPair<QString, QString> > &listKeybindings);

    /// 外壳主动打开一个文件（走渲染层开新标签）
    void openFileInRenderer(const QString &sPath);

    //-----------------------------------------------------------------
    //  会话恢复（对照 Electron main/ipc/state.ts 的 buffer state 机制）
    //-----------------------------------------------------------------
    //  原版 MarkText 的会话恢复走"渲染层自述 + 主进程落盘"：
    //    保存：渲染层每次改动 debounce 1s 后 invoke("update-buffer-state", 快照)
    //          —— 快照含 editor(tabs) / project / layout 三段；
    //    恢复：启动时主进程把快照 emit("mt::load-state", 快照)
    //          —— 渲染层 RESTORE_BUFFERED_STATE() 依此重建标签页与项目树。
    //  本工程此前两个通道都没接：保存侧落到 fallback 丢了，恢复侧从不触发，
    //  所以关掉再开永远回到空白页。这里把两段补齐。
    //
    //  与"最近文件列表"（ui/recentlyUsedFiles）的分工：
    //    最近文件只是菜单里的一串路径，不承载会话（不恢复顺序/光标/项目树）；
    //    会话快照才是"上次退出时开着哪些页"的真身，两者互不替代。
    //
    /// 渲染层上报会话快照（update-buffer-state）后由本层落盘。
    /// 会先剔除未落盘的标签页（pathname 为空 / isSaved=false）：
    /// 用户口径是"关闭时没有打开的文件就什么都不恢复"，
    /// 纯 untitled 的会话没有恢复价值（内容本就留不住）。
    void storeBufferedState(const QJsonObject &objState);

    /// 把已落盘的会话快照下发给渲染层（emit mt::load-state）。
    /// 无有效快照（文件不存在 / 已被清空）时返回 false，调用方据此
    /// 走"空白首页 + 恢复上次目录"的老路。
    bool restoreBufferedState();

    /// 报告当前会话是否记录过至少一个已落盘的文件（供外壳判断是否要
    /// 额外补发 mt::open-directory）。
    bool hasRestorableSession() const;

    /// 渲染层是否已初始化完成
    bool isRendererReady() const { return m_bRendererReady; }
    /// 页面是否已就绪（env.js 已握手）
    bool isBridgeReady() const { return m_bBridgeReady; }

    /// 外壳窗口（用于窗口类命令）；由外壳注入
    void setShellWindow(QWidget *pWindow);

signals:
    /// 渲染层回传的当前文档 markdown（供自动保存落盘）
    void markdownReady(const QString &sMarkdown);

    /// 渲染层初始化完成（收到 mt::window-initialized）
    void rendererReady();
    /// 会话快照已成功下发给渲染层（用于外壳判断"这次启动不是空白页"，
    /// 从而跳过 mt::open-directory 的自动补发，避免抢掉会话里的项目树）
    void sessionRestored();
    /// 渲染层完成退出前握手（收到 mt::close-window / mt::close-window-confirm）：
    /// 表示它已把最新会话快照刷出，外壳可以放行关闭了。
    void closeWindowRequested();
    /// 文档标题变化
    void titleChanged(const QString &sTitle);
    /// 渲染层请求开外部链接
    void openExternalRequested(const QString &sUrl);
    /// 渲染层发来的通知（外壳可转成托盘气泡/状态栏）
    void notificationRequested(const QString &sType, const QString &sTitle, const QString &sMessage);
    /// 渲染层诊断日志
    void logMessage(const QString &sLevel, const QString &sText);
    /// 渲染层请求外壳做的事（如开文件对话框）
    void shellActionRequested(const QString &sAction);
    /// 窗口类命令（minimize/maximize/close/toggle-fullscreen/zoom-in/zoom-out/always-on-top）
    void windowActionRequested(const QString &sAction);
    /// 偏好设置被渲染层改动
    void preferenceChangedByRenderer(const QString &sKey, const QJsonValue &value);
    /// 渲染层的布局状态变化（mt::view-layout-changed：侧边栏 / 标签栏的显示与否）
    /// —— 渲染层是自绘布局，"切换侧边栏/标签栏"由它自己改，外壳据此同步菜单勾选。
    void layoutChanged(bool bShowSideBar, bool bShowTabBar);

    //-----------------------------------------------------------------
    //  菜单状态回传（对齐 Electron 主进程 UpdateApplicationMenu）
    //
    //  官方在渲染层"选区/格式变化"时把菜单的勾选与可用态算好发回来，
    //  主进程据此刷新 native menu。缺了这一段，Qt 菜单就会一直停在
    //  "什么都没选中"的状态：光标已经在一段粗体里，菜单里的「粗体」
    //  仍然没打勾。
    //-----------------------------------------------------------------
    /// 光标/选区状态（mt::editor-selection-changed）。
    /// 载荷即官方 createApplicationMenuState(changes) 的原样结构。
    void selectionStateChanged(const QJsonObject &objState);
    /// 行内格式状态（mt::update-format-menu）：{"<格式type>": true, ...}
    void formatMenuStateChanged(const QJsonObject &objFormats);
    /// 行结束符状态（mt::update-line-ending-menu）："crlf" / "lf"
    void lineEndingMenuStateChanged(const QString &sLineEnding);

private:
    /// WebView2 executeScript 的回执（结果为 JSON 字符串）——
    /// 命中 m_fnScriptDone 里暂存的回调
    void onScriptResult(bool bSuccess, const QString &sResultJson);

    void registerInvokeHandlers();
    void registerSendHandlers();
    void handleInvoke(const QJsonObject &objMsg);
    void handleSend(const QJsonObject &objMsg);
    void sendReply(int nId, bool bOk, const QJsonValue &value, const QString &sError = QString());
    void sendReplyRaw(const QString &sJson);

    QJsonObject bootInfo() const;
    /// 给渲染层的偏好表：内容与 store 全量一致，但 `theme` 换成"实际生效的主题"。
    /// 渲染层拿到 followSystemTheme 并不会自己解析（原版这一步在 Electron 主进程
    /// 用 nativeTheme 完成），故下发前必须先解析，否则跟随系统时会用错主题。
    QJsonObject preferenceObjectForRenderer() const;
    QString readLocaleJson(const QString &sLanguage) const;
    /// write-file / output-file 的公共落盘实现（数据可能是字符串或二进制标记）
    bool writeFileFromArgs(const QJsonArray &arrArgs);

    /// 渲染层 mt::response-export 的回执处理：弹框 + 落盘
    void handleResponseForExport(const QJsonObject &objPayload);
    /// 取当前页面整页 HTML（用于打印；官方靠 webContents.print()，WebView2 无等价 API）
    void grabPageHtml(const std::function<void(const QString &)> &fnDone);
    /// 取当前文档的 markdown 原文（自动保存用）
    void grabCurrentMarkdown(const std::function<void(const QString &)> &fnDone);
    /// 用 Chromium CDP 的 Page.printToPDF 出PDF（等价官方 webContents.printToPDF）
    ///
    /// 为什么不用 QTextDocument：那个是 Qt 自己的排版引擎，只认极有限的 CSS，
    /// 复杂样式（主题变量、KaTeX、代码高亮）会被它丢掉，出不了官方那种 PDF。
    /// 官方那条路是 webContents.printToPDF —— **由 Chromium 内核渲染**，
    /// 故样式保真。WebView2 没有同名 API，但 ICoreWebView2_3 提供
    /// CallDevToolsProtocolMethod（= Chromium DevTools Protocol 原生入口），
    /// Page.printToPDF 正是其方法之一，故与官方同路径、同保真度。
    ///
    /// sParamsJson 为 Page.printToPDF 参数对象（单位英寸）
    void exportPdfNative(const QJsonObject &objPageOptions, const QString &sOutPath);

    /// CDP 出 PDF 的回调包装（内部把 base64 解成字节写文件）
    void onPdfRendered(bool bOk, const QString &sOutPath);

    /// 打印：用 CDP 出 PDF 字节流，再交给 QPrinter 送打印机
    ///（不用 QTextDocument，同样是为了样式保真）
    void printViaNative();

    /// 从渲染层的 pageOptions 构造 Page.printToPDF 参数（对齐官方 getPdfPageOptions）
    void pdfPageSizeInches(const QJsonObject &objPageOptions,
                           double *pdWidth, double *pdHeight, bool *pbLandscape);

    /// 渲染层 mt::response-file-save / -save-as：把渲染层发回的 markdown 落盘，
    /// 并按"新文件 / 覆盖保存"回 mt::set-pathname 或 mt::tab-saved。
    /// 参数顺序：id, filename, pathname, markdown, options, defaultPath
    void handleFileSaveRequest(const QJsonArray &arrArgs, bool bSaveAs);

    /// 未实现通道的统一兜底：记日志并按命名给出安全默认值
    QJsonValue fallbackInvoke(const QString &sChannel, const QJsonArray &arrArgs);

    /// 会话快照落盘位置：ini 的 [session] 节（值为单行 JSON 串，
    /// 用 ini 而非独立文件是为了跟偏好/最近文件一样"退出即 sync"）。
    static QString sessionIniValueKey();
    /// 从 ini 读回快照；无效（缺失/空串/JSON 坏/无有效页）时返回空对象
    QJsonObject loadSessionFromIni() const;

    PreferencesStore *m_pStore;

    QWidget *m_pShellWindow;

    PostJsonFn m_fnPost;
    QString    m_sWebUiDir;

    /// WebView2 宿主：executeScript 取页面内容要用；
    /// 其executeScriptFinished 信号由 setHost() 里接到 onScriptResult()。
    WebView2Host *m_pHost = nullptr;

    /// executeScript 的异步回调（同一时刻只会有一个在等）
    std::function<void(const QString &)> m_fnScriptDone;

    /// PDF 导出的目标路径（CDP 出PDF 是异步的，回执里要用它决定写哪）
    /// 自测用的 HTML 落盘路径（走 mt::response-export 回执，区别于正常导出）
    QString m_sSelfTestHtmlPath;
    /// 自测用的 PDF 落盘路径（同上：收到 mt::response-export 后跳过对话框）
    QString m_sPdfSelfTestPath;

    bool m_bBridgeReady;
    bool m_bRendererReady;
    bool m_bBootstrapSent;

    /// bootstrap-editor 里下发的 addBlankTab 值。
    /// 为 false（有会话待恢复）而会话又没恢复成时，需要补开空白编辑页。
    bool m_bBlankTabByBootstrap = true;

    /// 渲染层就绪前请求打开的文件（命令行参数/拖放常见），
    /// 等 mt::window-initialized 后依序补发
    QStringList m_listPendingFiles;

    QHash<QString, std::function<QJsonValue(const QJsonArray &)> > m_hashInvoke;
    QHash<QString, std::function<void(const QJsonArray &)> >        m_hashSend;

    QList<QPair<QString, QString> > m_listKeybindings;
};

#endif // MARKTEXTBRIDGE_H
