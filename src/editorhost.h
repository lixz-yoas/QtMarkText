#ifndef EDITORHOST_H
#define EDITORHOST_H

#include <QColor>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>

class QWidget;
class MarkTextBridge;
class WebView2Host;

/**
 * @brief 编辑器引擎接口 —— 外壳与 MarkText 渲染层之间的「挂载点」
 *
 * 为什么要有这一层：
 *   本轮（外壳层改造）明确「MarkText 内部实现尽量不变，只换外面的壳」。于是把
 *   渲染层抽象成一个接口，外壳只认接口，不认实现：
 *
 *     - PlaceholderEditorEngine —— 第 1/2 阶段的纯 Qt 编辑器（MarkdownEditor）
 *       实现本接口，作为当前占位引擎，保证工程随时可编译、可运行；
 *     - WebView2EditorEngine   —— 后续接入 MarkText 真正的渲染层
 *       （从已装成品 app.asar 里抽出的 out/renderer）时，只需新增一个实现类，
 *       MainWindow 一行都不用改。
 *
 * 接口是按 MarkText 渲染层「真实需要什么」裁剪的，不是凭空设计：
 *   MarkText 的渲染层通过 preload 暴露的 window.electron.* 向主进程要这些东西
 *   （见 packages/desktop/src/renderer/src/commands/index.ts）：
 *     - 文件读写（打开/保存/另存/导出）
 *     - 命令下发（mt::cmd-* ，如 mt::cmd-open-file / mt::cmd-toggle-autosave）
 *     - 偏好设置双向同步（mt::set-user-preference / broadcast-preferences-changed）
 *   对应到本接口就是 loadFromFile/saveToFile、execCommand、setPreference(All)。
 */
class IEditorEngine : public QObject
{
    Q_OBJECT

public:
    explicit IEditorEngine(QObject *pParent = nullptr);
    ~IEditorEngine() override;

    /// 引擎名（用于状态栏 / 关于对话框）
    virtual QString engineName() const = 0;
    /// 该文档的可视控件（将由外壳塞进 QTabWidget）
    virtual QWidget *view() const = 0;

    /// 当前文档路径（空 = 未命名）
    QString filePath() const;
    /// 是否已修改
    bool isModified() const;
    /// 显示名（未命名时返回"未命名.md"）
    QString displayName() const;

    // ---- 文本进出 ----
    /// 载入 Markdown 文本
    virtual void setMarkdownText(const QString &sText) = 0;
    /// 取回 Markdown 文本
    virtual QString markdownText() const = 0;
    /// 导出为 HTML（占位引擎给基础版；渲染层给带样式的完整版；返回空则外壳兜底）
    virtual QString exportHtml() const;

    // ---- 文件读写（外壳只做对话框，落盘交给引擎，便于渲染层接管编码/行尾处理）----
    virtual bool loadFromFile(const QString &sPath);
    virtual bool saveToFile(const QString &sPath);

    /// 在引擎里打开一个文件（渲染层引擎走 mt::open-new-tab 开新标签）。
    /// 默认实现等价于 loadFromFile（占位引擎）；渲染层引擎覆写。
    virtual void openFile(const QString &sPath);

    // ---- 命令下发 ----
    /// 下发一条 MarkText 命令，id 与菜单项 id 同源（如 "paragraph.codeFencesMenuItem"）；
    /// 返回 true 表示引擎认得并处理了，false 表示外壳应自行兜底或忽略。
    virtual bool execCommand(const QString &sCommandId) = 0;

    // ---- 偏好设置同步 ----
    /// 同步单项偏好（键名 = MarkText preference.json 的键名）
    virtual void setPreference(const QString &sKey, const QVariant &valueValue) = 0;
    /// 同步全量偏好（引擎启动时调用一次）
    virtual void setPreferenceAll(const QVariantMap &mapPreferences) = 0;

    void setFilePath(const QString &sPath);
    void setModified(bool bModified);

    /// 若该引擎背后有 MarkText 桥接层，返回它；否则返回 nullptr。
    /// 外壳据此接上「菜单 → 渲染层命令」「窗口控制」「偏好改动」等通路。
    virtual MarkTextBridge *bridge() const;

signals:
    /// 标题/修改状态变化，外壳据此刷新标签页与状态栏
    void titleChanged(const QString &sTitle);
    void modifiedChanged(bool bModified);

protected:
    QString m_sFilePath;    ///< 当前文档路径
    bool    m_bModified;    ///< 修改标志
};

//=====================================================================
//  占位引擎：包装第 1/2 阶段的纯 Qt MarkdownEditor
//=====================================================================
class MarkdownEditor;

class PlaceholderEditorEngine : public IEditorEngine
{
    Q_OBJECT

public:
    explicit PlaceholderEditorEngine(QObject *pOwner = nullptr, QWidget *pViewParent = nullptr);
    ~PlaceholderEditorEngine() override;

    QString engineName() const override;
    QWidget *view() const override;

    void setMarkdownText(const QString &sText) override;
    QString markdownText() const override;
    QString exportHtml() const override;
    bool execCommand(const QString &sCommandId) override;
    void setPreference(const QString &sKey, const QVariant &valueValue) override;
    void setPreferenceAll(const QVariantMap &mapPreferences) override;

private:
    MarkdownEditor *m_pEditor;
};

//=====================================================================
//  WebView2 引擎：把 MarkText 官方渲染层嵌进 Qt 外壳
//  链路：WebView2EditorEngine
//          └ WebView2Host       （WebView2 运行时，见 webview2host.h）
//          └ MarkTextBridge     （顶替 Electron 主进程，见 marktextbridge.h）
//          └ webui/             （从成品 app.asar 抽出的渲染层 + 原版 preload + env.js 垫片）
//=====================================================================
class WebView2EditorEngine : public IEditorEngine
{
    Q_OBJECT

public:
    explicit WebView2EditorEngine(QObject *pOwner = nullptr, QWidget *pViewParent = nullptr);
    ~WebView2EditorEngine() override;

    QString engineName() const override;
    QWidget *view() const override;

    void setMarkdownText(const QString &sText) override;
    QString markdownText() const override;
    QString exportHtml() const override;
    bool execCommand(const QString &sCommandId) override;
    void setPreference(const QString &sKey, const QVariant &valueValue) override;
    void setPreferenceAll(const QVariantMap &mapPreferences) override;

    MarkTextBridge *bridge() const override;

    void openFile(const QString &sPath) override;
    /// 渲染层是否就绪
    bool isRendererReady() const;

    /// 设置宿主底色：view 控件 palette + WebView2 默认背景色一起换，
    /// 保证加载间隙/未覆盖区域与当前主题的编辑器背景一致（消除闪白与色差边）。
    void setViewBackground(const QColor &color);

signals:
    /// WebView2 内按下了 Alt（对齐 MarkText「按 Alt 呼出菜单栏」）
    void altKeyPressed();

protected:
    bool eventFilter(QObject *pWatched, QEvent *pEvent) override;

private:
    void ensureAttached();
    void syncBounds();
    /// WebView2 挂载/运行失败时，在编辑区铺一层可见的诊断信息（而非无声黑屏）
    void showFailureNotice(const QString &sText);

    QWidget      *m_pView;
    WebView2Host *m_pHost;
    MarkTextBridge *m_pBridge;
    bool          m_bAttachRequested;
};

//=====================================================================
//  引擎工厂
//=====================================================================
class EditorHost
{
public:
    enum EngineKind
    {
        EnginePlaceholder = 0,   ///< 纯 Qt 编辑器（当前可用）
        EngineWebView2    = 1    ///< MarkText 渲染层（待接入）
    };

    /// 本机是否具备 WebView2 运行时（只查运行时，不查 SDK）
    static bool isWebView2RuntimeInstalled();

    /// 本机默认应使用的引擎
    static EngineKind availableEngine();

    /// 用户显式指定的引擎（环境变量 QTMT_ENGINE=webview2 可切到挂载点占位）
    static EngineKind requestedEngine();

    static QString engineTitle(EngineKind kind);
    static QString engineDescription(EngineKind kind);

    /// 造一个引擎实例（生命周期由 pOwner 管理）
    static IEditorEngine *create(EngineKind kind, QObject *pOwner, QWidget *pViewParent);
};

#endif // EDITORHOST_H
