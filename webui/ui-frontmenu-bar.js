/*
 * ui-frontmenu-bar.js —— 段落前「文字按钮面板」补丁（不改官方文件）
 *
 * ══════════════════════════════════════════════════════════════════
 *  需求（按用户给的示意图）
 * ══════════════════════════════════════════════════════════════════
 *   1) 点段落前那个按钮时**不再弹菜单**，改为弹出一个**面板**；
 *   2) 面板里是**带文字（并带图标）的圆角按钮**，按顺序排列、放不下自动换行；
 *   3) 顺序：**一级项在前，二级项依次在后**（一级项用主题色边框加以区分）；
 *   4) 鼠标移上去提示**快捷键**；
 *   5) 面板**不遮挡当前光标**；
 *   6) 面板**不超出软件窗口**。
 *
 *  示意图里的按钮（文字按钮，圆角，描边）：
 *      创建副本 | 删除整段 | 段落 | 标题1 | 标题2 | 标题3 | 标题4
 *      标题5 | 数学公式 | HTML块 | 代码块 | 有序列表 | 无序列表 | 任务列表
 *      Vega-Lite图 | 流程图 | 时序图 | PlantUML图 | Mermaid图
 *  注：示意图没画图标，按要求"带上图标"。
 *
 * ══════════════════════════════════════════════════════════════════
 *  做法：官方 DOM 原样保留（逻辑一行不改）→ 隐藏 → 旁边镜像出面板
 * ══════════════════════════════════════════════════════════════════
 *   面板里每个按钮的点击**转发给官方对应的 li**（li.click()），
 *   于是"点击切换该段格式"仍是官方行为，补丁不碰任何格式逻辑。
 *
 *   文字与顺序**全部取自官方 DOM**（官方 li 里的 span / 快捷键 / 图标），
 *   没有任何硬编码 —— 语言切换、菜单项增减都会自动跟上。
 *
 *   为什么不会被官方重渲染冲掉：官方每次打开都
 *   `patch(frontMenuContainer, vnode2)`，但它只 patch **那个内层 div 的子树**；
 *   本面板挂在 `.ag-front-menu` 容器上、是该 div 的**兄弟**，不在 patch 范围内。
 *
 * ══════════════════════════════════════════════════════════════════
 *  默认关闭，用环境变量开启
 * ══════════════════════════════════════════════════════════════════
 *   Qt 侧只在 QTMT_FRONTBAR=1 时才注入本脚本；
 *   不设（默认）则完全不注入，程序就是**官方原样的菜单**。
 *   另：即使注入后本脚本出错，官方菜单也只是"没被隐藏"，
 *       仍可正常使用（见 buildPanel 的「先建后藏」）。
 */
(function () {
  'use strict';

  var STYLE_ID = 'qtmt-frontmenu-bar-style';
  var BOX_CLASS = 'qtmt-frontbar-box';
  var PANEL_CLASS = 'qtmt-panel';
  var MIRROR_ATTR = 'data-qtmt-mirrored';
  var ORIGIN_ATTR = 'data-qtmt-origin';

  /* ── 可调参数（实机看着不合适就改这几个数）────────────────── */
  var GAP_ABOVE = 30;    // 面板下沿与「当前行」上沿的间距（px）
  var GAP_BELOW = 30;    // 上方空间不足、改放下方时与当前行下沿的间距（px）
  var EDGE = 8;          // 距窗口边缘的安全边距（px）
  var MAX_WIDTH = 680;   // 面板最大宽度（px）—— 窄了会多换行，宽了会横跨窗口
  /* ────────────────────────────────────────────────────────── */

  var CSS = [
    /* ---- 1. 外层浮层盒：背景与阴影交给面板，自身不绘制 ---- */
    '.' + BOX_CLASS + ' {',
    '  background: transparent !important;',
    '  box-shadow: none !important;',
    '  border: none !important;',
    '  border-radius: 0 !important;',
    '}',

    /* ---- 2. 容器 = 面板本体 ---- */
    '.ag-front-menu {',
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
    '  /* 位移由 JS 写 --qtmt-tx / --qtmt-ty（横向夹取 + 纵向上/下）*/',
    '  transform: translate(var(--qtmt-tx, 0px), var(--qtmt-ty, 0px)) !important;',
    '}',

    /* ---- 3. 官方原菜单：只隐藏，不动它 ----
     *  ★ 必须精确到「官方那个 div」★
     *  镜像面板 .qtmt-panel 也是容器的直接子 div，
     *  若写成 `.ag-front-menu > div` 会把面板自己也藏掉（r192 就是这么错的）。
     *  故按 data-qtmt-origin 标记隐藏；JS 侧另置 inline display:none 兜底。 */
    '.ag-front-menu[' + MIRROR_ATTR + '] > [' + ORIGIN_ATTR + '] { display: none !important; }',

    /* ---- 4. 面板：文字按钮自动换行 ---- */
    '.' + PANEL_CLASS + ' {',
    '  display: flex;',
    '  flex-direction: row;',
    '  flex-wrap: wrap;',
    '  align-items: center;',
    '  gap: 6px;',
    '}',
    '.' + PANEL_CLASS + ' .qtmt-btn {',
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
    '.' + PANEL_CLASS + ' .qtmt-btn:hover {',
    '  background: var(--floatHoverColor);',
    '  border-color: var(--themeColor);',
    '}',
    /* 一级项用主题色描边区分（示意图里前两项是不同颜色） */
    '.' + PANEL_CLASS + ' .qtmt-btn.qtmt-l1 { border-color: var(--themeColor); }',
    /* 删除段落用删除色 */
    '.' + PANEL_CLASS + ' .qtmt-btn.qtmt-del { color: var(--deleteColor); }',
    '.' + PANEL_CLASS + ' .qtmt-btn.qtmt-del .icon-wrapper { color: var(--deleteColor); }',

    /* 图标：复刻官方 .icon-wrapper 的着色手法（clone 过来的是原样结构）*/
    '.' + PANEL_CLASS + ' .qtmt-btn .icon-wrapper {',
    '  display: flex;',
    '  width: 15px;',
    '  height: 15px;',
    '  color: var(--iconColor);',
    '  flex: 0 0 auto;',
    '}',
    '.' + PANEL_CLASS + ' .qtmt-btn .icon-wrapper i.icon {',
    '  display: flex;',
    '  position: relative;',
    '  width: 15px;',
    '  height: 15px;',
    '  overflow: hidden;',
    '}',
    '.' + PANEL_CLASS + ' .qtmt-btn .icon-wrapper i.icon > i[class^=icon-] {',
    '  display: inline-block;',
    '  width: 100%;',
    '  height: 100%;',
    '  filter: drop-shadow(15px 0 currentColor);',
    '  position: relative;',
    '  left: -15px;',
    '}',

    /* ---- 5. 悬停提示：文字 + 快捷键 ---- */
    '.' + PANEL_CLASS + ' .qtmt-btn[data-tip]:hover::after {',
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

  /* ================================================================
   *  工具
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

  /** 取某项的文字与快捷键（官方 DOM：> span / > .short-cut span） */
  function labelOf(li) {
    var sp = li.querySelector(':scope > span');
    var key = li.querySelector(':scope > .short-cut span');
    return {
      text: sp ? (sp.textContent || '').trim() : '',
      cut: key ? (key.textContent || '').trim() : ''
    };
  }

  /** 一级项 = 顶层 ul 的直接子项（官方在容器里多套了一层 div）*/
  function topItems(menu) {
    for (var i = 0; i < menu.children.length; i++) {
      var c = menu.children[i];
      if (c.tagName !== 'DIV' || c.classList.contains(PANEL_CLASS)) {
        continue;
      }
      var ul = c.querySelector(':scope > ul');
      if (!ul) {
        continue;
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
    return [];
  }

  /** 二级项 = 「转换为」子菜单里的项 */
  function subItems(menu) {
    return Array.prototype.slice.call(
        menu.querySelectorAll('.submenu ul > li.item'));
  }

  /** 用官方 li 造一个「图标 + 文字」按钮；点击转发给官方 li */
  function makeButton(li, isLevel1) {
    var info = labelOf(li);
    var btn = document.createElement('div');
    var cls = 'qtmt-btn ' + (isLevel1 ? 'qtmt-l1' : 'qtmt-l2');
    if (li.classList.contains('delete')) {
      cls += ' qtmt-del';
    }
    btn.className = cls;

    // 悬停提示 = 文字 + 快捷键
    var tip = info.cut ? (info.text + '  ' + info.cut) : info.text;
    if (tip) {
      btn.setAttribute('data-tip', tip);
    }

    // 图标：直接 clone 官方的 .icon-wrapper（data-URI 图形与官方一致）
    var iw = li.querySelector('.icon-wrapper');
    if (iw) {
      btn.appendChild(iw.cloneNode(true));
    }

    // 文字
    var span = document.createElement('span');
    span.className = 'qtmt-text';
    span.textContent = info.text;
    btn.appendChild(span);

    // 不改逻辑：点击直接触发官方 li 的 click 处理器
    btn.addEventListener('mousedown', function (e) {
      e.preventDefault();
      e.stopPropagation();
    });
    btn.addEventListener('click', function (e) {
      e.preventDefault();
      e.stopPropagation();
      try {
        li.click();
      } catch (err) {
        /* 转发失败不影响官方菜单本身 */
      }
    });
    return btn;
  }

  /** 取容器里「官方那个 div」（frontMenuContainer）= 除镜像面板外的那个 div */
  function originContainer(menu) {
    for (var i = 0; i < menu.children.length; i++) {
      var c = menu.children[i];
      if (c.tagName === 'DIV' && !c.classList.contains(PANEL_CLASS)) {
        return c;
      }
    }
    return null;
  }

  /** 重建镜像面板 */
  function buildPanel(menu) {
    var tops = topItems(menu);
    if (tops.length === 0) {
      return;   // 官方还没渲染出内容（尚未打开过），先不动
    }

    var panel = menu.querySelector(':scope > .' + PANEL_CLASS);
    if (!panel) {
      panel = document.createElement('div');
      panel.className = PANEL_CLASS;
      // 挂在容器上（与官方 frontMenuContainer 平级），
      // 故不受官方 patch(frontMenuContainer, ...) 影响
      menu.appendChild(panel);
    }
    while (panel.firstChild) {
      panel.removeChild(panel.firstChild);
    }

    // ---- 一级项在前 ----
    // 「转换为」原本唯一的作用就是展开子菜单，而子菜单已平铺到本面板里，
    // 故不把它做成按钮（点了不会有反应），它的内容都在二级项里。
    for (var i = 0; i < tops.length; i++) {
      if (tops[i].classList.contains('turnInto')) {
        continue;
      }
      panel.appendChild(makeButton(tops[i], true));
    }

    // ---- 二级项依次在后 ----
    var subs = subItems(menu);
    for (var k = 0; k < subs.length; k++) {
      panel.appendChild(makeButton(subs[k], false));
    }

    // ---- 面板建好之后，才隐藏官方原菜单 ----
    // ★ 先建后藏：若建面板途中抛异常，官方菜单仍可见可用，
    //   不会出现"两边都没了"。
    if (panel.firstChild) {
      menu.setAttribute(MIRROR_ATTR, '1');
      var origin = originContainer(menu);
      if (origin) {
        origin.setAttribute(ORIGIN_ATTR, '1');
        if (origin.style.display !== 'none') {
          origin.style.display = 'none';
        }
      }
    }
  }

  /* ================================================================
   *  定位：不遮当前光标、不超窗口
   * ================================================================ */

  function place(menu) {
    var box = menu.parentNode;                  // .ag-float-wrapper
    if (!box || !box.getBoundingClientRect) {
      return;
    }
    var boxRect = box.getBoundingClientRect();

    // ★ 浮层未定位时官方给了 top:-1000 / right:-1000（见 .ag-float-wrapper 默认值）。
    //   此时量不到真实位置，硬算会把面板推飞 —— 直接跳过，等 popper 定位后再算
    //   （popper 会改 wrapper 的 inline style，会再次触发本函数）。
    if (boxRect.top < 0 || boxRect.left < 0
        || boxRect.width <= 0 || boxRect.height <= 0) {
      return;
    }

    var menuH = menu.offsetHeight || 0;
    var menuW = menu.offsetWidth || 0;

    // 纵向：默认整块上移到当前行之上；上方不够就改放下方
    var ty = (boxRect.top - menuH - GAP_ABOVE >= EDGE)
             ? -(menuH + GAP_ABOVE)
             : GAP_BELOW;

    // 横向：若会超出窗口左右就往回收
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
    injectStyle();
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

  var scheduled = false;
  function schedule() {
    if (scheduled) {
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
      /* 非 WebView2 环境（如浏览器预览）忽略 */
    }
  }

  function boot() {
    injectStyle();
    var target = document.body || document.documentElement;
    if (!target) {
      setTimeout(boot, 30);
      return;
    }
    new MutationObserver(schedule).observe(target, {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: ['class', 'style']
    });
    schedule();
    notify('FRONTBAR-LOADED: 段落前文字按钮面板已注入');
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
