/*
 * env.js —— WebView2 宿主环境垫片（QtMarkText 自建）
 * ===========================================================================
 * 定位：本文件顶替 Electron 的「运行时 + preload 运行环境」。
 *
 * 设计要点（为什么这么做）：
 *   MarkText 的渲染层（webui/renderer/index.html + assets）与 preload
 *   （webui/preload/index.js）**一行都不用改**。做法是在 preload 之前注入本文件，
 *   在全局补上 preload 需要的三样东西：
 *     1) globalThis.require —— preload 首行是 require("electron")，这里给个垫片；
 *     2) 垫片返回的 electron 对象 —— 把 ipcRenderer / shell / clipboard /
 *        webFrame / webUtils / contextBridge / windowControl 全部映射到
 *        WebView2 的 postMessage 通道上；
 *     3) bootInfo —— preload 顶部用 sendSync('mt::boot-info') 同步取值，
 *        这里直接返回 Qt 预先算好并注入的 window.__mtBridgeConfig.bootInfo。
 *   于是 preload 原文（含 contextBridge.exposeInMainWorld 的全部调用）能原样跑完，
 *   渲染层看到的 window.electron / window.fileUtils / window.path ... 与在 Electron
 *   下完全一致。
 *
 * 通信协议（与 Qt 侧 src/marktextbridge.cpp 一一对应）：
 *   JS → Qt：chrome.webview.postMessage(对象)
 *     { __mt:1, kind:"invoke", id:Number, channel:String, args:Array }   请求-应答
 *     { __mt:1, kind:"send",            channel:String, args:Array }     单向
 *     { __mt:1, kind:"log", level:String, text:String }                  诊断
 *   Qt → JS：PostWebMessageAsJson
 *     { __mt:1, kind:"reply", id:Number, ok:Bool, value:Any, error:String }
 *     { __mt:1, kind:"emit",  channel:String, args:Array }               主进程 → 渲染层
 *
 * 维护约定：本文件只写「环境」层，不写业务；任何 MarkText 的业务语义都应留在
 *           preload/index.js 与渲染层里，以保证「里面实现尽量不变」。
 */

/* eslint-disable no-console */
(function () {
    'use strict';

    if (window.__mtBridgeInstalled) {
        return;
    }
    window.__mtBridgeInstalled = true;

    var cfg = window.__mtBridgeConfig || {};
    var bootInfo = cfg.bootInfo || {};

    //=====================================================================
    //  0. 二进制编解码
    //  JSON 通道传不了 Uint8Array：约定 [ {__mtBinary:"<base64>"} ] 形式往返，
    //  由本层透明转换，渲染层与 preload 都无需感知。
    //=====================================================================
    function bytesToBase64(bytes) {
        var s = '';
        for (var i = 0; i < bytes.length; i++) {
            s += String.fromCharCode(bytes[i]);
        }
        return btoa(s);
    }

    function base64ToBytes(b64) {
        var s = atob(b64);
        var out = new Uint8Array(s.length);
        for (var i = 0; i < s.length; i++) {
            out[i] = s.charCodeAt(i);
        }
        return out;
    }

    function encodeValue(v) {
        if (v === null || v === undefined) {
            return v;
        }
        if (v instanceof Uint8Array) {
            return { __mtBinary: bytesToBase64(v) };
        }
        if (typeof ArrayBuffer !== 'undefined' && v instanceof ArrayBuffer) {
            return { __mtBinary: bytesToBase64(new Uint8Array(v)) };
        }
        if (Array.isArray(v)) {
            var arr = new Array(v.length);
            for (var i = 0; i < v.length; i++) {
                arr[i] = encodeValue(v[i]);
            }
            return arr;
        }
        if (typeof v === 'object') {
            // Node Buffer 的 JSON 形态
            if (v.type === 'Buffer' && Array.isArray(v.data)) {
                return { __mtBinary: bytesToBase64(new Uint8Array(v.data)) };
            }
            var o = {};
            for (var k in v) {
                if (Object.prototype.hasOwnProperty.call(v, k)) {
                    o[k] = encodeValue(v[k]);
                }
            }
            return o;
        }
        return v;
    }

    function decodeValue(v) {
        if (v === null || v === undefined) {
            return v;
        }
        if (Array.isArray(v)) {
            for (var i = 0; i < v.length; i++) {
                v[i] = decodeValue(v[i]);
            }
            return v;
        }
        if (typeof v === 'object') {
            if (typeof v.__mtBinary === 'string') {
                return base64ToBytes(v.__mtBinary);
            }
            for (var k in v) {
                if (Object.prototype.hasOwnProperty.call(v, k)) {
                    v[k] = decodeValue(v[k]);
                }
            }
            return v;
        }
        return v;
    }

    //=====================================================================
    //  1. 传输层：postMessage 双向通道
    //=====================================================================
    var webview = window.chrome && window.chrome.webview;
    var g_nSeq = 0;
    var g_pending = Object.create(null);   // id -> {resolve, reject, timer, channel}
    var g_listeners = Object.create(null); // channel -> [fn]
    var g_ready = false;
    var g_queue = [];                      // 页面就绪前的待发消息

    function rawPost(payload) {
        if (!webview) {
            return;
        }
        try {
            webview.postMessage(payload);
        } catch (e) {
            console.error('[mt-bridge] postMessage 失败: ' + e);
        }
    }

    function post(payload) {
        if (!g_ready) {
            g_queue.push(payload);
            return;
        }
        rawPost(payload);
    }

    function flushQueue() {
        var q = g_queue;
        g_queue = [];
        for (var i = 0; i < q.length; i++) {
            rawPost(q[i]);
        }
    }

    function emitLocal(channel, args) {
        var list = g_listeners[channel];
        if (!list) {
            return;
        }
        for (var i = 0; i < list.length; i++) {
            try {
                list[i].apply(null, args);
            } catch (e) {
                console.error('[mt-bridge] 监听器异常 ' + channel + ': ' + (e && e.stack || e));
            }
        }
    }

    function onMessage(event) {
        var msg = event && event.data;
        if (!msg || typeof msg !== 'object' || msg.__mt !== 1) {
            return;
        }
        if (msg.kind === 'reply') {
            var p = g_pending[msg.id];
            if (!p) {
                return;
            }
            delete g_pending[msg.id];
            if (p.timer) {
                clearTimeout(p.timer);
            }
            if (msg.ok) {
                p.resolve(decodeValue(msg.value));
            } else {
                console.error('[mt-bridge] 调用失败 ' + p.channel + ': ' + msg.error);
                p.reject(new Error(msg.error || ('调用失败: ' + p.channel)));
            }
        } else if (msg.kind === 'emit') {
            emitLocal(msg.channel, decodeValue(msg.args || []));
        } else if (msg.kind === 'ready-check') {
            g_ready = true;
            flushQueue();
        }
    }

    if (webview) {
        webview.addEventListener('message', onMessage);
    }

    /// 请求-应答（对应 Electron 的 ipcRenderer.invoke）
    function invoke(channel, args) {
        return new Promise(function (resolve, reject) {
            var id = ++g_nSeq;
            g_pending[id] = {
                resolve: resolve,
                reject: reject,
                channel: channel,
                timer: setTimeout(function () {
                    delete g_pending[id];
                    reject(new Error('调用超时: ' + channel));
                }, 30000)
            };
            post({ __mt: 1, kind: 'invoke', id: id, channel: channel, args: encodeValue(args || []) });
        });
    }

    /// 单向（对应 ipcRenderer.send）
    function send(channel, args) {
        post({ __mt: 1, kind: 'send', channel: channel, args: encodeValue(args || []) });
    }

    /// 同步取（对应 ipcRenderer.sendSync）。WebView2 无同步 IPC，
    /// 故走 Qt 预先注入的应答表；表里没有的通道返回 undefined。
    function sendSync(channel, args) {
        var table = cfg.syncResponses || {};
        if (Object.prototype.hasOwnProperty.call(table, channel)) {
            return table[channel];
        }
        console.warn('[mt-bridge] sendSync 未命中应答表，返回 undefined: ' + channel);
        return undefined;
    }

    //=====================================================================
    //  2. 全局补丁：require / console / 错误捕获
    //=====================================================================
    var electronShim = null;

    globalThis.require = function (name) {
        if (name === 'electron') {
            return electronShim;
        }
        throw new Error('[mt-bridge] 未提供的模块: ' + name);
    };

    var _origError = console.error.bind(console);
    var _origWarn = console.warn.bind(console);
    var _origLog = console.log.bind(console);

    function forward(level, args) {
        var text;
        try {
            text = Array.prototype.map.call(args, function (a) {
                if (typeof a === 'string') {
                    return a;
                }
                if (a instanceof Error) {
                    return a.stack || a.message;
                }
                try {
                    return JSON.stringify(a);
                } catch (e2) {
                    return String(a);
                }
            }).join(' ');
        } catch (e3) {
            text = '(参数序列化失败)';
        }
        post({ __mt: 1, kind: 'log', level: level, text: text });
    }

    console.error = function () { forward('error', arguments); _origError.apply(null, arguments); };
    console.warn = function () { forward('warn', arguments); _origWarn.apply(null, arguments); };
    console.log = function () { forward('info', arguments); _origLog.apply(null, arguments); };

    window.addEventListener('error', function (e) {
        forward('error', ['[window.onerror] ' + (e && e.message) + ' @ ' + (e && e.filename) + ':' + (e && e.lineno)]);
    });
    window.addEventListener('unhandledrejection', function (e) {
        var r = e && e.reason;
        forward('error', ['[unhandledrejection] ' + ((r && r.stack) || r)]);
    });

    //=====================================================================
    //  3. electron 垫片
    //=====================================================================
    var ipcRenderer = {
        invoke: function (channel) {
            return invoke(channel, Array.prototype.slice.call(arguments, 1));
        },
        send: function (channel) {
            send(channel, Array.prototype.slice.call(arguments, 1));
        },
        sendSync: function (channel) {
            return sendSync(channel, Array.prototype.slice.call(arguments, 1));
        },
        on: function (channel, listener) {
            if (!g_listeners[channel]) {
                g_listeners[channel] = [];
            }
            // preload 把 (event, ...args) 透传给业务监听器，事件对象只需最小可用
            var wrapped = function () { listener.apply(null, arguments); };
            wrapped.__mtRaw = listener;
            g_listeners[channel].push(wrapped);
        },
        once: function (channel, listener) {
            var self = this;
            var handler = function () {
                self.removeListener(channel, handler);
                listener.apply(null, arguments);
            };
            this.on(channel, handler);
        },
        removeListener: function (channel, listener) {
            var list = g_listeners[channel];
            if (!list) {
                return;
            }
            for (var i = 0; i < list.length; i++) {
                if (list[i] === listener || list[i].__mtRaw === listener) {
                    list.splice(i, 1);
                    return;
                }
            }
        },
        removeAllListeners: function (channel) {
            if (channel === undefined) {
                g_listeners = Object.create(null);
            } else {
                delete g_listeners[channel];
            }
        }
    };

    /// 事件对象的最小实现（渲染层基本只用它做占位）
    function mkEvent(channel) {
        return { type: 'mt', channel: channel, sender: null, preventDefault: function () {} };
    }

    // 把 emit 分发包装成 (event, ...args) 形态再交给业务监听器
    var _emitRaw = emitLocal;
    emitLocal = function (channel, args) {
        _emitRaw(channel, [mkEvent(channel)].concat(args || []));
    };

    electronShim = {
        ipcRenderer: ipcRenderer,
        shell: {
            openExternal: function (url) { return invoke('mt::shell::open-external', [url]); },
            showItemInFolder: function (fullPath) { send('mt::shell::show-item', [fullPath]); },
            openPath: function (fullPath) { return invoke('mt::shell::open-path', [fullPath]); }
        },
        clipboard: {
            writeText: function (text) { send('mt::clipboard::write-text', [text]); },
            readText: function () { return invoke('mt::clipboard::read-text', []); },
            guessFilePath: function () { return invoke('mt::clipboard::guess-file-path', []); }
        },
        webFrame: {
            setZoomFactor: function (factor) {
                if (typeof factor === 'number' && factor > 0) {
                    document.documentElement.style.zoom = String(factor);
                    send('mt::window-zoom', [{ zoomFactor: factor }]);
                }
            },
            setZoomLevel: function (level) {
                if (typeof level === 'number') {
                    document.documentElement.style.zoom = String(Math.pow(1.2, level));
                    send('mt::window-zoom', [{ zoomLevel: level }]);
                }
            }
        },
        webUtils: {
            // WebView2 的 File 对象不暴露本地路径（Chromium 已移除 file.path）。
            // 这里退化为文件名；真正的拖放路径改由 Qt 侧接管（见 readme 待办）。
            getPathForFile: function (file) {
                if (!file) {
                    return '';
                }
                if (typeof file.path === 'string' && file.path) {
                    return file.path;
                }
                send('mt::window::drop', [{ unsupportedFileObject: true, name: file.name }]);
                return file.name || '';
            }
        },
        contextBridge: {
            exposeInMainWorld: function (key, value) {
                try {
                    window[key] = value;
                } catch (e) {
                    console.error('[mt-bridge] exposeInMainWorld 失败 ' + key + ': ' + e);
                }
            }
        },
        process: {
            platform: bootInfo.platform || 'win32',
            arch: bootInfo.arch || 'x64',
            versions: bootInfo.versions || {},
            env: bootInfo.env || {},
            resourcesPath: (bootInfo.paths || {}).resources,
            cwd: (bootInfo.paths || {}).cwd
        },
        paths: bootInfo.paths || {},
        isUpdatable: !!bootInfo.isUpdatable,
        windowControl: {
            minimize: function () { send('mt::win::minimize', []); },
            maximize: function () { send('mt::win::maximize', []); },
            unmaximize: function () { send('mt::win::unmaximize', []); },
            toggleMaximize: function () { send('mt::win::toggle-maximize', []); },
            close: function () { send('mt::win::close', []); },
            setFullScreen: function (flag) { send('mt::win::set-fullscreen', [flag]); },
            toggleFullScreen: function () { send('mt::win::toggle-fullscreen', []); },
            isMaximized: function () { return invoke('mt::win::is-maximized', []); },
            isFullScreen: function () { return invoke('mt::win::is-fullscreen', []); },
            popupMenu: function (template, position) { send('mt::menu::popup', [template, position]); },
            popupApplicationMenu: function (position) { send('mt::menu::popup-application', [position]); }
        }
    };

    //=====================================================================
    //  4. 就绪握手：等 Qt 侧控制器就绪后再放行队列
    //=====================================================================
    window.__mtBridge = {
        invoke: invoke,
        send: send,
        sendSync: sendSync,
        emit: function (channel, args) {
            emitLocal(channel, args || []);
        },
        isReady: function () { return g_ready; },
        pendingCount: function () { return Object.keys(g_pending).length; }
    };

    // 通知 Qt「垫片已安装」。注意：这两条必须用 rawPost 直发，
    // 不能走带队列的 post()，否则在 Qt 放行前会被压进队列，形成互相等待的死锁。
    rawPost({ __mt: 1, kind: 'log', level: 'info', text: '[mt-bridge] env.js 已安装，等待 Qt 放行' });
    rawPost({ __mt: 1, kind: 'send', channel: 'mt::bridge-env-ready', args: [] });
})();
