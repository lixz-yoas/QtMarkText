#-------------------------------------------------
# QtMarkText —— MarkText 的 Qt 重写版（外壳层 + 渲染层挂载点）
# 工程：qmake / Qt 5.12.8 + mingw73_64
# 编码：UTF-8（无 BOM）+ CRLF
#-------------------------------------------------

QT += core gui widgets

TARGET = QtMarkText1
TEMPLATE = app
CONFIG += c++11

DEFINES += QT_DEPRECATED_WARNINGS

# 源码统一按 UTF-8 解释，避免中文乱码
QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8

# 第 1/2 阶段即以此口径验收（零警告零错误），沿用
QMAKE_CXXFLAGS += -Wall -Wextra

DESTDIR = $$PWD/bin

#-------------------------------------------------
# 渲染层内核：Microsoft Edge WebView2
#
# 说明：
#   1) SDK 头文件为 MIDL 生成的纯 C/C++ 头（不含 WRL），MinGW-w64 可直接编译；
#      仅需本工程自建的 EventToken.h shim（见 include/ 内注释）。
#   2) WebView2Loader 采用「动态加载」（webview2host.cpp 内 LoadLibrary），
#      因此这里的 -lWebView2Loader 只是显式声明依赖目录，便于将来切静态链接；
#      实际运行不依赖链接期符号 —— 没装 WebView2 Runtime 的机器也能启动。
#   3) 目录用绝对路径，避免 shadow build 下相对层级错位。
#   4) **刻意不用 INCLUDEPATH，而用 -isystem**：
#      官方 WebView2.h 第 19 行是 MSVC 专用的 #pragma warning，GCC 会报
#      -Wunknown-pragmas；而 GCC 的 diagnostic pragma 包夹对该告警无效（实测 7.3），
#      只有把该目录标记为「系统头目录」才能真正豁免。顺带也豁免第三方头自身的其它告警，
#      以守住本工程「零告警」的验收口径。
#-------------------------------------------------
QTMT_WEBVIEW2_DIR = $$PWD/third_party/webview2

QMAKE_CXXFLAGS += -isystem $$QTMT_WEBVIEW2_DIR/include
# -lole32/-luuid：CoInitializeEx / CoTaskMemFree / IID_IUnknown
LIBS           += -L$$QTMT_WEBVIEW2_DIR/lib -lWebView2Loader -lshell32 -lole32 -luuid

SOURCES += \
    src/main.cpp \
    src/mainwindow.cpp \
    src/marktextmenu.cpp \
    src/marktextthemes.cpp \
    src/preferencesstore.cpp \
    src/preferencesdialog.cpp \
    src/editorhost.cpp \
    src/webview2host.cpp \
    src/marktextbridge.cpp \
    src/markdownparser.cpp \
    src/markdownrenderer.cpp \
    src/markdowneditor.cpp

HEADERS += \
    src/mainwindow.h \
    src/marktextmenu.h \
    src/marktextthemes.h \
    src/preferencesstore.h \
    src/preferencesdialog.h \
    src/editorhost.h \
    src/webview2host.h \
    src/marktextbridge.h \
    src/markdownparser.h \
    src/markdownrenderer.h \
    src/markdowneditor.h
