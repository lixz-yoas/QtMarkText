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

    /// 把「菜单项 -> 快捷键」灌进来（供渲染层命令面板显示）
    void setKeybindings(const QList<QPair<QString, QString> > &listKeybindings);

    /// 外壳主动打开一个文件（走渲染层开新标签）
    void openFileInRenderer(const QString &sPath);

    /// 渲染层是否已初始化完成
    bool isRendererReady() const { return m_bRendererReady; }
    /// 页面是否已就绪（env.js 已握手）
    bool isBridgeReady() const { return m_bBridgeReady; }

    /// 外壳窗口（用于窗口类命令）；由外壳注入
    void setShellWindow(QWidget *pWindow);

signals:
    /// 渲染层初始化完成（收到 mt::window-initialized）
    void rendererReady();
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

private:
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
    /// 渲染层 mt::response-file-save / -save-as：把渲染层发回的 markdown 落盘，
    /// 并按"新文件 / 覆盖保存"回 mt::set-pathname 或 mt::tab-saved。
    /// 参数顺序：id, filename, pathname, markdown, options, defaultPath
    void handleFileSaveRequest(const QJsonArray &arrArgs, bool bSaveAs);

    /// 未实现通道的统一兜底：记日志并按命名给出安全默认值
    QJsonValue fallbackInvoke(const QString &sChannel, const QJsonArray &arrArgs);

    PreferencesStore *m_pStore;

    QWidget *m_pShellWindow;

    PostJsonFn m_fnPost;
    QString    m_sWebUiDir;

    bool m_bBridgeReady;
    bool m_bRendererReady;
    bool m_bBootstrapSent;

    /// 渲染层就绪前请求打开的文件（命令行参数/拖放常见），
    /// 等 mt::window-initialized 后依序补发
    QStringList m_listPendingFiles;

    QHash<QString, std::function<QJsonValue(const QJsonArray &)> > m_hashInvoke;
    QHash<QString, std::function<void(const QJsonArray &)> >        m_hashSend;

    QList<QPair<QString, QString> > m_listKeybindings;
};

#endif // MARKTEXTBRIDGE_H
