/*
 * EventToken.h —— MinGW-w64 侧 shim（自建）
 *
 * 背景：WebView2.h 第 2120 行 `#include "EventToken.h"`。
 * 该文件属于 Windows SDK，MinGW-w64 未随附，故本工程自建占位。
 *
 * 实际需要它做什么：
 *   新版 WebView2 SDK（>= 1.0.1900）的事件接口已全面改用
 *   EventRegistrationToken。但 WebView2.h 只 include 了 objidl.h / oaidl.h，
 *   而 MinGW-w64 把 EventRegistrationToken 放在 windows.foundation.h 里
 *   （WinRT 那一套），objidl.h 并不会把它带进来 ——
 *   于是直接编译会报 "'EventRegistrationToken' has not been declared"。
 *
 * 所以这里不是空占位，而是**转发到 windows.foundation.h**。
 *
 * 注意：windows.foundation.h 属于 WinRT 头，MinGW 下可用但会连带引入
 * Windows.Foundation 命名空间相关的少量声明；只取其中的结构体定义，
 * 不涉及 ABI 差异，故对纯 COM 场景（WebView2）安全。
 */
#ifndef QTMT_EVENTTOKEN_SHIM_H
#define QTMT_EVENTTOKEN_SHIM_H

/* MinGW-w64: EventRegistrationToken 定义在 windows.foundation.h */
#include <windows.foundation.h>

#endif /* QTMT_EVENTTOKEN_SHIM_H */
