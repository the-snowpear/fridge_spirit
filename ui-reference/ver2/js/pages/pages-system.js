/**
 * 冰箱小精灵 ver2 UI 预览 —— 系统模块（SYS-01 / SYS-02 / SYS-03）
 * ------------------------------------------------------------------
 * SYS-01 启动与自检（cabinMode:boot）：品牌区 + 启动进度 + 自检清单 +
 *   网络/数据恢复状态 + 可降级异常提示 + 进入系统/维护模式。
 * SYS-02 首次配网与设备绑定向导（cabinMode:wizard）：?step=1..4 驱动的
 *   4 步向导（欢迎语言 / 网络配置 / 语音权限 / 冰箱布局），顶部步进指示器。
 * SYS-03 待机颜文字（cabinMode:standby）：深墨绿低亮度整页，超大颜文字 +
 *   当前时间 + 一行提示 + 三个状态 chip，演示「人靠近回到首页」。
 * 三个页面均为 system:true（隐藏 72px 侧导航、主区 776px），整页自绘不用 ui.pageHead，
 * 仅在主区右上角放页面 ID 角标（预览工具栏可统一隐藏）。
 */
window.FS = window.FS || {};

(function () {
  var ui;
  function ready() { ui = FS.ui; }
  var esc = FS.esc;

  /* 系统页通用外壳：776px 整页 + 右上角页面 ID 角标 */
  function sysPage(id, cls, innerHtml) {
    return '<div class="sys-page ' + (cls || '') + '">' +
      '<span class="pg-id">' + esc(id) + '</span>' + innerHtml + '</div>';
  }

  /* ==================== SYS-01 启动与自检 ==================== */
  FS.router.register({
    id: 'SYS-01', name: '启动与自检', module: '系统', level: '一级页',
    system: true, cabinMode: 'boot', states: [],

    render: function () {
      ready();
      var d = FS.data;

      /* 品牌区 */
      var brand = '<div class="sys-brand">' +
        '<div class="sys-brand-name">冰箱小精灵</div>' +
        '<div class="sys-brand-ver">' + esc(d.device.version) + '</div></div>';

      /* 启动进度条 */
      var prog = '<div class="sys-boot-progress">' + ui.progress(86) +
        '<div class="sys-boot-progress-text">正在启动 … 86%</div></div>';

      /* 自检清单（FS.data.bootChecks 逐行，2 列网格） */
      var rows = d.bootChecks.map(function (c) {
        var ok = c.status === 'ok';
        return '<div class="sys-check-row">' +
          '<span class="sys-check-ic ' + (ok ? 'ok' : 'warn') + '">' + FS.icon(ok ? 'check' : 'warn') + '</span>' +
          '<span class="sys-check-name">' + esc(c.name) + '</span>' +
          (c.note ? '<span class="sys-check-note">' + esc(c.note) + '</span>' : '') +
          '</div>';
      }).join('');
      var checks = '<div class="sys-boot-checks"><div class="sys-check-grid">' + rows + '</div></div>';

      /* 网络与数据恢复状态行 */
      var net = '<div class="sys-boot-net">' + ui.statusDot('ok') +
        '<span>Wi-Fi 已连接（' + esc(d.device.wifi.ssid) + '） · 本地数据校验通过</span></div>';

      /* 异常提示演示卡（可降级运行） */
      var warn = '<div class="sys-boot-warn">' + FS.icon('warn') +
        '<div class="sys-boot-warn-text"><b>FY-SP703 未插入</b>，已切换板载麦克风，可降级运行；' +
        '语音唤醒与识别仍可正常使用。</div></div>';

      /* 底部按钮 */
      var foot = '<div class="sys-boot-foot">' +
        ui.btn({ label: '维护模式', kind: 'ghost', act: 'nav', href: '#/p/MORE-06' }) +
        '<span class="spacer"></span>' +
        ui.btn({ label: '进入系统', kind: 'primary', size: 'big', act: 'nav', href: '#/p/HOME-01' }) +
        '</div>';

      return sysPage('SYS-01', '', brand + prog + checks + net + warn + foot);
    }
  });

  /* ==================== SYS-02 首次配网与设备绑定向导 ==================== */
  var WIZ_STEPS = ['欢迎与语言', '网络配置', '语音与权限', '冰箱布局'];

  /* 顶部 4 步指示器：当前步高亮、已完成打勾 */
  function wizSteps(cur) {
    var html = '<div class="sys-steps">';
    WIZ_STEPS.forEach(function (label, i) {
      var n = i + 1;
      var cls = n < cur ? 'done' : (n === cur ? 'on' : '');
      var dot = n < cur ? FS.icon('check') : String(n);
      html += '<div class="sys-step ' + cls + '">' +
        '<span class="sys-step-dot">' + dot + '</span>' +
        '<span class="sys-step-label">' + n + ' ' + esc(label) + '</span></div>';
      if (n < WIZ_STEPS.length) html += '<span class="sys-step-line"></span>';
    });
    return html + '</div>';
  }

  /* 各步主体内容 */
  function wizBody(step) {
    var d = FS.data;

    /* 第 1 步：欢迎与语言/时区 */
    if (step === 1) {
      return '<div class="sys-wiz-title">欢迎使用冰箱小精灵</div>' +
        '<div class="sys-wiz-sub">让冰箱更懂你的厨房。请用 1 分钟完成初始设置。</div>' +
        ui.card({
          cls: 'sys-wiz-card', title: '设备介绍',
          body: ui.listRow({ icon: 'mic', title: '语音交互', sub: '呼叫「小冰小冰」，说话就能记库存、问菜谱', arrow: false }) +
                ui.listRow({ icon: 'camera', title: '拍照识别', sub: '拍一拍食材，自动登记名称与保质期', arrow: false }) +
                ui.listRow({ icon: 'bell', title: '临期提醒', sub: '主动提醒快过期的食材，减少浪费', arrow: false })
        }) +
        ui.card({
          cls: 'sys-wiz-card', title: '语言与时区',
          body: ui.formRow({ label: '系统语言', control: ui.inputLike({ value: '简体中文', act: 'sys:lang' }) }) +
                ui.formRow({ label: '时区', control: ui.inputLike({ value: '中国标准时间 (UTC+8)', act: 'sys:tz' }) })
        });
    }

    /* 第 2 步：Wi-Fi 网络配置 */
    if (step === 2) {
      var list = d.wifiList.map(function (w) {
        return '<div class="sys-wifi-row' + (w.connected ? ' sel' : '') + '" data-act="sys:wifi" data-arg="' + esc(w.ssid) + '">' +
          '<span class="sys-wifi-ic">' + FS.icon('wifi') + '</span>' +
          '<span class="sys-wifi-ssid">' + esc(w.ssid) + '</span>' +
          (w.secured ? '<span class="sys-wifi-ic">' + FS.icon('lock') + '</span>' : '') +
          '<span class="sys-wifi-rssi">' + w.rssi + 'dBm</span>' +
          (w.connected ? '<span class="sys-wifi-ok">' + FS.icon('check') + '已连接</span>' : '') +
          '</div>';
      }).join('');
      return '<div class="sys-wiz-title">连接 Wi-Fi</div>' +
        '<div class="sys-wiz-sub">选择家庭网络，设备联网后才能使用云端与 Agent 功能。</div>' +
        ui.card({ cls: 'sys-wiz-card', title: '可用网络', body: '<div class="sys-wifi-list">' + list + '</div>' }) +
        ui.card({
          cls: 'sys-wiz-card', title: '密码',
          body: ui.formRow({ label: 'Wi-Fi 密码', control: ui.inputLike({ placeholder: '点击输入密码', icon: 'lock', act: 'sys:wifi-pwd' }) })
        }) +
        '<a class="sys-skip" data-act="sys:wiz-skip">跳过，使用离线模式 ›</a>';
    }

    /* 第 3 步：语音与权限 */
    if (step === 3) {
      return '<div class="sys-wiz-title">语音与隐私权限</div>' +
        '<div class="sys-wiz-sub">设置语音、拍照与 Agent 自动化程度，之后可随时在设置中心修改。</div>' +
        ui.card({
          cls: 'sys-wiz-card', title: '功能开关',
          body: ui.formRow({ label: '语音唤醒', desc: '唤醒词「小冰小冰」', control: ui.sw({ on: true, act: 'sys:toggle' }) }) +
                ui.formRow({ label: '拍照识别', desc: '允许拍摄食材照片用于识别', control: ui.sw({ on: true, act: 'sys:toggle' }) }) +
                ui.formRow({ label: '图片本地留存', desc: '识别完成后保留本地照片', control: ui.sw({ on: false, act: 'sys:toggle' }) })
        }) +
        ui.card({
          cls: 'sys-wiz-card', title: 'Agent 权限模式',
          body: ui.formRow({
            label: '权限模式', desc: '标准模式：低风险自动执行，高影响需确认',
            control: ui.seg({ options: ['确认', '标准', '自动'], value: '标准', act: 'sys:perm' })
          })
        }) +
        ui.card({
          cls: 'sys-wiz-card',
          body: '<div class="sys-privacy">' + FS.icon('shield') +
            '<div class="sys-privacy-text">隐私说明：语音与照片仅在你主动使用时上传云端处理，' +
            '本地可随时清除；Agent 自动操作前会按上方权限模式请求确认。</div></div>'
        });
    }

    /* 第 4 步：冰箱布局 */
    var layouts = [['三门冰箱', 'fridge'], ['对开门', 'box'], ['单门', 'fridge'], ['自定义', 'grid']];
    var cards = layouts.map(function (L, i) {
      return '<div class="sys-layout-card' + (i === 0 ? ' sel' : '') + '" data-act="sys:layout" data-arg="' + esc(L[0]) + '">' +
        FS.icon(L[1]) + '<span class="sys-layout-name">' + esc(L[0]) + '</span></div>';
    }).join('');
    return '<div class="sys-wiz-title">选择冰箱布局</div>' +
      '<div class="sys-wiz-sub">选择与你家冰箱最接近的结构，之后可在布局管理中调整。</div>' +
      ui.card({ cls: 'sys-wiz-card', title: '默认布局', body: '<div class="sys-layout-grid">' + cards + '</div>' }) +
      '<div class="sys-layout-preview">' +
        '<div class="sys-layout-mini"><i class="a"></i><i class="b"></i><i class="c"></i><i class="d"></i><i class="e"></i></div>' +
        '<div class="sys-layout-cap">布局示意：冷冻区 + 左/右冷藏 + 门架 + 果蔬抽屉</div>' +
      '</div>';
  }

  FS.router.register({
    id: 'SYS-02', name: '首次配网与设备绑定向导', module: '系统', level: '一级页',
    system: true, cabinMode: 'wizard', states: [],
    gallery: [
      { label: '第1步 欢迎', hash: '#/p/SYS-02?step=1' },
      { label: '第2步 网络配置', hash: '#/p/SYS-02?step=2' },
      { label: '第3步 语音与权限', hash: '#/p/SYS-02?step=3' },
      { label: '第4步 冰箱布局', hash: '#/p/SYS-02?step=4' }
    ],

    render: function (pg) {
      ready();
      var step = parseInt(pg.params.get('step') || '1', 10);
      if (isNaN(step) || step < 1 || step > 4) step = 1;

      /* 底部步骤按钮条 */
      var foot = '<div class="sys-wiz-foot">';
      if (step > 1) foot += ui.btn({ label: '上一步', kind: 'ghost', act: 'sys:wiz-step', arg: String(step - 1) });
      foot += '<span class="spacer"></span>';
      if (step < 4) {
        foot += ui.btn({ label: '下一步', kind: 'primary', act: 'sys:wiz-step', arg: String(step + 1) });
      } else {
        foot += ui.btn({ label: '完成并开始使用', kind: 'primary', size: 'big', act: 'nav', href: '#/p/HOME-01' });
      }
      foot += '</div>';

      return sysPage('SYS-02', '', wizSteps(step) +
        '<div class="sys-wiz-body">' + wizBody(step) + '</div>' + foot);
    }
  });

  /* ==================== SYS-03 待机颜文字 ==================== */
  FS.router.register({
    id: 'SYS-03', name: '待机颜文字', module: '系统', level: '一级页',
    system: true, cabinMode: 'standby', states: [],

    render: function () {
      ready();
      var d = FS.data;
      /* 取 '=w=' 颜文字（无则回退第一个） */
      var face = d.kaomoji.filter(function (k) { return k === '=w='; })[0] || d.kaomoji[0];

      var inner =
        '<div class="sys-kaomoji">' + esc(face) + '</div>' +
        '<div class="sys-standby-time">' + esc(d.device.time) + '</div>' +
        '<div class="sys-standby-date">' + esc(d.device.date) + '</div>' +
        '<div class="sys-standby-hint">' + esc(d.standbyHint) + '</div>' +
        '<div class="sys-standby-chips">' +
          '<span class="sys-standby-chip warn">' + FS.icon('clock') + '3 项临期</span>' +
          '<span class="sys-standby-chip">' + FS.icon('check') + '系统正常</span>' +
          '<span class="sys-standby-chip">' + FS.icon('mic') + '叫我「小冰小冰」</span>' +
        '</div>' +
        '<div class="sys-standby-back">' +
          ui.btn({ label: '人靠近，回到首页', kind: 'ghost', act: 'nav', href: '#/p/HOME-01' }) +
        '</div>';

      return sysPage('SYS-03', 'standby', inner);
    }
  });

  /* ==================== 模块内动作 ==================== */
  /* 向导步骤跳转 */
  FS.action('sys:wiz-step', function (el) {
    FS.router.go('#/p/SYS-02?step=' + encodeURIComponent(el.getAttribute('data-arg')));
  });
  /* 跳过联网 → 离线模式直接到下一步 */
  FS.action('sys:wiz-skip', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已进入离线模式（演示），稍后可随时联网' });
    FS.router.go('#/p/SYS-02?step=3');
  });
  /* Wi-Fi 选择（演示：仅提示） */
  FS.action('sys:wifi', function (el) {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已选择「' + el.getAttribute('data-arg') + '」（演示）' });
  });
  /* Wi-Fi 密码输入 → 弹 OV-12 软键盘 */
  FS.action('sys:wifi-pwd', function () { FS.overlays.show('OV-12'); });
  /* 语言 / 时区（演示） */
  FS.action('sys:lang', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '演示：语言选择' }); });
  FS.action('sys:tz', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '演示：时区选择' }); });
  /* 开关 / 权限 / 布局（演示） */
  FS.action('sys:toggle', function (el) { el.classList.toggle('on'); });
  FS.action('sys:perm', function (el) {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已切换为「' + el.getAttribute('data-arg') + '模式」（演示）' });
  });
  FS.action('sys:layout', function (el) {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已选择「' + el.getAttribute('data-arg') + '」布局（演示）' });
  });
})();
