# QtMarkText 工程导览与构建运行指南

> 版本：20261012（主题对齐 ayu-mirage 后）
> 主工程：仓库根目录（`<repo>/`）
> 参考源码：MarkText v0.19.1 官方仓库（<https://github.com/marktext/marktext>，MIT）

---

## 一、一句话架构

**Qt 外壳（主进程替身）+ MarkText 官方渲染层（零改动）**：

```
QtMarkText.exe（Qt 5.12.8）
 ├─ MainWindow            窗口/菜单/标签页/文件树/状态栏（Qt 部分）
 ├─ MarkTextBridge        Electron 主进程替身：IPC 通道 + 菜单路由 + 偏好同步
 ├─ WebView2Host          WebView2 运行时宿主（动态加载，无链接期依赖）
 │    └─ WebView2 (Edge)  内嵌浏览器
 │          └─ https://marktext.local/renderer/index.html   ← webui/ 目录（虚拟主机映射）
 └─ bin/config/           QtMarkText.ini（偏好） + webview2/（浏览器用户数据）
```

关键点：**渲染层代码（webui/）一行未改**，全部来自本机 MarkText 成品 `app.asar` 抽取；
Qt 侧只负责"Electron 主进程原本该干的事"。

---

## 二、关联的代码 / 工程 / 文件夹全清单

### 2.1 主工程根目录 `<repo>/`

| 路径 | 内容 | 说明 |
|------|------|------|
| `QtMarkText.pro` | 2728 B | qmake 工程：QT=core/gui/widgets、`-isystem` WebView2 头、`DESTDIR=$$PWD/bin` |
| `QtMarkText.pro.user` | 22.6 KB | Qt Creator 4.11.2 工程设置（Kit、影子构建目录） |
| `readme.md` | 38.5 KB | 全程变更清单（按日期分节） |
| `sample.md` | 690 B | 渲染自测用示例文件 |
| `src/` | 24 个文件（12 模块） | 全部 C++ 源码，UTF-8 + CRLF |
| `webui/` | 887 个文件 / 24 MB | **MarkText 渲染层**（从 app.asar 抽取，零改动） |
| `tools/` | 2 个脚本 | `asar_tool.js`（asar 抽取）、`grab_window.py`（窗口截图验证） |
| `test/` | 4 个文件 | 不参与构建的验收程序（菜单导出 / 解析自测 / 冒烟） |
| `bin/` | 运行目录 | exe + 11 个运行库 DLL + `platforms/` 等 + `config/` |
| `build_shell/` | 命令行影子构建 | 无 Qt Creator 时用 `qmake + mingw32-make` 的备用构建目录 |

### 2.2 `src/` 模块职责（12 模块）

| 模块 | 文件 | 职责 |
|------|------|------|
| 入口 | `main.cpp` | UTF-8 本地编码、应用元信息、**命令行 md 文件参数**、引擎选择 |
| 外壳 | `mainwindow.h/.cpp` | 主窗口：菜单栏（Alt 呼出）、标签页、文件树 dock、状态栏、文件读写、`WM_NCCALCSIZE` 无边框、标题栏窗口按钮、**主题调色板** |
| 菜单 | `marktextmenu.h/.cpp` | 按 MarkText `templates/*.ts` 原样重建菜单树与勾选态、快捷键 |
| 主题 | `marktextthemes.h/.cpp` | 主题清单：浅色 10 个 + 深色 23 个（id 与 MarkText 一致） |
| 偏好 | `preferencesstore.h/.cpp` | 对齐 `preferences.json` 的读写 + **类型归一化**（QSettings 把 int/bool 读成 QString 的坑） + 默认值表 |
| 偏好 | `preferencesdialog.h/.cpp` | 偏好设置窗口（分类页与 MarkText 对齐） |
| 引擎 | `editorhost.h/.cpp` | `IEditorEngine` 接口 + `PlaceholderEditorEngine`（纯 Qt 兜底）+ `WebView2EditorEngine` + `EditorHost` 工厂 |
| 渲染宿主 | `webview2host.h/.cpp` | WebView2 封装：动态 `LoadLibrary`、消息收发、虚拟主机映射、缩放、**默认背景色**、非客户区拖动、Alt 键转发 |
| 桥接 | `marktextbridge.h/.cpp` | **最大模块**：约 30 个 invoke + 80 个 send 通道、菜单路由表、主题 URL 拼装、webui 目录定位、`mt::open-new-tab` 打开文件 |
| 兜底渲染 | `markdownparser.h/.cpp`、`markdownrenderer.h/.cpp`、`markdowneditor.h/.cpp` | 纯 Qt 的 Markdown 解析/渲染/编辑器（WebView2 不可用时降级，正常不生效） |

### 2.3 `webui/` —— MarkText 渲染层（改动为零）

| 路径 | 说明 |
|------|------|
| `webui/env.js` | **唯一新增**：Electron 环境垫片（`require('electron')` → ipcRenderer/shell/clipboard 等 shim；二进制 base64 编解码） |
| `webui/main/index.js` | MarkText 主进程 bundle（**不执行**，仅作通道对照参考） |
| `webui/preload/index.js` | MarkText 原版 preload（零改动） |
| `webui/renderer/index.html` + `assets/` | 渲染层入口与全部资源（JS/CSS/字体/KaTeX/图标，887 个文件） |
| `webui/locales/*.json` | 9 种语言包（含 zh-CN） |

### 2.4 外部依赖（工程外，必须存在）

| 路径 / 组件 | 用途 | 是否编译依赖 |
|-------------|------|--------------|
| `<repo>/third_party/webview2/`（由 `scripts/fetch-webview2-sdk.ps1` 获取） | WebView2 SDK：`include/WebView2.h` + `include/EventToken.h`（自建 shim）、`lib/libWebView2Loader.a`、`bin/WebView2Loader.dll` | 是（`.pro` 中以 `$$PWD` 相对定位） |
| Qt 5.12.x mingw73_64 套件目录（如 `<Qt>/5.12.9/mingw73_64/`） | qmake、Qt5*.dll | 是 |
| MinGW-w64 7.3 工具链目录（如 `<Qt>/Tools/mingw730_64/`） | g++、mingw32-make | 是 |
| Qt Creator（`<Qt>/Tools/QtCreator/bin/qtcreator.exe`） | IDE，推荐 4.11.2+ | 否 |
| MarkText v0.19.1 源码副本 | 对照主题 CSS、菜单模板、i18n | 否（只读参考） |
| 本机 MarkText 安装目录 | 渲染层 `app.asar` 的来源（已抽取到 `webui/`，仅在需重抽时用） | 否 |
| **WebView2 Runtime**（系统级） | 运行期浏览器内核（Win10/11 通常自带，或装 Evergreen Runtime） | 运行依赖 |

### 2.5 运行期目录

| 路径 | 说明 |
|------|------|
| `bin/QtMarkText.exe` | 主程序（约 405 KB） |
| `bin/*.dll`（11 个） | Qt5Core/Gui/Widgets/Svg、libEGL/GLESV2、libgcc/libstdc++/libwinpthread、D3Dcompiler_47、WebView2Loader |
| `bin/platforms/qwindows.dll`、`bin/styles/`、`bin/iconengines/`、`bin/imageformats/` | Qt 插件（缺则启动即崩） |
| `bin/config/QtMarkText.ini` | 偏好设置（当前 `theme=ayu-mirage`、`followSystemTheme=false`） |
| `bin/config/webview2/` | WebView2 用户数据（缓存/GPUCache，首次运行自动生成，可整包携带） |

### 2.6 依赖方向（谁用谁）

```
QtMarkText.pro ──> src/*.cpp
     │
     ├─ 编译期：third_party/webview2/include（头） + lib/libWebView2Loader.a
     └─ 运行期：
          ├─ bin/WebView2Loader.dll ──> 系统 WebView2 Runtime
          ├─ bin/../webui/（渲染层，路径查找顺序见 4.5）
          └─ bin/config/（偏好 + 浏览器用户数据）
```

---

## 三、Qt Creator 打开 / 编译 / 执行步骤

### 步骤 0：确认前置条件（一次即可）

| 检查项 | 期望值 | 缺了怎么办 |
|--------|--------|-----------|
| Qt Creator | `<Qt>/Tools/QtCreator/bin/qtcreator.exe`（4.11.2+） | 用同版本安装包补装 |
| Kit | Qt 5.12.x MinGW 64-bit | Qt Creator → 工具→选项→Kits 里确认 qmake 指向 `<Qt>/5.12.9/mingw73_64/bin/qmake.exe` |
| WebView2 SDK | `<repo>/third_party/webview2/` 存在 | 先运行 `scripts/fetch-webview2-sdk.ps1`；路径见 `.pro` 的 `QTMT_WEBVIEW2_DIR` |
| WebView2 Runtime | 系统已装 | 装 Evergreen Runtime（或 Edge 自带） |

### 步骤 1：打开工程

1. 启动 Qt Creator → **文件 → 打开文件或项目**
2. 选 `<repo>/QtMarkText.pro` → 打开
3. 若弹出 "Configure Project"，勾选 **Desktop Qt 5.12.8 MinGW 64-bit** → *Configure Project*

### 步骤 2：选择构建配置

左下角（或 构建 → 构建套件选择）：

| 项 | 建议值 |
|----|--------|
| Kit | Desktop Qt 5.12.8 MinGW 64-bit |
| Build | **Release**（默认 Debug 也能跑，但体积与速度差很多） |
| 构建目录 | 工程外的影子目录（如 `../build-QtMarkText-Desktop_Qt_5_12_9_MinGW_64_bit-Release`） |

> 注意：`.pro` 里写了 `DESTDIR = $$PWD/bin`，所以**不管影子目录在哪，exe 始终输出到
> `<repo>/bin/QtMarkText.exe`**。这是有意为之——webui 与运行库都在那边。
>
> 说明：`build_shell/` 是命令行备用构建（`qmake + mingw32-make`），与 Qt Creator 影子构建
> 并存互不干扰——两者产物都落到同一个 `bin/`。当前 `bin/QtMarkText.exe` 即命令构建产出。

### 步骤 3：编译

- 快捷键 **Ctrl+B**，或左下角 🔨 图标，或 构建 → 构建项目 "QtMarkText"
- 应达到 **0 错误 0 警告**（工程用 `-Wall -Wextra` 且第三方头用 `-isystem` 豁免）
- 编译日志出现 `-isystem <repo>/third_party/webview2/include` 属正常

### 步骤 4：运行

- **Ctrl+R**（运行），或左下角 ▶
- 首次运行会自动创建 `bin/config/` 与 `bin/config/webview2/`
- **带文件启动**（推荐，验证渲染）：项目 → 运行 → Run 设置里的"命令行参数"填
  `<repo>/sample.md`（或直接双击 exe 时把 md 拖到其图标上）
- 建议把运行设置的 **Working directory** 设为 `<repo>/bin`
  （程序内部用 `applicationDirPath()` 定位资源，工作目录只影响相对路径参数）

### 步骤 5：验证是否正常（自查清单）

| 检查点 | 期望现象 |
|--------|----------|
| 窗口 | 无系统标题栏；渲染层自绘标题栏（☰ + 标签 + 窗口按钮），可拖动/双击最大化 |
| 主题 | 默认 ayu-mirage 深色（背景 `#1f2430`，H1 金黄、H2 蓝、H3 绿） |
| 编辑 | 点进正文可直接打字；渲染层实时渲染 Markdown |
| 菜单 | 按 **Alt** 呼出 Qt 菜单栏（WebView2 拿焦点后 Qt 收不到，由引擎转发） |
| 文件 | 打开 md → 出现标签页；标题栏显示 `文件名 - MarkText` |
| 日志 | 桥接层通道日志经 `logMessage` 信号发出（当前未接接收者）。排查时在 `mainwindow.cpp` 连接该信号到 `qDebug()` 即可看到；或设 `QTMT_SELFTEST=1` 由页面自检回报 DOM |

### 步骤 6：分发（拷到别的机器）

1. 拷整个 `<repo>/bin/`（含 `platforms/`、`styles/`、插件、`config/`）
2. 拷 `<repo>/webui/` 到 **bin 的上一级**（即保持 `bin/../webui` 关系）
3. 目标机需有 WebView2 Runtime
4. 如重装 Qt 库：`<Qt>/5.12.9/mingw73_64/bin/windeployqt.exe bin\QtMarkText.exe`

---

## 四、常见坑与排查

| 现象 | 原因 | 处理 |
|------|------|------|
| 编辑区空白 / 全白 | WebView2 未挂载或渲染层未就绪 | 看日志是否有 `未找到 webui 资源目录` 或 `WebView2 挂载失败` |
| 启动即退出 | 缺 `platforms/qwindows.dll` 或 Qt DLL | 补 `bin/platforms/`、执行 windeployqt |
| 界面对但编辑区白屏 | QSettings 类型丢失导致渲染层抛错 | 已由 `PreferencesStore::value()` 类型归一化修复；勿绕过该函数直读 |
| 截图全黑（自动化验证时） | WebView2 冷启动后 DWM 尚无合成帧 | 最小化→还原一次再抓；`tools/grab_window.py` 已内置 |
| 编译报 `-Wunknown-pragmas` | WebView2 官方头有 MSVC pragma | 必须保持 `-isystem`（INCLUDEPATH 无效），勿改回 |
| `.pro` 里 webview2 路径失效 | SDK 缺失 | 重新运行 `scripts/fetch-webview2-sdk.ps1` |
| webui 换位置后白屏 | 目录查找顺序受限 | 设环境变量 `QTMT_WEBUI_DIR=<绝对路径>` 覆盖 |

---

## 五、辅助工具速查

| 工具 | 用法 | 用途 |
|------|------|------|
| `tools/asar_tool.js` | `node tools/asar_tool.js list\|get\|verify\|extractall <asar> <前缀> <目标目录>` | 从 MarkText 成品 app.asar 重抽渲染层 |
| `tools/grab_window.py` | `python tools/grab_window.py "" "<标题关键字>" <等待秒> <输出.bmp>` | 无头截图验证（PrintWindow，防黑帧） |
| `QTMT_SELFTEST=1` | 设为环境变量后启动 | 注入页面自检脚本，周期回报 DOM 文本与样式 |
| `QTMT_ENGINE` | `=webview2` / 其它 | 强制指定引擎（占位 / WebView2） |
| `QTMT_WEBUI_DIR` | 绝对路径 | 覆盖 webui 资源目录定位 |
