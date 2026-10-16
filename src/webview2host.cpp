/*
 * webview2host.cpp —— WebView2 运行时宿主封装实现
 * ===========================================================================
 * 实现要点：
 *   - WebView2Loader.dll 走 LoadLibrary 动态加载，GetProcAddress 取两个入口：
 *       CreateCoreWebView2EnvironmentWithOptions / GetAvailableCoreWebView2BrowserVersionString
 *   - WebView2.h 的 MIDL 接口在 MinGW 下可直接继承实现；每个回调覆写
 *     QueryInterface/AddRef/Release，并只对自己那一个 IID 返回 S_OK。
 *   - 所有异步回调都发生在 UI 线程，因此直接把 COM 事件转成 Qt 信号即可。
 * 参考：third_party/webview2/（SDK 归档位置与补丁说明）
 */

#include "webview2host.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <QWidget>

#include <vector>

#include <windows.h>

#include <wrl/client.h>

// WebView2.h 里带 MSVC 专用的 #pragma warning，GCC 会报 -Wunknown-pragmas；
// 该头是第三方 SDK，不能改，故在 include 处临时屏蔽。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#include "WebView2.h"
#pragma GCC diagnostic pop

using Microsoft::WRL::ComPtr;

//=====================================================================
//  WebView2Loader.dll 导出签名
//=====================================================================
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateCoreWebView2EnvironmentWithOptions)(
        PCWSTR browserExecutableFolder,
        PCWSTR userDataFolder,
        ICoreWebView2EnvironmentOptions *environmentOptions,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *environmentCreatedHandler);

typedef HRESULT(STDMETHODCALLTYPE *PFN_GetAvailableCoreWebView2BrowserVersionString)(
        PCWSTR browserExecutableFolder,
        LPWSTR *versionInfo);

//=====================================================================
//  极简 COM 回调基类
//  MinGW 没有 wrl/implements.h，故不用 Microsoft::WRL::Callback，
//  手写 QueryInterface/AddRef/Release（每个回调只认自己的 IID）。
//=====================================================================
template <typename TInterface>
class Wv2Callback : public TInterface
{
public:
    Wv2Callback() : m_nRef(1) {}
    virtual ~Wv2Callback() {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
    {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, iidOfInterface())) {
            *ppvObject = static_cast<TInterface *>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_nRef));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG nRef = InterlockedDecrement(&m_nRef);
        if (nRef == 0) {
            delete this;
        }
        return static_cast<ULONG>(nRef);
    }

protected:
    /// 子类给出自身接口 IID（由 WebView2.h 自带 IID_* 常量提供）
    virtual const IID &iidOfInterface() const = 0;

private:
    LONG m_nRef;
};

//=====================================================================
//  Impl
//=====================================================================
struct WebView2Host::Impl
{
    WebView2Host *pHost = nullptr;

    QWidget *pOwner = nullptr;
    HWND     hwnd = nullptr;

    QString  sUserDataDir;
    int      nX = 0;
    int      nY = 0;
    int      nWidth = 0;
    int      nHeight = 0;
    bool     bVisible = true;
    double   dZoom = 1.0;

    HMODULE hLoader = nullptr;
    PFN_CreateCoreWebView2EnvironmentWithOptions     pfnCreateEnv = nullptr;
    PFN_GetAvailableCoreWebView2BrowserVersionString pfnGetVersion = nullptr;

    ComPtr<ICoreWebView2Environment> pEnv;
    ComPtr<ICoreWebView2Controller>  pCtl;
    ComPtr<ICoreWebView2>            pWebView;
    ComPtr<ICoreWebView2_3>          pWebView3;

    /// 回调对象保活（COM 侧另有引用；这里再持一份，避免异步窗口期被析构）
    /// 用 std::vector 而非 QList：MinGW 版 wrl/client.h 的 ComPtr 提供了
    /// operator void**()，与 QList 内部 `static_cast<const void*>(&t)` 冲突。
    std::vector<ComPtr<IUnknown> > listHandlers;

    bool bNavFinished = false;
    bool bEnvOk = false;
    bool bCtlOk = false;

    /// 默认背景色（COREWEBVIEW2_COLOR）：控制器创建完成时应用，
    /// 消除"页面加载前闪白"。bDefaultBgSet=false 时保持 WebView2 默认（白）。
    COLORREF crDefaultBg = 0;
    bool     bDefaultBgSet = false;

    /// 把存储的背景色应用到控制器（控制器就绪后才有效）。
    /// put_DefaultBackgroundColor 在 ICoreWebView2Controller2（0.9.430+），
    /// 运行时过旧则静默跳过——只损失"加载前底色"，不影响功能。
    void applyDefaultBackground()
    {
        if (bDefaultBgSet && bCtlOk && pCtl != nullptr) {
            ICoreWebView2Controller2 *pCtl2 = nullptr;
            if (SUCCEEDED(pCtl->QueryInterface(IID_ICoreWebView2Controller2,
                                               reinterpret_cast<void **>(&pCtl2)))
                    && pCtl2 != nullptr) {
                const COLORREF c = crDefaultBg;
                COREWEBVIEW2_COLOR color;
                color.A = 0xFF;
                color.R = GetRValue(c);
                color.G = GetGValue(c);
                color.B = GetBValue(c);
                pCtl2->put_DefaultBackgroundColor(color);
                pCtl2->Release();
            }
        }
    }

    /// 保活一个回调对象
    void keepHandler(IUnknown *pHandler)
    {
        if (pHandler != nullptr) {
            ComPtr<IUnknown> spKeep = pHandler;
            listHandlers.push_back(spKeep);
        }
    }

    void applyBounds()
    {
        if (pCtl != nullptr) {
            RECT rc = { nX, nY, nX + nWidth, nY + nHeight };
            pCtl->put_Bounds(rc);
        }
    }

    void applyVisible()
    {
        if (pCtl != nullptr) {
            pCtl->put_IsVisible(bVisible ? TRUE : FALSE);
        }
    }

    void applyZoom()
    {
        if (pCtl != nullptr) {
            pCtl->put_ZoomFactor(dZoom);
        }
    }
};

//=====================================================================
//  COM 回调实现（匿名命名空间）
//=====================================================================
namespace {

class ControllerCompletedHandler;
class WebMessageHandler;
class NavigationCompletedHandler;
class TitleChangedHandler;
class NewWindowHandler;
class ProcessFailedHandler;

//---------------------------------------------------------------------
//  页面 → 原生 消息
//---------------------------------------------------------------------
class WebMessageHandler : public Wv2Callback<ICoreWebView2WebMessageReceivedEventHandler>
{
public:
    explicit WebMessageHandler(WebView2Host::Impl *pImpl) : m_pImpl(pImpl) {}

    const IID &iidOfInterface() const override
    {
        return IID_ICoreWebView2WebMessageReceivedEventHandler;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *pSender,
                                     ICoreWebView2WebMessageReceivedEventArgs *pArgs) override
    {
        Q_UNUSED(pSender);
        LPWSTR pszJson = nullptr;
        if (pArgs != nullptr && SUCCEEDED(pArgs->get_WebMessageAsJson(&pszJson)) && pszJson != nullptr) {
            emit m_pImpl->pHost->webMessageReceived(QString::fromWCharArray(pszJson));
            CoTaskMemFree(pszJson);
        }
        return S_OK;
    }

private:
    WebView2Host::Impl *m_pImpl;
};

//---------------------------------------------------------------------
//  导航完成
//---------------------------------------------------------------------
class NavigationCompletedHandler
        : public Wv2Callback<ICoreWebView2NavigationCompletedEventHandler>
{
public:
    explicit NavigationCompletedHandler(WebView2Host::Impl *pImpl) : m_pImpl(pImpl) {}

    const IID &iidOfInterface() const override
    {
        return IID_ICoreWebView2NavigationCompletedEventHandler;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *pSender,
                                     ICoreWebView2NavigationCompletedEventArgs *pArgs) override
    {
        Q_UNUSED(pSender);
        BOOL bSuccess = FALSE;
        COREWEBVIEW2_WEB_ERROR_STATUS eStatus = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
        if (pArgs != nullptr) {
            pArgs->get_IsSuccess(&bSuccess);
            pArgs->get_WebErrorStatus(&eStatus);
        }
        m_pImpl->bNavFinished = true;
        emit m_pImpl->pHost->navigationCompleted(bSuccess ? true : false, static_cast<int>(eStatus));
        return S_OK;
    }

private:
    WebView2Host::Impl *m_pImpl;
};

//---------------------------------------------------------------------
//  文档标题变化
//---------------------------------------------------------------------
class TitleChangedHandler
        : public Wv2Callback<ICoreWebView2DocumentTitleChangedEventHandler>
{
public:
    explicit TitleChangedHandler(WebView2Host::Impl *pImpl) : m_pImpl(pImpl) {}

    const IID &iidOfInterface() const override
    {
        return IID_ICoreWebView2DocumentTitleChangedEventHandler;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *pSender, IUnknown *pArgs) override
    {
        Q_UNUSED(pArgs);
        LPWSTR pszTitle = nullptr;
        if (pSender != nullptr && SUCCEEDED(pSender->get_DocumentTitle(&pszTitle)) && pszTitle != nullptr) {
            emit m_pImpl->pHost->documentTitleChanged(QString::fromWCharArray(pszTitle));
            CoTaskMemFree(pszTitle);
        }
        return S_OK;
    }

private:
    WebView2Host::Impl *m_pImpl;
};

//---------------------------------------------------------------------
//  新窗口请求（target=_blank 之类）→ 交给外壳决定
//---------------------------------------------------------------------
class NewWindowHandler
        : public Wv2Callback<ICoreWebView2NewWindowRequestedEventHandler>
{
public:
    explicit NewWindowHandler(WebView2Host::Impl *pImpl) : m_pImpl(pImpl) {}

    const IID &iidOfInterface() const override
    {
        return IID_ICoreWebView2NewWindowRequestedEventHandler;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *pSender,
                                     ICoreWebView2NewWindowRequestedEventArgs *pArgs) override
    {
        Q_UNUSED(pSender);
        if (pArgs != nullptr) {
            LPWSTR pszUri = nullptr;
            if (SUCCEEDED(pArgs->get_Uri(&pszUri)) && pszUri != nullptr) {
                emit m_pImpl->pHost->newWindowRequested(QString::fromWCharArray(pszUri));
                CoTaskMemFree(pszUri);
            }
            pArgs->put_Handled(TRUE);   // 不弹新窗口，由外壳处理
        }
        return S_OK;
    }

private:
    WebView2Host::Impl *m_pImpl;
};

//---------------------------------------------------------------------
//  渲染进程异常（排查白屏的第一手信息）
//---------------------------------------------------------------------
class ProcessFailedHandler
        : public Wv2Callback<ICoreWebView2ProcessFailedEventHandler>
{
public:
    explicit ProcessFailedHandler(WebView2Host::Impl *pImpl) : m_pImpl(pImpl) {}

    const IID &iidOfInterface() const override
    {
        return IID_ICoreWebView2ProcessFailedEventHandler;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *pSender,
                                     ICoreWebView2ProcessFailedEventArgs *pArgs) override
    {
        Q_UNUSED(pSender);
        COREWEBVIEW2_PROCESS_FAILED_KIND eKind =
                COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
        if (pArgs != nullptr) {
            pArgs->get_ProcessFailedKind(&eKind);
        }
        emit m_pImpl->pHost->processFailed(static_cast<int>(eKind),
                                           QStringLiteral("WebView2 进程异常，kind=%1")
                                                   .arg(static_cast<int>(eKind)));
        return S_OK;
    }

private:
    WebView2Host::Impl *m_pImpl;
};

//---------------------------------------------------------------------
//  ExecuteScript 完成
//---------------------------------------------------------------------
class ExecuteScriptHandler : public Wv2Callback<ICoreWebView2ExecuteScriptCompletedHandler>
{
public:
    explicit ExecuteScriptHandler(WebView2Host::Impl *pImpl) : m_pImpl(pImpl) {}

    const IID &iidOfInterface() const override
    {
        return IID_ICoreWebView2ExecuteScriptCompletedHandler;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hrResult, LPCWSTR pszResult) override
    {
        emit m_pImpl->pHost->executeScriptFinished(
                    SUCCEEDED(hrResult),
                    pszResult != nullptr ? QString::fromWCharArray(pszResult) : QString());
        return S_OK;
    }

private:
    WebView2Host::Impl *m_pImpl;
};

//---------------------------------------------------------------------
//  键盘加速键（Alt 等）→ 宿主
//  用途：对齐 MarkText 的「菜单栏默认隐藏，按 Alt 呼出」行为。
//        WebView2 拿走键盘焦点后 Qt 的 QShortcut 收不到 Alt，
//        必须由 WebView2 的 AcceleratorKeyPressed 转发。
//---------------------------------------------------------------------
class AcceleratorKeyHandler
        : public Wv2Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>
{
public:
    explicit AcceleratorKeyHandler(WebView2Host::Impl *pImpl) : m_pImpl(pImpl) {}

    const IID &iidOfInterface() const override
    {
        return IID_ICoreWebView2AcceleratorKeyPressedEventHandler;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller *pSender,
                                     ICoreWebView2AcceleratorKeyPressedEventArgs *pArgs) override
    {
        Q_UNUSED(pSender)
        UINT nVk = 0;
        COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
        if (pArgs != nullptr
                && SUCCEEDED(pArgs->get_VirtualKey(&nVk))
                && SUCCEEDED(pArgs->get_KeyEventKind(&kind))) {
            // 只关心 Alt（VK_MENU=0x12）。Alt 属于系统键（SystemKeyDown），
            // WebView2 不会自己消费它，无需 put_Handled，让冒泡照常。
            if (nVk == 0x12 && kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
                emit m_pImpl->pHost->acceleratorKeyPressed(int(kind), int(nVk));
            }
        }
        return S_OK;
    }

private:
    WebView2Host::Impl *m_pImpl;
};

//---------------------------------------------------------------------
//  控制器创建完成 → 配置 + 绑事件
//---------------------------------------------------------------------
class ControllerCompletedHandler
        : public Wv2Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>
{
public:
    explicit ControllerCompletedHandler(WebView2Host::Impl *pImpl) : m_pImpl(pImpl) {}

    const IID &iidOfInterface() const override
    {
        return IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hrResult,
                                     ICoreWebView2Controller *pController) override
    {
        WebView2Host::Impl *p = m_pImpl;
        if (FAILED(hrResult) || pController == nullptr) {
            qWarning("WebView2: 控制器创建失败 hr=0x%08lX", (unsigned long)hrResult);
            return S_OK;
        }
        p->pCtl = pController;

        HRESULT hr = pController->get_CoreWebView2(&p->pWebView);
        if (FAILED(hr) || p->pWebView == nullptr) {
            qWarning("WebView2: get_CoreWebView2 失败 hr=0x%08lX", (unsigned long)hr);
            return S_OK;
        }

        // 虚拟主机映射能力来自 ICoreWebView2_3 及以后
        //
        // 注意：这里刻意**不用** ComPtr::As()。
        // MinGW 版 wrl/client.h 的 As() 内部展开成 __uuidof(T)，而 MinGW 的
        // __uuidof 依赖 MIDL_INTERFACE 的 uuid 属性 —— 该属性在 GCC 下被忽略
        // （rpcndr.h 把 MIDL_INTERFACE(x) 退化成 `struct`），于是 __uuidof 落到
        // 声明未定义的模板 __mingw_uuidof<T>()，链接期报 undefined reference。
        // 官方 WebView2.h 自带全部 IID_* 常量（EXTERN_GUID），直接用它做 QueryInterface。
        ICoreWebView2_3 *pRawWebView3 = nullptr;
        if (SUCCEEDED(p->pWebView->QueryInterface(IID_ICoreWebView2_3,
                                                  reinterpret_cast<void **>(&pRawWebView3)))
                && pRawWebView3 != nullptr) {
            p->pWebView3.Attach(pRawWebView3);   // 交所有权给 ComPtr，避免多一次 Release
        }

        // 设置：**必须**打开 IsWebMessageEnabled，否则 postMessage 通道不通
        ComPtr<ICoreWebView2Settings> pSettings;
        if (SUCCEEDED(p->pWebView->get_Settings(&pSettings)) && pSettings != nullptr) {
            pSettings->put_IsWebMessageEnabled(TRUE);
            pSettings->put_IsScriptEnabled(TRUE);
            pSettings->put_AreDevToolsEnabled(TRUE);
            pSettings->put_AreDefaultContextMenusEnabled(TRUE);
            pSettings->put_AreDefaultScriptDialogsEnabled(FALSE);
            pSettings->put_IsStatusBarEnabled(FALSE);
            pSettings->put_IsZoomControlEnabled(FALSE);

            // 非客户区支持（Settings9+）：让 WebView2 原生处理页面里的
            // -webkit-app-region: drag —— 渲染层自绘标题栏（titleBarStyle=custom）
            // 由此获得 HTCAPTION 拖动 / Aero Snap / 双击最大化，与 Electron 行为一致。
            ICoreWebView2Settings9 *pSettings9 = nullptr;
            if (SUCCEEDED(pSettings->QueryInterface(IID_ICoreWebView2Settings9,
                                                    reinterpret_cast<void **>(&pSettings9)))
                    && pSettings9 != nullptr) {
                pSettings9->put_IsNonClientRegionSupportEnabled(TRUE);
                pSettings9->Release();
            }
        }

        EventRegistrationToken token = { 0 };

        WebMessageHandler *pMsg = new WebMessageHandler(p);
        p->keepHandler(static_cast<IUnknown *>(pMsg));
        p->pWebView->add_WebMessageReceived(pMsg, &token);
        pMsg->Release();

        NavigationCompletedHandler *pNav = new NavigationCompletedHandler(p);
        p->keepHandler(static_cast<IUnknown *>(pNav));
        p->pWebView->add_NavigationCompleted(pNav, &token);
        pNav->Release();

        TitleChangedHandler *pTitle = new TitleChangedHandler(p);
        p->keepHandler(static_cast<IUnknown *>(pTitle));
        p->pWebView->add_DocumentTitleChanged(pTitle, &token);
        pTitle->Release();

        NewWindowHandler *pNewWin = new NewWindowHandler(p);
        p->keepHandler(static_cast<IUnknown *>(pNewWin));
        p->pWebView->add_NewWindowRequested(pNewWin, &token);
        pNewWin->Release();

        ProcessFailedHandler *pFailed = new ProcessFailedHandler(p);
        p->keepHandler(static_cast<IUnknown *>(pFailed));
        p->pWebView->add_ProcessFailed(pFailed, &token);
        pFailed->Release();

        // Alt 键转发（控制器级事件，不在 pWebView 上）
        AcceleratorKeyHandler *pAccel = new AcceleratorKeyHandler(p);
        p->keepHandler(static_cast<IUnknown *>(pAccel));
        pController->add_AcceleratorKeyPressed(pAccel, &token);
        pAccel->Release();

        p->bCtlOk = true;
        p->applyDefaultBackground();   // 页面加载前先把底色铺成主题编辑器背景色
        p->applyBounds();
        p->applyVisible();
        p->applyZoom();
        emit p->pHost->controllerReady();
        return S_OK;
    }

private:
    WebView2Host::Impl *m_pImpl;
};

//---------------------------------------------------------------------
//  环境创建完成 → 建控制器
//---------------------------------------------------------------------
class EnvCompletedHandler
        : public Wv2Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>
{
public:
    explicit EnvCompletedHandler(WebView2Host::Impl *pImpl) : m_pImpl(pImpl) {}

    const IID &iidOfInterface() const override
    {
        return IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
    }

    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hrResult,
                                     ICoreWebView2Environment *pEnv) override
    {
        WebView2Host::Impl *p = m_pImpl;
        if (FAILED(hrResult) || pEnv == nullptr) {
            qWarning("WebView2: 环境创建失败 hr=0x%08lX", (unsigned long)hrResult);
            return S_OK;
        }
        p->pEnv = pEnv;
        p->bEnvOk = true;
        emit p->pHost->environmentReady();

        ControllerCompletedHandler *pCtlHandler = new ControllerCompletedHandler(p);
        p->keepHandler(static_cast<IUnknown *>(pCtlHandler));
        HRESULT hr = pEnv->CreateCoreWebView2Controller(p->hwnd, pCtlHandler);
        pCtlHandler->Release();
        if (FAILED(hr)) {
            qWarning("WebView2: CreateCoreWebView2Controller 失败 hr=0x%08lX", (unsigned long)hr);
        }
        return S_OK;
    }

private:
    WebView2Host::Impl *m_pImpl;
};


} // namespace

//=====================================================================
//  构造 / 析构
//=====================================================================
WebView2Host::WebView2Host(QObject *parent)
    : QObject(parent)
    , m_pImpl(new Impl)
{
    m_pImpl->pHost = this;
}

WebView2Host::~WebView2Host()
{
    // 先停浏览器，再释放 COM 对象，最后清回调，避免回调打到半析构的 Impl
    if (m_pImpl->pCtl != nullptr) {
        m_pImpl->pCtl->Close();
    }
    m_pImpl->pWebView3.Reset();
    m_pImpl->pWebView.Reset();
    m_pImpl->pCtl.Reset();
    m_pImpl->pEnv.Reset();
    m_pImpl->listHandlers.clear();
    if (m_pImpl->hLoader != nullptr) {
        FreeLibrary(m_pImpl->hLoader);
        m_pImpl->hLoader = nullptr;
    }
    delete m_pImpl;
    m_pImpl = nullptr;
}

//=====================================================================
//  运行时探测
//=====================================================================
bool WebView2Host::runtimeAvailable(QString *pszVersion)
{
    // 先按 exe 同目录找，再退回系统搜索路径
    QString sLoader = QCoreApplication::applicationDirPath() + QStringLiteral("/WebView2Loader.dll");
    HMODULE hLoader = nullptr;
    if (QFileInfo::exists(sLoader)) {
        hLoader = LoadLibraryW(reinterpret_cast<const wchar_t *>(sLoader.utf16()));
    }
    if (hLoader == nullptr) {
        hLoader = LoadLibraryW(L"WebView2Loader.dll");
    }
    if (hLoader == nullptr) {
        return false;
    }

    bool bOk = false;
    PFN_GetAvailableCoreWebView2BrowserVersionString pfn =
            reinterpret_cast<PFN_GetAvailableCoreWebView2BrowserVersionString>(
                GetProcAddress(hLoader, "GetAvailableCoreWebView2BrowserVersionString"));
    if (pfn != nullptr) {
        LPWSTR pszVer = nullptr;
        if (SUCCEEDED(pfn(nullptr, &pszVer)) && pszVer != nullptr) {
            if (pszVersion != nullptr) {
                *pszVersion = QString::fromWCharArray(pszVer);
            }
            CoTaskMemFree(pszVer);
            bOk = true;
        }
    }
    FreeLibrary(hLoader);
    return bOk;
}

//=====================================================================
//  挂载
//=====================================================================
bool WebView2Host::attach(QWidget *pOwner, const QString &sUserDataDir)
{
    if (m_pImpl->pCtl != nullptr || m_pImpl->bEnvOk) {
        return true;   // 已挂载
    }
    if (pOwner == nullptr) {
        return false;
    }

    // COM：Qt 主线程通常已初始化 OLE，这里做幂等确认
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Q_UNUSED(hrCo);

    // 动态加载 WebView2Loader.dll
    QString sLoader = QCoreApplication::applicationDirPath() + QStringLiteral("/WebView2Loader.dll");
    if (QFileInfo::exists(sLoader)) {
        m_pImpl->hLoader = LoadLibraryW(reinterpret_cast<const wchar_t *>(sLoader.utf16()));
    }
    if (m_pImpl->hLoader == nullptr) {
        m_pImpl->hLoader = LoadLibraryW(L"WebView2Loader.dll");
    }
    if (m_pImpl->hLoader == nullptr) {
        qWarning("WebView2: 找不到 WebView2Loader.dll");
        return false;
    }
    m_pImpl->pfnCreateEnv = reinterpret_cast<PFN_CreateCoreWebView2EnvironmentWithOptions>(
                GetProcAddress(m_pImpl->hLoader, "CreateCoreWebView2EnvironmentWithOptions"));
    if (m_pImpl->pfnCreateEnv == nullptr) {
        qWarning("WebView2: WebView2Loader.dll 缺少 CreateCoreWebView2EnvironmentWithOptions");
        return false;
    }

    m_pImpl->pOwner = pOwner;
    pOwner->setAttribute(Qt::WA_NativeWindow);
    pOwner->setAttribute(Qt::WA_OpaquePaintEvent);
    pOwner->setAutoFillBackground(true);
    m_pImpl->hwnd = reinterpret_cast<HWND>(pOwner->winId());
    if (m_pImpl->hwnd == nullptr) {
        qWarning("WebView2: 取窗口句柄失败");
        return false;
    }

    m_pImpl->sUserDataDir = sUserDataDir;
    QDir().mkpath(sUserDataDir);
    // WebView2 要求绝对路径
    QString sAbsUserData = QDir(sUserDataDir).absolutePath();
    sAbsUserData.replace(QLatin1Char('/'), QLatin1Char('\\'));

    EnvCompletedHandler *pEnvHandler = new EnvCompletedHandler(m_pImpl);
    m_pImpl->keepHandler(static_cast<IUnknown *>(pEnvHandler));
    HRESULT hr = m_pImpl->pfnCreateEnv(nullptr,
                                       reinterpret_cast<const wchar_t *>(sAbsUserData.utf16()),
                                       nullptr,
                                       pEnvHandler);
    pEnvHandler->Release();
    if (FAILED(hr)) {
        qWarning("WebView2: CreateCoreWebView2EnvironmentWithOptions 失败 hr=0x%08lX",
                 (unsigned long)hr);
        return false;
    }
    return true;
}

bool WebView2Host::isEnvironmentReady() const
{
    return m_pImpl->bEnvOk;
}

bool WebView2Host::isControllerReady() const
{
    return m_pImpl->bCtlOk;
}

//=====================================================================
//  布局
//=====================================================================
void WebView2Host::setBounds(int x, int y, int nWidth, int nHeight)
{
    m_pImpl->nX = x;
    m_pImpl->nY = y;
    m_pImpl->nWidth = qMax(0, nWidth);
    m_pImpl->nHeight = qMax(0, nHeight);
    m_pImpl->applyBounds();
}

void WebView2Host::setVisible(bool bVisible)
{
    m_pImpl->bVisible = bVisible;
    m_pImpl->applyVisible();
}

void WebView2Host::notifyParentMoved()
{
    if (m_pImpl->pCtl != nullptr) {
        m_pImpl->pCtl->NotifyParentWindowPositionChanged();
    }
}

double WebView2Host::zoomFactor() const
{
    return m_pImpl->dZoom;
}

void WebView2Host::setZoomFactor(double dFactor)
{
    m_pImpl->dZoom = dFactor;
    m_pImpl->applyZoom();
}

void WebView2Host::setDefaultBackgroundColor(const QColor &color)
{
    if (!color.isValid()) {
        return;
    }
    m_pImpl->crDefaultBg = RGB(color.red(), color.green(), color.blue());
    m_pImpl->bDefaultBgSet = true;
    m_pImpl->applyDefaultBackground();   // 控制器未就绪时只存值，就绪回调里统一应用
}

//=====================================================================
//  导航
//=====================================================================
bool WebView2Host::mapVirtualHost(const QString &sHostName, const QString &sFolderPath)
{
    if (m_pImpl->pWebView3 == nullptr) {
        qWarning("WebView2: 运行时版本过低，不支持 SetVirtualHostNameToFolderMapping");
        return false;
    }
    QString sAbs = QDir(sFolderPath).absolutePath();
    sAbs.replace(QLatin1Char('/'), QLatin1Char('\\'));
    HRESULT hr = m_pImpl->pWebView3->SetVirtualHostNameToFolderMapping(
                reinterpret_cast<const wchar_t *>(sHostName.utf16()),
                reinterpret_cast<const wchar_t *>(sAbs.utf16()),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    if (FAILED(hr)) {
        qWarning("WebView2: 虚拟主机映射失败 hr=0x%08lX (%s -> %s)",
                 (unsigned long)hr, qPrintable(sHostName), qPrintable(sAbs));
        return false;
    }
    return true;
}

void WebView2Host::navigate(const QString &sUrl)
{
    if (m_pImpl->pWebView == nullptr) {
        return;
    }
    m_pImpl->pWebView->Navigate(reinterpret_cast<const wchar_t *>(sUrl.utf16()));
}

void WebView2Host::navigateToString(const QString &sHtml)
{
    if (m_pImpl->pWebView == nullptr) {
        return;
    }
    m_pImpl->pWebView->NavigateToString(reinterpret_cast<const wchar_t *>(sHtml.utf16()));
}

bool WebView2Host::addInitScript(const QString &sJs)
{
    if (m_pImpl->pWebView == nullptr) {
        return false;
    }
    return SUCCEEDED(m_pImpl->pWebView->AddScriptToExecuteOnDocumentCreated(
                         reinterpret_cast<const wchar_t *>(sJs.utf16()), nullptr));
}

//=====================================================================
//  消息
//=====================================================================
void WebView2Host::postJson(const QString &sJson)
{
    if (m_pImpl->pWebView == nullptr) {
        return;
    }
    m_pImpl->pWebView->PostWebMessageAsJson(reinterpret_cast<const wchar_t *>(sJson.utf16()));
}

void WebView2Host::executeScript(const QString &sJs)
{
    if (m_pImpl->pWebView == nullptr) {
        return;
    }
    ExecuteScriptHandler *pHandler = new ExecuteScriptHandler(m_pImpl);
    m_pImpl->pWebView->ExecuteScript(reinterpret_cast<const wchar_t *>(sJs.utf16()), pHandler);
    pHandler->Release();
}

void WebView2Host::openDevTools()
{
    if (m_pImpl->pWebView != nullptr) {
        m_pImpl->pWebView->OpenDevToolsWindow();
    }
}

void WebView2Host::close()
{
    if (m_pImpl->pCtl != nullptr) {
        m_pImpl->pCtl->Close();
    }
}
