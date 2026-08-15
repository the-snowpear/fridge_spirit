/**
 * 冰箱小精灵 ver2 UI 预览 —— 对话模块（CHAT-01 / CHAT-02 / CHAT-03 / CHAT-04）
 * ------------------------------------------------------------------
 * CHAT-01 对话主页面（一级）：消息流（user/agent 气泡、tool 工具卡、result 结果卡）
 *   + 顶部权限/同步状态行 + 底部输入区（常用指令 chips + 假输入框 + 语音 + 发送/停止互斥切换）。
 * CHAT-02 会话历史（二级）：搜索条 + 同步筛选 seg + 会话列表 + 行操作（继续/重命名/归档/删除）。
 * CHAT-03 工具执行详情（二级，?tool=name）：工具信息、权限决策、参数摘要、执行时间线、
 *   结果/错误/重试、关联业务对象与操作日志。
 * CHAT-04 待确认行动（任务页）：计划摘要 + 逐条待确认动作（风险/影响/不可逆）+ 底部固定操作条。
 * 数据一律读 FS.data.chat / FS.data.pendingActions / FS.data.device，不新造业务数据。
 */
window.FS = window.FS || {};

(function () {
  var ui, q;
  /* Agent 忙碌态：null=跟随消息流（存在 status=running 的 tool 即忙碌）；显式 true/false 覆盖（发送/停止后） */
  var agentBusyOverride = null;
  function ready() { ui = FS.ui; q = FS.dataQ; }
  function esc(s) { return FS.esc(s); }

  /* 是否有工具/Agent 正在工作（决定发送按钮位显示「发送」还是「停止」） */
  function isAgentWorking() {
    if (agentBusyOverride !== null) return agentBusyOverride;
    var msgs = (FS.data.chat && FS.data.chat.messages) || [];
    for (var i = 0; i < msgs.length; i++) {
      if (msgs[i].type === 'tool' && msgs[i].status === 'running') return true;
    }
    return false;
  }

  /* 局部刷新输入行右侧发送/停止按钮，避免整页重绘丢滚动位置 */
  function refreshSendStopBtn() {
    var slot = document.getElementById('chat-send-stop');
    if (!slot) return;
    slot.outerHTML = renderSendStopBtn();
  }

  function renderSendStopBtn() {
    if (isAgentWorking()) {
      return '<button id="chat-send-stop" class="icon-btn chat-btn-stop" data-act="chat:stop" title="停止">' +
        FS.icon('stop') + '</button>';
    }
    return '<button id="chat-send-stop" class="icon-btn chat-btn-send" data-act="chat:send" title="发送">' +
      FS.icon('send') + '</button>';
  }

  /* ---- 工具卡补充元数据（仅用于 CHAT-03 详情展示的静态演示参数，非业务数据） ---- */
  var TOOL_META = {
    inventory_query: {
      summary: '读取本地库存，筛选临期项',
      params: [['筛选条件', '剩余天数 ≤ 3 天'], ['范围', '全部冷藏分区'], ['返回', '6 项食材']],
      objects: [{ icon: 'box', title: '临期食材 6 项', sub: '原味酸奶 / 卤牛肉 / 菠菜 …', href: '#/p/INV-01' }],
      log: ['14:20:11  inventory_query called', '14:20:11  perm: 低风险 → 自动执行', '14:20:11  done (0.4s), 6 rows'],
      timelineCur: 4, reversible: false, resultHref: '#/p/INV-01'
    },
    menu_generate: {
      summary: '结合库存与口味偏好生成菜谱',
      params: [['目标', '今晚晚餐'], ['约束', '优先消耗临期食材'], ['偏好', '2 人份 · 简单快手']],
      objects: [{ icon: 'recipe', title: '推荐：番茄炒蛋', sub: '库存覆盖率 90% · 约15分钟', href: '#/p/PLAN-04?recipe=r1' }],
      log: ['14:24:52  menu_generate called', '14:24:52  perm: 中风险 → 标准模式自动', '14:24:55  llm streaming…'],
      timelineCur: 2, reversible: false, resultHref: '#/p/PLAN-04?recipe=r1'
    },
    inventory_delete: {
      summary: '从库存中删除一个批次（写入类）',
      params: [['目标批次', '吐司面包 ×6片'], ['位置', '左冷藏 / 第1层'], ['原因', '已过期 3 天'], ['可恢复性', '不可恢复']],
      objects: [{ icon: 'box', title: '吐司面包 ×6片', sub: '左冷藏 / 第1层 · 已过期 3 天', href: '#/p/INV-03?id=it13' }],
      log: ['14:25:03  inventory_delete requested', '14:25:03  perm: 高风险 → 需用户确认', '14:25:03  waiting for user confirm…'],
      timelineCur: 2, reversible: false, resultHref: '#/p/INV-03?id=it13', highRisk: true
    }
  };

  /* ---- 结果卡跳转目标映射（chat:result 按 kind） ---- */
  var RESULT_HREF = {
    recipe: function (m) { return '#/p/PLAN-04?recipe=' + encodeURIComponent((m && m.refId) || 'r1'); },
    shopping: function () { return '#/p/PLAN-06'; },
    inventory: function () { return '#/p/INV-01'; }
  };

  /* 从消息流取某个 tool（按 name；找不到回退 inventory_delete） */
  function findTool(name) {
    var msgs = FS.data.chat.messages;
    var t = msgs.filter(function (m) { return m.type === 'tool' && m.name === name; })[0];
    if (!t) t = msgs.filter(function (m) { return m.type === 'tool' && m.name === 'inventory_delete'; })[0];
    return t;
  }

  /* ==================== CHAT-01 对话主页面 ==================== */
  FS.router.register({
    id: 'CHAT-01', name: '对话', module: '对话', level: '一级页', ctx: 'CTX-05',
    states: ['loading', 'offline', 'error'],

    render: function (pg) {
      ready();
      var d = FS.data;
      var msgs = d.chat.messages;

      /* 消息流：按 type 渲染；待确认工具卡下加「去确认」 */
      var flow = msgs.map(function (m) {
        if (m.type === 'user' || m.type === 'agent') return ui.chatBubble(m);
        if (m.type === 'tool') {
          var card = ui.toolCard(m);
          if (m.status === 'pending') {
            card += '<div class="chat-tool-act">' +
              ui.btn({ label: '去确认', kind: 'warn', size: 'sm', icon: 'shield', act: 'nav', href: '#/p/CHAT-04' }) +
              '<span class="chat-tool-note">标准模式：高风险操作需你确认后才执行</span></div>';
          }
          return card;
        }
        if (m.type === 'result') return ui.resultCard(m);
        return '';
      }).join('');

      /* 顶部状态行：权限模式 chip + 会话同步状态 + 历史入口 */
      var permChip = ui.chip(d.device.permission, d.device.permission === '标准模式' ? 'ok' : 'warn', 'shield');
      var syncDot = ui.statusDot(d.device.online ? 'ok' : 'offline', d.device.online ? '已同步' : '本地');
      var statusRow = '<div class="chat-status">' +
        '<div class="chat-status-left">' + permChip + syncDot + '</div>' +
        '<a class="chat-hist-link" data-act="nav" data-href="#/p/CHAT-02">' + FS.icon('history') + '会话历史</a>' +
        '</div>';

      /* 底部输入区：常用指令 + 输入框 + 语音 + 发送/停止（同一位置互斥切换，无独立底栏按钮） */
      var quickChips = ['今晚吃什么', '登记食材', '看看临期'].map(function (t) {
        return '<button class="chip chat-cmd" data-act="chat:quick" data-arg="' + esc(t) + '">' + esc(t) + '</button>';
      }).join('');
      var inputBar = '<div class="chat-input">' +
        '<div class="chat-cmds">' + quickChips + '</div>' +
        '<div class="chat-input-row">' +
          ui.inputLike({ placeholder: '对我说点什么，或点右侧语音…', icon: 'keyboard', act: 'chat:open-kb' }) +
          ui.iconBtn({ icon: 'mic', label: '语音输入', act: 'chat:voice' }) +
          renderSendStopBtn() +
        '</div>' +
        '</div>';

      /* 异步内容区（消息流）；状态切换器仅作用于消息区 */
      var asyncHtml = '<div class="chat-scroll-wrap">' + ui.scrollArea(flow, 'chat-scroll') + '</div>';

      return '<div class="pg">' +
        ui.pageHead({
          title: '对话', id: 'CHAT-01',
          sub: '与 Agent 讨论库存、菜谱和计划',
          actions: ui.iconBtn({ icon: 'history', label: '会话历史', act: 'nav', href: '#/p/CHAT-02' }) +
                   ui.iconBtn({ icon: 'plus', label: '新会话', act: 'chat:new' })
        }) +
        statusRow +
        '<div class="pg-body chat-body">' + FS.states.wrap(pg, asyncHtml, {
          emptyTitle: '还没有对话', emptyDesc: '试试下方常用指令或语音输入',
          errorDesc: '对话加载失败，请检查网络后重试'
        }) + '</div>' +
        inputBar +
        '</div>';
    }
  });

  /* ==================== CHAT-02 会话历史 ==================== */
  FS.router.register({
    id: 'CHAT-02', name: '会话历史', module: '对话', level: '二级页', ctx: 'CTX-05',
    states: ['loading', 'empty', 'error'],
    parent: 'CHAT-01',

    render: function (pg) {
      ready();
      var sessions = FS.data.chat.sessions;
      var params = pg.params || new URLSearchParams();
      var filter = params.get('filter') || 'all';
      var kw = params.get('kw') || '';

      /* 搜索条（假输入框，点击弹键盘）+ 同步筛选 seg */
      var searchBar = '<div class="chat-srch">' +
        ui.inputLike({ placeholder: '搜索会话标题或摘要…', icon: 'search', value: kw || '', act: 'chat:hist-search' }) +
        ui.seg({
          act: 'chat:hist-filter', value: filter,
          options: [{ value: 'all', label: '全部' }, { value: 'synced', label: '已同步' }, { value: 'local', label: '未同步' }]
        }) +
        '</div>';

      /* 过滤 */
      var list = sessions.filter(function (s) {
        if (filter === 'synced' && !s.synced) return false;
        if (filter === 'local' && s.synced) return false;
        if (kw && (s.title + s.summary).indexOf(kw) < 0) return false;
        return true;
      });

      /* 会话行：标题/时间/摘要/关联任务 chip/同步点 + 行操作图标 */
      var rows = list.map(function (s) {
        var syncDot = ui.statusDot(s.synced ? 'ok' : 'offline', s.synced ? '已同步' : '未同步');
        var taskChip = s.tasks ? ui.chip(s.tasks, 'info') : '';
        return '<div class="chat-sess">' +
          '<div class="chat-sess-main" data-act="chat:sess-open" data-arg="' + esc(s.id) + '">' +
            '<div class="chat-sess-title">' + esc(s.title) + taskChip + '</div>' +
            '<div class="chat-sess-sub">' + esc(s.summary) + '</div>' +
            '<div class="chat-sess-meta">' + syncDot + '<span class="chat-sess-time">' + esc(s.time) + '</span></div>' +
          '</div>' +
          '<div class="chat-sess-ops">' +
            ui.iconBtn({ icon: 'fwd', label: '继续会话', act: 'chat:sess-open', arg: s.id }) +
            ui.iconBtn({ icon: 'edit', label: '重命名', act: 'chat:sess-rename', arg: s.id }) +
            ui.iconBtn({ icon: 'download', label: '归档', act: 'chat:sess-archive', arg: s.id }) +
            ui.iconBtn({ icon: 'trash', label: '删除', act: 'chat:sess-del', arg: s.id, kind: 'danger' }) +
          '</div>' +
          '</div>';
      }).join('');

      var listCard = rows ||
        ui.emptyState({ icon: 'history', title: '没有匹配的会话', desc: filter === 'all' ? '历史会话会显示在这里' : '试试切换筛选条件' });

      var asyncHtml = searchBar + '<div class="chat-scroll-wrap">' + ui.scrollArea(listCard, 'chat-sess-scroll') + '</div>';

      return '<div class="pg">' +
        ui.pageHead({
          title: '会话历史', id: 'CHAT-02',
          sub: '查看、恢复和清理历史会话',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' }) +
                   ui.btn({ label: '新会话', kind: 'primary', size: 'sm', icon: 'plus', act: 'chat:new' })
        }) +
        '<div class="pg-body chat-body">' + FS.states.wrap(pg, asyncHtml, {
          emptyTitle: '还没有历史会话', emptyDesc: '与 Agent 的会话会保存在这里',
          errorDesc: '会话列表加载失败，请重试'
        }) + '</div></div>';
    }
  });

  /* ==================== CHAT-03 工具执行详情 ==================== */
  FS.router.register({
    id: 'CHAT-03', name: '工具执行详情', module: '对话', level: '二级页', ctx: 'CTX-05',
    states: ['error'],
    parent: 'CHAT-01',

    render: function (pg) {
      ready();
      var params = pg.params || new URLSearchParams();
      var t = findTool(params.get('tool') || 'inventory_delete');
      var meta = TOOL_META[t.name] || TOOL_META.inventory_delete;

      var riskTone = t.risk === '高' ? 'danger' : t.risk === '中' ? 'warn' : 'ok';
      var statusMap = { ok: ['ok', '已完成'], running: ['info', '执行中'], pending: ['warn', '待确认'], error: ['danger', '失败'] };
      var st = statusMap[t.status] || statusMap.ok;

      /* 工具信息 + 风险 + 权限决策说明 */
      var infoCard = ui.card({
        title: '工具信息',
        side: ui.chip(t.risk + '风险', riskTone),
        body:
          ui.kv('工具名称', t.label) +
          ui.kv('调用标识', t.name) +
          ui.kv('执行状态', st[1], st[0] === 'danger' ? 'danger-text' : st[0] === 'warn' ? 'warn-text' : 'ok-text') +
          (t.latency ? ui.kv('耗时', t.latency) : '') +
          ui.kv('功能说明', meta.summary) +
          '<div class="chat-perm">' + FS.icon('shield') +
            '<span><b>权限决策：</b>' + esc(FS.data.device.permission) +
            (t.risk === '高' ? '：高风险操作需用户确认，已挂起等待你处理。'
              : t.risk === '中' ? '：中等影响操作，标准模式下自动执行并留痕。'
              : '：低风险只读操作，自动执行。') + '</span></div>',
        cls: 'chat-tool-info'
      });

      /* 结构化参数摘要（kv 2-4 条） */
      var paramCard = ui.card({
        title: '参数摘要',
        body: meta.params.map(function (p) { return ui.kv(p[0], p[1]); }).join('')
      });

      /* 执行时间线（步骤列表，标当前节点） */
      var steps = ['收到请求', '权限检查', '等待用户确认', '执行', '完成'];
      var cur = meta.timelineCur;
      var timeline = steps.map(function (s, i) {
        var cls = i < cur ? 'done' : i === cur ? 'doing' : 'todo';
        var mark = cls === 'done' ? FS.icon('check') : '<span class="ap-dot"></span>';
        return '<div class="ctx-step ' + cls + '">' + mark + esc(s) +
          (cls === 'doing' ? '<span class="chip mini info">当前</span>' : '') + '</div>';
      }).join('');
      var timelineCard = ui.card({ title: '执行时间线', body: '<div class="ctx-steps chat-timeline">' + timeline + '</div>' });

      /* 结果 / 错误 / 重试区 */
      var resultBody;
      if (t.status === 'error') {
        resultBody = '<div class="chat-errbox">' + FS.icon('danger') +
          '<div><div class="chat-err-title">执行失败</div><div class="chat-err-sub">云端模型请求超时（30s），可重试</div></div></div>' +
          '<div class="chat-retry-row">' + ui.btn({ label: '重试', kind: 'primary', size: 'sm', icon: 'retry', act: 'chat:tool-retry', arg: t.name }) + '</div>';
      } else if (t.status === 'pending') {
        resultBody = '<div class="chat-waitbox">' + FS.icon('clock') +
          '<div><div class="chat-err-title">等待用户确认</div><div class="chat-err-sub">该高风险操作需你在「待确认行动」中确认后才会执行</div></div></div>' +
          '<div class="chat-retry-row">' + ui.btn({ label: '去确认', kind: 'warn', size: 'sm', icon: 'shield', act: 'nav', href: '#/p/CHAT-04' }) + '</div>';
      } else if (t.status === 'running') {
        resultBody = '<div class="chat-waitbox">' + FS.icon('zap') +
          '<div><div class="chat-err-title">正在执行</div><div class="chat-err-sub">模型生成中，可随时停止</div></div></div>' +
          '<div class="chat-retry-row">' + ui.btn({ label: '停止', kind: 'ghost', size: 'sm', icon: 'stop', act: 'chat:stop' }) + '</div>';
      } else {
        resultBody = '<div class="chat-okbox">' + FS.icon('check') +
          '<div><div class="chat-err-title">执行成功</div><div class="chat-err-sub">' + esc(t.detail) + '</div></div></div>';
      }
      var resultCard = ui.card({ title: '结果 / 错误', body: resultBody });

      /* 关联业务对象 + 操作日志（等宽字体） */
      var objRows = meta.objects.map(function (o) {
        return ui.listRow({ icon: o.icon, title: o.title, sub: o.sub, act: 'nav', href: o.href });
      }).join('');
      var logLines = meta.log.map(function (l) { return '<div class="chat-log-line">' + esc(l) + '</div>'; }).join('');
      var objCard = ui.card({
        title: '关联对象与日志',
        body: objRows + '<div class="chat-log">' + logLines + '</div>'
      });

      /* 底部操作：重试 / 撤销（可逆时）/ 打开结果页 / 复制诊断信息 */
      var foot =
        ui.btn({ label: '重试', kind: 'ghost', icon: 'retry', act: 'chat:tool-retry', arg: t.name }) +
        (meta.reversible ? ui.btn({ label: '撤销', kind: 'warn', icon: 'undo', act: 'chat:tool-undo', arg: t.name }) : '') +
        ui.btn({ label: '打开结果页', kind: 'ghost', icon: 'fwd', act: 'nav', href: meta.resultHref }) +
        '<div class="spacer"></div>' +
        ui.btn({ label: '复制诊断信息', kind: 'ghost', icon: 'copy', act: 'chat:tool-copy', arg: t.name });

      var asyncHtml = '<div class="chat-scroll-wrap">' + ui.scrollArea(infoCard + paramCard + timelineCard + resultCard + objCard, 'chat-detail-scroll') + '</div>';

      return '<div class="pg">' +
        ui.pageHead({
          title: '工具执行详情', id: 'CHAT-03',
          sub: t.label + '（' + t.name + '）',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body chat-body">' + FS.states.wrap(pg, asyncHtml, {
          errorDesc: '工具详情加载失败，请重试'
        }) + '</div>' +
        '<div class="pg-foot">' + foot + '</div>' +
        '</div>';
    }
  });

  /* ==================== CHAT-04 待确认行动 ==================== */
  FS.router.register({
    id: 'CHAT-04', name: '待确认行动', module: '对话', level: '任务页', ctx: 'CTX-05',
    states: ['empty'],
    parent: 'CHAT-01',

    render: function (pg) {
      ready();
      var acts = FS.data.pendingActions;
      var highCount = acts.filter(function (a) { return a.risk === '高'; }).length;

      /* 计划摘要卡 */
      var summaryCard = ui.card({
        title: '晚餐与库存整理',
        side: acts.length + ' 项行动' + (highCount ? ' · ' + highCount + ' 项高风险' : ''),
        body: '<div class="chat-plan-desc">' + FS.icon('zap') +
          '<span>Agent 根据当前库存与临期食材拟定了以下行动。请逐项确认；高风险项需你明确同意后才会执行。</span></div>',
        cls: 'chat-plan'
      });

      /* 逐条待确认动作 */
      var rows = acts.map(function (a) {
        var riskTone = a.risk === '高' ? 'danger' : a.risk === '中' ? 'warn' : 'ok';
        var irreversible = a.irreversible
          ? '<div class="chat-irrev">' + FS.icon('warn') + '此操作不可恢复，删除后将无法找回</div>' : '';
        return '<div class="chat-pa">' +
          '<div class="chat-pa-head"><span class="chat-pa-title">' + esc(a.title) + '</span>' + ui.chip(a.risk + '风险', riskTone) + '</div>' +
          '<div class="chat-pa-desc">' + esc(a.desc) + '</div>' +
          '<div class="chat-pa-obj">' + FS.icon('box') + '<span>' + esc(a.objects) + '</span></div>' +
          irreversible +
          '<div class="chat-pa-ops">' +
            ui.btn({ label: '确认', kind: a.risk === '高' ? 'danger' : 'primary', size: 'sm', icon: 'check', act: 'chat:pa-confirm', arg: a.id + '|' + a.risk }) +
            ui.btn({ label: '拒绝', kind: 'ghost', size: 'sm', act: 'chat:pa-reject', arg: a.id }) +
            ui.btn({ label: '修改', kind: 'ghost', size: 'sm', icon: 'edit', act: 'chat:pa-edit', arg: a.id }) +
          '</div>' +
          '</div>';
      }).join('');

      var asyncHtml = '<div class="chat-scroll-wrap">' + ui.scrollArea(summaryCard + rows, 'chat-pa-scroll') + '</div>';

      var foot =
        ui.btn({ label: '返回对话讨论', kind: 'ghost', icon: 'back', act: 'nav', href: '#/p/CHAT-01' }) +
        '<div class="spacer"></div>' +
        ui.btn({ label: '全部确认', kind: 'danger', icon: 'check', act: 'chat:pa-confirm-all' });

      return '<div class="pg">' +
        ui.pageHead({
          title: '待确认行动', id: 'CHAT-04',
          sub: '确认后 Agent 才会执行这些操作',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body chat-body">' + FS.states.wrap(pg, asyncHtml, {
          emptyTitle: '没有待确认的行动', emptyDesc: 'Agent 提出的写入/删除/上传类操作会先列在这里等你确认',
          icon: 'check'
        }) + '</div>' +
        '<div class="pg-foot">' + foot + '</div>' +
        '</div>';
    }
  });

  /* ==================== 模块内动作 ==================== */

  /* 点击工具卡 → 工具执行详情（?tool=name） */
  FS.action('chat:tool', function (el) {
    FS.router.go('#/p/CHAT-03?tool=' + encodeURIComponent(el.getAttribute('data-arg')));
  });

  /* 点击结果卡 → 按 kind 跳转业务页 */
  FS.action('chat:result', function (el) {
    var refId = el.getAttribute('data-arg') || '';
    var m = FS.data.chat.messages.filter(function (x) { return x.type === 'result' && (x.refId === refId || !refId); })[0];
    var kind = m ? m.kind : 'recipe';
    var fn = RESULT_HREF[kind] || RESULT_HREF.recipe;
    FS.router.go(fn(m || { refId: refId }));
  });

  /* CHAT-01 输入区：发送 → 进入忙碌显示停止；停止 → 回到发送 */
  FS.action('chat:quick', function (el) {
    agentBusyOverride = true;
    refreshSendStopBtn();
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：发送「' + el.getAttribute('data-arg') + '」' });
  });
  FS.action('chat:open-kb', function () { FS.overlays.show('OV-12'); });
  FS.action('chat:voice', function () { FS.overlays.show('OV-05'); });
  FS.action('chat:send', function () {
    agentBusyOverride = true;
    refreshSendStopBtn();
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：消息已发送，Agent 处理中…' });
  });
  FS.action('chat:stop', function () {
    agentBusyOverride = false;
    /* 同步把 mock 中 running 工具标为结束，避免 override 清掉后仍判为忙碌 */
    var msgs = (FS.data.chat && FS.data.chat.messages) || [];
    msgs.forEach(function (m) {
      if (m.type === 'tool' && m.status === 'running') {
        m.status = 'ok';
        m.detail = (m.detail || '') + '（已停止）';
        m.latency = m.latency || '—';
      }
    });
    refreshSendStopBtn();
    FS.overlays.show('OV-01', { tone: 'warn', text: '演示：已停止生成' });
  });
  FS.action('chat:new', function () {
    agentBusyOverride = false;
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：已新建会话' });
  });

  /* CHAT-02 会话历史操作 */
  FS.action('chat:sess-open', function (el) {
    FS.router.go('#/p/CHAT-01');
  });
  FS.action('chat:hist-search', function () { FS.overlays.show('OV-12'); });
  FS.action('chat:hist-filter', function (el) {
    var f = el.getAttribute('data-arg');
    FS.router.go('#/p/CHAT-02' + (f && f !== 'all' ? '?filter=' + f : ''));
  });
  FS.action('chat:sess-rename', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：重命名会话' });
  });
  FS.action('chat:sess-archive', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：会话已归档' });
  });
  FS.action('chat:sess-del', function () {
    FS.overlays.show('OV-03', { title: '删除这条会话？', desc: '会话及其消息记录将从本机移除，此操作不可恢复。', ok: '删除', danger: true });
  });

  /* CHAT-03 工具操作 */
  FS.action('chat:tool-retry', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：已重新发起执行' });
  });
  FS.action('chat:tool-undo', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：已撤销该操作' });
  });
  FS.action('chat:tool-copy', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：诊断信息已复制' });
  });

  /* CHAT-04 待确认操作 */
  FS.action('chat:pa-confirm', function (el) {
    var parts = (el.getAttribute('data-arg') || '').split('|');
    var risk = parts[1];
    if (risk === '高') { FS.overlays.show('OV-04'); return; }
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：已确认该行动' });
  });
  FS.action('chat:pa-reject', function () {
    FS.overlays.show('OV-01', { tone: 'warn', text: '演示：已拒绝该行动' });
  });
  FS.action('chat:pa-edit', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：修改参数' });
  });
  FS.action('chat:pa-confirm-all', function () {
    FS.overlays.show('OV-04');
  });

  /* ==================== 流程：Agent 高风险确认 ==================== */
  FS.flows.register('agent', {
    run: function (api) {
      api.go('#/p/CHAT-01');
      api.wait(1000).then(function () {
        if (api.cancelled()) return;
        api.show('OV-06', { step: 1, steps: ['理解意图', '规划步骤', '执行工具：inventory_delete', '等待确认'] });
        return api.wait(1800);
      }).then(function () {
        if (api.cancelled()) return;
        api.close('OV-06');
        api.show('OV-04');   /* 默认数据即高风险删除确认，停在等用户操作 */
      });
    }
  });
})();
