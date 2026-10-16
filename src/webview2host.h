/*
 * webview2host.h —— WebView2 运行时宿主封装
 * ===========================================================================
 * 职责：把 WebView2 运行时（Edge WebView2 Runtime）封成一个可嵌进 Qt 控件的浏览器，
 *       对上只暴露「导航 / 注入脚本 / 收发 JSON 消息 / 尺寸跟随」这几件事。
 *
 * 关键工程决策：
 *   1) **动态加载 WebView2Loader.dll**（LoadLibrary + GetProcAddress），不做链接期依赖。
 *      好处：QtMarkText.exe 在不装 WebView2 的机器上依旧能启动（只是退回占位引擎），
 *      也省掉了为 MinGW 维护导入库的长期负担。
 *   2) 直接用 SDK 官方头 build/native/include/WebView2.h（MIDL 生成、纯 C/C++、不含 WRL），
 *      MinGW-w64 可直接编译；IID 由该头自带（EXTERN_GUID）。
 *      唯一补丁是同目录下的 EventToken.h（Windows SDK 侧文件，MinGW 未提供）。
 *   3) 所有 COM 回调都在 UI 线程（COINIT_APARTMENTTHREADED）触发，与 Qt 同线程，
 *      故本类内部不做额外加锁。
 *
 * 依赖：third_party/webview2/{include,bin}
 */

#ifndef WEBVIEW2HOST_H
#define WEBVIEW2HOST_H

#include <QObject>
#include <QColor>
#include <QString>

class QWidget;

class WebView2Host : public QObject
{
    Q_OBJECT

public:
    explicit WebView2Host(QObject *parent = nullptr);
    ~WebView2Host() override;

    /// 探测本机是否具备运行条件（Loader 可加载 + 已安装 WebView2 运行时）
    static bool runtimeAvailable(QString *pszVersion = nullptr);

    /// 把 WebView2 挂到某个 Qt 控件上（内部取其 winId 作为父窗口）
    bool attach(QWidget *pOwner, const QString &sUserDataDir);

    bool isEnvironmentReady() const;
    bool isControllerReady() const;

    // ---- 布局跟随 ----
    void setBounds(int x, int y, int nWidth, int nHeight);
    void setVisible(bool bVisible);
    void notifyParentMoved();
    double zoomFactor() const;
    void   setZoomFactor(double dFactor);

    // ---- 底色 ----
    /// 设置 WebView2 默认背景色（控制器未就绪时先存值，就绪后自动应用）。
    /// 用途：页面加载完成前的"闪白"窗口期，让底色与当前主题的编辑器背景一致。
    void setDefaultBackgroundColor(const QColor &color);

    // ---- 导航 ----
    /// 把虚拟主机名映射到本地目录，从而可以用 https 源加载本地静态资源
    bool mapVirtualHost(const QString &sHostName, const QString &sFolderPath);
    void navigate(const QString &sUrl);
    void navigateToString(const QString &sHtml);

    /// 页面加载前注入脚本；按调用先后顺序执行
    bool addInitScript(const QString &sJs);

    // ---- 消息 ----
    /// 向页面投递 JSON（页面侧以 chrome.webview 'message' 事件收到对象）
    void postJson(const QString &sJson);
    /// 在页面里执行脚本（异步，结果通过 executeScriptFinished 回传）
    void executeScript(const QString &sJs);

    void openDevTools();
    void close();

    /// 内部实现（定义在 .cpp）。公开声明：同 .cpp 里的 COM 回调类需要引用它。
    struct Impl;

signals:
    /// 页面发来的 JSON 原文
    void webMessageReceived(const QString &sJson);
    void environmentReady();
    void controllerReady();
    void navigationCompleted(bool bSuccess, int nErrorCode);
    void documentTitleChanged(const QString &sTitle);
    void newWindowRequested(const QString &sUrl);
    void executeScriptFinished(bool bSuccess, const QString &sResultJson);
    void processFailed(int nKind, const QString &sDescription);
    /// 键盘加速键（nKeyKind 见 COREWEBVIEW2_KEY_KIND，nVirtualKey 为 Win32 VK_*）
    void acceleratorKeyPressed(int nKeyKind, int nVirtualKey);

private:
    Impl *m_pImpl;
};

#endif // WEBVIEW2HOST_H
