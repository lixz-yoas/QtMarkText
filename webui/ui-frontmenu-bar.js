/*
 * ui-frontmenu-bar.js —— 段落前「悬浮工具栏」补丁（不改官方文件）
 *
 * 需求：
 *   1) 每个段落前面那个按钮点开的弹出菜单，改成横向「悬浮栏按钮」；
 *   2) 鼠标移上去显示 tip（含文字与快捷键）；
 *   3) 点击即切换该段格式（不用菜单方式）；
 *   4) 悬浮栏不得超出本软件窗口；
 *   5) 悬浮栏不得覆盖当前所在行；
 *   6) 不按当前段落格式显示「按下」状态；
 *   7) 一级项在前、二级项依次在后，中间用分隔符隔开。
 *
 * 做法：**不修改官方 bundle**，只注入一段样式 + 少量 JS 适配。
 *   官方 FrontMenu 的 DOM 结构（见 index-B-PE276Q.js 的 FrontMenu 类）：
 *     .ag-float-wrapper            <- popper 定位的浮层盒（带背景/阴影）
 *       .ag-front-menu             <- 容器（官方给了 width:190px）
 *         ul
 *           li.item.duplicate      <- 一级：创建副本
 *           li.item.turnInto       <- 一级：转换为（内含二级）
 *             div.submenu > ul > li.item.<类型>   <- 二级
 *           li.item.new            <- 一级：新建段落
 *           li.item.delete         <- 一级：删除段落
 *   每项内有 div.icon-wrapper / span(文字) / div.short-cut(快捷键)。
 *
 *   于是纯 CSS 就能完成绝大部分改造：
 *     · 容器改横向工具栏   -> flex-direction: row
 *     · 只留图标，文字与快捷键挪进 tip
 *     · 用 `display: contents` 把 li.turnInto > .submenu > ul 拍平，
 *       让二级项成为同一个 flex 容器的子项，再用 `order` 把二级项
 *       排到所有一级项之后、并在中间画分隔符（无需移动 DOM，
 *       也就不怕官方每次重渲染把 DOM 改回去）
 *     · 去掉 .active 高亮
 *   JS 只做两件 CSS 做不到的事：
 *     · 给每项写 data-tip（文字 + 快捷键），供 ::after 渲染 tip
 *     · 判断上方是否有空间：不够就把悬浮栏放到下方（避免超出窗口上沿）
 *
 * 挂在 .ag-float-wrapper 上的 box 类名由本脚本添加，避免影响其它浮层。
 */
(function () {
  'use strict';

  var STYLE_ID = 'qtmt-frontmenu-bar-style';
  var BOX_CLASS = 'qtmt-frontbar-box';

  /* ---- 位置微调：这两个数值是视觉参数，实机看着不合适就改这里 ---- */
  // 悬浮栏底部与「当前行」上沿的间距（px）
  var GAP_ABOVE = 34;
  // 上方空间不足、改放下方时，与当前行下沿的间距（px）
  var GAP_BELOW = 30;
  // 距离窗口边缘的安全边距（px）
  var EDGE = 8;

  var CSS = [
    /* 1. 外层浮层盒：背景与阴影交给内层容器，自身不绘制 */
    '.' + BOX_CLASS + ' {',
    '  background: transparent !important;',
    '  box-shadow: none !important;',
    '  border: none !important;',
    '  border-radius: 0 !important;',
    '}',

    /* 2. 容器：横向工具栏，自行承载背景/边框 */
    '.ag-front-menu {',
    '  width: auto !important;',
    '  max-width: calc(100vw - ' + (EDGE * 2) + 'px) !important;',
    '  max-height: calc(100vh - ' + (EDGE * 2) + 'px) !important;',
    '  overflow: auto !important;',
    '  box-sizing: border-box !important;',
    '  background: var(--floatBgColor) !important;',
    '  border: 1px solid var(--floatBorderColor) !important;',
    '  border-radius: 8px !important;',
    '  box-shadow: var(--floatShadow) !important;',
    '  padding: 4px 6px !important;',
    '}',
    /* 位置：默认放到当前行上方；JS 判定空间不足时改为下方 */
    '.ag-front-menu[data-qtmt-pos="above"] { transform: translateY(calc(-100% - ' + GAP_ABOVE + 'px)) !important; }',
    '.ag-front-menu[data-qtmt-pos="below"] { transform: translateY(' + GAP_BELOW + 'px) !important; }',

    /* 3. 行内排列；放不下就换行（因此不会横向超出窗口） */
    '.ag-front-menu {',
    '  display: flex !important;',
    '  flex-direction: row !important;',
    '  flex-wrap: wrap !important;',
    '  align-items: center !important;',
    '  padding: 0 !important;',
    '}',

    /* 4. 只显示图标；文字与快捷键移入 tip */
    '.ag-front-menu li.item > span,',
    '.ag-front-menu li.item > .short-cut { display: none !important; }',
    '.ag-front-menu li.item {',
    '  position: relative !important;',
    '  height: 30px !important;',
    '  padding: 0 7px !important;',
    '  border-radius: 6px !important;',
    '  align-items: center !important;',
    '}',
    '.ag-front-menu li.item .icon-wrapper { margin: 0 !important; }',

    /* 5. 不按当前段落格式显示「按下」状态 */
    '.ag-front-menu li.active { background: transparent !important; }',
    '.ag-front-menu li.item.active:hover { background: var(--floatHoverColor) !important; }',

    /* 6. 一级在前、二级在后，中间加分隔符 */
    /*   拍平「转换为」的层级，让二级项进入同一个 flex 容器 */
    '.ag-front-menu li.item.turnInto { display: contents !important; }',
    '.ag-front-menu li.item.turnInto > .icon-wrapper,',
    '.ag-front-menu li.item.turnInto > span,',
    '.ag-front-menu li.item.turnInto > .short-cut { display: none !important; }',
    '.ag-front-menu li.item.turnInto > .submenu { display: contents !important; }',
    '.ag-front-menu .submenu {',
    '  position: static !important;',
    '  width: auto !important;',
    '  background: transparent !important;',
    '  box-shadow: none !important;',
    '  border: none !important;',
    '}',
    '.ag-front-menu .submenu ul { display: contents !important; }',
    /* 二级项统一排到一级项之后（DOM 顺序即「依次」） */
    '.ag-front-menu .submenu ul > li.item { order: 100 !important; }',
    /* 一级项顺序：创建副本 → 新建段落 → 删除段落 */
    '.ag-front-menu li.item.duplicate { order: 1 !important; }',
    '.ag-front-menu li.item.new { order: 2 !important; }',
    '.ag-front-menu li.item.delete { order: 3 !important; }',
    /* 分隔符：画在最后一级项（删除段落）的右侧 */
    '.ag-front-menu li.item.delete {',
    '  border-right: 1px solid var(--floatBorderColor) !important;',
    '  padding-right: 11px !important;',
    '  margin-right: 6px !important;',
    '}',

    /* 7. tip：文字 + 快捷键，悬停时显示在按钮上方 */
    '.ag-front-menu li.item[data-tip]:hover::after {',
    '  content: attr(data-tip);',
    '  position: absolute;',
    '  left: 50%;',
    '  bottom: calc(100% + 8px);',
    '  transform: translateX(-50%);',
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

  function injectStyle() {
    if (document.getElementById(STYLE_ID)) {
      return;
    }
    var style = document.createElement('style');
    style.id = STYLE_ID;
    style.textContent = CSS;
    (document.head || document.documentElement).appendChild(style);
  }

  /* 给一项写 data-tip：取项内 span 的文字与 .short-cut 里的快捷键 */
  function tagItem(li) {
    var label = li.querySelector(':scope > span');
    var key = li.querySelector(':scope > .short-cut span');
    var text = label ? (label.textContent || '').trim() : '';
    var cut = key ? (key.textContent || '').trim() : '';
    if (!text) {
      return;
    }
    var tip = cut ? text + '  ' + cut : text;
    if (li.getAttribute('data-tip') !== tip) {
      li.setAttribute('data-tip', tip);
    }
  }

  /* 决定悬浮栏放在当前行上方还是下方，保证不遮当前行、不出窗口上沿 */
  function place(menu) {
    var box = menu.parentNode;
    if (!box || !box.getBoundingClientRect) {
      return;
    }
    var boxRect = box.getBoundingClientRect();
    var menuH = menu.offsetHeight || 0;
    // boxRect.top 是「未施加 transform 时」悬浮栏的顶边，
    // 也就是 popper 默认放在当前行下方 10px 处的位置。
    var roomAbove = boxRect.top - menuH - GAP_ABOVE;
    var pos = roomAbove >= EDGE ? 'above' : 'below';
    if (menu.getAttribute('data-qtmt-pos') !== pos) {
      menu.setAttribute('data-qtmt-pos', pos);
    }
  }

  function decorate() {
    injectStyle();
    var menus = document.querySelectorAll('.ag-front-menu');
    for (var i = 0; i < menus.length; i++) {
      var menu = menus[i];
      var box = menu.parentNode;
      if (box && box.classList && !box.classList.contains(BOX_CLASS)) {
        box.classList.add(BOX_CLASS);
      }
      var items = menu.querySelectorAll('li.item');
      for (var j = 0; j < items.length; j++) {
        tagItem(items[j]);
      }
      place(menu);
    }
  }

  var scheduled = false;
  function schedule() {
    if (scheduled) {
      return;
    }
    scheduled = true;
    // 用 rAF 合并同一帧内的多次变动，避免频繁重排
    (window.requestAnimationFrame || function (fn) { setTimeout(fn, 16); })(function () {
      scheduled = false;
      try {
        decorate();
      } catch (e) {
        /* 补丁失败不影响官方功能，静默即可 */
      }
    });
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
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
