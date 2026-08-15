/**
 * 冰箱小精灵 ver2 UI 预览 —— 共享组件 builder（所有页面模块统一使用）
 * ------------------------------------------------------------------
 * 纪律：
 *  - 页面一律用这里的 builder 组装，禁止在页面内散写装饰性内联样式；
 *  - 每个组件对应一类 LVGL 对象（映射表见 README.md）；
 *  - 交互统一走 data-act + 全局委托（shell.js），页面用 FS.action(name, fn) 注册处理；
 *  - 导航直接用 data-href="#/p/XXX?..."（内置 act:'nav'），返回用 data-act="back"。
 */
window.FS = window.FS || {};

/* HTML 转义 */
FS.esc = function (s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
};

/* 动作注册表：data-act="name" data-arg="参数" → FS.actions[name](el, ev) */
FS.actions = {};
FS.action = function (name, fn) { FS.actions[name] = fn; };

FS.ui = (function () {
  var esc = FS.esc;
  var icon = FS.icon;

  /* 属性拼装 */
  function act(attrs) {
    var s = '';
    if (!attrs) return s;
    if (attrs.act) s += ' data-act="' + esc(attrs.act) + '"';
    if (attrs.arg !== undefined && attrs.arg !== null) s += ' data-arg="' + esc(attrs.arg) + '"';
    if (attrs.href) s += ' data-act="nav" data-href="' + esc(attrs.href) + '"';
    if (attrs.title) s += ' title="' + esc(attrs.title) + '"';
    return s;
  }

  /* ---- 按钮：.btn primary/ghost/danger/warn；大按钮 .big ---- */
  function btn(o) {
    o = o || {};
    var cls = 'btn ' + (o.kind || 'primary') + (o.size === 'big' ? ' big' : '') + (o.size === 'sm' ? ' sm' : '') + (o.block ? ' block' : '');
    return '<button class="' + cls + '"' + act(o) + (o.disabled ? ' disabled' : '') + '>' +
      (o.icon ? icon(o.icon) : '') + '<span>' + esc(o.label) + '</span></button>';
  }

  /* ---- 图标按钮（≥48px 触控；disabled 用于到边界的上移/下移这类动作） ---- */
  function iconBtn(o) {
    return '<button class="icon-btn ' + (o.kind || '') + '"' + act(o) + (o.disabled ? ' disabled' : '') +
      ' title="' + esc(o.label || '') + '">' +
      icon(o.icon) + (o.text ? '<span>' + esc(o.text) + '</span>' : '') + '</button>';
  }

  /* ---- 卡片容器 ---- */
  function card(o) {
    o = o || {};
    return '<section class="card ' + (o.cls || '') + '"' + act(o) + '>' +
      (o.title ? '<header class="card-head"><span class="card-title">' + esc(o.title) + '</span>' +
        (o.side ? '<span class="card-side">' + o.side + '</span>' : '') + '</header>' : '') +
      '<div class="card-body">' + (o.body || '') + '</div></section>';
  }

  /* ---- 首页统计卡 ---- */
  function statCard(o) {
    return '<div class="stat-card ' + (o.tone || '') + '"' + act(o) + '>' +
      '<span class="sc-icon">' + icon(o.icon, 'lg') + '</span>' +
      '<div class="sc-num">' + esc(o.value) + '</div>' +
      '<div class="sc-label">' + esc(o.label) + '</div></div>';
  }

  /* ---- 通用列表行（图标 + 主副文 + 右侧区） ---- */
  function listRow(o) {
    return '<div class="list-row ' + (o.cls || '') + '"' + act(o) + '>' +
      (o.icon ? '<span class="lr-icon ' + (o.tone || '') + '">' + icon(o.icon) + '</span>' : '') +
      '<div class="lr-main"><div class="lr-title">' + esc(o.title) + (o.titleExtra || '') + '</div>' +
      (o.sub ? '<div class="lr-sub">' + esc(o.sub) + '</div>' : '') + '</div>' +
      (o.side ? '<div class="lr-side">' + o.side + '</div>' : '') +
      (o.arrow === false ? '' : icon('fwd', 'lr-arrow')) + '</div>';
  }

  /* ---- 食材行（库存/搜索/计划共用） ---- */
  function foodRow(it, o) {
    o = o || {};
    var q = FS.dataQ;
    return '<div class="list-row food-row"' + act({ act: o.act || 'inv:detail', arg: it.id }) + '>' +
      '<span class="lr-icon food">' + icon('food') + '</span>' +
      '<div class="lr-main"><div class="lr-title">' + esc(it.name) +
        (it.estimated ? '<span class="chip mini warn">估算</span>' : '') +
        (it.confidence < 0.8 ? '<span class="chip mini danger">低置信</span>' : '') +
        (it.synced === false ? '<span class="chip mini offline">未同步</span>' : '') +
      '</div><div class="lr-sub">' + esc(q.locPath(it)) + ' · ' + esc(it.qty) + esc(it.unit || '') + '</div></div>' +
      '<div class="lr-side">' + daysPill(it.daysLeft) + '</div>' +
      icon('fwd', 'lr-arrow') + '</div>';
  }

  /* ---- 标记 chip / 剩余天数 pill ---- */
  function chip(label, tone, ic) {
    return '<span class="chip ' + (tone || '') + '">' + (ic ? icon(ic) : '') + esc(label) + '</span>';
  }
  function daysPill(d) {
    var q = FS.dataQ;
    return '<span class="days-pill ' + q.daysTone(d) + '">' + esc(q.daysText(d)) + '</span>';
  }

  /* ---- 分段选择器（空间/列表、日/周切换） ---- */
  function seg(o) {
    var html = '<div class="seg"' + (o.act ? ' data-seg="' + esc(o.act) + '"' : '') + '>';
    o.options.forEach(function (op) {
      var v = (typeof op === 'string') ? op : op.value;
      var label = (typeof op === 'string') ? op : op.label;
      html += '<button class="seg-item' + (v === o.value ? ' on' : '') + '"' + act({ act: o.act, arg: v }) + '>' + esc(label) + '</button>';
    });
    return html + '</div>';
  }

  /* ---- 角标 / 状态点 ---- */
  function badge(n) { return '<span class="badge">' + esc(n) + '</span>'; }
  function statusDot(status, label) {
    return '<span class="status-dot ' + status + '"></span>' + (label ? '<span class="sd-label">' + esc(label) + '</span>' : '');
  }

  /* ---- 路径面包屑：冰箱/分区/层/格 ---- */
  function crumb(parts) {
    var html = '<div class="crumb">';
    parts.forEach(function (p, i) {
      if (i) html += '<span class="crumb-sep">/</span>';
      if (p.href) html += '<a class="crumb-link"' + act({ href: p.href }) + '>' + esc(p.label) + '</a>';
      else html += '<span class="crumb-cur">' + esc(p.label) + '</span>';
    });
    return html + '</div>';
  }

  /* ---- 页头（标题 + 页面 ID 角标 + 右侧动作） ---- */
  function pageHead(o) {
    return '<header class="pg-head">' +
      '<div><span class="pg-title">' + esc(o.title) + '</span> <span class="pg-id">' + esc(o.id) + '</span>' +
      (o.sub ? '<div class="pg-sub">' + esc(o.sub) + '</div>' : '') + '</div>' +
      '<div class="pg-head-side">' + (o.actions || '') + '</div></header>';
  }

  function scrollArea(html, cls) {
    return '<div class="scroll-area ' + (cls || '') + '">' + html + '</div>';
  }

  function sectionTitle(label, side) {
    return '<div class="sec-title"><span>' + esc(label) + '</span>' + (side ? '<span class="sec-side">' + side + '</span>' : '') + '</div>';
  }

  /* ---- 空态 / 骨架屏（异步状态通用件） ---- */
  function emptyState(o) {
    return '<div class="empty-state">' + icon(o.icon || 'box', 'xl') +
      '<div class="es-title">' + esc(o.title || '暂无内容') + '</div>' +
      (o.desc ? '<div class="es-desc">' + esc(o.desc) + '</div>' : '') +
      (o.action || '') + '</div>';
  }
  function skeleton(rows) {
    var html = '<div class="skeleton-wrap">';
    for (var i = 0; i < (rows || 4); i++) html += '<div class="skeleton-row"></div>';
    return html + '</div>';
  }

  /* ---- 图片占位块（对应固件 TF 卡图片缺失时的占位对象） ---- */
  function imgPh(o) {
    o = o || {};
    var style = o.w && o.h ? ' style="width:' + o.w + 'px;height:' + o.h + 'px"' : '';
    return '<div class="img-ph ' + (o.cls || '') + '"' + style + '>' + icon(o.icon || 'image', 'lg') +
      '<span>' + esc(o.label || '图片占位') + '</span></div>';
  }

  /* ---- 键值行 / 进度条 ---- */
  function kv(label, value, tone) {
    return '<div class="kv-row"><span class="kv-label">' + esc(label) + '</span>' +
      '<span class="kv-value ' + (tone || '') + '">' + esc(value) + '</span></div>';
  }
  function progress(pct, tone) {
    return '<div class="progress ' + (tone || '') + '"><div class="progress-fill" style="width:' + Math.max(0, Math.min(100, pct)) + '%"></div></div>';
  }

  /* ---- 表单 ---- */
  function formRow(o) {
    return '<div class="form-row"><div class="fr-text"><div class="fr-label">' + esc(o.label) + '</div>' +
      (o.desc ? '<div class="fr-desc">' + esc(o.desc) + '</div>' : '') + '</div>' +
      '<div class="fr-control">' + (o.control || '') + '</div></div>';
  }
  function sw(o) {
    return '<button class="switch ' + (o.on ? 'on' : '') + '"' + act(o) + ' role="switch" aria-checked="' + !!o.on + '"><span class="sw-knob"></span></button>';
  }
  function stepper(o) {
    return '<div class="stepper">' +
      '<button class="stp-btn"' + act({ act: o.act, arg: 'dec' }) + '>' + icon('minus') + '</button>' +
      '<span class="stp-value">' + esc(o.value) + (o.unit ? '<small>' + esc(o.unit) + '</small>' : '') + '</span>' +
      '<button class="stp-btn"' + act({ act: o.act, arg: 'inc' }) + '>' + icon('plus') + '</button></div>';
  }
  function inputLike(o) {
    return '<button class="input-like"' + act(o) + '>' + (o.icon ? icon(o.icon) : '') +
      '<span class="' + (o.value ? '' : 'ph') + '">' + esc(o.value || o.placeholder || '') + '</span>' +
      icon('down', 'il-arrow') + '</button>';
  }

  /* ---- 对话气泡 / 工具调用卡 / 结果卡（CHAT 模块与 CTX-05 共用） ---- */
  function chatBubble(m) {
    if (m.type === 'user') {
      return '<div class="chat-row user"><div class="chat-bubble user">' + esc(m.text) +
        '<span class="cb-time">' + esc(m.time || '') + '</span></div></div>';
    }
    return '<div class="chat-row"><div class="chat-avatar">' + icon('zap') + '</div>' +
      '<div class="chat-bubble agent">' + esc(m.text) +
      '<span class="cb-time">' + esc(m.time || '') + '</span></div></div>';
  }
  var TOOL_STATUS = { ok: ['ok', '已完成'], running: ['info', '执行中'], pending: ['warn', '待确认'], error: ['danger', '失败'] };
  function toolCard(t, o) {
    o = o || {};
    var st = TOOL_STATUS[t.status] || TOOL_STATUS.ok;
    return '<div class="tool-card"' + act({ act: o.act || 'chat:tool', arg: t.name }) + '>' +
      '<span class="tc-icon ' + st[0] + '">' + icon('tool') + '</span>' +
      '<div class="tc-main"><div class="tc-title">' + esc(t.label) +
        '<span class="chip mini ' + (t.risk === '高' ? 'danger' : t.risk === '中' ? 'warn' : 'ok') + '">' + esc(t.risk) + '风险</span>' +
      '</div><div class="tc-sub">' + esc(t.name) + ' · ' + esc(t.detail) + '</div></div>' +
      '<span class="chip ' + st[0] + '">' + st[1] + '</span>' +
      (t.latency ? '<span class="tc-latency">' + esc(t.latency) + '</span>' : '') + '</div>';
  }
  function resultCard(o) {
    return '<div class="result-card"' + act({ act: o.act || 'chat:result', arg: o.refId || '' }) + '>' +
      '<span class="rc-icon">' + icon(o.kind === 'recipe' ? 'recipe' : o.kind === 'shopping' ? 'cart' : 'box') + '</span>' +
      '<div class="rc-main"><div class="rc-title">' + esc(o.title) + '</div>' +
      '<div class="rc-sub">' + esc(o.desc) + '</div></div>' + icon('fwd', 'lr-arrow') + '</div>';
  }

  /* ---- 菜谱卡（PLAN 与 CHAT 结果复用） ---- */
  function recipeCard(r, o) {
    o = o || {};
    return '<div class="recipe-card"' + act({ act: o.act || 'plan:recipe', arg: r.id }) + '>' +
      imgPh({ w: 96, h: 96, icon: 'food', label: r.name.slice(0, 2) }) +
      '<div class="rcp-main"><div class="rcp-title">' + esc(r.name) +
        (r.fav ? '<span class="rcp-fav">' + icon('star') + '</span>' : '') + '</div>' +
      '<div class="rcp-sub">' + esc(r.time) + ' · ' + esc(r.difficulty) + ' · 库存覆盖 ' + r.coverage + '%</div>' +
      (r.missing.length ? '<div class="rcp-missing">缺：' + esc(r.missing.join('、')) + '</div>' : '<div class="rcp-ok">食材齐全</div>') +
      '</div></div>';
  }

  /* ==================================================================
   * 库存三级空间的共用视觉语言（L1 分区 / L2 层 / L3 九宫格）
   * ------------------------------------------------------------------
   * 统一规则（三级完全一致，便于用户建立"颜色 = 新鲜度"的直觉）：
   *   · 语义档由该范围内"最差食材"决定，阈值统一走 dataQ.daysTone：
   *     danger 已过期或今天到期 / warn 3 天内临期 / ok 有食材且都还富余 / none 空范围
   *   · L1 分区块与 L2 层卡 = 语义浅底 + 左侧 4px 语义色条 + 大号项数 + 类型/状态标注
   *   · L3 九宫格为 ver1 风格大板切九格，每格底色更淡、由格内到期条承担主要语义色
   *   · 空范围一律弱化（虚线/灰底），视觉上"退到后面"
   * LVGL 对照：块 = lv_obj（bg_color 取语义浅底常量），色条 = 左侧 4px 子 obj，
   *   大号数字 = lv_label（36px 档），标注 = lv_label（18px 档）。
   * ================================================================== */

  /* 由一组食材推出语义档：danger > warn > ok > none
   * 阈值必须与 dataQ.daysTone 保持一致（<=0 danger / <=3 warn / 其余 ok），
   * 否则会出现"格子底色是黄的、格内到期条却是红的"这种自相矛盾的配色。 */
  function freshTone(items) {
    if (!items || !items.length) return 'none';
    var worstDays = null;
    items.forEach(function (it) {
      if (worstDays === null || it.daysLeft < worstDays) worstDays = it.daysLeft;
    });
    return FS.dataQ.daysTone(worstDays);
  }

  /* 语义档 → 中文状态标签（块右上角状态芯片） */
  var TONE_LABEL = { danger: '待处理', warn: '尽快吃', ok: '正常', none: '空' };

  /* 分区类型 → 图标（无对应类型时回落 fridge） */
  var ZONE_TYPE_ICON = {
    '冷冻': 'temp', '冷藏': 'fridge', '门架': 'door', '抽屉': 'box', '变温': 'drop'
  };
  function zoneTypeIcon(type) { return ZONE_TYPE_ICON[type] || 'fridge'; }

  /* 统计块的公共内页：大号项数 + 临期/过期标注 */
  function spaceStatBody(st, tone) {
    var notes = [];
    if (st.expired) notes.push('<b class="danger-text">' + st.expired + ' 过期</b>');
    if (st.expiring) notes.push('<b class="warn-text">' + st.expiring + ' 临期</b>');
    return '<div class="sp-count"><span class="sp-num">' + st.total + '</span><span class="sp-unit">项</span></div>' +
      (notes.length
        ? '<div class="sp-note">' + notes.join(' · ') + '</div>'
        : '<div class="sp-note muted">' + (tone === 'none' ? '暂无食材' : '保质充裕') + '</div>');
  }

  /* ---- L1 空间分区图（冰箱内胆卡内的 3×3 可合并宫格；位置由 rect 数据驱动） ---- */
  function zoneMap(o) {
    o = o || {};
    var q = FS.dataQ;
    var zones = o.zones || FS.data.layout.zones;
    var html = '<div class="zone-map">';
    zones.forEach(function (z) {
      var st = q.stats({ zone: z.id });
      var tone = freshTone(q.itemsIn({ zone: z.id }));
      var href = o.hrefPrefix + encodeURIComponent(z.id);
      var cellAct = o.edit
        ? act({ act: o.act || 'inv:lay-tap-zone', arg: z.id })
        : act({ href: href });
      var sel = o.selectedZoneId && o.selectedZoneId === z.id;
      html += '<div class="zone-cell tone-' + tone + (sel ? ' sel' : '') + (o.edit ? ' editable' : '') +
        '" style="grid-column:' + (z.rect.x + 1) + '/span ' + z.rect.w +
        ';grid-row:' + (z.rect.y + 1) + '/span ' + z.rect.h + '"' + cellAct + '>' +
        '<span class="sp-bar"></span>' +
        '<div class="zc-head">' +
          '<span class="zc-name">' + esc(z.name) + '</span>' +
          '<span class="zc-type">' + icon(zoneTypeIcon(z.type)) + esc(z.type) + '</span>' +
        '</div>' +
        spaceStatBody(st, tone) +
        '<span class="sp-state chip ' + (tone === 'none' ? 'offline' : tone) + '">' + TONE_LABEL[tone] + '</span>' +
        '</div>';
    });
    return html + '</div>';
  }

  /* ---- 冰箱内胆外框：把 L1 宫格包成"一台冰箱"，顶部给整机汇总 ---- */
  function fridgeBox(innerHtml, o) {
    o = o || {};
    var q = FS.dataQ;
    var st = o.stats || q.stats({});
    var sum = [st.total + ' 项'];
    if (st.expiring) sum.push('临期 ' + st.expiring);
    if (st.expired) sum.push('过期 ' + st.expired);
    if (st.unlocated) sum.push('未定位 ' + st.unlocated);
    return '<div class="fridge-box' + (o.fill ? ' fill' : '') + '">' +
      '<div class="fb-head">' +
        '<span class="fb-title">' + esc(o.title || '冰箱空间') + '</span>' +
        '<span class="fb-sum">' + esc(sum.join(' · ')) + '</span>' +
      '</div>' +
      '<div class="fb-body">' + innerHtml + '</div>' +
      '</div>';
  }

  /* ---- L2 层卡：层序徽标 + 层名 + 新鲜度条 + 前几项食材预览 ---- */
  function layerCard(zone, layer, o) {
    o = o || {};
    var q = FS.dataQ;
    var st = q.stats({ zone: zone.id, layer: layer.id });
    var items = q.itemsIn({ zone: zone.id, layer: layer.id });
    var tone = freshTone(items);
    /* 预览：按剩余天数升序取前 3 项，最紧急的排前面 */
    var preview = items.slice().sort(function (a, b) { return a.daysLeft - b.daysLeft; }).slice(0, 3);
    var more = items.length - preview.length;
    return '<div class="layer-card tone-' + tone + '"' + act({ href: o.href }) + '>' +
      '<span class="sp-bar"></span>' +
      '<span class="lc-ord">' + esc(o.index != null ? o.index : (layer.name || '').replace(/[^0-9]/g, '') || '·') + '</span>' +
      '<div class="lc-main">' +
        '<div class="lc-name">' + esc(layer.name) +
          (layer.grid ? '<span class="chip ok">' + icon('grid') + '精确到格</span>' : '') +
        '</div>' +
        '<div class="lc-meta">' + st.total + ' 项' +
          (st.expired ? ' · <b class="danger-text">' + st.expired + ' 过期</b>' : '') +
          (st.expiring ? ' · <b class="warn-text">' + st.expiring + ' 临期</b>' : '') +
        '</div>' +
        (preview.length
          ? '<div class="lc-preview">' + preview.map(function (it) {
              return '<span class="lc-tag ' + q.daysTone(it.daysLeft) + '">' + esc(it.name) + '</span>';
            }).join('') + (more > 0 ? '<span class="lc-tag more">+' + more + '</span>' : '') + '</div>'
          : '<div class="lc-preview"><span class="lc-tag empty">本层暂无食材</span></div>') +
      '</div>' +
      icon('fwd', 'lr-arrow') + '</div>';
  }

  /* 九宫格 cellId ↔ 坐标（A1 左上 … C3 右下） */
  function g9XY(id) {
    return { y: 'ABC'.indexOf(String(id).charAt(0)), x: parseInt(String(id).charAt(1), 10) - 1 };
  }
  function g9Id(x, y) { return 'ABC'[y] + (x + 1); }

  /* 格位中文名（对齐 ver1：行=内/中/外，列=左/中/右，比 A1/B2 更好念） */
  var G9_ROW_CN = { A: '内', B: '中', C: '外' };
  var G9_COL_CN = { 1: '左', 2: '中', 3: '右' };
  function g9NameCN(id) {
    return (G9_ROW_CN[String(id).charAt(0)] || '中') + '·' + (G9_COL_CN[String(id).charAt(1)] || '中');
  }

  /* ---- L3 九宫格（恒定 A1–C3 九个等大格，无合并位） ----
   * 视觉沿用 ver1：一整块大圆角板 + 发丝分隔线切成九格（不是九张独立卡片），
   * 每格内容自上而下 = 位置名 · 格号 / 食材名 + 数量 / 到期条。
   * 到期条底色仍按本格最差食材的语义档（ok/warn/danger），空格为虚线弱化。
   * LVGL 对照：board = 一个 lv_obj（圆角 + 裁剪），格 = 九个子 obj，
   *   分隔线用子 obj 的 1px 右/下边框，避免额外画线对象。
   */
  function grid9(zone, layer, o) {
    o = o || {};
    var q = FS.dataQ;
    var selected = {};
    (o.selectedCells || []).forEach(function (c) { selected[c] = true; });

    var html = '<div class="grid9-board' + (o.edit ? ' edit' : '') + (o.pick ? ' pick' : '') + '">';
    for (var y = 0; y < 3; y++) {
      for (var x = 0; x < 3; x++) {
        var id = g9Id(x, y);
        var items = q.itemsIn({ zone: zone.id, layer: layer.id, cell: id });
        var tone = freshTone(items);
        var isSel = !!selected[id];
        var cellAct;
        if (o.edit) {
          cellAct = act({ act: 'inv:lay-tap-cell', arg: id });
        } else if (o.pick) {
          /* 一格一物：已占用的格不可选，点了也不给动作，视觉上置灰 */
          cellAct = items.length
            ? act({ act: 'inv:pick-taken', arg: esc(items[0].name) })
            : act({ act: 'inv:pick-cell', arg: (o.zoneId || zone.id) + '|' + (o.layerId || layer.id) + '|' + id });
        } else {
          /* 浏览态：一格一物，点格直接进详情（有物）或新增（空格），不再经中转列表页 */
          cellAct = act({ act: 'inv:cell', arg: zone.id + '|' + layer.id + '|' + id });
        }

        var main, foot;
        if (items.length) {
          /* 一格一物：正常只有一项；历史数据若同格多项，仍按最紧急的一项展示 */
          var sorted = items.slice().sort(function (a, b) { return a.daysLeft - b.daysLeft; });
          var head = sorted[0];
          main = '<div class="g9-main">' +
            '<span class="g9-food">' + esc(head.name) + '</span>' +
            '<span class="g9-amount">' + esc(head.qty) + esc(head.unit || '') + '</span>' +
            '</div>' +
            /* 一格一物下这一行正常不出现；仅作历史/异常数据的兜底提示 */
            (items.length > 1
              ? '<div class="g9-more">⚠ 本格另有 ' + (items.length - 1) + ' 项待整理</div>'
              : '');
          /* 到期条：显示该格食材的剩余天数；选位态改为"已占用"提示不可选 */
          foot = o.pick
            ? '<div class="g9-expire none">已占用</div>'
            : '<div class="g9-expire ' + q.daysTone(head.daysLeft) + '">' +
              esc(q.daysText(head.daysLeft)) + '</div>';
        } else {
          main = '<div class="g9-main"><span class="g9-food empty">空位</span></div>';
          /* 浏览态点空格 = 在此位置新增，所以直接写「＋ 放入食材」而不是被动的「可存放」 */
          foot = '<div class="g9-expire none">' + (o.pick ? '放这里' : o.edit ? '可编辑' : '＋ 放入食材') + '</div>';
        }

        html += '<div class="g9-cell tone-' + tone + '"' +
          (items.length ? '' : ' data-empty="1"') +
          /* pick 态：空格可选高亮，占用格标记 taken 置灰 */
          (o.pick ? (items.length ? ' data-taken="1"' : ' data-pickable="1"') : '') +
          (o.edit ? ' data-editable="1"' : '') +
          (isSel ? ' data-sel="1"' : '') +
          cellAct + '>' +
          '<div class="g9-code">' + esc(g9NameCN(id)) + '<span class="g9-id">' + esc(id) + '</span></div>' +
          main +
          foot +
          '</div>';
      }
    }
    return html + '</div>';
  }

  /* ---- L1 布局编辑画布：固定 3 列、行数可向下扩展，分区占多格时视觉合并 ----
   * o.zones: 分区数组（草稿）
   * o.selectedKeys: ['x,y', ...] 当前选中的单位格
   * o.selectedZoneId: 整区高亮
   * o.editZoneId/o.shape: 编辑某分区时，暂时腾出该分区原位置并显示可落位的左上角
   * 点击：已占分区 → 选中；可落位空格 → 将选中分区放到此处
   */
  function layoutZoneGrid(o) {
    o = o || {};
    var q = FS.dataQ;
    var zones = o.zones || [];
    var editZoneId = o.editZoneId || null;
    var shape = o.shape || null;
    var addKey = o.addKey || null;
    var rowCount = Math.max(3, parseInt(o.rowCount, 10) || 3);
    var selected = {};
    (o.selectedKeys || []).forEach(function (k) { selected[k] = true; });

    zones.forEach(function (z) {
      var rect = z.rect || {};
      rowCount = Math.max(rowCount, (rect.y || 0) + (rect.h || 1));
    });
    if (addKey) {
      var addPoint = addKey.split(',');
      rowCount = Math.max(rowCount, (+addPoint[1] || 0) + 1);
    }

    /* 用低饱和色区分不同储物类型，延续 Ver1 的轻量层次。 */
    function layoutZoneTone(type) {
      if (type === '冷冻') return 'cool';
      if (type === '门架') return 'sand';
      if (type === '抽屉') return 'amber';
      if (type === '变温') return 'rose';
      return 'fresh';
    }

    /* 单位格 → 所属分区 */
    function zoneAt(x, y) {
      for (var i = 0; i < zones.length; i++) {
        var r = zones[i].rect;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return zones[i];
      }
      return null;
    }

    /* 选中分区暂时不参与碰撞检测；画布上直接标出能作为形状左上角的格位。 */
    function canPlaceAt(x, y) {
      if (!editZoneId || !shape || !shape.w || !shape.h) return false;
      if (x + shape.w > 3) return false;
      for (var yy = y; yy < y + shape.h; yy++) {
        for (var xx = x; xx < x + shape.w; xx++) {
          var hit = zoneAt(xx, yy);
          if (hit && hit.id !== editZoneId) return false;
        }
      }
      return true;
    }

    var skip = {};
    var html = '<div class="inv-lay-grid" role="grid" aria-label="分区形状画布" style="--lay-rows:' + rowCount + '">';
    for (var y = 0; y < rowCount; y++) {
      for (var x = 0; x < 3; x++) {
        var key = x + ',' + y;
        if (skip[key]) continue;
        var z = zoneAt(x, y);
        /* 正在编辑的分区从原位置暂时拿起，露出它可以重新落位的格子。 */
        if (z && z.id === editZoneId) z = null;
        if (z && z.rect.x === x && z.rect.y === y) {
          /* 分区锚点格：跨行跨列合并显示 */
          var r = z.rect;
          for (var dy = 0; dy < r.h; dy++) {
            for (var dx = 0; dx < r.w; dx++) {
              if (dx || dy) skip[(r.x + dx) + ',' + (r.y + dy)] = true;
            }
          }
          var st = q.stats({ zone: z.id });
          var zoneSel = o.selectedZoneId === z.id ||
            (o.selectedKeys || []).some(function (k) {
              var p = k.split(',');
              var cx = +p[0], cy = +p[1];
              return cx >= r.x && cx < r.x + r.w && cy >= r.y && cy < r.y + r.h;
            });
          html += '<button type="button" class="inv-lay-cell filled tone-' + layoutZoneTone(z.type) + (zoneSel ? ' sel' : '') + '"' +
            ' style="grid-column:' + (x + 1) + '/span ' + r.w +
            ';grid-row:' + (y + 1) + '/span ' + r.h + '"' +
            act({ act: 'inv:lay-tap-zone', arg: z.id }) + '>' +
            '<span class="inv-lay-name">' + esc(z.name) + '</span>' +
            '<span class="inv-lay-type">' + icon(zoneTypeIcon(z.type)) + esc(z.type) + '</span>' +
            '<span class="inv-lay-sub">' + (z.layers || []).length + ' 层 · ' + st.total + ' 项</span>' +
            '</button>';
        } else if (!z) {
          /* 画布只露出一个可用的添加入口，不显示没有动作的空格。 */
          if (key !== addKey) continue;
          html += '<button type="button" class="inv-lay-cell empty add"' +
            ' style="grid-column:' + (x + 1) + ';grid-row:' + (y + 1) + '"' +
            act({ act: 'inv:lay-add-zone-at', arg: key }) + '>' +
            '<span class="inv-lay-plus">' + icon('plus') + '</span>' +
            '<span class="inv-lay-add-label">添加</span>' +
            '</button>';
        }
        /* 非锚点的占位格已被 skip */
      }
    }
    return html + '</div>';
  }

  /* ---- L2 迷你九宫格缩略（只读；恒 9 格，仅按有无食材着色） ---- */
  function miniGrid9(zone, layer) {
    var q = FS.dataQ;
    var html = '<div class="inv-mini-grid" aria-hidden="true">';
    for (var y = 0; y < 3; y++) {
      for (var x = 0; x < 3; x++) {
        var id = g9Id(x, y);
        var n = (zone && layer) ? q.itemsIn({ zone: zone.id, layer: layer.id, cell: id }).length : 0;
        html += '<i class="mg-cell' + (n ? ' has' : '') + '"></i>';
      }
    }
    return html + '</div>';
  }

  /* ---- 定时器行（MORE-02 / CTX-07 / PLAN-05 共用） ---- */
  function timerRow(t) {
    var stateMap = { running: ['ok', '运行中'], paused: ['warn', '已暂停'], armed: ['info', '待触发'], done: ['offline', '已完成'], ringing: ['danger', '到时！'] };
    var st = stateMap[t.state] || stateMap.armed;
    return '<div class="list-row timer-row">' +
      '<span class="lr-icon ' + st[0] + '">' + icon(t.kind === '闹钟' || t.kind === '周期提醒' ? 'bell' : 'timer') + '</span>' +
      '<div class="lr-main"><div class="lr-title">' + esc(t.name) + '<span class="chip mini">' + esc(t.kind) + '</span></div>' +
      '<div class="lr-sub">来源：' + esc(t.source) + (t.repeat ? ' · ' + esc(t.repeat) : '') + '</div></div>' +
      '<div class="lr-side timer-face-sm">' + esc(t.remain || t.at) + '</div>' +
      '<span class="chip ' + st[0] + '">' + st[1] + '</span></div>';
  }

  /* ---- 健康图标（侧舱固定区） ---- */
  function healthIcon(h) {
    return '<div class="health-icon ' + h.status + '" title="' + esc(h.label) + '">' + icon(h.icon) +
      '<span class="hi-dot"></span></div>';
  }

  return {
    btn: btn, iconBtn: iconBtn, card: card, statCard: statCard,
    listRow: listRow, foodRow: foodRow, chip: chip, daysPill: daysPill,
    seg: seg, badge: badge, statusDot: statusDot, crumb: crumb,
    pageHead: pageHead, scrollArea: scrollArea, sectionTitle: sectionTitle,
    emptyState: emptyState, skeleton: skeleton, imgPh: imgPh,
    kv: kv, progress: progress, formRow: formRow, sw: sw, stepper: stepper, inputLike: inputLike,
    chatBubble: chatBubble, toolCard: toolCard, resultCard: resultCard,
    recipeCard: recipeCard, zoneMap: zoneMap, layerCard: layerCard, grid9: grid9,
    fridgeBox: fridgeBox, freshTone: freshTone, zoneTypeIcon: zoneTypeIcon,
    layoutZoneGrid: layoutZoneGrid, miniGrid9: miniGrid9,
    timerRow: timerRow, healthIcon: healthIcon
  };
})();
