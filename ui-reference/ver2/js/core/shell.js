/**
 * 冰箱小精灵 ver2 UI 预览 —— 设备外壳 + 预览工具栏
 * ------------------------------------------------------------------
 * 职责：
 *  - 设备舞台缩放（适应窗口 / 100%）；横屏 1024×600、竖屏 600×1024；
 *  - 外壳：横屏 main|nav|cabin；竖屏 上主区 / 中导航 / 下状态舱；
 *  - 系统页模式（SYS-*）：隐藏导航；
 *  - 预览工具栏：视图/布局/页面/状态/侧舱/缩放/角标 + 浮层 + 流程；
 *  - 全局事件委托：data-act → FS.actions；Esc 先关浮层再退页。
 */
window.FS = window.FS || {};

FS.shell = (function () {
  var view = 'device';           // device | gallery
  var zoom = 'fit';              // fit | 100
  var els = {};

  var NAV = [
    { m: '首页', icon: 'home' },
    { m: '库存', icon: 'box' },
    { m: '对话', icon: 'chat' },
    { m: '计划', icon: 'plan' },
    { m: '更多', icon: 'more' }
  ];

  /* ---------------- 舞台缩放（按当前布局宽高） ---------------- */
  function fit() {
    if (!els.viewport || !els.stage || !els.wrap) return;
    var size = FS.mode ? FS.mode.getSize() : { w: 1024, h: 600 };
    var availW = els.viewport.clientWidth - 48;
    var availH = els.viewport.clientHeight - 48;
    var s = zoom === 'fit' ? Math.min(availW / size.w, availH / size.h) : 1;
    s = Math.max(0.15, s);
    els.stage.style.transform = 'scale(' + s + ')';
    els.wrap.style.width = (size.w * s) + 'px';
    els.wrap.style.height = (size.h * s) + 'px';
  }

  function updateLayoutChrome() {
    if (!els.toolbar) return;
    var layout = FS.mode ? FS.mode.getLayout() : 'landscape';
    els.toolbar.querySelectorAll('[data-act="shell:layout"]').forEach(function (b) {
      b.classList.toggle('on', b.getAttribute('data-arg') === layout);
    });
    var note = document.getElementById('tb-note');
    if (note) {
      note.textContent = 'ver2 · ' + (FS.mode ? FS.mode.label() : '1024×600');
    }
    /* 竖屏隐私：禁用拍照登记流程按钮 */
    var photoBtn = els.toolbar.querySelector('[data-act="shell:flow"][data-arg="photo"]');
    if (photoBtn) {
      var priv = FS.mode && FS.mode.isPrivacy();
      photoBtn.disabled = !!priv;
      photoBtn.title = priv ? '隐私模式已关闭摄像头' : '拍照登记流程演示';
      photoBtn.classList.toggle('tb-disabled', !!priv);
    }
  }

  /* ---------------- 侧导航 / 底导航（同一 DOM，CSS 决定方向） ---------------- */
  function renderNav() {
    var cur = FS.router.moduleOf(FS.router.currentId());
    var alarm = FS.data.safetyEvents.filter(function (e) { return !e.confirmed; }).length;
    els.sidenav.innerHTML = NAV.map(function (n) {
      return '<button class="nav-item' + (n.m === cur ? ' on' : '') + '" data-act="shell:nav" data-arg="' + n.m + '">' +
        FS.icon(n.icon) + '<span>' + n.m + '</span>' +
        (n.m === '更多' && alarm ? '<span class="nav-badge">' + alarm + '</span>' : '') +
        '</button>';
    }).join('');
  }
  FS.action('shell:nav', function (el) { FS.router.goModule(el.getAttribute('data-arg')); });

  /* ---------------- 页面渲染 ---------------- */
  function renderPage() {
    var def = FS.router.currentPage();
    if (!def) return;
    els.stage.classList.toggle('mode-system', !!def.system);
    if (FS.mode) FS.mode.applyDom();
    renderNav();
    var pg = { id: def.id, params: FS.router.currentParams(), viewState: FS.states.current(), page: def };
    els.main.innerHTML = def.render(pg);
    if (def.onMount) def.onMount(els.main, pg);
    FS.cabin.refresh();
    var sel = document.getElementById('tb-page');
    if (sel && sel.value !== def.id) sel.value = def.id;
  }

  /* ---------------- 返回（先关浮层 → 模块内上一级 → 模块根页） ---------------- */
  function goBack() {
    if (FS.overlays.top()) { FS.overlays.close(); return; }
    FS.router.back();
  }
  FS.action('back', function () { goBack(); });
  FS.action('nav', function (el) {
    var href = el.getAttribute('data-href');
    if (href) FS.router.go(href);
  });

  /* ---------------- 工具栏 ---------------- */
  function tbBtn(label, act, arg, cls) {
    return '<button class="tb-btn ' + (cls || '') + '" data-act="' + act + '"' +
      (arg !== undefined ? ' data-arg="' + arg + '"' : '') + '>' + label + '</button>';
  }

  function buildToolbar() {
    var pages = FS.router.list();
    var modules = [];
    pages.forEach(function (p) { if (modules.indexOf(p.module) < 0) modules.push(p.module); });
    var pageOpts = modules.map(function (m) {
      return '<optgroup label="' + m + '">' + pages.filter(function (p) { return p.module === m; })
        .map(function (p) { return '<option value="' + p.id + '">' + p.id + ' ' + p.name + '</option>'; }).join('') + '</optgroup>';
    }).join('');

    var stateOpts = FS.states.list().map(function (s) {
      return '<option value="' + s.id + '">' + s.label + '</option>';
    }).join('');

    var ctxOpts = '<option value="auto">自动跟随</option>' + FS.cabin.defs.map(function (c) {
      return '<option value="' + c.id + '">' + c.id + ' ' + c.name + '</option>';
    }).join('');

    var OVL_SHORT = {
      'OV-01': '轻提示', 'OV-02': '通知条', 'OV-03': '确认框',
      'OV-04': '高风险确认', 'OV-05': '语音层', 'OV-06': 'Agent状态',
      'OV-07': '拍照进度', 'OV-08': '同步/离线', 'OV-09': '到时提醒',
      'OV-10': '安全告警', 'OV-11': '快速添加', 'OV-12': '软键盘'
    };
    var ovlBtns = FS.overlays.list().map(function (o) {
      var num = o.id.replace('OV-', '');
      var short = OVL_SHORT[o.id] || o.name;
      var label = '<span class="tb-ovl-num">' + num + '</span> ' + short + (o.blocking ? ' ·阻' : '');
      var cls = 'tb-ovl' + (o.blocking ? ' tb-blocking' : '');
      return '<button class="tb-btn ' + cls + '" data-act="shell:ovl" data-arg="' + o.id +
        '" title="' + o.id + ' ' + o.name + (o.blocking ? '（阻塞）' : '') + '">' + label + '</button>';
    }).join('');

    var layout = FS.mode ? FS.mode.getLayout() : 'landscape';

    els.toolbar.innerHTML =
      /* 行1：视图 + 布局 + 页面导航 + 演示态控制 */
      '<div class="tb-row">' +
        '<span class="tb-group">' +
          tbBtn('设备预览', 'shell:view', 'device', 'on') +
          tbBtn('页面总览', 'shell:view', 'gallery') +
        '</span>' +
        '<span class="tb-sep"></span>' +
        '<span class="tb-group">' +
          '<span class="tb-label">布局</span>' +
          tbBtn('横屏标准', 'shell:layout', 'landscape', layout === 'landscape' ? 'on' : '') +
          tbBtn('竖屏隐私', 'shell:layout', 'portrait', layout === 'portrait' ? 'on' : '') +
        '</span>' +
        '<span class="tb-sep"></span>' +
        '<span class="tb-group"><span class="tb-label">页面</span>' +
          tbBtn('‹', 'shell:sibling', '-1') +
          '<select class="tb-select" id="tb-page">' + pageOpts + '</select>' +
          tbBtn('›', 'shell:sibling', '1') + '</span>' +
        '<span class="tb-group"><span class="tb-label">状态</span><select class="tb-select" id="tb-state">' + stateOpts + '</select></span>' +
        '<span class="tb-group"><span class="tb-label">侧舱</span><select class="tb-select" id="tb-ctx">' + ctxOpts + '</select></span>' +
        '<span class="tb-group"><span class="tb-label">缩放</span><select class="tb-select" id="tb-zoom">' +
          '<option value="fit">适应窗口</option><option value="100">100%</option></select></span>' +
        tbBtn('页面角标', 'shell:pgid', '', 'on') +
        tbBtn('返回', 'shell:back') +
        '<span class="tb-note" id="tb-note">ver2 · ' + (FS.mode ? FS.mode.label() : '1024×600') + '</span>' +
      '</div>' +
      /* 行2：12 个浮层触发 */
      '<div class="tb-row">' +
        '<span class="tb-label">浮层</span>' +
        '<div class="tb-ovl-grid">' + ovlBtns + '</div>' +
        tbBtn('关闭全部', 'shell:ovl-close-all') +
      '</div>' +
      /* 行3：关键流程演示 */
      '<div class="tb-row">' +
        '<span class="tb-label">流程</span>' +
        '<div class="tb-flow-group">' +
          tbBtn('拍照登记', 'shell:flow', 'photo') +
          tbBtn('语音登记', 'shell:flow', 'voice') +
          tbBtn('Agent高风险', 'shell:flow', 'agent') +
          tbBtn('烹饪+计时', 'shell:flow', 'cook') +
          tbBtn('安全告警', 'shell:flow', 'safety', 'warn-on') +
          tbBtn('停止演示', 'shell:flow-stop') +
        '</div>' +
      '</div>';

    document.getElementById('tb-page').addEventListener('change', function () {
      FS.router.goPage(this.value);
    });
    document.getElementById('tb-state').addEventListener('change', function () {
      FS.states.set(this.value);
    });
    document.getElementById('tb-ctx').addEventListener('change', function () {
      FS.cabin.setMode(this.value);
    });
    document.getElementById('tb-zoom').addEventListener('change', function () {
      zoom = this.value; fit();
    });
    updateLayoutChrome();
  }

  /* 工具栏动作 */
  FS.action('shell:view', function (el) {
    view = el.getAttribute('data-arg');
    els.toolbar.querySelectorAll('[data-act="shell:view"]').forEach(function (b) {
      b.classList.toggle('on', b.getAttribute('data-arg') === view);
    });
    var isGal = view === 'gallery';
    els.wrap.style.display = isGal ? 'none' : '';
    els.gallery.style.display = isGal ? '' : 'none';
    if (isGal) FS.gallery.render(els.gallery);
    fit();
  });
  FS.action('shell:layout', function (el) {
    var next = el.getAttribute('data-arg');
    if (!FS.mode) return;
    FS.mode.setLayout(next);
  });
  FS.action('shell:sibling', function (el) { FS.router.sibling(parseInt(el.getAttribute('data-arg'), 10)); });
  FS.action('shell:pgid', function (el) {
    els.stage.classList.toggle('hide-pgid');
    el.classList.toggle('on');
  });
  FS.action('shell:back', function () { goBack(); });
  FS.action('shell:ovl', function (el) { FS.overlays.show(el.getAttribute('data-arg')); });
  FS.action('shell:ovl-close-all', function () { FS.overlays.closeAll(); });
  FS.action('shell:flow', function (el) { FS.flows.start(el.getAttribute('data-arg')); });
  FS.action('shell:flow-stop', function () { FS.flows.stop(); });

  /* ---------------- 全局事件委托 ---------------- */
  function onClick(ev) {
    var el = ev.target.closest('[data-act]');
    if (!el) return;
    /* 禁用按钮不响应（隐私模式拍照流程等） */
    if (el.disabled || el.getAttribute('aria-disabled') === 'true') {
      var act = el.getAttribute('data-act');
      if (act === 'cabin:camera' || (act === 'shell:flow' && el.getAttribute('data-arg') === 'photo')) {
        if (FS.mode && FS.mode.isPrivacy()) FS.mode.privacyCameraToast();
      }
      return;
    }
    var act = el.getAttribute('data-act');
    if (FS.actions[act]) { FS.actions[act](el, ev); }
  }

  function onKey(ev) {
    if (ev.key === 'Escape') goBack();
  }

  function onLayoutChange() {
    updateLayoutChrome();
    /* 若当前在拍照页且切到隐私，router 会在下次 go/sync 拦截；这里主动退回 */
    var id = FS.router.currentId();
    if (FS.mode && FS.mode.isPrivacy() && (id === 'INV-05' || id === 'INV-06')) {
      FS.mode.privacyCameraToast();
      FS.router.goPage('INV-01');
      return;
    }
    renderPage();
    fit();
  }

  /* ---------------- 启动 ---------------- */
  function start() {
    els = {
      toolbar: document.getElementById('toolbar'),
      viewport: document.getElementById('viewport'),
      wrap: document.getElementById('stage-wrap'),
      stage: document.getElementById('stage'),
      main: document.getElementById('main-area'),
      sidenav: document.getElementById('sidenav'),
      cabin: document.getElementById('cabin'),
      gallery: document.getElementById('gallery-root')
    };
    if (FS.mode) {
      FS.mode.init();
      FS.mode.onChange(onLayoutChange);
    }
    buildToolbar();
    FS.cabin.mount(els.cabin);
    FS.router.onChange(function () { renderPage(); });
    window.addEventListener('hashchange', function () { FS.router.sync(); });
    window.addEventListener('resize', fit);
    document.addEventListener('click', onClick);
    document.addEventListener('keydown', onKey);
    FS.router.sync();
    fit();
  }

  return {
    start: start,
    renderPage: renderPage,
    fit: fit,
    view: function () { return view; }
  };
})();
