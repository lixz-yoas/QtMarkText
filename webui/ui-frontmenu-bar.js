/*
 * ui-frontmenu-bar.js —— 段落前「文字按钮面板」（可实时开关，不改官方文件）
 *
 * ══════════════════════════════════════════════════════════════════
 *  设计要点
 * ══════════════════════════════════════════════════════════════════
 *  1) **默认完全惰性**：本脚本加载后什么都不做 ——
 *     不注入样式、不改 DOM、不起观察器。官方菜单保持 100% 原样。
 *  2) 由「视图 → 段落工具面板」菜单勾选项实时开关，走
 *     window.__QTMT_FRONTBAR__.set(true|false)。
 *  3) **所有样式都限定在 `html.qtmt-frontbar-on` 之下** ——
 *     关闭时连同该 class 一起摘掉，官方菜单立刻恢复原样，
 *     绝不会出现"补丁半生效把官方菜单改坏"的状态。
 *  4) 开启时才镜像面板；镜像面板里的按钮点击**转发给官方 li**，
 *     故「点击切换该段格式」始终是官方行为。
 *
 * ══════════════════════════════════════════════════════════════════
 *  面板内容（取自官方 DOM，零硬编码）
 * ══════════════════════════════════════════════════════════════════
 *  第一行/前段：一级项（创建副本 / 新段落 / 删除）
 *  其后      ：二级项（段落 / 标题 1-6 / 数学公式 / HTML 块 / 代码块 /
 *             列表 / 任务列表 / 各图表 …，随段落类型由官方过滤决定）
 *  按钮 = 图标 + 文字；悬停提示文字与快捷键。
 *
 *  实现方式：官方 DOM 原样保留、逻辑一行不改，只隐藏 → 旁边镜像面板。
 *  镜像面板挂在 .ag-front-menu 容器上，是官方 frontMenuContainer 的**兄弟**，
 *  故不受官方 patch(frontMenuContainer, ...) 重渲染影响。
 */
(function () {
  'use strict';

  /* 已初始化过就不重复注册（页面可能被重载） */
  if (window.__QTMT_FRONTBAR__) {
    return;
  }

  var STYLE_ID = 'qtmt-frontmenu-bar-style';
  var ON_CLASS = 'qtmt-frontbar-on';        // 挂在 <html> 上，所有补丁样式的总开关
  var BOX_CLASS = 'qtmt-frontbar-box';
  var PANEL_CLASS = 'qtmt-panel';
  var MIRROR_ATTR = 'data-qtmt-mirrored';
  var ORIGIN_ATTR = 'data-qtmt-origin';

  /* ── 可调参数 ────────────────────────────────────────────── */
  var GAP_ABOVE = 30;    // 面板下沿与「当前行」上沿的间距（px）
  var GAP_BELOW = 30;    // 上方空间不足、改放下方时与当前行下沿的间距（px）
  var EDGE = 8;          // 距窗口边缘的安全边距（px）
  var MAX_WIDTH = 680;   // 面板最大宽度（px）
  /* ────────────────────────────────────────────────────────── */

  // 所有选择器都以 `html.<ON_CLASS>` 开头 —— 这是"关掉即恢复"的关键。
  var P = 'html.' + ON_CLASS + ' ';

  var CSS = [
    /* 1. 外层浮层盒：背景与阴影交给面板，自身不绘制 */
    P + '.' + BOX_CLASS + ' {',
    '  background: transparent !important;',
    '  box-shadow: none !important;',
    '  border: none !important;',
    '  border-radius: 0 !important;',
    '}',

    /* 2. 容器 = 面板本体 */
    P + '.ag-front-menu {',
    '  width: auto !important;',
    '  max-width: min(' + MAX_WIDTH + 'px, calc(100vw - ' + (EDGE * 2) + 'px)) !important;',
    '  max-height: calc(100vh - ' + (EDGE * 2) + 'px) !important;',
    '  overflow: visible !important;',
    '  box-sizing: border-box !important;',
    '  background: var(--floatBgColor) !important;',
    '  border: 1px solid var(--floatBorderColor) !important;',
    '  border-radius: 8px !important;',
    '  box-shadow: var(--floatShadow) !important;',
    '  padding: 7px 8px !important;',
    '  transform: translate(var(--qtmt-tx, 0px), var(--qtmt-ty, 0px)) !important;',
    '}',

    /* 3. 官方原菜单：只隐藏，不动它。
     *  ★ 必须精确到「官方那个 div」—— 镜像面板也是容器的子 div，
     *    写成 `> div` 会把面板自己也藏掉（r192 就是这么错的）。 */
    P + '.ag-front-menu[' + MIRROR_ATTR + '] > [' + ORIGIN_ATTR + '] {',
    '  display: none !important;',
    '}',

    /* 4. 面板：文字按钮自动换行 */
    P + '.' + PANEL_CLASS + ' {',
    '  display: flex;',
    '  flex-direction: row;',
    '  flex-wrap: wrap;',
    '  align-items: center;',
    '  gap: 6px;',
    '}',
    P + '.' + PANEL_CLASS + ' .qtmt-btn {',
    '  display: inline-flex;',
    '  align-items: center;',
    '  gap: 5px;',
    '  position: relative;',
    '  height: 26px;',
    '  padding: 0 9px;',
    '  box-sizing: border-box;',
    '  border: 1px solid var(--floatBorderColor);',
    '  border-radius: 5px;',
    '  background: transparent;',
    '  color: var(--editorColor);',
    '  font-size: 13px;',
    '  line-height: 1;',
    '  white-space: nowrap;',
    '  cursor: pointer;',
    '  flex: 0 0 auto;',
    '}',
    P + '.' + PANEL_CLASS + ' .qtmt-btn:hover {',
    '  background: var(--floatHoverColor);',
    '  border-color: var(--themeColor);',
    '}',
    /* 一级项用主题色描边区分 */
    P + '.' + PANEL_CLASS + ' .qtmt-btn.qtmt-l1 { border-color: var(--themeColor); }',
    /* 删除项用删除色 */
    P + '.' + PANEL_CLASS + ' .qtmt-btn.qtmt-del { color: var(--deleteColor); }',
    P + '.' + PANEL_CLASS + ' .qtmt-btn.qtmt-del .icon-wrapper { color: var(--deleteColor); }',

    /* 图标：复刻官方 .icon-wrapper 的着色手法 */
    P + '.' + PANEL_CLASS + ' .qtmt-btn .icon-wrapper {',
    '  display: flex;',
    '  width: 15px;',
    '  height: 15px;',
    '  color: var(--iconColor);',
    '  flex: 0 0 auto;',
    '}',
    P + '.' + PANEL_CLASS + ' .qtmt-btn .icon-wrapper i.icon {',
    '  display: flex;',
    '  position: relative;',
    '  width: 15px;',
    '  height: 15px;',
    '  overflow: hidden;',
    '}',
    P + '.' + PANEL_CLASS + ' .qtmt-btn .icon-wrapper i.icon > i[class^=icon-] {',
    '  display: inline-block;',
    '  width: 100%;',
    '  height: 100%;',
    '  filter: drop-shadow(15px 0 currentColor);',
    '  position: relative;',
    '  left: -15px;',
    '}',

    /* 5. 悬停提示：文字 + 快捷键 */
    P + '.' + PANEL_CLASS + ' .qtmt-btn[data-tip]:hover::after {',
    '  content: attr(data-tip);',
    '  position: absolute;',
    '  left: 0;',
    '  bottom: calc(100% + 6px);',
    '  white-space: nowrap;',
    '  padding: 3px 8px;',
    '  border-radius: 5px;',
    '  font-size: 12px;',
    '  background: var(--floatBgColor);',
    '  color: var(--floatFontColor);',
    '  border: 1px solid var(--floatBorderColor);',
    '  box-shadow: var(--floatShadow);',
    '  pointer-events: none;',
    '  z-index: 20;',
    '}'
  ].join('\n');

  var enabled = false;
  var observer = null;
  var scheduled = false;

  /* ================================================================
   *  样式注入 / 移除
   * ================================================================ */

  function injectStyle() {
    if (document.getElementById(STYLE_ID)) {
      return;
    }
    var style = document.createElement('style');
    style.id = STYLE_ID;
    style.textContent = CSS;
    (document.head || document.documentElement).appendChild(style);
  }

  function removeStyle() {
    var el = document.getElementById(STYLE_ID);
    if (el && el.parentNode) {
      el.parentNode.removeChild(el);
    }
  }

  /* ================================================================
   *  工具
   * ================================================================ */

  function labelOf(li) {
    var sp = li.querySelector(':scope > span');
    var key = li.querySelector(':scope > .short-cut span');
    return {
      text: sp ? (sp.textContent || '').trim() : '',
      cut: key ? (key.textContent || '').trim() : ''
    };
  }

  /** 官方那个 div（frontMenuContainer）= 除镜像面板外的那个 div */
  function originContainer(menu) {
    for (var i = 0; i < menu.children.length; i++) {
      var c = menu.children[i];
      if (c.tagName === 'DIV' && !c.classList.contains(PANEL_CLASS)) {
        return c;
      }
    }
    return null;
  }

  /** 一级项 = 顶层 ul 的直接子项 */
  function topItems(menu) {
    var origin = originContainer(menu);
    if (!origin) {
      return [];
    }
    var ul = origin.querySelector(':scope > ul');
    if (!ul) {
      return [];
    }
    var out = [];
    for (var k = 0; k < ul.children.length; k++) {
      var li = ul.children[k];
      if (li.tagName === 'LI' && li.classList.contains('item')) {
        out.push(li);
      }
    }
    return out;
  }

  /** 二级项 = 「转换为」子菜单里的项（官方按段落类型过滤后的那一份）*/
  function subItems(menu) {
    return Array.prototype.slice.call(menu.querySelectorAll('.submenu ul > li.item'));
  }

  function makeButton(li, isLevel1) {
    var info = labelOf(li);
    var btn = document.createElement('div');
    var cls = 'qtmt-btn ' + (isLevel1 ? 'qtmt-l1' : 'qtmt-l2');
    if (li.classList.contains('delete')) {
      cls += ' qtmt-del';
    }
    btn.className = cls;

    var tip = info.cut ? (info.text + '  ' + info.cut) : info.text;
    if (tip) {
      btn.setAttribute('data-tip', tip);
    }

    var iw = li.querySelector('.icon-wrapper');
    if (iw) {
      btn.appendChild(iw.cloneNode(true));
    }

    var span = document.createElement('span');
    span.className = 'qtmt-text';
    span.textContent = info.text;
    btn.appendChild(span);

    btn.addEventListener('mousedown', function (e) {
      e.preventDefault();
      e.stopPropagation();
    });
    btn.addEventListener('click', function (e) {
      e.preventDefault();
      e.stopPropagation();
      try {
        li.click();          // 交给官方处理器 —— 补丁不碰格式逻辑
      } catch (err) {
        /* 转发失败不影响官方菜单 */
      }
    });
    return btn;
  }

  /* ================================================================
   *  构建 / 还原
   * ================================================================ */

  function buildPanel(menu) {
    var tops = topItems(menu);
    if (tops.length === 0) {
      return false;   // 官方还没渲染出内容（尚未打开过）
    }

    var panel = menu.querySelector(':scope > .' + PANEL_CLASS);
    if (!panel) {
      panel = document.createElement('div');
      panel.className = PANEL_CLASS;
      menu.appendChild(panel);
    }
    while (panel.firstChild) {
      panel.removeChild(panel.firstChild);
    }

    // 一级项在前（「转换为」不作按钮：它唯一作用是展开子菜单，
    // 而子菜单已铺进本面板，留着就是点了没反应的死按钮）
    for (var i = 0; i < tops.length; i++) {
      if (tops[i].classList.contains('turnInto')) {
        continue;
      }
      panel.appendChild(makeButton(tops[i], true));
    }
    // 二级项依次在后
    var subs = subItems(menu);
    for (var k = 0; k < subs.length; k++) {
      panel.appendChild(makeButton(subs[k], false));
    }
    if (!panel.firstChild) {
      return false;
    }

    // 面板建好之后才隐藏官方（先建后藏：任何失败都不至于两边都没了）
    menu.setAttribute(MIRROR_ATTR, '1');
    var origin = originContainer(menu);
    if (origin) {
      origin.setAttribute(ORIGIN_ATTR, '1');
      if (origin.style.display !== 'none') {
        origin.style.display = 'none';
      }
    }
    return true;
  }

  /** 把某个菜单还原成官方原样 */
  function restoreMenu(menu) {
    var panel = menu.querySelector(':scope > .' + PANEL_CLASS);
    if (panel && panel.parentNode) {
      panel.parentNode.removeChild(panel);
    }
    menu.removeAttribute(MIRROR_ATTR);
    var origin = menu.querySelector(':scope > [' + ORIGIN_ATTR + ']');
    if (origin) {
      origin.removeAttribute(ORIGIN_ATTR);
      origin.style.display = '';      // 交还给官方样式
    }
  }

  /* ================================================================
   *  定位：不遮当前光标、不超窗口
   * ================================================================ */

  function place(menu) {
    var box = menu.parentNode;                 // .ag-float-wrapper
    if (!box || !box.getBoundingClientRect) {
      return;
    }
    var boxRect = box.getBoundingClientRect();

    // ★ 浮层未定位时官方给的是 top:-1000px / right:-1000px，
    //   此时量不到真实位置，硬算会把面板推飞出屏 —— 直接跳过，
    //   等 popper 改了 wrapper 的 inline style（会再次触发本函数）再算。
    if (boxRect.top < 0 || boxRect.left < 0
        || boxRect.width <= 0 || boxRect.height <= 0) {
      return;
    }

    var menuH = menu.offsetHeight || 0;
    var menuW = menu.offsetWidth || 0;

    var ty = (boxRect.top - menuH - GAP_ABOVE >= EDGE)
             ? -(menuH + GAP_ABOVE)
             : GAP_BELOW;

    var tx = 0;
    var winW = document.documentElement.clientWidth;
    if (boxRect.left < EDGE) {
      tx = EDGE - boxRect.left;
    } else if (boxRect.left + menuW > winW - EDGE) {
      tx = (winW - EDGE) - (boxRect.left + menuW);
    }

    menu.style.setProperty('--qtmt-ty', ty + 'px');
    menu.style.setProperty('--qtmt-tx', tx + 'px');
  }

  /* ================================================================
   *  调度
   * ================================================================ */

  function decorate() {
    if (!enabled) {
      return;
    }
    var menus = document.querySelectorAll('.ag-front-menu');
    for (var i = 0; i < menus.length; i++) {
      var menu = menus[i];
      var box = menu.parentNode;
      if (box && box.classList && !box.classList.contains(BOX_CLASS)) {
        box.classList.add(BOX_CLASS);
      }
      buildPanel(menu);
      place(menu);
    }
  }

  function schedule() {
    if (!enabled || scheduled) {
      return;
    }
    scheduled = true;
    (window.requestAnimationFrame || function (fn) { setTimeout(fn, 16); })(function () {
      scheduled = false;
      try {
        decorate();
      } catch (e) {
        /* 补丁失败不能影响官方功能，静默即可 */
      }
    });
  }

  function notify(text) {
    try {
      window.chrome.webview.postMessage({ __mt: 1, kind: 'log', level: 'info', text: text });
    } catch (e) {
      /* 非 WebView2 环境忽略 */
    }
  }

  /* ================================================================
   *  对外 API（由「视图 → 段落工具面板」菜单调用）
   * ================================================================ */

  function setEnabled(on) {
    on = !!on;
    if (on === enabled) {
      return enabled;
    }
    enabled = on;

    var root = document.documentElement;
    if (on) {
      injectStyle();
      if (root.classList) {
        root.classList.add(ON_CLASS);
      }
      if (!observer) {
        observer = new MutationObserver(schedule);
      }
      var target = document.body || root;
      observer.observe(target, {
        childList: true,
        subtree: true,
        attributes: true,
        attributeFilter: ['class', 'style']
      });
      decorate();
    } else {
      if (observer) {
        observer.disconnect();
      }
      // 先还原 DOM，再摘掉总开关 class 与样式表 —— 顺序保证中途不会露出半成品
      var menus = document.querySelectorAll('.ag-front-menu');
      for (var i = 0; i < menus.length; i++) {
        restoreMenu(menus[i]);
        var box = menus[i].parentNode;
        if (box && box.classList) {
          box.classList.remove(BOX_CLASS);
        }
        // 清掉补丁写过的位移变量，避免残留
        menus[i].style.removeProperty('--qtmt-tx');
        menus[i].style.removeProperty('--qtmt-ty');
      }
      if (root.classList) {
        root.classList.remove(ON_CLASS);
      }
      removeStyle();
    }
    notify('FRONTBAR-' + (on ? 'ON' : 'OFF'));
    return enabled;
  }

  window.__QTMT_FRONTBAR__ = {
    set: setEnabled,
    toggle: function () { return setEnabled(!enabled); },
    isOn: function () { return enabled; }
  };

  // ---- 接 Qt 侧的开关指令（「视图 → 段落工具面板」）----
  // 走 mt:: 消息通道（env.js 把它分发给 ipcRenderer 监听者）。
  // 之所以不用 executeScript：实测那条路在本环境里脚本不执行。
  //
  // ★ 注册时机要小心 ★
  //   window.electron 是 preload 通过 contextBridge.exposeInMainWorld 挂上的
  //   （env.js 的实现就是 `window[key] = value`，同步生效）。
  //   正常情况下本脚本（第 4 个 init 脚本）跑时它已存在；
  //   但为稳妥，这里做两件事：
  //     ① 退路：env.js 早早提供了 globalThis.require('electron')，直接可用；
  //     ② 轮询重试：都拿不到就每 200ms 试一次，最多 30 秒，并记日志。
  var ipcHooked = false;

  function getIpc() {
    try {
      if (window.electron && window.electron.ipcRenderer
          && typeof window.electron.ipcRenderer.on === 'function') {
        return window.electron.ipcRenderer;
      }
    } catch (e) { /* 继续试退路 */ }
    try {
      var e2 = (typeof globalThis !== 'undefined' && globalThis.require)
               ? globalThis.require('electron') : null;
      if (e2 && e2.ipcRenderer && typeof e2.ipcRenderer.on === 'function') {
        return e2.ipcRenderer;
      }
    } catch (e3) { /* 拿不到就返回 null */ }
    return null;
  }

  function hookIpc() {
    if (ipcHooked) {
      return true;
    }
    var ipc = getIpc();
    if (!ipc) {
      return false;
    }
    try {
      ipc.on('mt::frontbar-set', function (event, on) {
        notify('FRONTBAR-CMD:' + (on ? 'on' : 'off'));
        setEnabled(!!on);
      });
      ipcHooked = true;
      notify('FRONTBAR-HOOKED');
      return true;
    } catch (e) {
      return false;
    }
  }

  notify('FRONTBAR-BOOT electron=' + (typeof window.electron)
         + ' req=' + (typeof (globalThis.require))
         + ' chrome=' + (typeof (window.chrome && window.chrome.webview))
         + ' hooked=' + ipcHooked);

  if (!hookIpc()) {
    var nTick = 0;
    var timer = setInterval(function () {
      if (hookIpc()) {
        clearInterval(timer);
      } else if (++nTick > 150) {          // 30 秒后放弃
        clearInterval(timer);
        notify('FRONTBAR-NO-HOOK');
      }
    }, 200);
  }
})();
