/**
 * 冰箱小精灵 ver2 UI 预览 —— 库存模块（INV-01 .. INV-08）
 * ------------------------------------------------------------------
 * 三级空间结构（L1 分区 → L2 层 → L3 可选九宫格）+ 空间/列表双视图。
 * INV-01 为数据驱动单模板：URL 参数驱动 scope（zone/layer/cell/view）；
 *   另支持 pick=1 位置选择模式（INV-04 新增/编辑食材的「选择位置」流程）。
 * 识别结果必须确认后才写入库存（INV-06）；布局修改先存草稿后提交（INV-08）。
   * INV-08 为三列可扩展画布单模板：level=zones|layers|cells，画布点选 + 上下文底栏。
 * 本模块同时注册 photo（拍照登记）与 voice（语音登记）两个关键流程。
 */
window.FS = window.FS || {};

(function () {
  var ui, q;
  function ready() { ui = FS.ui; q = FS.dataQ; }

  /* ---- INV-04 表单位置草稿（跨 INV-01 选位流程回写） ---- */
  var invDraft = {
    editId: null,   /* 当前编辑的食材 id（null=新增） */
    from: null,     /* 来源：voice 等 */
    loc: null       /* { zone, layer, cells: string[] } */
  };

  /* ==================== 位置 / scope 工具 ==================== */

  /* 解析当前 URL 参数为 scope 对象 */
  function scopeOf(pg) {
    return {
      zone: pg.params.get('zone') || null,
      layer: pg.params.get('layer') || null,
      cell: pg.params.get('cell') || null,
      view: pg.params.get('view') === 'list' ? 'list' : 'space',
      pick: pg.params.get('pick') === '1'
    };
  }

  /* 由 scope 构造 INV-01 hash（可带 view 覆盖；pick 模式保留 pick=1） */
  function invHash(scope, view) {
    var p = new URLSearchParams();
    if (scope.pick) p.set('pick', '1');
    if (scope.zone) p.set('zone', scope.zone);
    if (scope.layer) p.set('layer', scope.layer);
    var v = view || scope.view;
    if (!scope.pick && v === 'list') p.set('view', 'list');
    return '#/p/INV-01' + (p.toString() ? '?' + p.toString() : '');
  }

  /* 从当前库存层级进入对应的独立空间编辑页；back 保留用户的原始浏览位置。 */
  function studioEntryHash(scope) {
    var level = scope.layer ? 'layer' : (scope.zone ? 'layers' : 'zones');
    return studioHash(level, scope.zone, scope.layer, invHash(scope, scope.view));
  }

  /* 回到 INV-04（保留编辑 id / from） */
  function inv04Hash() {
    var p = new URLSearchParams();
    if (invDraft.editId) p.set('id', invDraft.editId);
    if (invDraft.from) p.set('from', invDraft.from);
    return '#/p/INV-04' + (p.toString() ? '?' + p.toString() : '');
  }

  /* scope 的可读名称（用于位置名/副标题） */
  function scopeName(scope) {
    if (!scope.zone) return '全冰箱';
    var n = q.zoneName(scope.zone);
    if (scope.layer) n += ' · ' + q.layerName(scope.zone, scope.layer);
    if (scope.cell) n += ' · ' + scope.cell;
    return n;
  }

  /* 位置草稿的可读文案 */
  function locLabel(loc) {
    if (!loc || !loc.zone) return '';
    var n = q.zoneName(loc.zone);
    if (loc.layer) n += ' / ' + q.layerName(loc.zone, loc.layer);
    if (loc.cells && loc.cells.length) n += ' / ' + loc.cells.join('+');
    return n;
  }

  /* 选位完成：写入草稿并返回 INV-04 */
  function finishLocPick(loc) {
    invDraft.loc = {
      zone: loc.zone,
      layer: loc.layer || null,
      cells: loc.cells && loc.cells.length ? loc.cells.slice() : []
    };
    FS.router.go(inv04Hash());
    FS.overlays.show('OV-01', { tone: 'ok', text: '已选择位置：' + locLabel(invDraft.loc) });
  }

  /* 面包屑（冰箱/分区/层/格，可点击回跳 INV-01 对应 scope） */
  function invCrumb(scope) {
    var base = scope.pick ? '#/p/INV-01?pick=1' : '#/p/INV-01';
    var parts = [{ label: '冰箱', href: base }];
    if (scope.zone) {
      parts.push({
        label: q.zoneName(scope.zone),
        href: base + (scope.pick ? '&' : '?') + 'zone=' + encodeURIComponent(scope.zone)
      });
    }
    if (scope.layer) {
      var lay = { label: q.layerName(scope.zone, scope.layer) };
      if (scope.cell) {
        lay.href = base + (scope.pick ? '&' : '?') + 'zone=' + encodeURIComponent(scope.zone) +
          '&layer=' + encodeURIComponent(scope.layer);
      }
      parts.push(lay);
    }
    if (scope.cell) parts.push({ label: scope.cell });
    /* 末段为当前位置：去掉 href 变为当前态 */
    var last = parts[parts.length - 1];
    if (last.href) parts[parts.length - 1] = { label: last.label };
    return ui.crumb(parts);
  }

  /* INV-03/07 的 parent：按参数返回对应 INV-01 scope */
  function parentScope(params) {
    var zone = params.get('zone'), layer = params.get('layer');
    var h = '#/p/INV-01';
    if (zone) h += '?zone=' + encodeURIComponent(zone);
    if (zone && layer) h += '&layer=' + encodeURIComponent(layer);
    return h;
  }

  /* 获取该 scope 的食材（cell 参数对 itemsIn 需传 cell 字段） */
  function itemsInScope(scope) {
    return q.itemsIn({ zone: scope.zone, layer: scope.layer, cell: scope.cell });
  }

  /* ==================== INV-01 库存浏览器 ==================== */
  FS.router.register({
    id: 'INV-01', name: '库存浏览器', module: '库存', level: '一级页', ctx: 'CTX-02',
    states: ['loading', 'empty', 'offline', 'error'],
    gallery: [
      { label: 'L1 根 · 空间视图', hash: '#/p/INV-01' },
      { label: 'L2 分区 · 空间视图', hash: '#/p/INV-01?zone=right' },
      { label: 'L3 九宫格 · 空间视图', hash: '#/p/INV-01?zone=right&layer=r2' },
      { label: 'L1 根 · 列表视图', hash: '#/p/INV-01?view=list' },
      { label: 'L2 分区 · 列表视图', hash: '#/p/INV-01?zone=right&view=list' }
    ],

    render: function (pg) {
      ready();
      var scope = scopeOf(pg);
      var zone = scope.zone ? q.zone(scope.zone) : null;
      var layer = (zone && scope.layer) ? q.layer(scope.zone, scope.layer) : null;
      var pick = !!scope.pick;
      var crumb = invCrumb(scope);

      /* ---- 位置选择模式（pick=1）：L1 分区 → L2 层 → L3 九宫格，选完回 INV-04 ---- */
      if (pick) {
        return renderLocPick(pg, scope, zone, layer, crumb);
      }

      /* L2/L3 顶部返回严格回到父空间，不依赖浏览器历史，深链进入时也不会跳错页。 */
      var browseParentScope = {
        zone: scope.layer ? scope.zone : null,
        layer: null,
        view: scope.view,
        pick: false
      };
      var browseActions =
        (scope.zone
          ? ui.iconBtn({ icon: 'back', label: '返回上一层', act: 'nav', href: invHash(browseParentScope) })
          : '') +
        ui.iconBtn({ icon: 'search', label: '搜索', act: 'nav', href: '#/p/INV-07' }) +
        ui.iconBtn({ icon: 'plus', label: '新增', act: 'nav', href: '#/p/INV-04' }) +
        ui.iconBtn({ icon: 'grid', label: '空间编辑', act: 'nav', href: studioEntryHash(scope) });

      /* 顶部固定 空间/列表 切换（切换不丢 scope） */
      var seg = ui.seg({
        act: 'inv:view',
        options: [{ value: 'space', label: '空间视图' }, { value: 'list', label: '列表视图' }],
        value: scope.view
      });

      /* 当前范围统计（三级 + 双视图共用一条信息条，避免各层级各写一套） */
      var st = q.stats({ zone: scope.zone, layer: scope.layer, cell: scope.cell });
      var scopeItems = itemsInScope(scope);
      var tone = ui.freshTone(scopeItems);
      var statChips =
        '<span class="inv-stat-chip"><b>' + scopeItems.length + '</b> 项</span>' +
        (st.expired ? '<span class="inv-stat-chip danger">' + st.expired + ' 过期</span>' : '') +
        (st.expiring ? '<span class="inv-stat-chip warn">' + st.expiring + ' 临期</span>' : '') +
        (!st.expired && !st.expiring && scopeItems.length ? '<span class="inv-stat-chip ok">保质充裕</span>' : '') +
        (st.unlocated ? '<span class="inv-stat-chip">未定位 ' + st.unlocated + '</span>' : '');

      var body = '';
      if (scope.view === 'list') {
        /* ---- 列表视图：当前 scope 食材（统计已在顶部信息条） ---- */
        var list = scopeItems.length
          ? scopeItems.map(function (it) { return ui.foodRow(it); }).join('')
          : ui.emptyState({ icon: 'box', title: '此范围暂无食材', desc: '可通过新增或调整范围查看' });
        body = ui.scrollArea('<div class="inv-list">' + list + '</div>');
      } else if (!zone) {
        /* ---- 空间视图 L1 根：冰箱内胆卡 + 分区热力宫格（占满主区，不滚动） ---- */
        body = ui.fridgeBox(ui.zoneMap({ hrefPrefix: '#/p/INV-01?zone=' }), {
          title: '冰箱空间 · 5 个分区', stats: st, fill: true
        });
      } else if (zone && !layer) {
        /* ---- 空间视图 L2 分区：层卡列表（可滚动） ---- */
        var layers = (zone.layers || []).filter(function (l) { return l.on; });
        var cards = layers.map(function (l, i) {
          /* L3 始终显示九宫格空间视图，不再根据 l.grid 区分 */
          var href = '#/p/INV-01?zone=' + encodeURIComponent(zone.id) + '&layer=' + encodeURIComponent(l.id);
          return ui.layerCard(zone, l, { href: href, index: i + 1 });
        }).join('');
        body = ui.fridgeBox(
          ui.scrollArea('<div class="inv-layers">' + cards + '</div>'),
          { title: zone.name + ' · ' + layers.length + ' 层', stats: st, fill: true }
        );
      } else {
        /* ---- 空间视图 L3 层：恒定 A1–C3 九宫格 ----
         * 这一级不再套内胆卡头（层名已在页头副标题与面包屑里），把整块高度让给九宫格，
         * 让每格能放下"食材名 + 数量 + 到期条"的完整信息（对齐 ver1 的信息密度）。
         * 一格一物：点击由 grid9 内部走 inv:cell（有物进详情 / 空格进新增）。 */
        body = '<div class="inv-g9-wrap">' + ui.grid9(zone, layer) + '</div>';
      }

      return '<div class="pg inv-browse-pg">' +
        ui.pageHead({
          title: '库存浏览器', id: 'INV-01',
          sub: scopeName(scope),
          actions: browseActions
        }) +
        '<div class="pg-body">' +
          '<div class="inv-toolbar">' + seg +
            /* 范围文字已经明确“冰箱”路径；不再重复放图标，为统计胶囊保留单行空间。 */
            '<div class="inv-scope-info tone-' + tone + '">' +
              crumb +
              '<div class="inv-stat-row">' + statChips + '</div>' +
            '</div>' +
          '</div>' +
          FS.states.wrap(pg, body, {
            emptyTitle: '还没有库存数据', emptyDesc: '先添加一些食材吧',
            emptyAction: ui.btn({ label: '添加食材', act: 'nav', href: '#/p/INV-04' })
          }) +
        '</div></div>';
    }
  });

  /* INV-01 位置选择模式渲染（从 INV-04「选择位置」进入）
   * 三步：选分区 → 选层 → 选格（第 3 步可跳过，直接"确认本层"）。
   * 复用浏览态同一套 zoneMap / layerCard / grid9，用户不用重新学一套空间视觉。 */
  function renderLocPick(pg, scope, zone, layer, crumb) {
    var stepHint = !zone
      ? '第 1 步 · 选择分区'
      : (zone && !layer)
        ? '第 2 步 · 选择层'
        : '第 3 步 · 选择一个空格';

    var body = '';
    if (!zone) {
      /* L1：点分区进入 L2 */
      body = ui.fridgeBox(ui.zoneMap({ hrefPrefix: '#/p/INV-01?pick=1&zone=' }), {
        title: '点选分区，开始指定存放位置', fill: true, stats: q.stats({})
      });
    } else if (zone && !layer) {
      /* L2：点层进入 L3 */
      var layers = (zone.layers || []).filter(function (l) { return l.on; });
      var cards = layers.map(function (l, i) {
        var href = '#/p/INV-01?pick=1&zone=' + encodeURIComponent(zone.id) +
          '&layer=' + encodeURIComponent(l.id);
        return ui.layerCard(zone, l, { href: href, index: i + 1 });
      }).join('');
      body = ui.fridgeBox(
        ui.scrollArea('<div class="inv-layers">' + cards + '</div>'),
        { title: '在「' + zone.name + '」中选择一层', fill: true, stats: q.stats({ zone: zone.id }) }
      );
    } else {
      /* L3：点空格 → 回写并返回 INV-04。一格一物，已占用的格不可选。 */
      var freeN = 0;
      for (var gy = 0; gy < 3; gy++) {
        for (var gx = 0; gx < 3; gx++) {
          var cid = 'ABC'[gy] + (gx + 1);
          if (!q.itemsIn({ zone: zone.id, layer: layer.id, cell: cid }).length) freeN++;
        }
      }
      body = ui.fridgeBox(
        ui.grid9(zone, layer, {
          pick: true,
          zoneId: zone.id,
          layerId: layer.id
        }),
        {
          title: freeN ? '点一个空格放入（本层还剩 ' + freeN + ' 个空格）' : '本层已放满，请换一层',
          fill: true, stats: q.stats({ zone: zone.id, layer: layer.id })
        }
      );
    }

    /* 一格一物：没有"只记到层"这种精度，底栏不再提供跳过按钮 */
    var foot = '<div class="pg-foot inv-pick-foot">' +
      ui.btn({ label: '取消', kind: 'ghost', act: 'inv:pick-cancel' }) +
      '<div class="spacer"></div>' +
      (zone && layer
        ? ui.btn({ label: '换一层', kind: 'ghost', icon: 'back', act: 'nav',
                   href: '#/p/INV-01?pick=1&zone=' + encodeURIComponent(zone.id) })
        : '') +
      '</div>';

    return '<div class="pg inv-pick-pg inv-browse-pg">' +
      ui.pageHead({
        title: '选择位置', id: 'INV-01',
        sub: stepHint + ' · ' + scopeName(scope),
        actions: ui.iconBtn({ icon: 'x', label: '取消', act: 'inv:pick-cancel' })
      }) +
      '<div class="pg-body">' +
        '<div class="inv-toolbar inv-pick-bar">' +
          '<span class="inv-pick-steps">' +
            '<span class="inv-step' + (!zone ? ' on' : ' done') + '">1 分区</span>' +
            '<span class="inv-step-sep">›</span>' +
            '<span class="inv-step' + (zone && !layer ? ' on' : (layer ? ' done' : '')) + '">2 层</span>' +
            '<span class="inv-step-sep">›</span>' +
            '<span class="inv-step' + (layer ? ' on' : '') + '">3 格子</span>' +
          '</span>' +
          crumb +
        '</div>' +
        body +
      '</div>' +
      foot +
      '</div>';
  }

  /* 空间/列表 视图切换：保留 scope 仅改 view */
  FS.action('inv:view', function (el) {
    var v = el.getAttribute('data-arg');
    var params = FS.router.currentParams();
    var scope = {
      zone: params.get('zone'), layer: params.get('layer'),
      view: v === 'list' ? 'list' : 'space'
    };
    FS.router.go(invHash(scope, v));
  });

  /* ==================== 旧位置中转页已取消 ====================
   * 原「位置内食材」是"一个格里可能有多项"时的中转列表页。
   * 现在改为一格一物：点九宫格里有食材的格 → 直接进 INV-03 食材详情（inv:cell 动作），
   * 空格 → 直接进 INV-04 新增并预填该位置。中转页没有存在意义，故整页移除。
   * 页面模板数由 31 → 30。
   */

  /* 点九宫格格子：有食材进详情，空格进新增（位置预填） */
  FS.action('inv:cell', function (el) {
    ready();
    var parts = (el.getAttribute('data-arg') || '').split('|');
    if (parts.length < 3) return;
    var zoneId = parts[0], layerId = parts[1], cell = parts[2];
    var items = q.itemsIn({ zone: zoneId, layer: layerId, cell: cell });
    if (items.length) {
      /* 带上位置参数：返回键能回到本层九宫格，侧舱「当前位置」也能显示完整路径 */
      FS.router.go('#/p/INV-03?id=' + encodeURIComponent(items[0].id) +
        '&zone=' + encodeURIComponent(zoneId) +
        '&layer=' + encodeURIComponent(layerId) +
        '&cell=' + encodeURIComponent(cell));
      return;
    }
    /* 空格：带着位置进新增页，省掉用户再走一遍选位流程 */
    invDraft.editId = null;
    invDraft.from = 'cell';
    invDraft.loc = { zone: zoneId, layer: layerId, cells: [cell] };
    FS.router.go('#/p/INV-04?from=cell');
  });

  /* 批量选择（勾选模式视觉演示）：行首 checkbox + 底部移动/删除条 */
  FS.action('inv:batch', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：进入批量选择（勾选模式）' });
  });

  /* ==================== INV-03 食材详情 ==================== */
  FS.router.register({
    id: 'INV-03', name: '食材详情', module: '库存', level: '二级页', ctx: 'CTX-02',
    states: ['loading', 'error'],
    parent: parentScope,

    render: function (pg) {
      ready();
      var it = q.item(pg.params.get('id')) || FS.data.items[0];
      var loc = q.locPath(it);

      /* 基本信息卡：名称/分类/数量单位（stepper） */
      var info = ui.card({
        title: it.name,
        side: ui.chip(it.cat, 'info'),
        body:
          ui.formRow({ label: '数量', control: ui.stepper({ act: 'inv:qty', value: it.qty, unit: it.unit || '' }) }) +
          ui.kv('登记时间', it.added + ' · 来源 ' + it.source) +
          ui.kv('置信度', (it.confidence * 100).toFixed(0) + '%', it.confidence < 0.8 ? 'danger-text' : 'ok-text')
      });

      /* 日期卡：生产/到期 + 估算旗标 */
      var dates = ui.card({
        title: '日期信息',
        body:
          ui.kv('登记日期', it.added) +
          ui.kv('到期日期', it.expire + (it.estimated ? '（估算）' : ''), q.daysTone(it.daysLeft) === 'danger' ? 'danger-text' : '') +
          ui.kv('剩余', q.daysText(it.daysLeft), q.daysTone(it.daysLeft) === 'ok' ? 'ok-text' : q.daysTone(it.daysLeft) === 'warn' ? 'warn-text' : 'danger-text')
      });

      /* 位置卡：全路径 + 精度说明 */
      var locCard = ui.card({
        title: '存放位置',
        body: ui.kv('位置', loc) +
          ui.kv('精度', it.loc && it.loc.cells && it.loc.cells.length ? '定位到格子' : (it.loc ? '定位到层' : '位置待补充'))
      });

      /* 最近操作记录 */
      var logCard = ui.card({
        title: '最近操作',
        body:
          ui.listRow({ icon: 'plus', title: '登记入库', sub: it.added + ' · ' + it.source, arrow: false }) +
          ui.listRow({ icon: 'minus', title: '消耗 1' + (it.unit || '份'), sub: '近 3 天 · 手动', arrow: false })
      });

      /* 推荐做法：关联菜谱 */
      var rec = FS.data.recipes.slice(0, 2).map(function (r) {
        return ui.listRow({ icon: 'recipe', title: r.name, sub: r.time + ' · ' + r.difficulty, act: 'plan:recipe', arg: r.id });
      }).join('');
      var recCard = ui.card({ title: '推荐做法', body: rec });

      var body = ui.scrollArea(info + dates + locCard + logCard + recCard, 'inv-detail');

      return '<div class="pg">' +
        ui.pageHead({
          title: '食材详情', id: 'INV-03', sub: it.name,
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body">' + FS.states.wrap(pg, body, { errorDesc: '加载食材信息失败' }) + '</div>' +
        '<div class="pg-foot">' +
          ui.btn({ label: '编辑', kind: 'primary', icon: 'edit', act: 'nav', href: '#/p/INV-04?id=' + encodeURIComponent(it.id) }) +
          ui.btn({ label: '移动位置', kind: 'ghost', act: 'inv:move' }) +
          ui.btn({ label: '加入购物清单', kind: 'ghost', act: 'inv:to-shopping' }) +
          '<div class="spacer"></div>' +
          ui.btn({ label: '删除', kind: 'danger', act: 'inv:delete', arg: it.name }) +
        '</div></div>';
    }
  });

  FS.action('inv:qty', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '演示：数量已更新' }); });
  /* 详情页「移动位置」：复用同一选位流程，回写到编辑页草稿 */
  FS.action('inv:move', function () {
    var params = FS.router.currentParams();
    var id = params.get('id');
    if (id) {
      invDraft.editId = id;
      var it = FS.dataQ.item(id);
      if (it && it.loc) {
        invDraft.loc = {
          zone: it.loc.zone,
          layer: it.loc.layer || null,
          cells: (it.loc.cells || []).slice()
        };
      }
    }
    FS.router.go('#/p/INV-01?pick=1');
  });
  FS.action('inv:to-shopping', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '已加入购物清单（演示）' }); });
  FS.action('inv:delete', function (el) {
    FS.overlays.show('OV-03', { title: '删除这条食材？', desc: '「' + el.getAttribute('data-arg') + '」将从库存中移除，此操作不可恢复。', ok: '删除', danger: true });
  });

  /* ==================== INV-04 新增/编辑食材 ==================== */
  FS.router.register({
    id: 'INV-04', name: '新增/编辑食材', module: '库存', level: '任务页', ctx: 'CTX-02',
    states: [],
    parent: 'INV-01',

    render: function (pg) {
      ready();
      var id = pg.params.get('id');
      var it = id ? q.item(id) : null;
      var from = pg.params.get('from');

      /* 同步草稿上下文：切换编辑对象时从食材回填位置；新增保留已选位置 */
      invDraft.from = from || null;
      if (id) {
        if (invDraft.editId !== id) {
          invDraft.editId = id;
          invDraft.loc = (it && it.loc)
            ? { zone: it.loc.zone, layer: it.loc.layer || null, cells: (it.loc.cells || []).slice() }
            : null;
        }
      } else {
        invDraft.editId = null;
      }

      var title = it ? '编辑食材' : (from === 'voice' ? '语音登记确认' : '新增食材');
      var cats = ['蔬菜', '肉类', '蛋奶', '水果', '主食', '速冻', '调味', '饮品', '其他'];

      var catChips = '<div class="inv-chiprow">' + cats.map(function (c) {
        var on = it && it.cat === c;
        return '<button class="chip inv-cat' + (on ? ' ok' : '') + '" data-act="inv:cat" data-arg="' + c + '">' + c + '</button>';
      }).join('') + '</div>';

      /* 位置：整行可点 → 进入 L1/L2/L3 选位；已选则显示路径，再点即重选（无额外按钮文案） */
      var loc = invDraft.loc;
      var locText = locLabel(loc);
      var locRow = ui.formRow({
        label: '存放位置',
        desc: '分区 → 层 → 格（一格一物）',
        control:
          '<button type="button" class="inv-loc-value' + (locText ? ' has' : '') + '" data-act="inv:loc-start" title="选择位置">' +
            FS.icon('location') +
            (locText
              ? '<span class="inv-loc-text">' + FS.esc(locText) + '</span>'
              : '<span class="inv-loc-text inv-loc-placeholder">选择位置</span>') +
            FS.icon('fwd', 'inv-loc-arrow') +
          '</button>'
      });

      var mergeSeg = ui.seg({
        act: 'inv:merge', value: 'new',
        options: [{ value: 'merge', label: '合并到现有批次' }, { value: 'new', label: '新建批次' }]
      });

      var form =
        ui.formRow({ label: '名称', control: ui.inputLike({ value: it ? it.name : '', placeholder: '输入食材名称', icon: 'edit', act: 'inv:name' }) }) +
        ui.formRow({ label: '分类', control: catChips }) +
        ui.formRow({ label: '数量', control: ui.stepper({ act: 'inv:qty', value: it ? it.qty : '1', unit: it ? it.unit || '个' : '个' }) }) +
        ui.formRow({ label: '到期日期', desc: '可点击选择，估算时勾选右侧开关', control: ui.inputLike({ value: it ? it.expire : '', placeholder: '选择日期', icon: 'clock', act: 'inv:date' }) + ui.sw({ on: it ? !!it.estimated : false, act: 'inv:est' }) }) +
        ui.formRow({ label: '日期为估算', desc: '', control: ui.sw({ on: it ? !!it.estimated : false, act: 'inv:est' }) }) +
        locRow +
        ui.formRow({ label: '备注', control: '<div class="inv-note-control">' + ui.inputLike({ value: it ? it.note : '', placeholder: '备注（可选）', act: 'inv:note' }) + '</div>' }) +
        ui.formRow({ label: '来源', control: '<span class="inv-src">' + FS.esc(it ? it.source : (from === 'voice' ? '语音' : '手动')) + '</span>' }) +
        ui.formRow({ label: '同名批次', control: mergeSeg });

      return '<div class="pg">' +
        ui.pageHead({
          title: title, id: 'INV-04', sub: it ? '编辑 ' + it.name : '逐项填写登记信息',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body">' + ui.scrollArea('<div class="inv-form">' + form + '</div>') + '</div>' +
        '<div class="pg-foot">' +
          ui.btn({ label: '保存', kind: 'primary', icon: 'check', act: 'inv:save' }) +
          ui.btn({ label: '语音录入', kind: 'ghost', icon: 'mic', act: 'inv:voice-input' }) +
          ui.btn({ label: '自动补全', kind: 'ghost', icon: 'zap', act: 'inv:auto-fill' }) +
          '<div class="spacer"></div>' +
          ui.btn({ label: '取消', kind: 'ghost', act: 'back' }) +
        '</div></div>';
    }
  });

  FS.action('inv:name', function () { FS.overlays.show('OV-12'); });
  FS.action('inv:date', function () { FS.overlays.show('OV-12', { tab: '日期' }); });
  FS.action('inv:cat', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '演示：已选择分类' }); });
  FS.action('inv:est', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '演示：切换估算开关' }); });
  FS.action('inv:note', function () { FS.overlays.show('OV-12'); });
  FS.action('inv:merge', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '演示：批次合并选项' }); });
  FS.action('inv:save', function () {
    invDraft.loc = null;
    invDraft.editId = null;
    FS.overlays.show('OV-01', { tone: 'ok', text: '已保存（演示）' });
    FS.router.back();
  });
  /* 语音录入沿用全局语音登记流程：识别完成后回到本表单供用户确认。 */
  FS.action('inv:voice-input', function () { FS.flows.start('voice'); });
  /* 自动补全只填充可推断字段；提交前仍由用户核对并点击保存。 */
  FS.action('inv:auto-fill', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已自动补全可推断字段，请确认后保存（演示）' });
  });

  /* ---- 位置选择：开始 / 取消 / 确认格子 / 确认本层 / 清除 ---- */
  FS.action('inv:loc-start', function () {
    /* 从 L1 分区图开始；已选位置时同样入口 = 重新选择 */
    FS.router.go('#/p/INV-01?pick=1');
  });
  FS.action('inv:pick-cancel', function () {
    FS.router.go(inv04Hash());
  });
  FS.action('inv:pick-cell', function (el) {
    var parts = (el.getAttribute('data-arg') || '').split('|');
    if (parts.length < 3) return;
    finishLocPick({
      zone: parts[0],
      layer: parts[1],
      cells: parts[2].split('+').filter(Boolean)
    });
  });
  /* 一格一物：点已占用的格给出解释，而不是静默无反应 */
  FS.action('inv:pick-taken', function (el) {
    FS.overlays.show('OV-01', {
      tone: 'warn',
      text: '该位置已放「' + (el.getAttribute('data-arg') || '其它食材') + '」，一格只放一样，请选空格'
    });
  });

  /* ==================== INV-05 拍照取景与拍摄 ==================== */
  FS.router.register({
    id: 'INV-05', name: '拍照取景与拍摄', module: '库存', level: '任务页', ctx: 'CTX-04',
    states: [],

    render: function (pg) {
      ready();
      /* 静态取景框：占满页头与底栏之间的中间区；网络状态叠在取景框底部，不另占高度 */
      var frame = '<div class="inv-frame">' +
        '<div class="inv-frame-grid"><i></i><i></i><i></i><i></i></div>' +
        '<span class="inv-corner tl"></span><span class="inv-corner tr"></span><span class="inv-corner bl"></span><span class="inv-corner br"></span>' +
        ui.imgPh({ icon: 'camera', label: '取景预览（演示）' }) +
        '<div class="inv-net">' + ui.statusDot('ok') +
          '<span class="sd-label">Wi-Fi -58dBm · 云端在线 · 可上传</span></div>' +
        '</div>';

      return '<div class="pg">' +
        ui.pageHead({
          title: '拍照登记', id: 'INV-05', sub: '对准食材，保持平稳',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body inv-cam">' + frame + '</div>' +
        '<div class="pg-foot">' +
          ui.btn({ label: '重拍', kind: 'ghost', icon: 'camera', act: 'inv:reshoot' }) +
          '<div class="spacer"></div>' +
          ui.btn({ label: '拍摄', kind: 'primary', size: 'big', icon: 'camera', act: 'inv:shoot' }) +
          '<div class="spacer"></div>' +
          ui.btn({ label: '取消', kind: 'ghost', act: 'back' }) +
        '</div></div>';
    }
  });

  FS.action('inv:reshoot', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '演示：重新取景' }); });
  FS.action('inv:shoot', function () { FS.flows.start('photo'); });

  /* ==================== INV-06 识别结果确认 ==================== */
  FS.router.register({
    id: 'INV-06', name: '识别结果确认', module: '库存', level: '任务页', ctx: 'CTX-04',
    states: ['error'],

    render: function (pg) {
      ready();
      var list = FS.data.recognize.map(function (r) {
        var confTone = r.confidence >= 0.85 ? 'ok' : r.confidence >= 0.7 ? 'warn' : 'danger';
        var warn = r.warnings.length
          ? '<div class="inv-warn">' + FS.icon('warn') + '<span>' + FS.esc(r.warnings.join('；')) + '</span></div>'
          : '';
        var merge = r.mergeWith
          ? '<div class="inv-merge">' + FS.icon('info') + '<span>建议合并：' + FS.esc(r.mergeWith) + '</span></div>'
          : '';
        return '<div class="inv-reco' + (r.keep ? ' keep' : '') + '">' +
          '<label class="inv-check"><input type="checkbox"' + (r.keep ? ' checked' : '') + '></label>' +
          '<div class="inv-reco-main">' +
            '<div class="inv-reco-top">' +
              ui.inputLike({ value: r.name, act: 'inv:reco-name' }) +
              '<span class="inv-reco-qty">' + FS.esc(r.qty) + FS.esc(r.unit || '') + '</span>' +
              ui.chip((r.confidence * 100).toFixed(0) + '%', confTone) +
            '</div>' +
            '<div class="inv-reco-loc">' + FS.icon('location') + '<span>建议位置：' + FS.esc(r.suggestLoc) + '</span>' +
              ui.btn({ label: '换位置', kind: 'ghost', size: 'sm', act: 'inv:reco-loc' }) + '</div>' +
            warn + merge +
          '</div></div>';
      }).join('');

      var body =
        '<div class="inv-rulebar">' + FS.icon('shield') + '<span>识别结果需确认后才会写入库存</span></div>' +
        ui.scrollArea('<div class="inv-reco-list">' + list + '</div>');

      return '<div class="pg">' +
        ui.pageHead({
          title: '识别结果确认', id: 'INV-06', sub: '核对候选并确认入库',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body">' + FS.states.wrap(pg, body, { errorDesc: '识别失败，请重试拍摄' }) + '</div>' +
        '<div class="pg-foot">' +
          ui.btn({ label: '确认并同步', kind: 'primary', icon: 'check', act: 'inv:reco-ok' }) +
          ui.btn({ label: '存为待确认', kind: 'ghost', act: 'inv:reco-pending' }) +
          '<div class="spacer"></div>' +
          ui.btn({ label: '全部取消', kind: 'ghost', act: 'back' }) +
        '</div></div>';
    }
  });

  FS.action('inv:reco-name', function () { FS.overlays.show('OV-12'); });
  FS.action('inv:reco-loc', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '演示：换位置' }); });
  FS.action('inv:reco-ok', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已确认并同步到库存（演示）' });
    FS.router.go('#/p/INV-01');
  });
  FS.action('inv:reco-pending', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已存为待确认（演示）' });
    FS.router.go('#/p/INV-01');
  });

  /* ==================== INV-07 搜索与筛选结果 ==================== */
  FS.router.register({
    id: 'INV-07', name: '搜索与筛选结果', module: '库存', level: '二级页', ctx: 'CTX-02',
    states: ['loading', 'empty', 'error'],
    parent: parentScope,

    render: function (pg) {
      ready();
      var filter = pg.params.get('filter') || 'all';
      var items = FS.data.items.slice();
      if (filter === 'expiring') items = q.expiring(3);
      else if (filter === 'expired') items = q.expired();
      else if (filter === 'unlocated') items = q.unlocated();

      var chips = '<div class="inv-chiprow">' +
        [['all', '全部'], ['expiring', '临期'], ['expired', '过期'], ['unlocated', '未定位']].map(function (f) {
          return '<button class="chip inv-filter' + (filter === f[0] ? ' ok' : '') + '" data-act="inv:filter" data-arg="' + f[0] + '">' + f[1] + '</button>';
        }).join('') +
        ui.inputLike({ value: '分类', placeholder: '分类', act: 'inv:filter-cat' }) +
        '</div>';

      var sort = ui.seg({
        act: 'inv:sort', value: 'expire',
        options: [{ value: 'expire', label: '按到期' }, { value: 'added', label: '按加入时间' }, { value: 'name', label: '按名称' }]
      });

      var list = items.length
        ? items.map(function (it) { return ui.foodRow(it); }).join('')
        : ui.emptyState({ icon: 'search', title: '没有符合条件的食材', desc: '试试调整筛选或搜索词' });

      var body =
        '<div class="inv-searchbar">' +
          ui.inputLike({ placeholder: '搜索食材名称', icon: 'search', act: 'inv:search' }) +
          ui.btn({ label: '搜索', kind: 'primary', act: 'inv:search' }) +
        '</div>' +
        chips + sort +
        ui.scrollArea('<div class="inv-list">' + list + '</div>');

      return '<div class="pg">' +
        ui.pageHead({
          title: '搜索与筛选', id: 'INV-07', sub: '共 ' + items.length + ' 项结果',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body">' +
          FS.states.wrap(pg, body, { emptyTitle: '没有符合条件的食材', emptyDesc: '试试调整筛选或搜索词' }) +
        '</div>' +
        '<div class="pg-foot">' +
          ui.btn({ label: '批量选择', kind: 'ghost', act: 'inv:batch' }) +
          '<div class="spacer"></div>' +
          ui.btn({ label: '清除筛选', kind: 'ghost', act: 'nav', href: '#/p/INV-01' }) +
        '</div></div>';
    }
  });

  FS.action('inv:search', function () { FS.overlays.show('OV-12'); });
  FS.action('inv:filter', function (el) {
    FS.router.go('#/p/INV-07?filter=' + encodeURIComponent(el.getAttribute('data-arg')));
  });
  FS.action('inv:filter-cat', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '演示：分类下拉' }); });
  FS.action('inv:sort', function () { FS.overlays.show('OV-01', { tone: 'ok', text: '演示：切换排序' }); });

  /* ==================== INV-08 空间形状编辑（按入口层级分成独立页） ====================
   * URL：#/p/INV-08?level=zones                         → 分区形状
   *       #/p/INV-08?level=layers&zone=right             → 指定分区的层结构
   *       #/p/INV-08?level=layer&zone=right&layer=r2     → 指定层的设置
   * 设计要点：
   *   · 不在页内放标签栏或分区芯片；从库存 L1/L2/L3 进入对应独立编辑页，返回按原路径回退。
   *   · L1 沿用 ver1 的“点分区后调宽/调高”交互；尺寸编辑放在右侧动态上下文区。
   *   · L3 九宫格恒为 A1–C3 九格，一格一项食材；这里只展示规则，不允许关闭。
   *   · 所有改动先进内存草稿，底栏一次提交写回 FS.data.layout；支持一步撤销。
   *   · 命名不强制走软键盘：常用名做成预置芯片一点即改，仍保留键盘入口。
   * LVGL 对照：分段 = 互斥 lv_btn 组；画布 = LV_GRID；命名面板 = 页面内子容器
   *   （不占用 12 个全局浮层名额）。
   */

  /* 布局草稿（内存；进入页时从正式 layout clone） */
  var layoutDraft = {
    ready: false,
    dirty: false,
    changeCount: 0,
    zones: null,
    /* 选中：L1 空位单位格 keys "x,y"（可多选成矩形）；整区 zoneId */
    selectedKeys: [],
    selectedZoneId: null,
    /* 当前选中分区准备落位的宽高；未选分区时为 null。 */
    shape: null,
    zoneCreateMode: false,
    /* 命名面板：{ kind:'zone'|'layer', zoneId, layerId } */
    naming: null,
    pendingAction: null,   /* OV-03 确认后执行 */
    undoSnap: null         /* 一步撤销 */
  };

  /* 暴露给侧舱读取 */
  FS.layoutStudio = layoutDraft;

  /* 预置名称/类型（触屏上比软键盘输入中文快得多） */
  var ZONE_NAME_PRESETS = ['冷冻区', '冷藏区', '左冷藏', '右冷藏', '变温室', '门架', '果蔬抽屉', '保鲜层'];
  var ZONE_TYPE_PRESETS = ['冷冻', '冷藏', '变温', '门架', '抽屉'];
  var LAYER_NAME_PRESETS = ['第1层', '第2层', '第3层', '第4层', '上格', '中格', '下格', '抽屉'];

  function cloneLayoutZones() {
    return JSON.parse(JSON.stringify(FS.data.layout.zones || []));
  }

  function ensureLayoutDraft() {
    if (!layoutDraft.ready || !layoutDraft.zones) {
      layoutDraft.zones = cloneLayoutZones();
      layoutDraft.ready = true;
      layoutDraft.dirty = false;
      layoutDraft.changeCount = 0;
      layoutDraft.selectedKeys = [];
      layoutDraft.selectedZoneId = null;
      layoutDraft.shape = null;
      layoutDraft.zoneCreateMode = false;
      layoutDraft.naming = null;
      layoutDraft.undoSnap = null;
    }
    return layoutDraft;
  }

  function markLayoutDirty(n) {
    layoutDraft.dirty = true;
    layoutDraft.changeCount += (n == null ? 1 : n);
  }

  function pushUndo() {
    layoutDraft.undoSnap = JSON.stringify({
      zones: layoutDraft.zones,
      selectedKeys: layoutDraft.selectedKeys,
      selectedZoneId: layoutDraft.selectedZoneId,
      shape: layoutDraft.shape,
      zoneCreateMode: layoutDraft.zoneCreateMode
    });
  }

  function draftZone(id) {
    ensureLayoutDraft();
    return (layoutDraft.zones || []).find(function (z) { return z.id === id; }) || null;
  }

  function draftLayer(zoneId, layerId) {
    var z = draftZone(zoneId);
    if (!z) return null;
    return (z.layers || []).find(function (l) { return l.id === layerId; }) || null;
  }

  /* ---- 几何工具：固定 3 列、行数可扩展的单位格 ---- */
  function unitKey(x, y) { return x + ',' + y; }
  function parseUnitKey(k) {
    var p = String(k).split(',');
    return { x: parseInt(p[0], 10), y: parseInt(p[1], 10) };
  }

  /* 选中集合是否构成满矩形；是则返回矩形范围 */
  function unitRect(keys) {
    if (!keys || !keys.length) return false;
    var pts = keys.map(parseUnitKey);
    var xs = pts.map(function (p) { return p.x; });
    var ys = pts.map(function (p) { return p.y; });
    var minX = Math.min.apply(null, xs), maxX = Math.max.apply(null, xs);
    var minY = Math.min.apply(null, ys), maxY = Math.max.apply(null, ys);
    if (keys.length !== (maxX - minX + 1) * (maxY - minY + 1)) return false;
    var set = {};
    keys.forEach(function (k) { set[k] = true; });
    for (var y = minY; y <= maxY; y++) {
      for (var x = minX; x <= maxX; x++) {
        if (!set[unitKey(x, y)]) return false;
      }
    }
    return { minX: minX, maxX: maxX, minY: minY, maxY: maxY, w: maxX - minX + 1, h: maxY - minY + 1 };
  }

  /* 草稿中占据该单位格的分区 */
  function zoneAtUnit(x, y) {
    ensureLayoutDraft();
    for (var i = 0; i < layoutDraft.zones.length; i++) {
      var r = layoutDraft.zones[i].rect;
      if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return layoutDraft.zones[i];
    }
    return null;
  }

  /* 画布总行数由当前最底部的分区决定，至少保留首屏 3 行。 */
  function layoutRowCount() {
    ensureLayoutDraft();
    return (layoutDraft.zones || []).reduce(function (rows, z) {
      var r = z.rect || {};
      return Math.max(rows, (r.y || 0) + (r.h || 1));
    }, 3);
  }

  /* Ver1 同款：空位优先显示 1×1 添加入口；首屏已满则在下一行继续显示。 */
  function firstOpenUnitKey() {
    ensureLayoutDraft();
    var rows = layoutRowCount();
    for (var y = 0; y < rows; y++) {
      for (var x = 0; x < 3; x++) {
        if (!zoneAtUnit(x, y)) return unitKey(x, y);
      }
    }
    return unitKey(0, rows);
  }

  function canPlaceSelectedZone(x, y, shape) {
    var selectedId = layoutDraft.selectedZoneId;
    if (!selectedId || !shape || x + shape.w > 3) return false;
    for (var yy = y; yy < y + shape.h; yy++) {
      for (var xx = x; xx < x + shape.w; xx++) {
        var hit = zoneAtUnit(xx, yy);
        if (hit && hit.id !== selectedId) return false;
      }
    }
    return true;
  }

  /* 点空位：把新格并入选区并补成最小外接矩形；越界/撞到已有分区则只保留新格 */
  function expandUnitSelection(current, key) {
    var set = {};
    current.forEach(function (k) { set[k] = true; });
    if (set[key]) return [];                 /* 再点已选格 = 取消选择 */
    set[key] = true;
    var r = unitRect(Object.keys(set));
    if (!r) return [key];
    var out = [];
    for (var y = r.minY; y <= r.maxY; y++) {
      for (var x = r.minX; x <= r.maxX; x++) out.push(unitKey(x, y));
    }
    /* 补出来的矩形若压到已有分区，退回只选新格 */
    var clean = out.every(function (k) {
      var p = parseUnitKey(k);
      return !zoneAtUnit(p.x, p.y);
    });
    return clean ? out : [key];
  }

  /* INV-08 用 level 参数表示独立编辑页；不再在页面内切换标签。 */
  function studioHash(level, zoneId, layerId, backHash) {
    var p = new URLSearchParams();
    p.set('level', level || 'zones');
    if (zoneId) p.set('zone', zoneId);
    if (layerId) p.set('layer', layerId);
    if (backHash) p.set('back', backHash);
    var s = p.toString();
    return '#/p/INV-08' + (s ? '?' + s : '');
  }

  function refreshLay() {
    /* 同 hash 也要重渲选中态；优先 shell 直接刷页 */
    if (FS.shell && FS.shell.renderPage) {
      FS.shell.renderPage();
    } else {
      FS.router.go(location.hash || '#/p/INV-08?level=zones');
    }
    if (FS.cabin && FS.cabin.refresh) FS.cabin.refresh();
  }

  function askConfirm(title, desc, ok, fn, danger) {
    layoutDraft.pendingAction = fn;
    FS.overlays.show('OV-03', { title: title, desc: desc, ok: ok || '确认', danger: !!danger });
  }

  /* 劫持 OV-03 确认：若有布局 pending 则执行 */
  (function hookLayConfirm() {
    var prev = FS.actions['ovl:confirm'];
    FS.action('ovl:confirm', function (el) {
      var id = el.getAttribute('data-arg');
      if (id === 'OV-03' && layoutDraft.pendingAction) {
        var fn = layoutDraft.pendingAction;
        layoutDraft.pendingAction = null;
        if (FS.overlays && FS.overlays.close) FS.overlays.close('OV-03');
        fn();
        return;
      }
      if (typeof prev === 'function') prev(el);
      else {
        if (FS.overlays) {
          FS.overlays.close(id);
          FS.overlays.show('OV-01', { tone: 'ok', text: '已确认' });
        }
      }
    });
  })();

  /* 提交影响面：草稿中被删掉的分区/层里若仍挂着食材，必须先提示迁移 */
  function impactSummary() {
    ready();
    ensureLayoutDraft();
    var warnings = [];
    var affected = 0;
    var liveZones = {}, liveLayers = {};
    layoutDraft.zones.forEach(function (z) {
      liveZones[z.id] = true;
      (z.layers || []).forEach(function (l) { liveLayers[z.id + '/' + l.id] = true; });
    });
    (FS.data.layout.zones || []).forEach(function (z) {
      if (!liveZones[z.id]) {
        var n = q.itemsIn({ zone: z.id }).length;
        if (n) {
          warnings.push('分区「' + z.name + '」将被删除，含 ' + n + ' 项食材需迁移');
          affected += n;
        }
        return;
      }
      (z.layers || []).forEach(function (l) {
        if (!liveLayers[z.id + '/' + l.id]) {
          var m = q.itemsIn({ zone: z.id, layer: l.id }).length;
          if (m) {
            warnings.push('「' + z.name + ' / ' + l.name + '」将被删除，含 ' + m + ' 项食材需迁移');
            affected += m;
          }
        }
      });
    });
    return { warnings: warnings, affected: affected, changeCount: layoutDraft.changeCount };
  }

  /* ---- 渲染：分区形状（3×3 画布） ---- */
  function renderLayZones() {
    ready();
    ensureLayoutDraft();
    var addKey = firstOpenUnitKey();
    var addPoint = parseUnitKey(addKey);
    var rows = Math.max(layoutRowCount(), addPoint.y + 1);
    var extended = rows > 3;

    return '<div class="inv-studio inv-studio-zones' + (extended ? ' is-extended' : '') + '">' +
      '<div class="inv-studio-hint info">' + FS.icon('info') +
        '<span>选择分区后在右侧调整</span></div>' +
      ui.fridgeBox(
        ui.layoutZoneGrid({
          zones: layoutDraft.zones,
          selectedKeys: layoutDraft.selectedKeys,
          selectedZoneId: layoutDraft.selectedZoneId,
          addKey: addKey,
          rowCount: rows
        }),
        { title: '分区形状 · ' + layoutDraft.zones.length + ' 个分区', fill: !extended, stats: q.stats({}) }
      ) +
      '</div>';
  }

  /* ---- 渲染：指定分区的层结构（不在页内切换其它分区） ---- */
  function renderLayLayers(zoneId, backHash) {
    ready();
    ensureLayoutDraft();
    var zone = draftZone(zoneId);
    if (!zone) {
      return '<div class="inv-studio">' +
        ui.emptyState({ icon: 'box', title: '未找到要编辑的分区', desc: '返回库存空间后重新选择一个分区' }) +
        '</div>';
    }

    var layers = zone.layers || [];
    var rows = layers.map(function (l, idx) {
      var cnt = q.itemsIn({ zone: zone.id, layer: l.id }).length;
      var layerBack = studioHash('layers', zone.id, null, backHash);
      return '<div class="inv-layer-card inv-layer-entry" data-act="nav" data-href="' +
        studioHash('layer', zone.id, l.id, layerBack) + '">' +
        '<span class="inv-layer-idx">' + (idx + 1) + '</span>' +
        '<div class="inv-layer-main">' +
          '<div class="inv-layer-title">' + FS.esc(l.name) +
            (cnt ? '<span class="chip">' + cnt + ' 项</span>' : '<span class="chip offline">空层</span>') +
          '</div>' +
          '<div class="inv-layer-meta">点击查看本层设置与格位规则</div>' +
        '</div>' +
        FS.icon('fwd') +
      '</div>';
    }).join('');

    return '<div class="inv-studio">' +
      '<div class="inv-studio-hint info">' + FS.icon('info') +
        '<span>「' + FS.esc(zone.name) + '」共 ' + layers.length + ' 层；点一层进入单独设置页</span></div>' +
      ui.scrollArea('<div class="inv-layer-list">' + (rows ||
        ui.emptyState({ icon: 'box', title: '这个分区还没有层', desc: '点击底栏「添加层」建立第一层' })) + '</div>') +
      '</div>';
  }

  /* ---- 渲染：指定层的设置（L3 入口的独立页面） ---- */
  function renderLayLayer(zoneId, layerId) {
    ready();
    ensureLayoutDraft();
    var zone = draftZone(zoneId);
    var layer = draftLayer(zoneId, layerId);
    if (!zone || !layer) {
      return '<div class="inv-studio">' +
        ui.emptyState({ icon: 'box', title: '未找到要编辑的层', desc: '返回上一页后重新选择' }) +
        '</div>';
    }
    var count = q.itemsIn({ zone: zone.id, layer: layer.id }).length;
    return '<div class="inv-studio">' +
      '<div class="inv-studio-hint info">' + FS.icon('info') +
        '<span>「' + FS.esc(layer.name) + '」使用固定 3×3 格位，一格只放一项食材</span></div>' +
      '<div class="inv-layer-detail">' +
        '<div class="inv-layer-detail-name">' + FS.esc(layer.name) + '</div>' +
        '<div class="inv-layer-detail-meta">' + FS.esc(zone.name) + ' · 当前 ' + count + ' 项食材</div>' +
        '<div class="inv-layer-detail-rule">' + FS.icon('grid') +
          '<span>格位规则：登记食材时必须选择一个空格</span></div>' +
      '</div>' +
      '</div>';
  }

  /* ---- 渲染：命名 / 类型面板（页面内子容器，不占全局浮层名额） ---- */
  function renderNamingPanel() {
    var n = layoutDraft.naming;
    if (!n) return '';
    var isZone = n.kind === 'zone';
    var target = isZone ? draftZone(n.zoneId) : draftLayer(n.zoneId, n.layerId);
    if (!target) return '';
    var presets = isZone ? ZONE_NAME_PRESETS : LAYER_NAME_PRESETS;

    var nameChips = presets.map(function (p) {
      return '<button class="inv-preset' + (p === target.name ? ' on' : '') + '"' +
        ' data-act="inv:lay-apply-name" data-arg="' + FS.esc(p) + '">' + FS.esc(p) + '</button>';
    }).join('');

    var typeRow = isZone
      ? '<div class="inv-name-label">分区类型</div>' +
        '<div class="inv-preset-row">' + ZONE_TYPE_PRESETS.map(function (t) {
          return '<button class="inv-preset' + (t === target.type ? ' on' : '') + '"' +
            ' data-act="inv:lay-apply-type" data-arg="' + FS.esc(t) + '">' +
            FS.icon(ui.zoneTypeIcon(t)) + FS.esc(t) + '</button>';
        }).join('') + '</div>'
      : '';

    return '<div class="inv-name-scrim" data-act="inv:lay-name-close"></div>' +
      '<div class="inv-name-panel">' +
        '<div class="inv-name-head">' +
          '<span class="inv-name-title">' + (isZone ? '分区设置' : '层设置') + '：' + FS.esc(target.name) + '</span>' +
          ui.iconBtn({ icon: 'x', label: '关闭', act: 'inv:lay-name-close' }) +
        '</div>' +
        '<div class="inv-name-label">常用名称（点一下即改）</div>' +
        '<div class="inv-preset-row">' + nameChips + '</div>' +
        typeRow +
        '<div class="inv-name-foot">' +
          ui.btn({ label: '用键盘输入', kind: 'ghost', icon: 'keyboard', act: 'inv:lay-name-kb' }) +
          '<div class="spacer"></div>' +
          ui.btn({ label: '完成', kind: 'primary', icon: 'check', act: 'inv:lay-name-close' }) +
        '</div>' +
      '</div>';
  }

  /* ---- 底栏：只保留删除与提交；具体形状操作在画布上完成 ---- */
  function layFoot(level, zoneId, layerId, backHash) {
    ensureLayoutDraft();
    var deleteAction = level === 'zones'
      ? (layoutDraft.selectedZoneId
        ? ui.btn({ label: '删除', kind: 'danger', icon: 'trash', act: 'inv:lay-del-zone' })
        : ui.btn({ label: '删除', kind: 'ghost', disabled: true }))
      : (level === 'layer'
        ? ui.btn({ label: '删除', kind: 'danger', icon: 'trash', act: 'inv:lay-del-layer', arg: zoneId + '|' + layerId })
        : ui.btn({ label: '删除', kind: 'ghost', disabled: true }));

    return '<div class="pg-foot inv-studio-foot">' +
      '<div class="inv-ctx-bar">' + deleteAction + '</div>' +
      '<div class="spacer"></div>' +
      ui.btn({ label: '提交', kind: 'warn', icon: 'check', act: 'inv:lay-submit' }) +
      '</div>';
  }

  FS.router.register({
    id: 'INV-08', name: '空间形状编辑', module: '库存', level: '管理页', ctx: 'CTX-02',
    states: [],
    parent: 'INV-01',
    gallery: [
      { label: '分区形状', hash: '#/p/INV-08?level=zones' },
      { label: '右冷藏 · 层结构', hash: '#/p/INV-08?level=layers&zone=right' },
      { label: '右冷藏 · 第2层设置', hash: '#/p/INV-08?level=layer&zone=right&layer=r2' }
    ],

    render: function (pg) {
      ready();
      ensureLayoutDraft();
      var level = pg.params.get('level');
      if (level !== 'layers' && level !== 'layer') level = 'zones';
      var zoneId = pg.params.get('zone') || null;
      var layerId = pg.params.get('layer') || null;
      var backHash = pg.params.get('back') || '#/p/INV-01';

      var zone = zoneId ? draftZone(zoneId) : null;
      var layer = zone && layerId ? draftLayer(zoneId, layerId) : null;
      var title = level === 'zones'
        ? '分区形状编辑'
        : (level === 'layers'
          ? (zone ? zone.name + ' · 层结构' : '层结构编辑')
          : (layer ? layer.name + ' · 层设置' : '层设置'));
      var sub = level === 'zones'
        ? '选择分区，在右侧调整它的宽度和高度'
        : (level === 'layers' ? '查看当前分区的层级' : '查看固定格位规则');
      if (layoutDraft.dirty) sub += ' · 未提交 ' + layoutDraft.changeCount + ' 处';

      var bodyInner = level === 'zones'
        ? renderLayZones()
        : (level === 'layers' ? renderLayLayers(zoneId, backHash) : renderLayLayer(zoneId, layerId));

      var badge = layoutDraft.dirty
        ? '<span class="chip warn inv-draft-chip">未提交 ' + layoutDraft.changeCount + '</span>'
        : '<span class="chip ok inv-draft-chip">已同步</span>';

      return '<div class="pg inv-studio-pg">' +
        ui.pageHead({
          title: title, id: 'INV-08', sub: sub,
          actions:
            badge +
            (layoutDraft.undoSnap
              ? ui.iconBtn({ icon: 'undo', label: '撤销一步', act: 'inv:lay-undo' })
              : '') +
            ui.iconBtn({ icon: 'back', label: '返回', act: 'inv:lay-back', arg: backHash })
        }) +
        '<div class="pg-body">' +
          bodyInner +
        '</div>' +
        layFoot(level, zoneId, layerId, backHash) +
        renderNamingPanel() +
      '</div>';
    }
  });

  /* ---------- 分区形状：点选 ---------- */
  FS.action('inv:lay-begin-new-zone', function () {
    ensureLayoutDraft();
    layoutDraft.zoneCreateMode = true;
    layoutDraft.selectedKeys = [];
    layoutDraft.selectedZoneId = null;
    refreshLay();
  });

  FS.action('inv:lay-cancel-new-zone', function () {
    ensureLayoutDraft();
    layoutDraft.zoneCreateMode = false;
    layoutDraft.selectedKeys = [];
    layoutDraft.selectedZoneId = null;
    refreshLay();
  });

  FS.action('inv:lay-tap-unit', function (el) {
    ensureLayoutDraft();
    FS.overlays.show('OV-01', { tone: 'info', text: '请先点一个分区，再在右侧选择宽度和高度' });
  });

  FS.action('inv:lay-tap-zone', function (el) {
    ensureLayoutDraft();
    var zone = draftZone(el.getAttribute('data-arg'));
    if (!zone) return;
    /* 再次点同一分区 = 直接取消选择，方便回看整张布局。 */
    if (layoutDraft.selectedZoneId === zone.id) {
      layoutDraft.selectedKeys = [];
      layoutDraft.selectedZoneId = null;
      layoutDraft.shape = null;
      refreshLay();
      return;
    }
    layoutDraft.zoneCreateMode = false;
    layoutDraft.selectedKeys = [];
    layoutDraft.selectedZoneId = zone.id;
    layoutDraft.shape = { w: zone.rect.w, h: zone.rect.h };
    refreshLay();
  });

  /* 点击空位中的“添加”区：立即生成一个 1×1 分区，并在右侧打开其编辑字段。 */
  FS.action('inv:lay-add-zone-at', function (el) {
    ready();
    ensureLayoutDraft();
    var point = parseUnitKey(el.getAttribute('data-arg'));
    if (zoneAtUnit(point.x, point.y)) return;
    pushUndo();
    var nid = 'z' + (layoutDraft.changeCount + 1) + '_' + point.x + point.y;
    layoutDraft.zones.push({
      id: nid,
      name: '新分区' + (layoutDraft.zones.length + 1),
      type: '冷藏',
      note: '可放食材',
      rect: { x: point.x, y: point.y, w: 1, h: 1 },
      layers: [{ id: nid + '_l1', name: '第1层', on: true, grid: false }]
    });
    layoutDraft.selectedKeys = [];
    layoutDraft.selectedZoneId = nid;
    layoutDraft.shape = { w: 1, h: 1 };
    layoutDraft.zoneCreateMode = false;
    markLayoutDirty();
    refreshLay();
  });

  FS.action('inv:lay-select-shape', function (el) {
    ensureLayoutDraft();
    var selected = draftZone(layoutDraft.selectedZoneId);
    if (!selected) {
      FS.overlays.show('OV-01', { tone: 'warn', text: '请先在画布上选择一个分区' });
      return;
    }
    var parts = (el.getAttribute('data-arg') || '').split('x');
    var shape = { w: parseInt(parts[0], 10), h: parseInt(parts[1], 10) };
    if (!shape.w || !shape.h || !canPlaceSelectedZone(selected.rect.x, selected.rect.y, shape)) {
      FS.overlays.show('OV-01', { tone: 'warn', text: '该尺寸会超出 3 列范围或压到其他分区' });
      return;
    }
    if (selected.rect.w === shape.w && selected.rect.h === shape.h) return;
    pushUndo();
    selected.rect = { x: selected.rect.x, y: selected.rect.y, w: shape.w, h: shape.h };
    layoutDraft.shape = shape;
    markLayoutDirty();
    refreshLay();
  });

  /* ver1 风格右侧表单：名称/备注直接写草稿，宽高滑条固定在 1–3，且不能撞到其它分区。 */
  FS.action('inv:lay-context-edit', function (el) {
    ensureLayoutDraft();
    var form = el.closest('.ctx-layout-form');
    var zone = draftZone(form && form.getAttribute('data-zone'));
    if (!zone) return;
    var field = el.getAttribute('data-layout-field');
    var nextValue = el.value;
    if (field === 'name' || field === 'note') {
      var value = field === 'name' ? nextValue.trim() : nextValue;
      if (field === 'name' && !value) value = '未命名分区';
      if ((zone[field] || '') === value) return;
      pushUndo();
      zone[field] = value;
      markLayoutDirty();
      refreshLay();
      return;
    }
    var shape = { w: zone.rect.w, h: zone.rect.h };
    if (field === 'width') shape.w = Math.max(1, Math.min(3, parseInt(nextValue, 10) || 1));
    if (field === 'height') shape.h = Math.max(1, Math.min(3, parseInt(nextValue, 10) || 1));
    if (!canPlaceSelectedZone(zone.rect.x, zone.rect.y, shape)) {
      FS.overlays.show('OV-01', { tone: 'warn', text: '该尺寸会超出 3 列范围或压到其他分区' });
      refreshLay();
      return;
    }
    if (zone.rect.w === shape.w && zone.rect.h === shape.h) return;
    pushUndo();
    zone.rect = { x: zone.rect.x, y: zone.rect.y, w: shape.w, h: shape.h };
    layoutDraft.shape = shape;
    markLayoutDirty();
    refreshLay();
  });

  /* 名称与备注在右侧编辑，软键盘停靠左侧主区底部，避免遮挡当前编辑上下文。 */
  FS.action('inv:lay-context-keyboard', function (el) {
    var field = el.getAttribute('data-layout-field');
    if (field !== 'name' && field !== 'note') return;
    var value = el.value || '';
    FS.overlays.show('OV-12', {
      tab: '键盘',
      preedit: value,
      cands: field === 'name'
        ? ['冷冻区', '冷藏区', '门架', '果蔬抽屉']
        : ['可放食材', '常用食材', '临期优先', '请保持整洁']
    });
  });

  FS.action('inv:lay-clear-sel', function () {
    layoutDraft.selectedKeys = [];
    layoutDraft.selectedZoneId = null;
    layoutDraft.shape = null;
    layoutDraft.zoneCreateMode = false;
    refreshLay();
  });

  FS.action('inv:lay-new-zone', function () {
    ready();
    ensureLayoutDraft();
    var r = unitRect(layoutDraft.selectedKeys);
    if (!r) {
      FS.overlays.show('OV-01', { tone: 'warn', text: '请先选中连续的空位矩形' });
      return;
    }
    for (var y = r.minY; y <= r.maxY; y++) {
      for (var x = r.minX; x <= r.maxX; x++) {
        if (zoneAtUnit(x, y)) {
          FS.overlays.show('OV-01', { tone: 'warn', text: '选区里已有分区' });
          return;
        }
      }
    }
    pushUndo();
    var nid = 'z' + layoutDraft.changeCount + '_' + r.minX + r.minY + r.w + r.h;
    var name = '新分区' + (layoutDraft.zones.length + 1);
    layoutDraft.zones.push({
      id: nid,
      name: name,
      type: '冷藏',
      rect: { x: r.minX, y: r.minY, w: r.w, h: r.h },
      layers: [{ id: nid + '_l1', name: '第1层', on: true, grid: false }]
    });
    layoutDraft.selectedKeys = [];
    layoutDraft.selectedZoneId = nid;
    layoutDraft.shape = { w: r.w, h: r.h };
    layoutDraft.zoneCreateMode = false;
    markLayoutDirty();
    /* 建完立刻进命名面板：新分区默认名没有意义，趁上下文还在改掉 */
    layoutDraft.naming = { kind: 'zone', zoneId: nid };
    refreshLay();
  });

  FS.action('inv:lay-split-zone', function () {
    ready();
    ensureLayoutDraft();
    var z = draftZone(layoutDraft.selectedZoneId);
    if (!z || !z.rect || z.rect.w * z.rect.h <= 1) {
      FS.overlays.show('OV-01', { tone: 'warn', text: '单格分区无需拆分' });
      return;
    }
    var cnt = q.itemsIn({ zone: z.id }).length;
    askConfirm(
      '把「' + z.name + '」拆成单格？',
      cnt
        ? '该分区含 ' + cnt + ' 项食材。拆分后只保留左上角一格，其余释放为空位；食材仍属于本分区，位置精度不变。'
        : '拆分后只保留左上角一格，其余释放为空位。',
      '拆分',
      function () {
        pushUndo();
        z.rect = { x: z.rect.x, y: z.rect.y, w: 1, h: 1 };
        markLayoutDirty();
        FS.overlays.show('OV-01', { tone: 'ok', text: '已拆成单格（草稿）' });
        refreshLay();
      }
    );
  });

  FS.action('inv:lay-del-zone', function () {
    ready();
    ensureLayoutDraft();
    var z = draftZone(layoutDraft.selectedZoneId);
    if (!z) return;
    var cnt = q.itemsIn({ zone: z.id }).length;
    if (cnt > 0) {
      /* 非空分区不能删：直接告诉去哪迁移，而不是只丢一句警告 */
      askConfirm(
        '「' + z.name + '」里还有 ' + cnt + ' 项食材',
        '删除分区前需要先把这些食材移到别处。要现在去看这个分区的库存吗？',
        '去查看',
        function () {
          FS.router.go('#/p/INV-01?zone=' + encodeURIComponent(z.id) + '&view=list');
        }
      );
      return;
    }
    askConfirm('删除分区「' + z.name + '」？', '这是个空分区，删除后它占的格子会释放为空位，提交后生效。', '删除', function () {
      pushUndo();
      layoutDraft.zones = layoutDraft.zones.filter(function (x) { return x.id !== z.id; });
      layoutDraft.selectedZoneId = null;
      markLayoutDirty();
      FS.overlays.show('OV-01', { tone: 'ok', text: '已删除分区（草稿）' });
      refreshLay();
    }, true);
  });

  /* ---------- 层结构 ---------- */
  FS.action('inv:lay-add-layer', function (el) {
    ensureLayoutDraft();
    var zoneId = el.getAttribute('data-arg') || FS.router.currentParams().get('zone');
    var zone = draftZone(zoneId);
    if (!zone) {
      FS.overlays.show('OV-01', { tone: 'warn', text: '请先选择一个分区' });
      return;
    }
    pushUndo();
    zone.layers = zone.layers || [];
    var n = zone.layers.length + 1;
    var lid = zone.id + '_l' + n + '_' + layoutDraft.changeCount;
    zone.layers.push({ id: lid, name: '第' + n + '层', on: true, grid: false });
    markLayoutDirty();
    FS.overlays.show('OV-01', { tone: 'ok', text: '已添加「第' + n + '层」（草稿）' });
    refreshLay();
  });

  FS.action('inv:lay-move', function (el) {
    ensureLayoutDraft();
    var parts = (el.getAttribute('data-arg') || '').split('|');
    if (parts.length < 3) return;
    var dir = parts[0], zone = draftZone(parts[1]);
    if (!zone || !zone.layers) return;
    var idx = zone.layers.findIndex(function (l) { return l.id === parts[2]; });
    if (idx < 0) return;
    var j = dir === 'up' ? idx - 1 : idx + 1;
    if (j < 0 || j >= zone.layers.length) return;   /* 边界按钮已禁用，这里只兜底 */
    pushUndo();
    var tmp = zone.layers[idx];
    zone.layers[idx] = zone.layers[j];
    zone.layers[j] = tmp;
    markLayoutDirty();
    refreshLay();
  });

  /* 「精确到格」开关：只影响登记时是否必须选格，不改九宫格形状 */
  FS.action('inv:lay-toggle-grid', function (el) {
    ensureLayoutDraft();
    var parts = (el.getAttribute('data-arg') || '').split('|');
    var layer = draftLayer(parts[0], parts[1]);
    if (!layer) return;
    pushUndo();
    layer.grid = !layer.grid;
    markLayoutDirty();
    FS.overlays.show('OV-01', {
      tone: 'ok',
      text: '「' + layer.name + '」' + (layer.grid ? '登记时需选到格' : '允许只记到层')
    });
    refreshLay();
  });

  FS.action('inv:lay-del-layer', function (el) {
    ready();
    ensureLayoutDraft();
    var parts = (el.getAttribute('data-arg') || '').split('|');
    var zoneId = parts[0], layerId = parts[1];
    var zone = draftZone(zoneId);
    var layer = draftLayer(zoneId, layerId);
    if (!zone || !layer) return;
    if ((zone.layers || []).length <= 1) {
      FS.overlays.show('OV-01', { tone: 'warn', text: '每个分区至少保留一层' });
      return;
    }
    var cnt = q.itemsIn({ zone: zoneId, layer: layerId }).length;
    if (cnt > 0) {
      askConfirm(
        '「' + layer.name + '」里还有 ' + cnt + ' 项食材',
        '删除这一层前需要先把食材移到别处。要现在去看这一层的库存吗？',
        '去查看',
        function () {
          FS.router.go('#/p/INV-01?zone=' + encodeURIComponent(zoneId) +
            '&layer=' + encodeURIComponent(layerId) + '&view=list');
        }
      );
      return;
    }
    askConfirm('删除「' + layer.name + '」？', '这是一个空层，提交后生效。', '删除', function () {
      pushUndo();
      zone.layers = (zone.layers || []).filter(function (l) { return l.id !== layerId; });
      markLayoutDirty();
      FS.overlays.show('OV-01', { tone: 'ok', text: '已删除空层（草稿）' });
      var p = FS.router.currentParams();
      var back = p && p.get ? (p.get('back') || '#/p/INV-01') : '#/p/INV-01';
      FS.router.go(studioHash('layers', zoneId, null, back));
    }, true);
  });

  /* ---------- 命名 / 类型面板 ---------- */
  FS.action('inv:lay-name-zone', function () {
    ensureLayoutDraft();
    if (!layoutDraft.selectedZoneId) return;
    layoutDraft.naming = { kind: 'zone', zoneId: layoutDraft.selectedZoneId };
    refreshLay();
  });

  FS.action('inv:lay-name-layer', function (el) {
    ensureLayoutDraft();
    var parts = (el.getAttribute('data-arg') || '').split('|');
    if (parts.length < 2) return;
    layoutDraft.naming = { kind: 'layer', zoneId: parts[0], layerId: parts[1] };
    refreshLay();
  });

  FS.action('inv:lay-name-close', function () {
    layoutDraft.naming = null;
    refreshLay();
  });

  FS.action('inv:lay-name-kb', function () {
    /* 预览里软键盘不回填文本；真机由 lv_keyboard 的 READY 事件写回草稿 */
    FS.overlays.show('OV-12');
  });

  FS.action('inv:lay-apply-name', function (el) {
    ensureLayoutDraft();
    var n = layoutDraft.naming;
    if (!n) return;
    var target = n.kind === 'zone' ? draftZone(n.zoneId) : draftLayer(n.zoneId, n.layerId);
    if (!target) return;
    var name = el.getAttribute('data-arg');
    if (!name || name === target.name) return;
    pushUndo();
    target.name = name;
    markLayoutDirty();
    refreshLay();
  });

  FS.action('inv:lay-apply-type', function (el) {
    ensureLayoutDraft();
    var n = layoutDraft.naming;
    if (!n || n.kind !== 'zone') return;
    var z = draftZone(n.zoneId);
    if (!z) return;
    var t = el.getAttribute('data-arg');
    if (!t || t === z.type) return;
    pushUndo();
    z.type = t;
    markLayoutDirty();
    refreshLay();
  });

  /* ---------- 提交 / 撤销 / 关闭 ---------- */
  FS.action('inv:lay-submit', function () {
    ensureLayoutDraft();
    if (!layoutDraft.dirty) {
      FS.overlays.show('OV-01', { tone: 'ok', text: '没有需要提交的改动' });
      return;
    }
    var imp = impactSummary();
    if (imp.warnings.length) {
      FS.overlays.show('OV-01', {
        tone: 'warn',
        text: '仍有 ' + imp.affected + ' 项食材需要先迁移，暂不能提交删除操作'
      });
      return;
    }
    var desc = '将写入 ' + layoutDraft.changeCount + ' 处形状修改。';
    desc += ' 未检测到需要迁移的食材。';
    askConfirm('提交空间形状修改？', desc, '提交', function () {
      FS.data.layout.zones = JSON.parse(JSON.stringify(layoutDraft.zones));
      layoutDraft.dirty = false;
      layoutDraft.changeCount = 0;
      layoutDraft.undoSnap = null;
      try { sessionStorage.removeItem('fs_layout_draft'); } catch (e) {}
      FS.overlays.show('OV-01', { tone: 'ok', text: '空间形状已提交' });
      refreshLay();
    });
  });

  FS.action('inv:lay-undo', function () {
    if (!layoutDraft.undoSnap) {
      FS.overlays.show('OV-01', { tone: 'warn', text: '没有可撤销的步骤' });
      return;
    }
    try {
      var s = JSON.parse(layoutDraft.undoSnap);
      layoutDraft.zones = s.zones;
      layoutDraft.selectedKeys = s.selectedKeys || [];
      layoutDraft.selectedZoneId = s.selectedZoneId || null;
      layoutDraft.shape = s.shape || null;
      layoutDraft.zoneCreateMode = !!s.zoneCreateMode;
      layoutDraft.undoSnap = null;
      layoutDraft.naming = null;
      markLayoutDirty(0);
      FS.overlays.show('OV-01', { tone: 'ok', text: '已撤销一步' });
      refreshLay();
    } catch (e) {
      layoutDraft.undoSnap = null;
    }
  });

  function resetLayoutDraft() {
    layoutDraft.ready = false;
    layoutDraft.zones = null;
    layoutDraft.dirty = false;
    layoutDraft.changeCount = 0;
    layoutDraft.selectedKeys = [];
    layoutDraft.selectedZoneId = null;
    layoutDraft.shape = null;
    layoutDraft.zoneCreateMode = false;
    layoutDraft.naming = null;
    layoutDraft.undoSnap = null;
  }

  function leaveLayoutStudio(targetHash) {
    if (layoutDraft.naming) { layoutDraft.naming = null; refreshLay(); return; }
    if (layoutDraft.dirty) {
      askConfirm('放弃未提交的形状修改？', '有 ' + layoutDraft.changeCount + ' 处改动尚未提交。', '放弃并返回', function () {
        resetLayoutDraft();
        FS.router.go(targetHash || '#/p/INV-01');
      }, true);
    } else {
      resetLayoutDraft();
      FS.router.go(targetHash || '#/p/INV-01');
    }
  }

  FS.action('inv:lay-back', function (el) {
    leaveLayoutStudio(el.getAttribute('data-arg') || '#/p/INV-01');
  });

  FS.action('inv:lay-close', function () {
    leaveLayoutStudio('#/p/INV-01');
  });

  /* ==================== 流程编排 ==================== */

  /* photo 拍照登记：INV-05 → 上传/识别进度 → INV-06 → toast */
  FS.flows.register('photo', {
    run: function (api) {
      var steps = ['上传照片', '云端识别', '等待确认'];
      api.go('#/p/INV-05');
      api.wait(1000).then(function () {
        if (api.cancelled()) return;
        api.show('OV-07', { step: 0, pct: 20, steps: steps });
        return api.wait(1200);
      }).then(function () {
        if (api.cancelled()) return;
        api.show('OV-07', { step: 1, pct: 62, steps: steps });
        return api.wait(1500);
      }).then(function () {
        if (api.cancelled()) return;
        api.close('OV-07');
        api.go('#/p/INV-06');
        api.toast('识别完成，3 项候选待确认');
      });
    }
  });

  /* voice 语音登记：OV-05 聆听 → OV-06 解析 → INV-04 草稿 */
  FS.flows.register('voice', {
    run: function (api) {
      api.show('OV-05');
      api.wait(2200).then(function () {
        if (api.cancelled()) return;
        api.close('OV-05');
        api.show('OV-06', { step: 1, steps: ['理解意图', '解析食材：菠菜×1把', '匹配位置：冷藏', '生成登记草稿'] });
        return api.wait(1800);
      }).then(function () {
        if (api.cancelled()) return;
        api.close('OV-06');
        api.go('#/p/INV-04?from=voice');
        api.toast('已解析语音，请确认登记信息');
      });
    }
  });
})();
