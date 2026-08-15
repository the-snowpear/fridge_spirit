/**
 * 冰箱小精灵 ver2 UI 预览 —— 状态侧舱（独立状态机）
 * ------------------------------------------------------------------
 * 横屏：248×600 竖栏 = 固定区 + CTX + 底部语音/拍照大按钮。
 * 竖屏隐私：600×288 底舱 = 左列（状态卡 + 与横屏同款 CTX）| 右列（语音/摄像大按钮占满舱高）。
 * 侧舱不随主页面重建；auto 跟随页面/浮层，也可工具栏锁定 CTX-01..08。
 * 系统页简化：无语音/拍照；竖屏仅左列状态。
 */
window.FS = window.FS || {};

FS.cabin = (function () {
  var mode = 'auto';
  var el = null;

  var CTX_DEFS = [
    { id: 'CTX-01', name: '默认/首页' },
    { id: 'CTX-02', name: '库存' },
    { id: 'CTX-03', name: '语音' },
    { id: 'CTX-04', name: '拍照识别' },
    { id: 'CTX-05', name: 'Agent 执行' },
    { id: 'CTX-06', name: '计划/烹饪' },
    { id: 'CTX-07', name: '定时器' },
    { id: 'CTX-08', name: '异常/安全' }
  ];

  function isPortrait() {
    return FS.mode && FS.mode.getLayout() === 'portrait';
  }

  function isPrivacy() {
    return FS.mode && FS.mode.isPrivacy();
  }

  function ctxForPage(id) {
    id = id || '';
    if (/^CHAT/.test(id)) return 'CTX-05';
    if (id === 'INV-05' || id === 'INV-06') return 'CTX-04';
    if (/^INV/.test(id)) return 'CTX-02';
    if (id === 'PLAN-05') return 'CTX-06';
    if (/^PLAN/.test(id)) return 'CTX-06';
    if (id === 'MORE-02' || id === 'MORE-03' || id === 'MORE-07' || id === 'MORE-08') return 'CTX-07';
    if (id === 'MORE-04') return 'CTX-08';
    return 'CTX-01';
  }

  function autoCtx() {
    var ov = FS.overlays;
    if (ov.isOpen('OV-10')) return 'CTX-08';
    if (ov.isOpen('OV-05')) return 'CTX-03';
    if (ov.isOpen('OV-07')) return 'CTX-04';
    if (ov.isOpen('OV-06')) return 'CTX-05';
    return ctxForPage(FS.router.currentId());
  }

  function currentCtx() { return mode === 'auto' ? autoCtx() : mode; }
  function setMode(m) { mode = m; refresh(); }

  /* ---------- CTX 模块化（竖屏：模块横排 + 模块内纵排 + 整体左右滑） ----------
   * 横屏仍用单列 ctx-panel；竖屏把同一批横屏组件按「模块」切开横滑。
   */
  function ctxMod(title, bodyHtml, size) {
    /* size: '' 标准 | narrow 窄 | wide 宽 | summary 今日摘要更宽 */
    return '<section class="ctx-mod' + (size ? ' ' + size : '') + '">' +
      (title ? '<div class="ctx-title">' + FS.esc(title) + '</div>' : '') +
      '<div class="ctx-mod-body">' + (bodyHtml || '') + '</div>' +
      '</section>';
  }

  function ctxHScroll(modsHtml, panelCls) {
    return '<div class="ctx-panel ctx-hscroll' + (panelCls ? ' ' + panelCls : '') + '">' +
      '<div class="ctx-htrack">' + modsHtml + '</div></div>';
  }

  /* 库存路径：每一层级单独一行（竖屏当前位置模块用） */
  function pathLevelsHtml(parts) {
    parts = (parts || []).filter(function (s) { return s != null && String(s).trim() !== ''; });
    if (!parts.length) parts = ['冰箱'];
    return '<div class="ctx-path-stack">' + parts.map(function (seg, i) {
      return '<div class="ctx-path-level' + (i === parts.length - 1 ? ' cur' : '') + '">' +
        (i > 0 ? '<span class="ctx-path-dot">·</span>' : '') +
        '<span class="ctx-path-seg">' + FS.esc(seg) + '</span></div>';
    }).join('') + '</div>';
  }

  /* 从路由 + 食材详情推导当前位置路径（下钻/详情页都能保留层级） */
  function invPathFromRoute() {
    var q = FS.dataQ;
    var id = FS.router.currentId() || '';
    var p = FS.router.currentParams();
    if (!p || typeof p.get !== 'function') p = new URLSearchParams();
    var path = ['冰箱'];
    var zone = p.get('zone');
    var layer = p.get('layer');
    var cell = p.get('cell');

    /* INV-03 详情页 URL 常只有 id：从食材 loc 回填层级 */
    if ((!zone || !zone.length) && (id === 'INV-03' || id === 'INV-04')) {
      var it = q.item(p.get('id') || p.get('edit'));
      if (it && it.loc) {
        zone = it.loc.zone || null;
        layer = it.loc.layer || null;
        if (it.loc.cells && it.loc.cells.length) cell = it.loc.cells.join('+');
      }
    }

    if (zone) {
      var zn = q.zoneName(zone);
      path.push(zn || zone);
    }
    if (zone && layer) {
      var ln = q.layerName(zone, layer);
      path.push(ln || layer);
    }
    if (cell) path.push(cell);
    return path;
  }

  function invScopeFromRoute() {
    var q = FS.dataQ;
    var id = FS.router.currentId() || '';
    var p = FS.router.currentParams();
    if (!p || typeof p.get !== 'function') p = new URLSearchParams();
    var scope = {};
    var zone = p.get('zone');
    var layer = p.get('layer');
    var cell = p.get('cell');
    if ((!zone || !zone.length) && (id === 'INV-03' || id === 'INV-04')) {
      var it = q.item(p.get('id') || p.get('edit'));
      if (it && it.loc) {
        zone = it.loc.zone || null;
        layer = it.loc.layer || null;
        if (it.loc.cells && it.loc.cells.length) cell = it.loc.cells.join('+');
      }
    }
    if (zone) scope.zone = zone;
    if (layer) scope.layer = layer;
    if (cell) scope.cell = cell;
    return scope;
  }

  var R = {};

  R['CTX-01'] = function () {
    var q = FS.dataQ, ui = FS.ui;
    var exp = q.expiring(3), t = FS.data.timers.find(function (x) { return x.state === 'running'; });
    if (isPortrait()) {
      return ctxHScroll(
        ctxMod('今日摘要',
          ui.kv('库存', FS.data.items.length + ' 项') +
          ui.kv('临期', exp.length + ' 项', exp.length ? 'warn-text' : '') +
          ui.kv('过期', q.expired().length + ' 项', 'danger-text') +
          ui.kv('晚餐', '番茄炒蛋+菠菜汤'),
          'summary') +
        ctxMod('下一事项',
          t ? '<div class="ctx-card">' + FS.icon('timer') + '<div><b>' + FS.esc(t.name) + '</b> 还剩 ' + FS.esc(t.remain) +
            '<div class="ctx-sub">来源：' + FS.esc(t.source) + '</div></div></div>'
            : '<div class="ctx-sub">暂无进行中的事项</div>',
          t ? 'wide' : '') +
        ctxMod('最近同步', '<div class="ctx-sub">今天 14:31 · 库存 20 项已同步</div>')
      );
    }
    return '<div class="ctx-panel">' +
      '<div class="ctx-title">今日摘要</div>' +
      ui.kv('库存', FS.data.items.length + ' 项') +
      ui.kv('临期', exp.length + ' 项', exp.length ? 'warn-text' : '') +
      ui.kv('过期', q.expired().length + ' 项', 'danger-text') +
      ui.kv('晚餐', '番茄炒蛋+菠菜汤') +
      '<div class="ctx-title">下一事项</div>' +
      (t ? '<div class="ctx-card">' + FS.icon('timer') + '<div><b>' + FS.esc(t.name) + '</b> 还剩 ' + FS.esc(t.remain) + '<div class="ctx-sub">来源：' + FS.esc(t.source) + '</div></div></div>'
         : '<div class="ctx-sub">暂无进行中的事项</div>') +
      '<div class="ctx-title">最近同步</div>' +
      '<div class="ctx-sub">今天 14:31 · 库存 20 项已同步</div>' +
      '</div>';
  };

  R['CTX-02'] = function () {
    var q = FS.dataQ, ui = FS.ui;
    var pageId = FS.router.currentId() || '';

    /* INV-08 空间形状编辑：侧舱显示当前独立页面 / 草稿 / 选中摘要 */
    if (pageId === 'INV-08') {
      var p = FS.router.currentParams();
      if (!p || typeof p.get !== 'function') p = new URLSearchParams();
      var level = p.get('level');
      if (level !== 'layers' && level !== 'layer') level = 'zones';
      var levelLabel = level === 'layers' ? '层结构' : (level === 'layer' ? '层设置' : '分区形状');
      var draft = FS.layoutStudio || {};
      var dirty = !!draft.dirty;
      var changes = draft.changeCount || 0;
      /* 选中摘要：优先整区，其次空位多选；草稿名可能还没写回 data，优先读草稿 */
      var selText = '无';
      if (draft.selectedZoneId) {
        var dz = draft.zones && draft.zones.find(function (x) { return x.id === draft.selectedZoneId; });
        var z = q.zone(draft.selectedZoneId);
        selText = dz ? dz.name : (z ? z.name : draft.selectedZoneId);
      } else if (draft.selectedKeys && draft.selectedKeys.length) {
        selText = draft.selectedKeys.length + ' 个空位';
      }
      var zoneP = p.get('zone');
      var pathLay = ['形状编辑', levelLabel];
      if ((level === 'layers' || level === 'layer') && zoneP) {
        var dzp = draft.zones && draft.zones.find(function (x) { return x.id === zoneP; });
        pathLay.push(dzp ? dzp.name : (q.zoneName(zoneP) || zoneP));
      }
      if (level === 'layer' && p.get('layer')) {
        pathLay.push(q.layerName(zoneP, p.get('layer')) || p.get('layer'));
      }

      /* L1 沿用 ver1 的表单编辑：选中分区后只显示名称、宽度、高度和备注。 */
      var sizeEditor = '';
      if (level === 'zones') {
        var selectedDraftZone = draft.selectedZoneId && draft.zones && draft.zones.find(function (x) {
          return x.id === draft.selectedZoneId;
        });
        if (selectedDraftZone) {
          var rect = selectedDraftZone.rect || { x: 0, y: 0, w: 1, h: 1 };
          sizeEditor = '<div class="ctx-layout-title">编辑分区</div>' +
            '<div class="ctx-layout-selected">已选「' + FS.esc(selectedDraftZone.name || '未命名分区') + '」<span>再次点击画布可取消</span></div>' +
            '<div class="ctx-layout-form" data-zone="' + FS.esc(selectedDraftZone.id) + '">' +
            '<label>名称<input type="text" value="' + FS.esc(selectedDraftZone.name || '') + '" data-layout-field="name"></label>' +
            '<label>宽度 <output>' + rect.w + ' 列</output><input type="range" min="1" max="3" step="1" value="' + rect.w + '" data-layout-field="width"></label>' +
            '<label>高度 <output>' + rect.h + ' 行</output><input type="range" min="1" max="3" step="1" value="' + rect.h + '" data-layout-field="height"></label>' +
            '<label>备注<input type="text" value="' + FS.esc(selectedDraftZone.note || '') + '" placeholder="可放食材" data-layout-field="note"></label>' +
            '</div>';
        } else {
          sizeEditor = '<div class="ctx-layout-empty">请先在左侧画布选择一个分区</div>';
        }
      }

      if (isPortrait()) {
        return ctxHScroll(
          ctxMod('形状编辑', pathLevelsHtml(pathLay), 'narrow') +
          ctxMod('草稿',
            ui.kv('页面', levelLabel) +
            ui.kv('改动', dirty ? changes + ' 处未提交' : '已同步', dirty ? 'warn-text' : 'ok-text') +
            ui.kv('选中', selText),
            'summary') +
          (level === 'zones' ? ctxMod('', sizeEditor, 'wide') : '')
        );
      }
      /* L1 右侧只保留 ver1 风格的四项编辑字段。 */
      if (level === 'zones') {
        return '<div class="ctx-panel ctx-shape-editor-panel">' + sizeEditor + '</div>';
      }
      return '<div class="ctx-panel">' +
        '<div class="ctx-title">形状编辑</div>' +
        '<div class="ctx-path">' + FS.esc(pathLay.join(' / ')) + '</div>' +
        '<div class="ctx-title">草稿状态</div>' +
        ui.kv('页面', levelLabel) +
        ui.kv('改动', dirty ? changes + ' 处未提交' : '已同步', dirty ? 'warn-text' : 'ok-text') +
        ui.kv('选中', selText) +
        (level === 'zones' ? '<div class="ctx-title">尺寸编辑</div>' + sizeEditor : '') +
        '</div>';
    }

    var path = invPathFromRoute();
    var scope = invScopeFromRoute();
    var st = q.stats(scope);
    if (isPortrait()) {
      /* 当前位置：每层级一行、模块收窄；范围统计放宽 */
      return ctxHScroll(
        ctxMod('当前位置', pathLevelsHtml(path), 'narrow') +
        ctxMod('范围统计',
          ui.kv('食材', st.total + ' 项') +
          ui.kv('临期', st.expiring + ' 项', st.expiring ? 'warn-text' : '') +
          ui.kv('已过期', st.expired + ' 项', st.expired ? 'danger-text' : '') +
          ui.kv('未定位', q.unlocated().length + ' 项'),
          'summary') +
        ctxMod('操作',
          '<button class="ctx-act" data-act="ovl:goto" data-arg="#/p/INV-08?level=zones">' + FS.icon('grid') + '空间形状编辑</button>', 'wide')
      );
    }
    return '<div class="ctx-panel">' +
      '<div class="ctx-title">当前位置</div>' +
      '<div class="ctx-path">' + FS.esc(path.join(' / ')) + '</div>' +
      '<div class="ctx-title">范围统计</div>' +
      ui.kv('食材', st.total + ' 项') +
      ui.kv('临期', st.expiring + ' 项', st.expiring ? 'warn-text' : '') +
      ui.kv('已过期', st.expired + ' 项', st.expired ? 'danger-text' : '') +
      ui.kv('未定位', q.unlocated().length + ' 项') +
      '<button class="ctx-act" data-act="ovl:goto" data-arg="#/p/INV-08?level=zones">' + FS.icon('grid') + '空间形状编辑</button>' +
      '</div>';
  };

  R['CTX-03'] = function () {
    if (isPortrait()) {
      return ctxHScroll(
        ctxMod('语音',
          '<div class="ctx-voice-state">' + FS.icon('mic', 'lg') + '<span>聆听中…</span></div>') +
        ctxMod('实时转写',
          '<div class="ctx-card"><div class="ctx-sub">转写</div>帮我把昨天买的菠菜登记到冷藏</div>' +
          '<div class="ctx-hint">说「取消」放弃，说「好了」提交</div>', 'wide') +
        ctxMod('操作',
          '<div class="ctx-btn-row">' +
            FS.ui.btn({ label: '取消', kind: 'ghost', size: 'sm', act: 'ovl:close', arg: 'OV-05' }) +
            FS.ui.btn({ label: '提交', kind: 'primary', size: 'sm', act: 'ovl:confirm', arg: 'OV-05' }) +
          '</div>', 'wide')
      );
    }
    return '<div class="ctx-panel">' +
      '<div class="ctx-title">语音</div>' +
      '<div class="ctx-voice-state">' + FS.icon('mic', 'lg') + '<span>聆听中…</span></div>' +
      '<div class="ctx-card"><div class="ctx-sub">实时转写</div>帮我把昨天买的菠菜登记到冷藏</div>' +
      '<div class="ctx-hint">说「取消」放弃，说「好了」提交</div>' +
      '<div class="ctx-btn-row">' +
        FS.ui.btn({ label: '取消', kind: 'ghost', size: 'sm', act: 'ovl:close', arg: 'OV-05' }) +
        FS.ui.btn({ label: '提交', kind: 'primary', size: 'sm', act: 'ovl:confirm', arg: 'OV-05' }) +
      '</div></div>';
  };

  R['CTX-04'] = function () {
    var ui = FS.ui;
    if (isPortrait()) {
      if (isPrivacy()) {
        return ctxHScroll(
          ctxMod('拍照识别',
            ui.kv('摄像头', '已关闭', 'warn-text') +
            ui.kv('原因', '隐私模式')) +
          ctxMod('说明',
            '<div class="ctx-hint">竖屏隐私模式不开启摄像头，请使用手动/语音登记或切换横屏标准</div>', 'wide')
        );
      }
      return ctxHScroll(
        ctxMod('拍照识别',
          ui.kv('摄像头', '已就绪', 'ok-text') +
          ui.kv('上传进度', '62%') +
          ui.progress(62, 'ok')) +
        ctxMod('识别',
          ui.kv('识别进度', '云端识别中…') +
          ui.kv('待确认', '3 项候选', 'warn-text') +
          '<div class="ctx-hint">识别结果需人工确认后才会写入库存</div>', 'wide')
      );
    }
    if (isPrivacy()) {
      return '<div class="ctx-panel">' +
        '<div class="ctx-title">拍照识别</div>' +
        ui.kv('摄像头', '已关闭', 'warn-text') +
        ui.kv('原因', '隐私模式') +
        '<div class="ctx-hint">竖屏隐私模式不开启摄像头，请使用手动/语音登记或切换横屏标准</div>' +
        '</div>';
    }
    return '<div class="ctx-panel">' +
      '<div class="ctx-title">拍照识别</div>' +
      ui.kv('摄像头', '已就绪', 'ok-text') +
      ui.kv('上传进度', '62%') +
      ui.progress(62, 'ok') +
      ui.kv('识别进度', '云端识别中…') +
      ui.kv('待确认', '3 项候选', 'warn-text') +
      '<div class="ctx-hint">识别结果需人工确认后才会写入库存</div></div>';
  };

  R['CTX-05'] = function () {
    var ui = FS.ui;
    if (isPortrait()) {
      return ctxHScroll(
        ctxMod('Agent 执行',
          '<div class="ctx-card"><div class="ctx-sub">当前计划</div><div class="ctx-card-text">查询临期食材 → 生成晚餐建议</div></div>', 'summary') +
        ctxMod('步骤',
          '<div class="ctx-steps">' +
            '<div class="ctx-step done">' + FS.icon('check') + '<span>inventory_query</span></div>' +
            '<div class="ctx-step doing"><span class="ap-dot"></span><span>menu_generate</span></div>' +
            '<div class="ctx-step todo"><span class="ap-dot"></span><span>生成回复</span></div>' +
          '</div>', 'wide') +
        ctxMod('确认',
          '<button class="ctx-act" data-act="nav" data-href="#/p/CHAT-04">' + FS.icon('shield') + '去确认</button>' +
          ui.kv('待确认', '1 项高风险', 'warn-text'), 'wide')
      );
    }
    return '<div class="ctx-panel">' +
      '<div class="ctx-title">Agent 执行</div>' +
      '<div class="ctx-card"><div class="ctx-sub">当前计划</div>查询临期食材 → 生成晚餐建议</div>' +
      '<div class="ctx-steps">' +
        '<div class="ctx-step done">' + FS.icon('check') + 'inventory_query</div>' +
        '<div class="ctx-step doing"><span class="ap-dot"></span>menu_generate</div>' +
        '<div class="ctx-step todo"><span class="ap-dot"></span>生成回复</div>' +
      '</div>' +
      ui.kv('待确认', '1 项高风险', 'warn-text') +
      '<button class="ctx-act" data-act="nav" data-href="#/p/CHAT-04">' + FS.icon('shield') + '去确认</button>' +
      '</div>';
  };

  R['CTX-06'] = function () {
    var ui = FS.ui;
    if (isPortrait()) {
      return ctxHScroll(
        ctxMod('烹饪中',
          '<div class="ctx-card"><div class="ctx-sub">番茄炒蛋 · 第 3/6 步</div>下番茄块，中火炒出汁</div>' +
          ui.progress(50, 'ok'), 'wide') +
        ctxMod('本步',
          ui.kv('本步食材', '番茄 ×2') +
          ui.kv('缺失食材', '无', 'ok-text'), 'wide') +
        ctxMod('操作',
          '<button class="ctx-act" data-act="nav" data-href="#/p/PLAN-05">' + FS.icon('fwd') + '下一步</button>', 'wide')
      );
    }
    return '<div class="ctx-panel">' +
      '<div class="ctx-title">烹饪中</div>' +
      '<div class="ctx-card"><div class="ctx-sub">番茄炒蛋 · 第 3/6 步</div>下番茄块，中火炒出汁</div>' +
      ui.progress(50, 'ok') +
      ui.kv('本步食材', '番茄 ×2') +
      ui.kv('缺失食材', '无', 'ok-text') +
      '<button class="ctx-act" data-act="nav" data-href="#/p/PLAN-05">' + FS.icon('fwd') + '下一步</button>' +
      '</div>';
  };

  R['CTX-07'] = function () {
    var ui = FS.ui;
    var t = FS.data.timers.filter(function (x) { return x.state === 'running'; })[0];
    if (isPortrait()) {
      if (!t) {
        return ctxHScroll(ctxMod('定时器', '<div class="ctx-sub">暂无运行中的定时器</div>'));
      }
      return ctxHScroll(
        ctxMod('最近到期',
          '<div class="ctx-timer-face">' + FS.esc(t.remain) + '</div>' +
          '<div class="ctx-sub" style="text-align:center">' + FS.esc(t.name) + '</div>') +
        ctxMod('详情',
          '<div class="ctx-sub">来源：' + FS.esc(t.source) + '</div>' +
          '<div class="ctx-btn-row">' +
            ui.btn({ label: '暂停', kind: 'ghost', size: 'sm', act: 'ctx:demo' }) +
            ui.btn({ label: '结束', kind: 'danger', size: 'sm', act: 'ctx:demo' }) +
          '</div>') +
        ctxMod('操作',
          '<button class="ctx-act" data-act="nav" data-href="#/p/MORE-02">' + FS.icon('timer') + '全部定时器</button>', 'wide')
      );
    }
    if (!t) return '<div class="ctx-panel"><div class="ctx-title">定时器</div><div class="ctx-sub">暂无运行中的定时器</div></div>';
    return '<div class="ctx-panel">' +
      '<div class="ctx-title">最近到期</div>' +
      '<div class="ctx-timer-face">' + FS.esc(t.remain) + '</div>' +
      '<div class="ctx-sub" style="text-align:center">' + FS.esc(t.name) + ' · 来源：' + FS.esc(t.source) + '</div>' +
      '<div class="ctx-btn-row">' +
        ui.btn({ label: '暂停', kind: 'ghost', size: 'sm', act: 'ctx:demo' }) +
        ui.btn({ label: '结束', kind: 'danger', size: 'sm', act: 'ctx:demo' }) +
      '</div>' +
      '<button class="ctx-act" data-act="nav" data-href="#/p/MORE-02">' + FS.icon('timer') + '全部定时器</button>' +
      '</div>';
  };

  R['CTX-08'] = function () {
    var ui = FS.ui;
    var ev = FS.data.safetyEvents.find(function (e) { return !e.confirmed; });
    if (isPortrait()) {
      if (!ev) {
        return ctxHScroll(ctxMod('异常/安全', '<div class="ctx-sub">当前无未处理的异常</div>'));
      }
      return ctxHScroll(
        ctxMod('异常/安全',
          '<div class="ctx-card danger-card">' + FS.icon('warn') + '<div><b>' + FS.esc(ev.title) + '</b>' +
          '<div class="ctx-sub">' + FS.esc(ev.time) + '</div></div></div>', 'wide') +
        ctxMod('详情',
          '<div class="ctx-sub">' + FS.esc(ev.desc) + '</div>' +
          ui.kv('风险级别', '中', 'warn-text') +
          ui.kv('恢复条件', '确认门体关严'), 'wide') +
        ctxMod('操作',
          '<button class="ctx-act danger" data-act="ovl:show" data-arg="OV-10">' + FS.icon('shield') + '立即处理</button>', 'wide'),
        'warn'
      );
    }
    if (!ev) return '<div class="ctx-panel"><div class="ctx-title">异常/安全</div><div class="ctx-sub">当前无未处理的异常</div></div>';
    return '<div class="ctx-panel warn">' +
      '<div class="ctx-title">异常/安全</div>' +
      '<div class="ctx-card danger-card">' + FS.icon('warn') + '<div><b>' + FS.esc(ev.title) + '</b>' +
      '<div class="ctx-sub">' + FS.esc(ev.time) + '</div><div class="ctx-sub">' + FS.esc(ev.desc) + '</div></div></div>' +
      ui.kv('风险级别', '中', 'warn-text') +
      ui.kv('恢复条件', '确认门体关严') +
      '<button class="ctx-act danger" data-act="ovl:show" data-arg="OV-10">' + FS.icon('shield') + '立即处理</button>' +
      '</div>';
  };

  FS.action('ctx:demo', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：操作已执行' });
  });
  FS.action('ovl:show', function (el) { FS.overlays.show(el.getAttribute('data-arg')); });

  /* ---------- 渲染 ---------- */

  function splitDate(d) {
    if (d.dateDay || d.weekday) {
      return { day: d.dateDay || d.date || '', week: d.weekday || '' };
    }
    var raw = String(d.date || '');
    var parts = raw.split(/\s+/);
    if (parts.length >= 2) {
      return { day: parts[0], week: parts.slice(1).join(' ') };
    }
    return { day: raw, week: '' };
  }

  function healthList() {
    var privacy = isPrivacy();
    return FS.data.health.map(function (h) {
      if (privacy && h.key === 'camera') {
        return { key: h.key, icon: h.icon, label: '摄像头（隐私关闭）', status: 'off' };
      }
      return h;
    });
  }

  /* 横屏固定区 */
  function fixedHtml(simple) {
    var d = FS.data.device;
    var dt = splitDate(d);
    var privacy = isPrivacy();
    var html = '<div class="cabin-fixed">' +
      '<div class="cabin-clock">' +
        '<div class="cabin-time">' + FS.esc(d.time) + '</div>' +
        '<div class="cabin-date-block">' +
          '<div class="cabin-date">' + FS.esc(dt.day) + '</div>' +
          (dt.week ? '<div class="cabin-weekday">' + FS.esc(dt.week) + '</div>' : '') +
        '</div>' +
      '</div>';
    if (privacy && !simple) {
      html += '<div class="cabin-privacy-chip" title="竖屏隐私模式：摄像头不开启">' +
        FS.icon('lock') + '<span>隐私模式</span></div>';
    }
    html +=
      '<div class="cabin-meta">' +
        '<div class="cabin-env">' + FS.icon('temp') + '<span>' + FS.esc(d.temp) + '</span>' +
        FS.icon('drop') + '<span>' + FS.esc(d.hum) + '</span></div>';
    if (!simple) {
      html += '<div class="cabin-health">' +
        healthList().map(function (h) { return FS.ui.healthIcon(h); }).join('') + '</div>';
    }
    return html + '</div></div>';
  }

  function actionsHtml() {
    var privacy = isPrivacy();
    var camLabel = privacy ? '摄像关' : '拍照';
    var camBtn;
    if (privacy) {
      camBtn = '<button class="cabin-act camera privacy-off" data-act="cabin:camera" ' +
        'aria-disabled="true" title="隐私模式已关闭摄像头">' +
        FS.icon('lock', 'lg') + '<span>' + camLabel + '</span></button>';
    } else {
      camBtn = '<button class="cabin-act camera" data-act="cabin:camera">' +
        FS.icon('camera', 'lg') + '<span>拍照</span></button>';
    }
    return '<div class="cabin-actions">' +
      '<button class="cabin-act voice" data-act="cabin:voice">' + FS.icon('mic', 'lg') + '<span>语音</span></button>' +
      camBtn +
      '</div>';
  }

  /* 竖屏状态信息卡（时间放大居中 + 日期/隐私/环境/健康，间距更松） */
  function portraitStatusHtml(simple) {
    var d = FS.data.device;
    var dt = splitDate(d);
    var dateText = dt.day + (dt.week ? ' · ' + dt.week : '');
    var privacy = isPrivacy();
    var html = '<div class="cabin-p-bar">' +
      '<div class="cabin-p-left">' +
        '<div class="cabin-time cabin-p-time">' + FS.esc(d.time) + '</div>' +
        '<div class="cabin-p-side">' +
          '<div class="cabin-p-time-row">' +
            '<div class="cabin-p-date">' + FS.esc(dateText) + '</div>';
    if (privacy && !simple) {
      html += '<div class="cabin-privacy-chip" title="竖屏隐私模式：摄像头不开启">' +
        FS.icon('lock') + '<span>隐私模式</span></div>';
    }
    html +=
          '</div>' +
          '<div class="cabin-p-meta-row">' +
            '<div class="cabin-env">' + FS.icon('temp') + '<span>' + FS.esc(d.temp) + '</span>' +
            FS.icon('drop') + '<span>' + FS.esc(d.hum) + '</span></div>';
    if (!simple) {
      html += '<div class="cabin-health">' +
        healthList().map(function (h) { return FS.ui.healthIcon(h); }).join('') + '</div>';
    }
    html += '</div></div></div></div>';
    return html;
  }

  function ctxBlock(ctxId) {
    return '<div class="cabin-ctx" data-ctx="' + ctxId + '">' +
      '<div class="ctx-id">' + ctxId + '</div>' +
      (R[ctxId] ? R[ctxId]() : '') +
      '</div>';
  }

  FS.action('cabin:voice', function () { FS.overlays.show('OV-05'); });
  FS.action('cabin:camera', function () {
    if (isPrivacy()) {
      if (FS.mode && FS.mode.privacyCameraToast) FS.mode.privacyCameraToast();
      return;
    }
    FS.router.go('#/p/INV-05');
  });

  function systemHtml(kind) {
    if (kind === 'standby') {
      return isPortrait() ? portraitStatusHtml(true) : fixedHtml(true);
    }
    if (kind === 'wizard') {
      var wiz = '<div class="ctx-panel"><div class="ctx-title">配网向导</div>' +
        FS.ui.kv('当前步骤', '第 2 步 / 共 4 步') +
        FS.ui.kv('网络', '未连接', 'warn-text') +
        '<div class="ctx-hint">需要帮助可呼叫「小冰小冰」</div></div>';
      if (isPortrait()) {
        return '<div class="cabin-p-main">' + portraitStatusHtml(true) +
          '<div class="cabin-ctx">' + wiz + '</div></div>';
      }
      return fixedHtml(true) + wiz;
    }
    var boot = '<div class="ctx-panel"><div class="ctx-title">自检摘要</div>' +
      FS.ui.kv('通过', '6 项', 'ok-text') +
      FS.ui.kv('降级', '1 项', 'warn-text') +
      '<div class="ctx-sub">FY-SP703 未插入</div></div>';
    if (isPortrait()) {
      return '<div class="cabin-p-main">' + portraitStatusHtml(true) +
        '<div class="cabin-ctx">' + boot + '</div></div>';
    }
    return fixedHtml(true) + boot;
  }

  function refresh() {
    if (!el) return;
    var def = FS.router.currentPage();
    el.classList.toggle('cabin-system', !!(def && def.system));
    if (def && def.system) {
      el.innerHTML = systemHtml(def.cabinMode || 'boot');
      return;
    }
    var ctx = currentCtx();
    if (isPortrait()) {
      /* 左：状态卡 + 横屏同款 CTX；右：语音/摄像大按钮占满舱高 */
      el.innerHTML =
        '<div class="cabin-p-main">' +
          portraitStatusHtml(false) +
          ctxBlock(ctx) +
        '</div>' +
        '<div class="cabin-p-acts">' + actionsHtml() + '</div>';
      bindLayoutFormInputs();
      return;
    }
    el.innerHTML = fixedHtml(false) + ctxBlock(ctx) + actionsHtml();
    bindLayoutFormInputs();
  }

  /* 右侧动态上下文内的 ver1 风格表单不走 click 委托，单独监听输入事件。 */
  function bindLayoutFormInputs() {
    if (!el) return;
    el.querySelectorAll('.ctx-layout-form [data-layout-field]').forEach(function (input) {
      var eventName = input.type === 'range' ? 'input' : 'change';
      input.addEventListener(eventName, function (ev) {
        var handler = FS.actions['inv:lay-context-edit'];
        if (typeof handler === 'function') handler(input, ev);
      });
      if (input.type === 'text') {
        input.addEventListener('focus', function (ev) {
          var handler = FS.actions['inv:lay-context-keyboard'];
          if (typeof handler === 'function') handler(input, ev);
        });
      }
    });
  }

  function mount(node) { el = node; refresh(); }

  function renderFull(ctxId) {
    if (isPortrait()) {
      return '<div class="cabin-p-main">' + portraitStatusHtml(false) + ctxBlock(ctxId) + '</div>' +
        '<div class="cabin-p-acts">' + actionsHtml() + '</div>';
    }
    return fixedHtml(false) + ctxBlock(ctxId) + actionsHtml();
  }

  return {
    mount: mount, refresh: refresh,
    setMode: setMode, currentCtx: currentCtx,
    ctxForPage: ctxForPage,
    defs: CTX_DEFS,
    renderCtx: function (id) { return R[id] ? R[id]() : ''; },
    renderFull: renderFull
  };
})();
