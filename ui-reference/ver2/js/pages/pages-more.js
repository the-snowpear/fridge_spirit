/**
 * 冰箱小精灵 ver2 UI 预览 —— 更多模块（MORE-01 .. MORE-08）
 * ------------------------------------------------------------------
 * MORE-01 更多入口（一级，可滚动）：定时器 / 闹钟 / 秒表 / 设备安全 / 设置 / 维护。
 * MORE-02 定时器（二级）：主计时面 + 快捷预设 + 运行中/已完成列表。
 * MORE-03 定时器/闹钟编辑（任务页）：?type=timer|alarm 决定表单与返回目标。
 * MORE-04 设备与厨房安全 · MORE-05 设置中心 · MORE-06 维护中心（分区模板）。
 * MORE-07 闹钟（二级）：固定闹钟 + 周期提醒卡片，大触控开关。
 * MORE-08 秒表（二级）：大数字面 + 开始/暂停/计次/复位 + 计次列表。
 *
 * 分区模板（MORE-04/05/06 共用 renderSplitShell 外壳）。
 * 本模块注册流程：FS.flows.register('safety') —— OV-10 关键安全告警全屏接管。
 */
window.FS = window.FS || {};

(function () {
  var ui;
  /* 秒表演示态（不落 mock-data，仅本页交互） */
  var swState = { running: false, ms: 0, laps: [], tick: null, base: 0, startedAt: 0 };

  function ready() { ui = FS.ui; }
  function esc(s) { return FS.esc(s); }
  function icon(n, c) { return FS.icon(n, c); }

  /* 统计未确认安全告警数（MORE-01 角标 / 侧导航同源） */
  function pendingSafetyCount() {
    return FS.data.safetyEvents.filter(function (e) { return !e.confirmed; }).length;
  }
  /* 运行中倒计时数（MORE-01 定时器卡角标） */
  function runningTimerCount() {
    return FS.data.timers.filter(function (t) {
      return t.kind === '倒计时' && (t.state === 'running' || t.state === 'paused');
    }).length;
  }
  /* 待触发闹钟/周期数 */
  function armedAlarmCount() {
    return FS.data.timers.filter(function (t) {
      return (t.kind === '闹钟' || t.kind === '周期提醒') && t.state === 'armed';
    }).length;
  }
  function timersOfKind(kinds) {
    var set = {};
    (kinds || []).forEach(function (k) { set[k] = true; });
    return FS.data.timers.filter(function (t) { return set[t.kind]; });
  }
  function fmtSw(ms) {
    /* 固件按毫秒保存；网页参考也显示三位毫秒，保证视觉和交互语义一致。 */
    var t = Math.max(0, Math.floor(Number(ms) || 0));
    var m = Math.floor(t / 60000);
    var s = Math.floor((t % 60000) / 1000);
    var milli = t % 1000;
    function z(n) { return (n < 10 ? '0' : '') + n; }
    function z3(n) { return (n < 100 ? '0' : '') + (n < 10 ? '0' : '') + n; }
    return z(m) + ':' + z(s) + '.' + z3(milli);
  }

  /* ==================== 分区模板共用外壳 ==================== */
  /**
   * 渲染「左栏目 + 右内容」分区页骨架。
   * o = { pageId, title, sub, tabs:[{key,label,ic,danger?,badge?}], curKey,
   *       renderBody(key)→html, stateOpts? }
   * 左栏项点击 = data-href 跳同页 ?tab=key；右栏内容包进 FS.states.wrap 以支持六态演示。
   */
  function renderSplitShell(pg, o) {
    var tabsHtml = o.tabs.map(function (t) {
      var on = t.key === o.curKey;
      return '<button class="more-tab' + (on ? ' on' : '') + (t.danger ? ' danger' : '') + '"' +
        ' data-act="nav" data-href="#/p/' + o.pageId + '?tab=' + encodeURIComponent(t.key) + '">' +
        icon(t.ic) + '<span>' + esc(t.label) + '</span>' +
        (t.badge ? '<span class="more-tab-badge">' + esc(t.badge) + '</span>' : '') +
        '</button>';
    }).join('');

    var body = '<div class="more-split">' +
      '<aside class="more-side">' + tabsHtml + '</aside>' +
      '<div class="more-content scroll-area">' +
        FS.states.wrap(pg, o.renderBody(o.curKey), o.stateOpts || {}) +
      '</div>' +
      '</div>';

    return '<div class="pg">' +
      ui.pageHead({
        title: o.title, id: o.pageId, sub: o.sub,
        actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
      }) +
      '<div class="pg-body more-split-body">' + body + '</div></div>';
  }

  /* 取当前 tab 参数（带默认值与合法性回退） */
  function curTab(pg, tabs, dflt) {
    var t = pg.params.get('tab');
    var ok = tabs.some(function (x) { return x.key === t; });
    return ok ? t : dflt;
  }

  /* ================================================================
   * MORE-01 更多入口（一级页，整页可滚动）
   * ============================================================== */
  FS.router.register({
    id: 'MORE-01', name: '更多', module: '更多', level: '一级页', ctx: 'CTX-01',
    states: [],

    render: function (pg) {
      ready();
      var d = FS.data.device;
      var runT = runningTimerCount();
      var alarms = armedAlarmCount();
      var safety = pendingSafetyCount();

      /* 时间工具区 + 系统工具区，卡片超过一屏时整页滚动 */
      var timeCards =
        entryCard({
          href: '#/p/MORE-02', ic: 'timer', tone: 'sage', name: '定时器',
          desc: runT ? '运行中 ' + runT + ' 个倒计时' : '厨房倒计时 · 快捷预设',
          badge: runT ? String(runT) : ''
        }) +
        entryCard({
          href: '#/p/MORE-07', ic: 'bell', tone: 'sage', name: '闹钟',
          desc: alarms ? alarms + ' 个待触发 · 含周期提醒' : '固定闹钟 · 周期提醒',
          badge: alarms ? String(alarms) : ''
        }) +
        entryCard({
          href: '#/p/MORE-08', ic: 'stopwatch', tone: 'sage', name: '秒表',
          desc: swState.running ? '计时中 ' + fmtSw(swState.ms) : '计次 · 复位 · 大数字面'
        });

      var sysCards =
        entryCard({
          href: '#/p/MORE-04', ic: 'shield', tone: safety ? 'danger' : 'sage', name: '设备与厨房安全',
          desc: safety ? '有 ' + safety + ' 条未确认告警' : '设备正常 · 无未处理告警',
          badge: safety ? String(safety) : '', badgeDanger: true
        }) +
        entryCard({
          href: '#/p/MORE-05', ic: 'settings', tone: 'sage', name: '设置中心',
          desc: '网络 · AI 模型 · 权限 · 语音显示'
        }) +
        entryCard({
          href: '#/p/MORE-06', ic: 'tool', tone: 'sage', name: '维护中心',
          desc: '存储 · 插件 · 日志 · 系统更新',
          badge: 'OTA'
        });

      var summary = '<div class="more-summary">' +
        '<span class="more-sum-item">' + ui.statusDot('ok') + '<span class="sd-label">设备正常</span></span>' +
        '<span class="more-sum-item">' + icon('wifi') + '<span class="sd-label">Wi-Fi · ' + esc(d.wifi.ssid) + '</span></span>' +
        '<span class="more-sum-item">' + icon('tf') + '<span class="sd-label">TF 余 ' + esc(d.tf.free) + '</span></span>' +
        '</div>';

      var body =
        '<div class="more-hub-sec">' +
          '<div class="more-hub-label">时间工具</div>' +
          '<div class="more-grid">' + timeCards + '</div>' +
        '</div>' +
        '<div class="more-hub-sec">' +
          '<div class="more-hub-label">系统与维护</div>' +
          '<div class="more-grid">' + sysCards + '</div>' +
        '</div>' +
        summary;

      return '<div class="pg">' +
        ui.pageHead({
          title: '更多', id: 'MORE-01',
          sub: d.date + ' · 时间工具 / 设备 / 设置'
        }) +
        '<div class="pg-body more-hub-body">' +
          ui.scrollArea(body, 'more-hub-scroll') +
        '</div></div>';
    }
  });

  /* 入口大卡：图标 + 名称 + 摘要 + 可选角标 */
  function entryCard(o) {
    return '<div class="more-entry" data-act="nav" data-href="' + esc(o.href) + '">' +
      '<span class="me-icon ' + (o.tone || 'sage') + '">' + icon(o.ic, 'lg') + '</span>' +
      (o.badge ? '<span class="me-badge' + (o.badgeDanger ? ' danger' : '') + '">' + esc(o.badge) + '</span>' : '') +
      '<div class="me-name">' + esc(o.name) + '</div>' +
      '<div class="me-desc">' + esc(o.desc) + '</div>' +
      icon('fwd', 'me-arrow') +
      '</div>';
  }

  /* ================================================================
   * MORE-02 定时器（二级页 · 倒计时专用）
   * ============================================================== */
  FS.router.register({
    id: 'MORE-02', name: '定时器', module: '更多', level: '二级页', ctx: 'CTX-07',
    states: ['empty'],
    parent: 'MORE-01',

    render: function (pg) {
      ready();
      var list = timersOfKind(['倒计时']);
      var active = list.filter(function (t) { return t.state === 'running' || t.state === 'paused'; });
      var done = list.filter(function (t) { return t.state === 'done'; });
      var primary = active[0];

      /* 主计时面：当前最优先的运行中倒计时 */
      var hero = primary
        ? '<div class="tt-hero">' +
            '<div class="tt-hero-label">' + esc(primary.name) +
              (primary.state === 'paused' ? '<span class="chip mini warn">已暂停</span>' : '<span class="chip mini ok">运行中</span>') +
            '</div>' +
            '<div class="tt-hero-face">' + esc(primary.remain) + '</div>' +
            '<div class="tt-hero-sub">总时长 ' + esc(primary.total) + ' · 来源 ' + esc(primary.source) + '</div>' +
            '<div class="tt-hero-ops">' +
              ui.btn({ label: primary.state === 'paused' ? '继续' : '暂停', kind: 'ghost', icon: primary.state === 'paused' ? 'play' : 'pause', act: 'more:timer-pause', arg: primary.id }) +
              ui.btn({ label: '+1 分钟', kind: 'ghost', icon: 'plus', act: 'more:timer-plus', arg: primary.id }) +
              ui.btn({ label: '取消', kind: 'danger', icon: 'x', act: 'more:timer-cancel', arg: primary.id }) +
            '</div>' +
          '</div>'
        : '<div class="tt-hero idle">' +
            '<div class="tt-hero-label">厨房倒计时</div>' +
            '<div class="tt-hero-face muted">00:00</div>' +
            '<div class="tt-hero-sub">点下方预设立即开始，或新建自定义时长</div>' +
          '</div>';

      /* 快捷预设：大触控芯片，一键开计时 */
      var presets = [1, 3, 5, 10, 15, 30].map(function (m) {
        return '<button class="tt-preset" data-act="more:timer-preset" data-arg="' + m + '">' +
          '<span class="tt-preset-num">' + m + '</span><span class="tt-preset-unit">分钟</span></button>';
      }).join('');

      var otherActive = active.slice(1);
      var otherHtml = otherActive.length
        ? '<div class="more-group">' + otherActive.map(runTimerRow).join('') + '</div>'
        : '';
      var doneHtml = done.length
        ? '<div class="more-group done">' + done.map(function (t) { return ui.timerRow(t); }).join('') + '</div>'
        : '';

      var asyncHtml =
        hero +
        '<div class="tt-presets-wrap">' +
          '<div class="more-hub-label">快捷开始</div>' +
          '<div class="tt-presets">' + presets + '</div>' +
        '</div>' +
        (otherActive.length ? ui.sectionTitle('其他运行中（' + otherActive.length + '）') + otherHtml : '') +
        (done.length ? ui.sectionTitle('已完成（' + done.length + '）',
          '<button class="more-textbtn" data-act="more:timer-ring">' + icon('bell') + '到时演示</button>') + doneHtml : '') +
        (!active.length && !done.length ? '' : '');

      return '<div class="pg">' +
        ui.pageHead({
          title: '定时器', id: 'MORE-02',
          sub: '倒计时 · 厨房计时',
          actions: ui.btn({ label: '新建', kind: 'primary', size: 'sm', icon: 'plus', act: 'nav', href: '#/p/MORE-03?type=timer' }) +
                   ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body more-tool-body">' +
          ui.scrollArea(FS.states.wrap(pg, asyncHtml, {
            emptyTitle: '还没有倒计时',
            emptyDesc: '用快捷预设立刻开始，或新建自定义时长',
            emptyAction: ui.btn({ label: '新建定时器', act: 'nav', href: '#/p/MORE-03?type=timer' })
          }), 'more-tool-scroll') +
        '</div></div>';
    }
  });

  function runTimerRow(t) {
    var ctrl =
      '<div class="more-timer-ctrl">' +
        ui.btn({ label: t.state === 'paused' ? '继续' : '暂停', kind: 'ghost', size: 'sm', act: 'more:timer-pause', arg: t.id }) +
        ui.btn({ label: '+1分钟', kind: 'ghost', size: 'sm', act: 'more:timer-plus', arg: t.id }) +
        ui.btn({ label: '取消', kind: 'danger', size: 'sm', act: 'more:timer-cancel', arg: t.id }) +
      '</div>';
    return '<div class="more-timer-run">' + ui.timerRow(t) + ctrl + '</div>';
  }

  /* ================================================================
   * MORE-07 闹钟（二级页 · 固定闹钟 + 周期提醒）
   * ============================================================== */
  FS.router.register({
    id: 'MORE-07', name: '闹钟', module: '更多', level: '二级页', ctx: 'CTX-07',
    states: ['empty'],
    parent: 'MORE-01',

    render: function (pg) {
      ready();
      var alarms = timersOfKind(['闹钟']);
      var cycles = timersOfKind(['周期提醒']);

      function alarmCard(t) {
        var on = t.state === 'armed' || t.state === 'ringing';
        return '<div class="al-card' + (on ? '' : ' off') + '">' +
          '<div class="al-card-main" data-act="nav" data-href="#/p/MORE-03?type=alarm&id=' + esc(t.id) + '">' +
            '<div class="al-time">' + esc(t.at || '--:--') + '</div>' +
            '<div class="al-name">' + esc(t.name) +
              '<span class="chip mini ' + (t.kind === '周期提醒' ? 'info' : '') + '">' + esc(t.kind) + '</span></div>' +
            '<div class="al-meta">' +
              (t.repeat ? esc(t.repeat) + ' · ' : '') +
              '来源 ' + esc(t.source) +
            '</div>' +
          '</div>' +
          '<button class="al-toggle' + (on ? ' on' : '') + '" data-act="more:alarm-toggle" data-arg="' + esc(t.id) + '" title="' + (on ? '关闭' : '开启') + '">' +
            '<span class="al-knob"></span>' +
          '</button>' +
        '</div>';
      }

      var alarmHtml = alarms.length
        ? '<div class="al-list">' + alarms.map(alarmCard).join('') + '</div>'
        : '<div class="more-none">还没有闹钟，点右上角新建</div>';
      var cycleHtml = cycles.length
        ? '<div class="al-list">' + cycles.map(alarmCard).join('') + '</div>'
        : '<div class="more-none">还没有周期提醒</div>';

      var asyncHtml =
        ui.sectionTitle('闹钟（' + alarms.length + '）',
          '<button class="more-textbtn" data-act="more:timer-ring">' + icon('bell') + '到时演示</button>') +
        alarmHtml +
        ui.sectionTitle('周期提醒（' + cycles.length + '）') +
        cycleHtml;

      return '<div class="pg">' +
        ui.pageHead({
          title: '闹钟', id: 'MORE-07',
          sub: '固定时刻 · 周期提醒',
          actions: ui.btn({ label: '新建', kind: 'primary', size: 'sm', icon: 'plus', act: 'nav', href: '#/p/MORE-03?type=alarm' }) +
                   ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body more-tool-body">' +
          ui.scrollArea(FS.states.wrap(pg, asyncHtml, {
            emptyTitle: '还没有闹钟',
            emptyDesc: '设置固定时刻或周期提醒',
            emptyAction: ui.btn({ label: '新建闹钟', act: 'nav', href: '#/p/MORE-03?type=alarm' })
          }), 'more-tool-scroll') +
        '</div></div>';
    }
  });

  /* ================================================================
   * MORE-08 秒表（二级页）
   * ============================================================== */
  FS.router.register({
    id: 'MORE-08', name: '秒表', module: '更多', level: '二级页', ctx: 'CTX-07',
    states: [],
    parent: 'MORE-01',

    render: function (pg) {
      ready();
      /* 若正在跑，用当前时刻刷新显示值；状态按毫秒保存，页面显示三位毫秒。 */
      if (swState.running && swState.startedAt) {
        swState.ms = swState.base + (Date.now() - swState.startedAt);
      }
      var face = fmtSw(swState.ms);
      var laps = swState.laps;
      var lapsHtml = laps.length
        ? '<div class="sw-laps">' +
            laps.map(function (L, i) {
              return '<div class="sw-lap">' +
                '<span class="sw-lap-n">计次 ' + (laps.length - i) + '</span>' +
                '<span class="sw-lap-t">' + esc(L) + '</span></div>';
            }).join('') +
          '</div>'
        : '<div class="more-none sw-laps-empty">计次记录会出现在这里</div>';

      var body =
        '<div class="sw-stage">' +
          '<div class="sw-face" id="sw-face">' + face + '</div>' +
          '<div class="sw-status">' + (swState.running ? '计时中' : (swState.ms ? '已暂停' : '就绪')) + '</div>' +
          '<div class="sw-ops">' +
            '<button class="sw-btn ghost" data-act="more:sw-reset"' + (!swState.ms && !laps.length ? ' disabled' : '') + '>' +
              icon('retry') + '<span>复位</span></button>' +
            '<button class="sw-btn primary big" data-act="more:sw-toggle">' +
              icon(swState.running ? 'pause' : 'play', 'lg') +
              '<span>' + (swState.running ? '暂停' : '开始') + '</span></button>' +
            '<button class="sw-btn ghost" data-act="more:sw-lap"' + (!swState.running && !swState.ms ? ' disabled' : '') + '>' +
              icon('list') + '<span>计次</span></button>' +
          '</div>' +
        '</div>' +
        '<div class="sw-laps-wrap">' +
          '<div class="more-hub-label">计次（' + laps.length + '）</div>' +
          lapsHtml +
        '</div>';

      return '<div class="pg">' +
        ui.pageHead({
          title: '秒表', id: 'MORE-08',
          sub: '计次 · 复位',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body more-tool-body sw-body">' +
          ui.scrollArea(body, 'more-tool-scroll') +
        '</div></div>';
    },

    onMount: function () {
      /* 运行中每 80ms 只刷数字，避免整页重绘 */
      if (swState.tick) { clearInterval(swState.tick); swState.tick = null; }
      if (!swState.running) return;
      swState.tick = setInterval(function () {
        if (!swState.running) return;
        swState.ms = swState.base + (Date.now() - swState.startedAt);
        var el = document.getElementById('sw-face');
        if (el) el.textContent = fmtSw(swState.ms);
        else { clearInterval(swState.tick); swState.tick = null; }
      }, 80);
    }
  });

  /* ================================================================
   * MORE-03 定时器/闹钟编辑（任务页 · ?type=timer|alarm）
   * ============================================================== */
  FS.router.register({
    id: 'MORE-03', name: '定时器/闹钟编辑', module: '更多', level: '任务页', ctx: 'CTX-07',
    states: [],
    parent: function (params) {
      var t = (params && params.get) ? params.get('type') : '';
      if (t === 'alarm') return '#/p/MORE-07';
      return '#/p/MORE-02';
    },

    render: function (pg) {
      ready();
      var type = pg.params.get('type') || 'timer';
      var isAlarm = type === 'alarm';
      var typeVal = isAlarm ? '定时' : '倒计时';
      var typeSeg = ui.seg({
        act: 'more:tt-type', value: typeVal,
        options: isAlarm ? ['定时', '周期'] : ['倒计时']
      });

      var timeBlock = isAlarm
        ? ui.formRow({
            label: '响铃时间', desc: '选择到点时刻（OV-12）',
            control: ui.inputLike({ value: '07:30', act: 'more:tt-time', icon: 'clock' })
          }) +
          ui.formRow({
            label: '重复规则', desc: '周期提醒的触发频率',
            control: '<div class="more-repeat">' +
              ['每天', '工作日', '每周日', '自定义'].map(function (r, i) {
                return '<button class="more-chip' + (i === 0 ? ' on' : '') + '" data-act="more:tt-repeat" data-arg="' + esc(r) + '">' + esc(r) + '</button>';
              }).join('') + '</div>'
          })
        : ui.formRow({
            label: '时长', desc: '倒计时时长（时 / 分）',
            control: '<div class="more-hm">' +
              ui.stepper({ act: 'more:tt-hour', value: '0', unit: '时' }) +
              '<span class="more-hm-sep">:</span>' +
              ui.stepper({ act: 'more:tt-min', value: '10', unit: '分' }) +
              '</div>'
          }) +
          ui.formRow({
            label: '或从预设选择', desc: '常用时长滚轮选择（OV-12 数值）',
            control: ui.inputLike({ value: '10 分钟', act: 'more:tt-preset', icon: 'clock' })
          });

      var form =
        '<div class="card"><div class="card-body more-form">' +
          ui.formRow({ label: '类型', desc: isAlarm ? '固定闹钟 / 周期提醒' : '厨房倒计时', control: typeSeg }) +
          timeBlock +
          ui.formRow({
            label: '名称', desc: '在列表和到时提醒中显示',
            control: ui.inputLike({ value: isAlarm ? '早餐提醒' : '煮蛋', act: 'more:tt-name', icon: 'edit' })
          }) +
          ui.formRow({
            label: '提醒声音', desc: '到时的提示方式',
            control: ui.seg({ act: 'more:tt-sound', value: '铃声', options: ['铃声', '语音播报'] })
          }) +
          ui.formRow({
            label: '播报文字', desc: '语音播报时朗读的内容',
            control: ui.inputLike({ placeholder: isAlarm ? '如：该吃早餐了' : '如：鸡蛋煮好了', act: 'more:tt-say', icon: 'volume' })
          }) +
          (isAlarm ? '' : ui.formRow({
            label: '关联', desc: '可关联菜谱 / 计划 / 食材',
            control: ui.inputLike({ placeholder: '选择关联对象', act: 'more:tt-link', icon: 'recipe' })
          })) +
          ui.formRow({
            label: '同步到小程序', desc: '在家庭小程序中同步显示与提醒',
            control: ui.sw({ on: true, act: 'more:tt-sync' })
          }) +
        '</div></div>';

      var foot =
        ui.btn({ label: '删除', kind: 'danger', icon: 'trash', act: 'more:tt-del' }) +
        '<span class="spacer"></span>' +
        ui.btn({ label: '试听', kind: 'ghost', icon: 'volume', act: 'more:tt-preview' }) +
        ui.btn({ label: '取消', kind: 'ghost', act: 'more:tt-cancel-edit' }) +
        ui.btn({ label: '保存', kind: 'ghost', icon: 'check', act: 'more:tt-save' }) +
        ui.btn({
          label: isAlarm ? '保存并开启' : '立即开始',
          kind: 'primary', icon: isAlarm ? 'bell' : 'play',
          act: 'more:tt-start'
        });

      return '<div class="pg">' +
        ui.pageHead({
          title: isAlarm ? '编辑闹钟' : '编辑定时器', id: 'MORE-03',
          sub: isAlarm ? '固定时刻或周期提醒' : '新建或修改倒计时',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body"><div class="scroll-area">' + form + '</div></div>' +
        '<div class="pg-foot">' + foot + '</div></div>';
    }
  });

  /* ================================================================
   * MORE-04 设备与厨房安全（二级页，分区模板）
   * ============================================================== */
  var M04_TABS = [
    { key: 'devices', label: '设备概览', ic: 'fridge' },
    { key: 'connect', label: '连接状态', ic: 'wifi' },
    { key: 'sensors', label: '传感器', ic: 'radar' },
    { key: 'safety', label: '厨房安全', ic: 'shield', danger: true, badge: String(pendingSafetyCount() || '') }
  ];

  FS.router.register({
    id: 'MORE-04', name: '设备与厨房安全', module: '更多', level: '二级页', ctx: 'CTX-08',
    states: ['loading', 'error'],
    parent: 'MORE-01',

    render: function (pg) {
      ready();
      var cur = curTab(pg, M04_TABS, 'devices');
      return renderSplitShell(pg, {
        pageId: 'MORE-04', title: '设备与厨房安全', sub: '硬件健康 · 连接 · 传感器 · 安全事件',
        tabs: M04_TABS, curKey: cur,
        renderBody: m04Body,
        stateOpts: { errorDesc: '设备状态读取失败，请重试' }
      });
    }
  });

  function m04Body(tab) {
    if (tab === 'connect') return m04Connect();
    if (tab === 'sensors') return m04Sensors();
    if (tab === 'safety') return m04Safety();
    return m04Devices();
  }

  /* 设备概览：FS.data.devices 列表（状态点 + 描述 + 重新初始化演示） */
  function m04Devices() {
    var rows = FS.data.devices.map(function (dv) {
      return '<div class="list-row">' +
        '<span class="lr-icon ' + (dv.status === 'ok' ? 'ok' : 'warn') + '">' + icon('fridge') + '</span>' +
        '<div class="lr-main"><div class="lr-title">' + esc(dv.name) + '</div>' +
        '<div class="lr-sub">' + esc(dv.desc) + '</div></div>' +
        ui.statusDot(dv.status) +
        ui.btn({ label: '重新初始化', kind: 'ghost', size: 'sm', act: 'more:dev-reinit', arg: dv.name }) +
        '</div>';
    }).join('');
    return ui.sectionTitle('设备总览', '<span class="sec-side">' + FS.data.devices.length + ' 个硬件单元</span>') +
      '<div class="more-group">' + rows + '</div>';
  }

  /* 连接：Wi-Fi（device.wifi + wifiList 前2）/ 云端 MQTT / 同步状态 */
  function m04Connect() {
    var d = FS.data.device;
    var wifiTop =
      ui.kv('当前网络', d.wifi.ssid + ' · ' + d.wifi.rssi + 'dBm', d.wifi.ok ? 'ok-text' : 'danger-text') +
      ui.kv('连接状态', d.wifi.ok ? '已连接' : '已断开', d.wifi.ok ? 'ok-text' : 'danger-text');
    var wifiRows = FS.data.wifiList.slice(0, 2).map(function (w) {
      return '<div class="list-row">' +
        '<span class="lr-icon ' + (w.connected ? 'ok' : '') + '">' + icon('wifi') + '</span>' +
        '<div class="lr-main"><div class="lr-title">' + esc(w.ssid) + (w.connected ? '<span class="chip mini ok">已连接</span>' : '') + '</div>' +
        '<div class="lr-sub">' + w.rssi + 'dBm · ' + (w.secured ? '已加密' : '开放') + '</div></div>' +
        (w.connected ? '' : ui.btn({ label: '连接', kind: 'ghost', size: 'sm', act: 'more:wifi-conn', arg: w.ssid })) +
        '</div>';
    }).join('');

    var mqtt =
      ui.kv('云端 MQTT', d.cloud ? '已连接' : '未连接', d.cloud ? 'ok-text' : 'danger-text') +
      ui.kv('ESP32-C6 协处理器', d.c6.ver + ' · ' + (d.c6.ok ? 'SDIO 正常' : '异常'), d.c6.ok ? 'ok-text' : 'danger-text') +
      ui.kv('最近同步', '今天 14:31 · 库存 20 项') +
      '<div class="more-rowbtns">' +
        ui.btn({ label: '重新连接', kind: 'ghost', size: 'sm', icon: 'retry', act: 'more:reconnect' }) +
        ui.btn({ label: '重新配网', kind: 'ghost', size: 'sm', act: 'more:reprov' }) +
      '</div>';

    return ui.sectionTitle('Wi-Fi') +
      ui.card({ body: wifiTop }) +
      '<div class="more-group">' + wifiRows + '</div>' +
      ui.sectionTitle('云端与同步') +
      ui.card({ body: mqtt });
  }

  /* 传感器：FS.data.sensors（名称 + 值 + 状态点 + 校准按钮） */
  function m04Sensors() {
    var rows = FS.data.sensors.map(function (s) {
      return '<div class="list-row">' +
        '<span class="lr-icon ' + (s.status === 'ok' ? 'ok' : 'warn') + '">' + icon('radar') + '</span>' +
        '<div class="lr-main"><div class="lr-title">' + esc(s.name) + '</div></div>' +
        '<div class="lr-side"><b>' + esc(s.value) + '</b></div>' +
        ui.statusDot(s.status) +
        ui.btn({ label: '校准', kind: 'ghost', size: 'sm', act: 'more:sensor-cal', arg: s.name }) +
        '</div>';
    }).join('');
    return ui.sectionTitle('传感器', '<span class="sec-side">数据来自 fridge_sensors</span>') +
      '<div class="more-group">' + rows + '</div>' +
      '<div class="more-note">' + icon('info') + '光照 / IMU / 雷达等传感器统一经快照接口读取，勿直接读原始通道。</div>';
  }

  /* 厨房安全：未确认红色置顶 + 确认按钮 + 历史事件 */
  function m04Safety() {
    var evs = FS.data.safetyEvents;
    var pending = evs.filter(function (e) { return !e.confirmed; });
    var history = evs.filter(function (e) { return e.confirmed; });

    var pendingHtml = pending.map(function (e) {
      return '<div class="list-row more-safety-pending">' +
        '<span class="lr-icon danger">' + icon('warn') + '</span>' +
        '<div class="lr-main"><div class="lr-title">' + esc(e.title) + '<span class="chip mini danger">未确认</span></div>' +
        '<div class="lr-sub">' + esc(e.time) + ' · ' + esc(e.desc) + '</div></div>' +
        ui.btn({ label: '确认', kind: 'danger', size: 'sm', act: 'more:safety-ack', arg: e.title }) +
        '</div>';
    }).join('');

    var historyHtml = history.length
      ? '<div class="more-group done">' + history.map(function (e) {
          return '<div class="list-row">' +
            '<span class="lr-icon ok">' + icon('check') + '</span>' +
            '<div class="lr-main"><div class="lr-title">' + esc(e.title) + '</div>' +
            '<div class="lr-sub">' + esc(e.time) + ' · ' + esc(e.desc) + '</div></div>' +
            ui.chip('已确认', 'ok') + '</div>';
        }).join('') + '</div>'
      : '';

    return ui.sectionTitle('未确认告警（' + pending.length + '）') +
      (pendingHtml || '<div class="more-none">暂无未确认告警</div>') +
      (history.length ? ui.sectionTitle('历史事件') + historyHtml : '') +
      '<div class="more-note">' + icon('info') + '燃气 / 烟雾传感器为未来扩展，当前为门磁与温度异常监测。</div>';
  }

  /* ================================================================
   * MORE-05 设置中心（二级页，分区模板）
   * ============================================================== */
  var M05_ICONS = ['wifi', 'zap', 'shield', 'volume', 'lock'];

  FS.router.register({
    id: 'MORE-05', name: '设置中心', module: '更多', level: '二级页', ctx: 'CTX-01',
    states: [],
    parent: 'MORE-01',

    render: function (pg) {
      ready();
      var groups = FS.data.settings;
      var tabs = groups.map(function (g, i) {
        return { key: String(i), label: g.group, ic: M05_ICONS[i] || 'settings' };
      });
      var cur = curTab(pg, tabs, '0');
      var idx = parseInt(cur, 10);

      return renderSplitShell(pg, {
        pageId: 'MORE-05', title: '设置中心', sub: '网络 · AI · 权限 · 语音显示 · 隐私',
        tabs: tabs, curKey: cur,
        renderBody: function () { return m05Group(groups[idx]); }
      });
    }
  });

  /* 渲染某一设置组的所有行（type → 对应控件） */
  function m05Group(g) {
    if (!g) return ui.emptyState({ icon: 'settings', title: '分组不存在' });
    var rows = g.rows.map(function (r) {
      var control = '';
      if (r.type === 'switch') {
        control = ui.sw({ on: !!r.value, act: 'more:set-toggle', arg: g.group + '/' + r.label });
      } else if (r.type === 'seg') {
        control = ui.seg({ act: 'more:set-seg', value: r.value, options: r.options });
      } else if (r.type === 'value') {
        control = '<button class="more-value" data-act="more:set-value" data-arg="' + esc(g.group + '/' + r.label) + '">' +
          '<span>' + esc(r.value) + '</span>' + icon('fwd', 'lr-arrow') + '</button>';
      } else if (r.type === 'btn') {
        control = ui.btn({ label: r.value, kind: 'ghost', size: 'sm', act: 'more:set-btn', arg: r.label });
      } else if (r.type === 'slider') {
        control = '<div class="more-slider">' + ui.progress(r.value, '') +
          '<span class="more-slider-val">' + esc(r.value) + '</span></div>';
      }
      return ui.formRow({ label: r.label, desc: r.desc, control: control });
    }).join('');

    return ui.sectionTitle(g.group, '<span class="sec-side">修改即保存（演示）</span>') +
      '<div class="card"><div class="card-body more-form">' + rows + '</div></div>';
  }

  /* ================================================================
   * MORE-06 维护中心（二级页，分区模板）
   * ============================================================== */
  var M06_TABS = [
    { key: 'storage', label: '存储', ic: 'tf' },
    { key: 'plugins', label: '插件', ic: 'grid' },
    { key: 'logs', label: '日志', ic: 'log' },
    { key: 'ota', label: '系统更新', ic: 'download', badge: 'OTA' },
    { key: 'about', label: '关于', ic: 'info' }
  ];

  FS.router.register({
    id: 'MORE-06', name: '维护中心', module: '更多', level: '二级页', ctx: 'CTX-01',
    states: ['loading', 'error'],
    parent: 'MORE-01',

    render: function (pg) {
      ready();
      var cur = curTab(pg, M06_TABS, 'storage');
      return renderSplitShell(pg, {
        pageId: 'MORE-06', title: '维护中心', sub: '存储 · 插件 · 日志 · 系统更新 · 关于',
        tabs: M06_TABS, curKey: cur,
        renderBody: m06Body,
        stateOpts: { errorDesc: '维护信息读取失败，请重试' }
      });
    }
  });

  function m06Body(tab) {
    if (tab === 'plugins') return m06Plugins();
    if (tab === 'logs') return m06Logs();
    if (tab === 'ota') return m06Ota();
    if (tab === 'about') return m06About();
    return m06Storage();
  }

  /* 存储：storageDirs 表格 + TF 总用量 progress + 清理缓存 */
  function m06Storage() {
    var d = FS.data.device;
    /* 由 free/total 估算占用百分比（26.4G 余 / 29.1G 总 ≈ 9%） */
    var pct = 9;
    var rows = FS.data.storageDirs.map(function (s) {
      return '<div class="more-dirrow">' +
        '<span class="more-dir-path">' + esc(s.path) + '</span>' +
        '<span class="more-dir-size">' + esc(s.size) + '</span>' +
        ui.statusDot(s.status) +
        '</div>';
    }).join('');

    return ui.sectionTitle('TF 卡用量', '<span class="sec-side">余量 ' + esc(d.tf.free) + ' / ' + esc(d.tf.total) + '</span>') +
      ui.card({ body:
        ui.progress(pct, 'ok') +
        '<div class="more-dir-head"><span>数据目录</span><span>大小</span><span>状态</span></div>' +
        rows +
        '<div class="more-rowbtns">' +
          ui.btn({ label: '清理缓存', kind: 'ghost', size: 'sm', icon: 'trash', act: 'more:clear-cache' }) +
          ui.btn({ label: '导出诊断摘要', kind: 'ghost', size: 'sm', icon: 'upload', act: 'more:export-diag' }) +
        '</div>'
      });
  }

  /* 插件：名称 / 版本 / 权限说明 / 启用 switch */
  function m06Plugins() {
    var rows = FS.data.plugins.map(function (p) {
      return '<div class="list-row">' +
        '<span class="lr-icon ' + (p.on ? 'ok' : 'offline') + '">' + icon('grid') + '</span>' +
        '<div class="lr-main"><div class="lr-title">' + esc(p.name) + '<span class="chip mini">v' + esc(p.ver) + '</span></div>' +
        '<div class="lr-sub">权限：' + esc(p.perm) + '</div></div>' +
        ui.sw({ on: p.on, act: 'more:plugin-toggle', arg: p.name }) +
        '</div>';
    }).join('');
    return ui.sectionTitle('已安装插件（' + FS.data.plugins.length + '）') +
      '<div class="more-group">' + rows + '</div>' +
      '<div class="more-note">' + icon('info') + '插件启用状态保存于 TF 卡，重启后生效；高风险权限需用户确认。</div>';
  }

  /* 日志：等宽字体 + 级别着色（INFO/WARN/ERR） + 导出按钮 */
  function m06Logs() {
    var rows = FS.data.logs.map(function (l) {
      var cls = l.level === 'ERR' ? 'danger-text' : l.level === 'WARN' ? 'warn-text' : 'ok-text';
      return '<div class="more-logrow">' +
        '<span class="more-log-time">' + esc(l.time) + '</span>' +
        '<span class="more-log-level ' + cls + '">' + esc(l.level) + '</span>' +
        '<span class="more-log-text">' + esc(l.text) + '</span>' +
        '</div>';
    }).join('');
    return ui.sectionTitle('运行日志',
        '<button class="more-textbtn" data-act="more:export-log">' + icon('upload') + '导出</button>') +
      '<div class="card"><div class="card-body more-logbox">' + rows + '</div></div>';
  }

  /* 系统更新：ota 信息 + 检查更新 / 开始 OTA（弹 OV-03） */
  function m06Ota() {
    var o = FS.data.ota;
    var hasNew = o.latest !== o.current;
    return ui.sectionTitle('系统更新') +
      ui.card({ body:
        ui.kv('当前版本', o.current) +
        ui.kv('最新版本', o.latest, hasNew ? 'ok-text' : '') +
        ui.kv('状态', o.state, hasNew ? 'warn-text' : 'ok-text') +
        ui.kv('更新包大小', o.size) +
        ui.kv('启动槽位', o.slot) +
        '<div class="more-rowbtns">' +
          ui.btn({ label: '检查更新', kind: 'ghost', size: 'sm', icon: 'retry', act: 'more:ota-check' }) +
          ui.btn({ label: '开始 OTA', kind: 'primary', size: 'sm', icon: 'download', act: 'more:ota-start', disabled: !hasNew }) +
        '</div>' +
        '<div class="more-note">' + icon('info') + 'OTA 升级过程中请勿断电；失败将自动回滚到当前版本。</div>'
      });
  }

  /* 关于：设备型号 / 固件 / 序列号 kv + 恢复出厂设置（OV-04 高风险样式） */
  function m06About() {
    var d = FS.data.device;
    return ui.sectionTitle('关于本机') +
      ui.card({ body:
        ui.kv('设备型号', '冰箱小精灵 · ESP32-P4 CB V1.2') +
        ui.kv('主控', 'ESP32-P4NRW32 · 360MHz · 32MB PSRAM') +
        ui.kv('固件版本', d.version) +
        ui.kv('协处理器', 'ESP32-C6-MINI-1 · ' + d.c6.ver) +
        ui.kv('序列号', 'FS-P4-2026-0722-A3') +
        ui.kv('首次激活', '2026-07-02')
      }) +
      '<div class="more-danger-zone">' +
        ui.sectionTitle('危险操作') +
        ui.btn({ label: '恢复出厂设置', kind: 'danger', icon: 'power', act: 'more:factory-reset' }) +
        '<div class="more-note danger-text">' + icon('warn') + '将清空全部库存、设置与本地数据，不可恢复。</div>' +
      '</div>';
  }

  /* ================================================================
   * 流程：safety 关键安全告警（OV-10 全屏接管，停在等用户操作）
   * ============================================================== */
  FS.flows.register('safety', {
    run: function (api) {
      /* 全屏接管，展示关键安全告警；用户点「我知道了」即关闭。
       * OV-10 打开时侧舱自动跟随 CTX-08（overlays.render → cabin.refresh，无需额外代码）。 */
      api.show('OV-10', {
        title: '冷藏温度异常升高',
        desc: '冷藏室温度已升至 9.6°C 并持续 10 分钟，请检查冰箱门是否关严。',
        level: '严重'
      });
    }
  });

  /* ================================================================
   * 模块内动作（统一 more: 前缀）
   * ============================================================== */
  function toast(text, tone) { FS.overlays.show('OV-01', { tone: tone || 'ok', text: text }); }

  /* MORE-02 定时器控制 */
  FS.action('more:timer-pause', function (el) { toast('演示：已' + (el.textContent.indexOf('继续') >= 0 ? '继续' : '暂停') + '该定时器'); });
  FS.action('more:timer-plus', function () { toast('演示：已延长 1 分钟'); });
  FS.action('more:timer-cancel', function () {
    FS.overlays.show('OV-03', { title: '取消这个定时器？', desc: '取消后不再提醒，此操作不可恢复。', ok: '取消定时器', danger: true });
  });
  FS.action('more:timer-ring', function () { FS.overlays.show('OV-09', { name: '煮蛋', src: '菜谱·番茄炒蛋' }); });
  FS.action('more:timer-preset', function (el) {
    var m = el.getAttribute('data-arg') || '5';
    toast('演示：已启动 ' + m + ' 分钟倒计时');
  });

  /* MORE-07 闹钟开关 */
  FS.action('more:alarm-toggle', function (el) {
    var id = el.getAttribute('data-arg');
    var t = FS.data.timers.filter(function (x) { return x.id === id; })[0];
    if (!t) return;
    t.state = t.state === 'armed' ? 'done' : 'armed';
    toast(t.state === 'armed' ? '已开启「' + t.name + '」' : '已关闭「' + t.name + '」');
    if (FS.shell && FS.shell.renderPage) FS.shell.renderPage();
  });

  /* MORE-08 秒表 */
  function swRerender() {
    if (FS.router.currentId() === 'MORE-08' && FS.shell && FS.shell.renderPage) FS.shell.renderPage();
  }
  FS.action('more:sw-toggle', function () {
    if (swState.running) {
      swState.ms = swState.base + (Date.now() - swState.startedAt);
      swState.base = swState.ms;
      swState.running = false;
      swState.startedAt = 0;
      if (swState.tick) { clearInterval(swState.tick); swState.tick = null; }
    } else {
      swState.running = true;
      swState.startedAt = Date.now();
    }
    swRerender();
  });
  FS.action('more:sw-lap', function () {
    if (swState.running) swState.ms = swState.base + (Date.now() - swState.startedAt);
    if (!swState.ms) return;
    swState.laps.unshift(fmtSw(swState.ms));
    swRerender();
  });
  FS.action('more:sw-reset', function () {
    swState.running = false;
    swState.ms = 0;
    swState.base = 0;
    swState.startedAt = 0;
    swState.laps = [];
    if (swState.tick) { clearInterval(swState.tick); swState.tick = null; }
    swRerender();
  });

  /* MORE-03 编辑表单（演示） */
  FS.action('more:tt-type', function () { toast('演示：切换类型，表单随之变化'); });
  FS.action('more:tt-hour', function () { toast('演示：调整小时'); });
  FS.action('more:tt-min', function () { toast('演示：调整分钟'); });
  FS.action('more:tt-preset', function () { FS.overlays.show('OV-12', { tab: '数值' }); });
  FS.action('more:tt-time', function () { FS.overlays.show('OV-12', { tab: '日期' }); });
  FS.action('more:tt-name', function () { FS.overlays.show('OV-12', { tab: '键盘', preedit: 'zhu dan', cands: ['煮蛋', '蒸蛋', '煮粥', '炖汤', '解冻'] }); });
  FS.action('more:tt-repeat', function () { toast('演示：选择重复规则'); });
  FS.action('more:tt-sound', function () { toast('演示：切换提醒声音'); });
  FS.action('more:tt-say', function () { FS.overlays.show('OV-12', { tab: '键盘', preedit: 'ji dan', cands: ['鸡蛋煮好了', '可以出锅了', '时间到', '请查看', '记得翻面'] }); });
  FS.action('more:tt-link', function () { toast('演示：选择关联菜谱 / 计划 / 食材'); });
  FS.action('more:tt-sync', function () { toast('演示：切换同步到小程序'); });
  FS.action('more:tt-preview', function () { toast('演示：播放提醒音（铃声 + 语音播报）'); });
  FS.action('more:tt-save', function () { toast('已保存（演示）'); FS.router.back(); });
  FS.action('more:tt-start', function () { toast('已生效（演示）'); FS.router.back(); });
  FS.action('more:tt-cancel-edit', function () { FS.router.back(); });
  FS.action('more:tt-del', function () {
    FS.overlays.show('OV-03', { title: '删除这条提醒？', desc: '删除后不可恢复。', ok: '删除', danger: true });
  });

  /* MORE-04 设备 / 连接 / 传感器 / 安全 */
  FS.action('more:dev-reinit', function (el) { toast('演示：重新初始化 ' + el.getAttribute('data-arg')); });
  FS.action('more:wifi-conn', function (el) { toast('演示：连接到 ' + el.getAttribute('data-arg')); });
  FS.action('more:reconnect', function () { toast('演示：正在重新连接云端…'); });
  FS.action('more:reprov', function () { toast('演示：进入配网流程'); });
  FS.action('more:sensor-cal', function (el) { toast('演示：校准 ' + el.getAttribute('data-arg')); });
  FS.action('more:safety-ack', function () { toast('已确认告警（演示）'); });

  /* MORE-05 设置修改（统一 toast 演示） */
  FS.action('more:set-toggle', function (el) { toast('演示：已切换「' + el.getAttribute('data-arg').split('/')[1] + '」'); });
  FS.action('more:set-seg', function () { toast('演示：已切换权限模式'); });
  FS.action('more:set-value', function (el) { toast('演示：进入「' + el.getAttribute('data-arg').split('/')[1] + '」配置'); });
  FS.action('more:set-btn', function (el) { toast('演示：' + el.getAttribute('data-arg') + '…'); });

  /* MORE-06 维护 */
  FS.action('more:clear-cache', function () { toast('演示：缓存已清理，释放 1.8G'); });
  FS.action('more:export-diag', function () { toast('演示：诊断摘要已导出到 TF 卡'); });
  FS.action('more:plugin-toggle', function (el) { toast('演示：切换插件「' + el.getAttribute('data-arg') + '」启用状态'); });
  FS.action('more:export-log', function () { toast('演示：日志已导出'); });
  FS.action('more:ota-check', function () { toast('演示：正在检查更新…'); });
  FS.action('more:ota-start', function () {
    FS.overlays.show('OV-03', { title: '开始 OTA 升级？', desc: '将升级到 ' + FS.data.ota.latest + '（' + FS.data.ota.size + '），过程中请勿断电。', ok: '开始升级', danger: false });
  });
  FS.action('more:factory-reset', function () {
    FS.overlays.show('OV-04', {
      title: '恢复出厂设置', risk: '高',
      objects: '全部库存、设置、会话与本地数据',
      note: '此操作不可恢复，设备将回到首次开机状态。请确认已备份重要数据。',
      src: '维护中心 · 关于本机'
    });
  });
})();
