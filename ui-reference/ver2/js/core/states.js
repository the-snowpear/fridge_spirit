/**
 * 冰箱小精灵 ver2 UI 预览 —— 页面异步状态切换器（六态）
 * ------------------------------------------------------------------
 * 规范要求所有异步页面具备：正常 / 加载中 / 空 / 离线 / 错误 / 待确认 / 冲突。
 * 页面在注册时声明 states[]（支持哪些态），渲染时用 FS.states.wrap(pg, 正常HTML)
 * 包住"异步内容区"：当前演示态生效时，该函数返回对应的通用状态面板。
 * 工具栏（设备框外）切换全局演示态；页面未声明的态自动按正常渲染。
 */
window.FS = window.FS || {};

FS.states = (function () {
  var current = 'normal';

  var LIST = [
    { id: 'normal',   label: '正常' },
    { id: 'loading',  label: '加载中' },
    { id: 'empty',    label: '空' },
    { id: 'offline',  label: '离线' },
    { id: 'error',    label: '错误' },
    { id: 'pending',  label: '待确认' },
    { id: 'conflict', label: '冲突' }
  ];

  function set(s) {
    current = s;
    if (FS.shell && FS.shell.renderPage) FS.shell.renderPage();
  }

  /* 通用状态面板（LVGL 侧对应：静态灰块/提示条 + 按钮，不做循环动画） */
  function panel(kind, pg, opts) {
    opts = opts || {};
    var ui = FS.ui;
    var retry = '<div style="margin-top:12px">' + ui.btn({ label: '重试', kind: 'ghost', act: 'state:retry' }) + '</div>';
    switch (kind) {
      case 'loading':
        return '<div class="state-panel">' + ui.skeleton(opts.rows || 5) + '<div class="state-hint">加载中…</div></div>';
      case 'empty':
        return '<div class="state-panel">' + ui.emptyState({
          icon: opts.icon || 'box',
          title: opts.emptyTitle || '这里还没有内容',
          desc: opts.emptyDesc || '添加后会显示在这里',
          action: opts.emptyAction || ''
        }) + '</div>';
      case 'error':
        return '<div class="state-panel">' + ui.emptyState({
          icon: 'danger', title: '加载失败',
          desc: opts.errorDesc || '网络请求超时，请检查网络后重试',
          action: retry
        }) + '</div>';
      case 'conflict':
        return '<div class="state-panel"><div class="conflict-box">' +
          '<div class="cf-head">' + FS.icon('warn', 'lg') + '<div><div class="cf-title">数据冲突</div>' +
          '<div class="cf-sub">本地与云端的这条数据不一致，请选择保留哪个版本</div></div></div>' +
          '<div class="cf-cols">' +
            '<div class="cf-col"><div class="cf-tag">本地（14:20）</div><div class="cf-line">数量：3 个</div><div class="cf-line">位置：右冷藏/第2层</div>' +
              ui.btn({ label: '保留本地', kind: 'primary', size: 'sm', act: 'state:retry' }) + '</div>' +
            '<div class="cf-col"><div class="cf-tag">云端（12:05）</div><div class="cf-line">数量：5 个</div><div class="cf-line">位置：右冷藏/第1层</div>' +
              ui.btn({ label: '保留云端', kind: 'ghost', size: 'sm', act: 'state:retry' }) + '</div>' +
          '</div></div></div>';
    }
    return '';
  }

  /* 包住异步内容区：返回生效状态面板，否则原样返回内容 */
  function wrap(pg, contentHtml, opts) {
    var vs = pg.viewState;
    if (!vs || vs === 'normal') return contentHtml;
    if (!pg.page.states || pg.page.states.indexOf(vs) < 0) return contentHtml;
    if (vs === 'offline') {
      return '<div class="notice-bar offline">' + FS.icon('cloudoff') +
        '<span class="nb-text">离线模式：展示本地缓存数据，变更将在联网后同步</span></div>' +
        '<div class="state-dim">' + contentHtml + '</div>';
    }
    if (vs === 'pending') {
      return '<div class="notice-bar warn">' + FS.icon('shield') +
        '<span class="nb-text">有 <b>2 项</b>变更等待确认后才会生效</span>' +
        '<button class="nb-act" data-act="state:retry">去确认</button></div>' + contentHtml;
    }
    return panel(vs, pg, opts);
  }

  FS.action('state:retry', function () {
    set('normal');
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：已恢复正常状态' });
  });

  return {
    list: function () { return LIST; },
    current: function () { return current; },
    set: set,
    wrap: wrap
  };
})();
