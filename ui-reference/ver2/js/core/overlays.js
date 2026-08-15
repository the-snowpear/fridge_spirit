/**
 * 冰箱小精灵 ver2 UI 预览 —— 全局浮层栈（OV-01..OV-12）
 * ------------------------------------------------------------------
 * 规则（对齐规划文档）：
 *  - 阻塞型（OV-03/04/10/12）：铺 scrim 遮罩并拦截下层交互；返回键先关浮层；
 *  - OV-10 关键安全告警：全屏 1024×600 接管（P0，最高抢占级）；
 *  - OV-01 toast 2.5s 自消；OV-02 通知条嵌主区顶部，不阻塞；
 *  - OV-12 软键盘/选择器停靠主区底部（滚轮 = lv_roller 对照）。
 * 浮层显隐会通知侧舱刷新（CTX 自动跟随，如 OV-05 → CTX-03）。
 */
window.FS = window.FS || {};

FS.overlays = (function () {
  var ui = function () { return FS.ui; };
  var defs = {};       // id → { id, name, blocking, dock?, render(data) }
  var stack = [];      // 打开中的浮层 [{ id, data }]
  var toastTimer = null;

  function register(def) { defs[def.id] = def; }

  function show(id, data) {
    var def = defs[id];
    if (!def) return;
    /* 同一浮层不重复入栈，仅刷新数据 */
    var exist = stack.find(function (o) { return o.id === id; });
    if (exist) exist.data = data || exist.data;
    else stack.push({ id: id, data: data || (def.defaults ? def.defaults() : {}) });
    render();
    /* toast 2.5s 自消（计时逻辑放 show，避免总览缩略图触发计时器） */
    if (id === 'OV-01') {
      clearTimeout(toastTimer);
      toastTimer = setTimeout(function () { close('OV-01'); }, 2500);
    }
  }

  function close(id) {
    if (!id) { stack.pop(); }
    else stack = stack.filter(function (o) { return o.id !== id; });
    render();
  }

  function closeAll() { stack = []; render(); }

  function top() { return stack.length ? stack[stack.length - 1] : null; }

  /* 栈顶是否为阻塞型（返回键优先关它；shell 据此拦截页面交互） */
  function topBlocking() {
    var t = top();
    return t && defs[t.id] && defs[t.id].blocking;
  }

  function isOpen(id) { return stack.some(function (o) { return o.id === id; }); }

  /* 渲染整个浮层根（阻塞层在下，非阻塞在上，按入栈顺序） */
  function render() {
    var root = document.getElementById('ovl-root');
    if (!root) return;
    var html = '';
    var hasBlocking = false;
    stack.forEach(function (o) {
      var def = defs[o.id];
      if (!def) return;
      if (def.blocking) {
        hasBlocking = true;
        html += '<div class="ovl-scrim" data-act="ovl:scrim" data-arg="' + o.id + '"></div>';
      }
      html += '<div class="ovl-slot ' + (def.dock || '') + '" data-ovl="' + o.id + '">' + def.render(o.data) + '</div>';
    });
    root.innerHTML = html;
    root.classList.toggle('has-blocking', hasBlocking);
    if (FS.cabin && FS.cabin.refresh) FS.cabin.refresh();
  }

  /* scrim 点击：阻塞型确认类默认不关（规范：需明确选择），仅关闭可取消的 */
  FS.action('ovl:scrim', function (el) {
    var id = el.getAttribute('data-arg');
    if (id === 'OV-12') close(id);      /* 键盘可点遮罩收起 */
  });

  FS.action('ovl:close', function (el) { close(el.getAttribute('data-arg')); });
  FS.action('ovl:close-all', function () { closeAll(); });

  /* =============== OV-01 轻提示 Toast（非阻塞，2.5s 自消） =============== */
  register({
    id: 'OV-01', name: '轻提示 Toast', blocking: false, dock: 'dock-toast',
    defaults: function () { return { tone: 'ok', text: '已保存' }; },
    render: function (d) {
      return '<div class="toast ' + (d.tone || 'ok') + '">' + FS.icon(d.tone === 'danger' ? 'danger' : d.tone === 'warn' ? 'warn' : 'check') +
        '<span>' + FS.esc(d.text) + '</span></div>';
    }
  });

  /* =============== OV-02 顶部通知条（非阻塞，嵌入主区顶部） =============== */
  register({
    id: 'OV-02', name: '顶部/区域通知条', blocking: false, dock: 'dock-topbar',
    defaults: function () { return { tone: 'warn', icon: 'cloudoff', text: '网络已断开，进入离线模式，操作将保存在本地', act: '重连' }; },
    render: function (d) {
      return '<div class="notice-bar ' + (d.tone || 'warn') + '">' + FS.icon(d.icon || 'info') +
        '<span class="nb-text">' + FS.esc(d.text) + '</span>' +
        (d.act ? '<button class="nb-act" data-act="ovl:demo" data-arg="通知条动作">' + FS.esc(d.act) + '</button>' : '') +
        '<button class="nb-close" data-act="ovl:close" data-arg="OV-02">' + FS.icon('x') + '</button></div>';
    }
  });

  /* =============== OV-03 通用确认框（阻塞） =============== */
  register({
    id: 'OV-03', name: '通用确认框', blocking: true, dock: 'dock-center',
    defaults: function () { return { title: '删除这条食材？', desc: '「剩米饭」将从库存中移除，此操作不可恢复。', ok: '删除', danger: true }; },
    render: function (d) {
      return '<div class="dialog">' +
        '<div class="dlg-title">' + FS.esc(d.title) + '</div>' +
        '<div class="dlg-desc">' + FS.esc(d.desc) + '</div>' +
        '<div class="dlg-btns">' +
          ui().btn({ label: '取消', kind: 'ghost', act: 'ovl:close', arg: 'OV-03' }) +
          ui().btn({ label: d.ok || '确认', kind: d.danger ? 'danger' : 'primary', act: 'ovl:confirm', arg: 'OV-03' }) +
        '</div></div>';
    }
  });

  /* =============== OV-04 Agent 高风险操作确认（阻塞） =============== */
  register({
    id: 'OV-04', name: 'Agent 高风险操作确认', blocking: true, dock: 'dock-center',
    defaults: function () {
      return { title: '删除过期批次「吐司面包」', risk: '高', objects: '吐司面包 ×6片（左冷藏/第1层）', note: '删除后不可恢复，且会同步到云端家庭库存。', src: 'Agent · 会话「今晚吃什么」' };
    },
    render: function (d) {
      return '<div class="dialog dlg-risk">' +
        '<div class="dlg-risk-head">' + FS.icon('danger', 'lg') + '<div><div class="dlg-title">Agent 请求高风险操作</div>' +
        '<div class="dlg-sub">' + FS.esc(d.src) + '</div></div>' +
        '<span class="chip danger">高风险</span></div>' +
        '<div class="dlg-risk-body">' +
          ui().kv('操作', d.title) +
          ui().kv('影响对象', d.objects) +
          ui().kv('可恢复性', '不可恢复', 'danger-text') +
        '</div>' +
        '<div class="dlg-note">' + FS.icon('info') + FS.esc(d.note) + '</div>' +
        '<div class="dlg-btns">' +
          ui().btn({ label: '拒绝', kind: 'ghost', act: 'ovl:close', arg: 'OV-04' }) +
          ui().btn({ label: '修改参数', kind: 'ghost', act: 'ovl:demo', arg: '修改参数' }) +
          ui().btn({ label: '确认执行', kind: 'danger', act: 'ovl:confirm', arg: 'OV-04' }) +
        '</div></div>';
    }
  });

  /* =============== OV-05 语音聆听与转写层（非阻塞） =============== */
  register({
    id: 'OV-05', name: '语音聆听与转写层', blocking: false, dock: 'dock-voice',
    defaults: function () { return { state: '聆听中', text: '帮我把昨天买的菠菜登记到冷藏…' }; },
    render: function (d) {
      return '<div class="voice-panel">' +
        '<div class="vp-mic"><span class="vp-ring"></span>' + FS.icon('mic', 'xl') + '</div>' +
        '<div class="vp-main"><div class="vp-state">' + FS.esc(d.state) + ' <span class="vp-dots"><i></i><i></i><i></i></span></div>' +
        '<div class="vp-text">' + FS.esc(d.text) + '</div></div>' +
        '<div class="vp-btns">' +
          ui().btn({ label: '取消', kind: 'ghost', act: 'ovl:close', arg: 'OV-05' }) +
          ui().btn({ label: '提交', kind: 'primary', icon: 'check', act: 'ovl:confirm', arg: 'OV-05' }) +
        '</div></div>';
    }
  });

  /* =============== OV-06 Agent 处理状态层（非阻塞，可中止） =============== */
  register({
    id: 'OV-06', name: 'Agent 处理状态层', blocking: false, dock: 'dock-agent',
    defaults: function () { return { step: 1, steps: ['理解意图', '规划步骤', '执行工具：inventory_query', '生成回复'] }; },
    render: function (d) {
      var html = '<div class="agent-panel"><div class="ap-head">' + FS.icon('zap') + '<span>Agent 正在处理</span></div><div class="ap-steps">';
      d.steps.forEach(function (s, i) {
        var st = i < d.step ? 'done' : i === d.step ? 'doing' : 'todo';
        html += '<div class="ap-step ' + st + '">' + (st === 'done' ? FS.icon('check') : '<span class="ap-dot"></span>') + FS.esc(s) + '</div>';
      });
      return html + '</div><div class="ap-btns">' +
        ui().btn({ label: '中止', kind: 'ghost', size: 'sm', act: 'ovl:close', arg: 'OV-06' }) + '</div></div>';
    }
  });

  /* =============== OV-07 拍照上传与识别进度（非阻塞） =============== */
  register({
    id: 'OV-07', name: '拍照上传与识别进度', blocking: false, dock: 'dock-agent',
    defaults: function () { return { step: 1, steps: ['上传照片', '云端识别', '等待确认'], pct: 62 }; },
    render: function (d) {
      var html = '<div class="agent-panel"><div class="ap-head">' + FS.icon('camera') + '<span>拍照识别</span></div>' +
        ui().progress(d.pct || 50, 'ok') + '<div class="ap-steps">';
      d.steps.forEach(function (s, i) {
        var st = i < d.step ? 'done' : i === d.step ? 'doing' : 'todo';
        html += '<div class="ap-step ' + st + '">' + (st === 'done' ? FS.icon('check') : '<span class="ap-dot"></span>') + FS.esc(s) + '</div>';
      });
      return html + '</div></div>';
    }
  });

  /* =============== OV-08 同步/离线状态层（非阻塞） =============== */
  register({
    id: 'OV-08', name: '同步/离线状态层', blocking: false, dock: 'dock-topbar',
    defaults: function () { return { pending: 3 }; },
    render: function (d) {
      return '<div class="notice-bar offline">' + FS.icon('cloudoff') +
        '<span class="nb-text">离线模式：所有功能可本地使用，<b>' + d.pending + ' 条变更</b>等待联网后同步</span>' +
        '<button class="nb-act" data-act="ovl:demo" data-arg="立即同步">立即同步</button>' +
        '<button class="nb-close" data-act="ovl:close" data-arg="OV-08">' + FS.icon('x') + '</button></div>';
    }
  });

  /* =============== OV-09 定时器/闹钟到时层（非阻塞，P2） =============== */
  register({
    id: 'OV-09', name: '定时器/闹钟到时层', blocking: false, dock: 'dock-center',
    defaults: function () { return { name: '煮蛋', src: '菜谱·番茄炒蛋' }; },
    render: function (d) {
      return '<div class="ring-panel">' +
        '<div class="rp-icon">' + FS.icon('timer', 'xl') + '</div>' +
        '<div class="rp-face">00:00</div>' +
        '<div class="rp-name">' + FS.esc(d.name) + ' 时间到！</div>' +
        '<div class="rp-sub">来源：' + FS.esc(d.src) + '</div>' +
        '<div class="rp-btns">' +
          ui().btn({ label: '结束', kind: 'primary', act: 'ovl:close', arg: 'OV-09' }) +
          ui().btn({ label: '延长1分钟', kind: 'ghost', act: 'ovl:demo', arg: '延长1分钟' }) +
          ui().btn({ label: '稍后提醒', kind: 'ghost', act: 'ovl:close', arg: 'OV-09' }) +
        '</div></div>';
    }
  });

  /* =============== OV-10 关键安全告警（阻塞，P0 全屏接管） =============== */
  register({
    id: 'OV-10', name: '关键安全告警', blocking: true, dock: 'dock-full',
    defaults: function () { return { title: '冷藏温度异常升高', desc: '冷藏室温度已升至 9.6°C 并持续 10 分钟，请检查冰箱门是否关严。', level: '严重' }; },
    render: function (d) {
      return '<div class="safety-full">' +
        '<div class="sf-icon">' + FS.icon('warn', 'xl') + '</div>' +
        '<div class="sf-level">关键安全告警 · ' + FS.esc(d.level) + '</div>' +
        '<div class="sf-title">' + FS.esc(d.title) + '</div>' +
        '<div class="sf-desc">' + FS.esc(d.desc) + '</div>' +
        '<div class="sf-btns">' +
          ui().btn({ label: '我知道了', kind: 'danger', size: 'big', act: 'ovl:close', arg: 'OV-10' }) +
          ui().btn({ label: '查看设备状态', kind: 'ghost', size: 'big', act: 'ovl:goto', arg: '#/p/MORE-04' }) +
        '</div>' +
        '<div class="sf-hint">告警确认前将持续播报（P0 最高优先级，不自动关闭）</div></div>';
    }
  });

  /* =============== OV-11 快速添加面板（非阻塞，底部面板） =============== */
  register({
    id: 'OV-11', name: '快速添加面板', blocking: false, dock: 'dock-sheet',
    defaults: function () { return {}; },
    render: function () {
      return '<div class="sheet">' +
        '<div class="sheet-head"><span class="dlg-title">快速登记食材</span>' +
        '<button class="nb-close" data-act="ovl:close" data-arg="OV-11">' + FS.icon('x') + '</button></div>' +
        '<div class="sheet-opts">' +
          '<button class="sheet-opt" data-act="ovl:goto" data-arg="#/p/INV-05"><span class="so-icon sage">' + FS.icon('camera', 'lg') + '</span><span class="so-label">拍照登记</span><span class="so-sub">识别食材并确认</span></button>' +
          '<button class="sheet-opt" data-act="ovl:goto" data-arg="voice:add"><span class="so-icon yolk">' + FS.icon('mic', 'lg') + '</span><span class="so-label">语音登记</span><span class="so-sub">说出食材和数量</span></button>' +
          '<button class="sheet-opt" data-act="ovl:goto" data-arg="#/p/INV-04"><span class="so-icon blue">' + FS.icon('edit', 'lg') + '</span><span class="so-label">手动登记</span><span class="so-sub">逐项填写信息</span></button>' +
        '</div></div>';
    }
  });

  /* =============== OV-12 软键盘/数值与日期选择器（阻塞，停靠主区底部） =============== */
  register({
    id: 'OV-12', name: '软键盘/数值与日期选择器', blocking: true, dock: 'dock-kb',
    defaults: function () { return { tab: '键盘', preedit: 'bo cai', cands: ['菠菜', '薄饼', '白菜', '煲仔', '包菜'] }; },
    render: function (d) {
      var rows = [
        ['q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'],
        ['a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l'],
        ['⇧', 'z', 'x', 'c', 'v', 'b', 'n', 'm', '⌫']
      ];
      var kb = '<div class="kb">';
      rows.forEach(function (r) {
        kb += '<div class="kb-row">';
        r.forEach(function (k) { kb += '<button class="kb-key" data-act="ovl:demo" data-arg="按键 ' + k + '">' + k + '</button>'; });
        kb += '</div>';
      });
      kb += '<div class="kb-row"><button class="kb-key wide" data-act="ovl:demo" data-arg="符号">符</button>' +
        '<button class="kb-key space" data-act="ovl:demo" data-arg="空格">空格</button>' +
        '<button class="kb-key wide ok" data-act="ovl:close" data-arg="OV-12">完成</button></div></div>';

      var roller = function (cols) {
        var h = '<div class="roller-wrap">';
        cols.forEach(function (c) {
          h += '<div class="roller">';
          c.items.forEach(function (v, i) { h += '<div class="roller-item' + (i === c.cur ? ' cur' : '') + '">' + v + '</div>'; });
          h += '</div><span class="roller-unit">' + c.unit + '</span>';
        });
        return h + '</div>';
      };
      var numPicker = roller([
        { items: ['0', '1', '2', '3', '4'], cur: 2, unit: '个' }
      ]) + '<div class="picker-note">数量选择（= lv_roller）</div>';
      var datePicker = roller([
        { items: ['2025', '2026', '2027'], cur: 1, unit: '年' },
        { items: ['06', '07', '08'], cur: 1, unit: '月' },
        { items: ['27', '28', '29'], cur: 1, unit: '日' }
      ]);

      var tabBody = d.tab === '数值' ? numPicker : d.tab === '日期' ? datePicker :
        '<div class="kb-cands">' + d.cands.map(function (c, i) {
          return '<button class="kb-cand' + (i === 0 ? ' on' : '') + '" data-act="ovl:demo" data-arg="候选 ' + c + '">' + c + '</button>';
        }).join('') + '</div>' +
        '<div class="kb-preedit">' + FS.esc(d.preedit) + '<span class="kb-caret"></span></div>' + kb;

      return '<div class="kb-panel">' +
        '<div class="kb-tabs">' + ['键盘', '数值', '日期'].map(function (t) {
          return '<button class="kb-tab' + (t === d.tab ? ' on' : '') + '" data-act="ovl:kb-tab" data-arg="' + t + '">' + t + '</button>';
        }).join('') +
        '<button class="nb-close" data-act="ovl:close" data-arg="OV-12">' + FS.icon('x') + '</button></div>' +
        tabBody + '</div>';
    }
  });

  /* 浮层内动作：tab 切换 / 演示动作 / 确认后给反馈 / 跳转并关闭浮层 */
  FS.action('ovl:kb-tab', function (el) {
    var t = stack.find(function (o) { return o.id === 'OV-12'; });
    if (t) { t.data.tab = el.getAttribute('data-arg'); render(); }
  });
  FS.action('ovl:demo', function (el) {
    show('OV-01', { tone: 'ok', text: '演示：' + el.getAttribute('data-arg') });
  });
  FS.action('ovl:confirm', function (el) {
    close(el.getAttribute('data-arg'));
    show('OV-01', { tone: 'ok', text: '已确认' });
  });
  FS.action('ovl:goto', function (el) {
    var arg = el.getAttribute('data-arg');
    closeAll();
    if (arg === 'voice:add') { show('OV-05'); return; }
    FS.router.go(arg);
  });

  /* 供页面总览使用：渲染单个浮层的静态快照（不入栈、不触发计时器） */
  function renderOne(id) {
    var def = defs[id];
    if (!def) return '';
    var data = def.defaults ? def.defaults() : {};
    return (def.blocking ? '<div class="ovl-scrim"></div>' : '') +
      '<div class="ovl-slot ' + (def.dock || '') + '">' + def.render(data) + '</div>';
  }

  return {
    register: register, show: show, close: close, closeAll: closeAll,
    top: top, topBlocking: topBlocking, isOpen: isOpen, render: render,
    renderOne: renderOne,
    list: function () { return Object.keys(defs).map(function (k) { return defs[k]; }); }
  };
})();
