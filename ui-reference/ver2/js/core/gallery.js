/**
 * 冰箱小精灵 ver2 UI 预览 —— 页面总览（缩略图墙）
 * ------------------------------------------------------------------
 * 用与设备视图完全相同的渲染函数生成 1024×600 舞台，再 scale(0.3125) 缩成
 * 320×188 缩略图：30 个页面模板（INV-01 出 5 张变体）+ 12 浮层 + 8 侧舱 CTX。
 * 点击任意缩略图 → 切回设备预览并深链到对应页面/浮层/侧舱状态。
 */
window.FS = window.FS || {};

FS.gallery = (function () {

  /* 单个 1024×600 舞台的 HTML（主区页面 + 侧导航 + 侧舱 + 可选浮层） */
  function stageHtml(opts) {
    var def = FS.router.get(opts.pageId);
    if (!def) return '';
    var pg = {
      id: def.id,
      params: opts.params || new URLSearchParams(),
      viewState: opts.viewState || 'normal',
      page: def
    };
    var cur = FS.router.moduleOf(def.id);
    var nav = [{ m: '首页', icon: 'home' }, { m: '库存', icon: 'box' }, { m: '对话', icon: 'chat' }, { m: '计划', icon: 'plan' }, { m: '更多', icon: 'more' }]
      .map(function (n) {
        return '<button class="nav-item' + (n.m === cur ? ' on' : '') + '">' + FS.icon(n.icon) + '<span>' + n.m + '</span></button>';
      }).join('');
    var cabin = FS.cabin.renderFull(opts.ctx || FS.cabin.ctxForPage(opts.pageId));
    var ovl = opts.overlay ? '<div class="ovl-root-gal">' + FS.overlays.renderOne(opts.overlay) + '</div>' : '';
    return '<div id="shell" class="' + (def.system ? 'mode-system-gal' : '') + '">' +
      '<main id="main-area" class="' + (def.system ? 'sys' : '') + '">' + def.render(pg) + '</main>' +
      (def.system ? '' : '<nav id="sidenav">' + nav + '</nav>') +
      '<aside id="cabin">' + cabin + '</aside>' +
      ovl + '</div>';
  }

  function thumb(label, sub, innerHtml, arg) {
    return '<div class="gal-thumb" data-act="gal:open" data-arg="' + FS.esc(arg) + '" title="点击进入设备预览">' +
      '<div class="gal-stage"><div class="gal-stage-inner">' + innerHtml + '</div></div>' +
      '<div class="gal-label">' + FS.esc(label) + (sub ? '<span>' + FS.esc(sub) + '</span>' : '') + '</div></div>';
  }

  function render(root) {
    var html = '<div class="gal">';

    /* ---- 页面模板（按模块分组；含 gallery 变体，如 INV-01 五态）。
     *      总数从注册表动态取，避免删/加页后标题数字对不上。 ---- */
    html += '<h2 class="gal-h2">页面模板（' + FS.router.list().length + '）</h2>';
    var modules = [];
    FS.router.list().forEach(function (p) { if (modules.indexOf(p.module) < 0) modules.push(p.module); });
    modules.forEach(function (m) {
      var pages = FS.router.list().filter(function (p) { return p.module === m; });
      html += '<h3 class="gal-h3">' + m + '（' + pages.length + '）</h3><div class="gal-grid">';
      pages.forEach(function (p) {
        if (p.gallery && p.gallery.length) {
          p.gallery.forEach(function (v) {
            var parsed = FS.router.parse(v.hash);
            html += thumb(p.id + ' ' + p.name, v.label,
              stageHtml({ pageId: p.id, params: parsed ? parsed.params : null }), v.hash);
          });
        } else {
          html += thumb(p.id + ' ' + p.name, p.level,
            stageHtml({ pageId: p.id }), '#/p/' + p.id);
        }
      });
      html += '</div>';
    });

    /* ---- 12 个全局浮层（垫 HOME-01 作为底页） ---- */
    html += '<h2 class="gal-h2">全局浮层（12）</h2><div class="gal-grid">';
    FS.overlays.list().forEach(function (o) {
      html += thumb(o.id + ' ' + o.name, o.blocking ? '阻塞型' : '非阻塞',
        stageHtml({ pageId: 'HOME-01', overlay: o.id }), 'ovl:' + o.id);
    });
    html += '</div>';

    /* ---- 8 个侧舱 CTX 状态 ---- */
    html += '<h2 class="gal-h2">状态侧舱情境（8）</h2><div class="gal-grid">';
    FS.cabin.defs.forEach(function (c) {
      html += thumb(c.id + ' ' + c.name, '状态侧舱',
        stageHtml({ pageId: 'HOME-01', ctx: c.id }), 'ctx:' + c.id);
    });
    html += '</div></div>';

    root.innerHTML = html;
  }

  /* 缩略图点击：深链回设备预览 */
  FS.action('gal:open', function (el) {
    var arg = el.getAttribute('data-arg');
    /* 切回设备视图 */
    var viewBtn = document.querySelector('[data-act="shell:view"][data-arg="device"]');
    if (viewBtn) viewBtn.click();
    if (arg.indexOf('ovl:') === 0) {
      FS.overlays.show(arg.slice(4));
    } else if (arg.indexOf('ctx:') === 0) {
      FS.cabin.setMode(arg.slice(4));
      var sel = document.getElementById('tb-ctx');
      if (sel) sel.value = arg.slice(4);
    } else {
      FS.router.go(arg);
    }
  });

  return { render: render };
})();
