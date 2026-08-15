/**
 * 冰箱小精灵 ver2 UI 预览 —— 计划模块（PLAN-01..06）
 * ------------------------------------------------------------------
 * 覆盖"吃什么、何时吃、需要买什么"全链路：
 *   PLAN-01 计划总览（一级）：今日菜单 / 本周摘要 / 购物摘要 / 临期利用 / Agent 待采纳计划。
 *   PLAN-02 日/周菜单（二级）：日/周双视图 + 日期切换，含缺失食材冲突警告。
 *   PLAN-03 菜谱库（二级）：分类 seg + 搜索 + 菜谱卡网格。
 *   PLAN-04 菜谱详情（二级）：库存覆盖率 / 消耗与缺失 / 步骤摘要，底部固定操作条。
 *   PLAN-05 烹饪模式（任务页）：单步大字模式，可创建关联定时器并切侧舱到 CTX-07。
 *   PLAN-06 购物清单（二级）：待买/建议/已购三组 + 小程序同步状态。
 * 同时注册 cook 流程：PLAN-04 → PLAN-05 → 创建定时器 → OV-09 到时层。
 * 规则：PLAN-01 不放定时器（规范 7.1）；烹饪单步减干扰；实际消耗写回库存需 OV-03 确认。
 */
window.FS = window.FS || {};

(function () {
  var ui, q;
  function ready() { ui = FS.ui; q = FS.dataQ; }

  /* 餐次固定顺序（早/午/晚/加餐） */
  var MEALS = ['早餐', '午餐', '晚餐', '加餐'];

  /* 取当前菜谱（?recipe=r1，缺省 r1，兜底 r1） */
  function curRecipe(pg) {
    var id = pg.params.get('recipe') || 'r1';
    return q.recipe(id) || q.recipe('r1');
  }

  /* 当前选中的菜单日下标（?day=i，缺省=今天所在行） */
  function curDayIdx(pg) {
    var days = FS.data.menu.days;
    var def = 0;
    days.forEach(function (d, i) { if (d.today) def = i; });
    var v = parseInt(pg.params.get('day'), 10);
    if (isNaN(v) || v < 0 || v >= days.length) return def;
    return v;
  }

  /* ==================== PLAN-01 计划总览（一级页） ==================== */
  FS.router.register({
    id: 'PLAN-01', name: '计划总览', module: '计划', level: '一级页', ctx: 'CTX-06',
    states: ['loading', 'empty', 'offline', 'error'],

    render: function (pg) {
      ready();
      var d = FS.data;
      var menu = d.menu;
      var todayIdx = curDayIdx(pg);
      var today = menu.days[todayIdx];

      /* ---- 今日菜单：早/午/晚/加餐 纵向列表（一行一餐，避免 2×2 宫格） ---- */
      var slots = MEALS.map(function (m) {
        var dish = today.meals[m] || '';
        return '<div class="plan-slot' + (dish ? '' : ' empty') + '">' +
          '<div class="plan-slot-meal">' + m + '</div>' +
          '<div class="plan-slot-dish">' + (dish ? FS.esc(dish) : '<span class="plan-slot-none">未安排</span>') + '</div>' +
          '</div>';
      }).join('');
      var todayCard = ui.card({
        title: '今日菜单',
        side: '<a data-act="nav" data-href="#/p/PLAN-02">' + today.date + ' · 编辑 ›</a>',
        body: '<div class="plan-today-grid plan-today-list">' + slots + '</div>',
        cls: 'plan-today'
      });

      /* ---- 本周菜单摘要条（7 天 mini 条，冲突天标红点） ---- */
      var miniDays = menu.days.map(function (day, i) {
        var wd = day.date.split(' ')[0];
        return '<button class="plan-mini-day' + (day.today ? ' today' : '') + (i === todayIdx ? ' cur' : '') + '"' +
          ' data-act="nav" data-href="#/p/PLAN-02?day=' + i + '">' +
          '<span class="plan-mini-wd">' + wd + '</span>' +
          '<span class="plan-mini-dish">' + FS.esc(day.meals['晚餐'] || '—') + '</span>' +
          (day.conflict ? '<span class="plan-dot" title="有缺失食材"></span>' : '') +
          '</button>';
      }).join('');
      var weekCard = ui.card({
        title: '本周菜单',
        side: menu.week,
        body: '<div class="plan-week-strip">' + miniDays + '</div>',
        cls: 'plan-week'
      });

      /* ---- 购物清单摘要 ---- */
      var pending = d.shopping.filter(function (s) { return s.group === '待购买'; });
      var suggest = d.shopping.filter(function (s) { return s.group === '建议购买'; });
      var shopCard = ui.card({
        title: '购物清单',
        side: '<a data-act="nav" data-href="#/p/PLAN-06">全部 ›</a>',
        body: '<div class="plan-shop-sum" data-act="nav" data-href="#/p/PLAN-06">' +
          ui.statCard({ icon: 'cart', value: pending.length, label: '待购买', tone: 'warn' }) +
          ui.statCard({ icon: 'info', value: suggest.length, label: '建议购买' }) +
          '</div>',
        cls: 'plan-shop'
      });

      /* ---- 临期利用计划卡 ---- */
      var useRows =
        ui.listRow({ icon: 'food', title: '菠菜 · 明天到期', sub: '已排入今晚「菠菜鸡蛋汤」', side: ui.chip('已安排', 'ok'), act: 'nav', href: '#/p/PLAN-04?recipe=r2', arrow: false }) +
        ui.listRow({ icon: 'food', title: '卤牛肉 · 今天到期', sub: '已排入今天午餐「卤牛肉拌面」', side: ui.chip('缺料', 'danger'), act: 'nav', href: '#/p/PLAN-04?recipe=r4', arrow: false });
      var expireCard = ui.card({
        title: '临期利用计划',
        side: '优先消耗',
        body: useRows,
        cls: 'plan-expire'
      });

      /* ---- Agent 待采纳计划卡 ---- */
      var agentCard = '<div class="plan-agent-card">' +
        '<span class="plan-agent-icon">' + FS.icon('zap') + '</span>' +
        '<div class="plan-agent-text">' +
          '<div class="plan-agent-title">Agent 生成了周四菜单，是否采纳？</div>' +
          '<div class="plan-agent-sub">结合现有库存与临期食材，预计可减少 2 项浪费</div>' +
        '</div>' +
        '<div class="plan-agent-btns">' +
          ui.btn({ label: '查看', kind: 'ghost', size: 'sm', act: 'nav', href: '#/p/PLAN-02?day=3' }) +
          ui.btn({ label: '采纳', kind: 'primary', size: 'sm', act: 'plan:adopt-menu' }) +
        '</div>' +
        ui.iconBtn({ icon: 'x', label: '忽略', act: 'plan:dismiss-agent' }) +
        '</div>';

      var cols1 = '<div class="plan-cols-2">' + todayCard + '<div class="plan-col-side">' + shopCard + expireCard + '</div></div>';

      /* 包进 scroll-area：竖屏主区有限，避免本周菜单被导航/状态舱切腰 */
      var asyncHtml = '<div class="scroll-area plan-scroll">' + cols1 + weekCard + agentCard + '</div>';

      return '<div class="pg plan-overview">' +
        ui.pageHead({
          title: '计划', id: 'PLAN-01',
          sub: d.device.date + ' · 本周已排 5 天晚餐',
          actions: ui.iconBtn({ icon: 'search', label: '搜索菜谱', act: 'nav', href: '#/p/PLAN-03' }) +
                   ui.iconBtn({ icon: 'cart', label: '购物清单', act: 'nav', href: '#/p/PLAN-06' })
        }) +
        '<div class="pg-body plan-body">' + FS.states.wrap(pg, asyncHtml, {
          emptyTitle: '还没有任何计划',
          emptyDesc: '让 Agent 根据库存生成一周菜单吧',
          emptyAction: ui.btn({ label: '生成菜单', act: 'nav', href: '#/p/CHAT-01' })
        }) + '</div></div>';
    }
  });

  /* ==================== PLAN-02 日/周菜单（二级页） ==================== */
  FS.router.register({
    id: 'PLAN-02', name: '日/周菜单', module: '计划', level: '二级页', ctx: 'CTX-06',
    states: ['loading', 'error'],
    parent: 'PLAN-01',

    render: function (pg) {
      ready();
      var menu = FS.data.menu;
      var view = pg.params.get('view') || 'day';
      var dayIdx = curDayIdx(pg);
      var day = menu.days[dayIdx];

      /* ---- 顶部：日/周 seg + 日期切换条 ---- */
      var segBar = '<div class="plan-menu-bar">' +
        ui.seg({
          act: 'plan:menu-view', value: view,
          options: [{ value: 'day', label: '日视图' }, { value: 'week', label: '周视图' }]
        }) +
        '<div class="plan-day-chips">' +
        menu.days.map(function (dd, i) {
          var wd = dd.date.split(' ')[0];
          return '<button class="plan-day-chip' + (i === dayIdx ? ' on' : '') + (dd.today ? ' today' : '') + '"' +
            ' data-act="plan:menu-day" data-arg="' + i + '">' + wd +
            (dd.conflict ? '<span class="plan-dot"></span>' : '') + '</button>';
        }).join('') +
        '</div></div>';

      var bodyHtml;
      if (view === 'week') {
        /* ---- 周视图：7×4 紧凑表格 ---- */
        var head = '<div class="plan-wt-row plan-wt-head"><div class="plan-wt-corner">餐次</div>' +
          menu.days.map(function (dd) {
            return '<div class="plan-wt-dayh' + (dd.today ? ' today' : '') + '">' + dd.date.split(' ')[0] +
              (dd.conflict ? '<span class="plan-dot"></span>' : '') + '</div>';
          }).join('') + '</div>';
        var rows = MEALS.map(function (m) {
          return '<div class="plan-wt-row"><div class="plan-wt-meal">' + m + '</div>' +
            menu.days.map(function (dd, i) {
              var dish = dd.meals[m] || '';
              return '<button class="plan-wt-cell' + (dish ? '' : ' empty') + (dd.today ? ' today-col' : '') + '"' +
                ' data-act="plan:menu-day" data-arg="' + i + '" title="' + FS.esc(dd.date + ' ' + m) + '">' +
                (dish ? FS.esc(dish) : '<span class="plan-wt-none">—</span>') + '</button>';
            }).join('') + '</div>';
        }).join('');
        bodyHtml = '<div class="plan-week-table">' + head + rows + '</div>' +
          '<div class="plan-table-hint">' + FS.icon('info') + '点击单元格进入对应日期的日视图；红点表示该天有缺失食材冲突。</div>';
      } else {
        /* ---- 日视图：当天 4 个餐次大卡 ---- */
        bodyHtml = '<div class="plan-day-grid">' +
          MEALS.map(function (m) {
            var dish = day.meals[m] || '';
            var conflictLine = '';
            /* 演示：仅给含缺失食材的菜谱挂冲突警告行 */
            if (m === '午餐' && dish === '香煎鸡胸肉') {
              conflictLine = '<div class="plan-conflict">' + FS.icon('warn') + '缺：黑胡椒 <a data-act="plan:missing-shop" data-arg="黑胡椒">加入购物清单</a></div>';
            } else if (m === '午餐' && dish === '卤牛肉拌面') {
              conflictLine = '<div class="plan-conflict">' + FS.icon('warn') + '缺：面条、青菜 <a data-act="plan:missing-shop" data-arg="面条">加入购物清单</a></div>';
            } else if (m === '早餐' && dish.indexOf('需购买') >= 0) {
              conflictLine = '<div class="plan-conflict">' + FS.icon('warn') + '缺：吐司面包 <a data-act="plan:missing-shop" data-arg="吐司面包">加入购物清单</a></div>';
            }
            if (!dish) {
              return '<div class="plan-meal-card empty">' +
                '<div class="plan-meal-head"><span class="plan-meal-name">' + m + '</span></div>' +
                '<div class="plan-meal-none">未安排</div>' +
                '<div class="plan-meal-ops">' + ui.btn({ label: '安排菜谱', kind: 'ghost', size: 'sm', act: 'nav', href: '#/p/PLAN-03' }) + '</div>' +
                '</div>';
            }
            return '<div class="plan-meal-card">' +
              '<div class="plan-meal-head"><span class="plan-meal-name">' + m + '</span>' +
                '<span class="plan-meal-time">' + FS.icon('clock') + '约 15 分钟</span></div>' +
              '<div class="plan-meal-dish">' + FS.esc(dish) + '</div>' +
              '<div class="plan-meal-use">消耗：番茄×2、鸡蛋×3</div>' +
              conflictLine +
              '<div class="plan-meal-ops">' +
                ui.btn({ label: '换一道', kind: 'ghost', size: 'sm', act: 'nav', href: '#/p/PLAN-03' }) +
                ui.btn({ label: '移到其他日期', kind: 'ghost', size: 'sm', act: 'plan:move-meal' }) +
                ui.btn({ label: '缺料入清单', kind: 'ghost', size: 'sm', act: 'plan:missing-shop', arg: dish }) +
              '</div>' +
              '</div>';
          }).join('') +
          '</div>';
      }

      var asyncHtml = segBar + bodyHtml;

      return '<div class="pg">' +
        ui.pageHead({
          title: '日/周菜单', id: 'PLAN-02',
          sub: menu.week + ' · 本周 2 天有缺失食材',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' }) +
                   ui.iconBtn({ icon: 'plus', label: '让 Agent 排餐', act: 'nav', href: '#/p/CHAT-01' })
        }) +
        '<div class="pg-body">' + FS.states.wrap(pg, asyncHtml) + '</div></div>';
    }
  });

  /* ==================== PLAN-03 菜谱库（二级页） ==================== */
  FS.router.register({
    id: 'PLAN-03', name: '菜谱库', module: '计划', level: '二级页', ctx: 'CTX-06',
    states: ['loading', 'empty', 'error'],
    parent: 'PLAN-01',

    render: function (pg) {
      ready();
      var cat = pg.params.get('cat') || 'recent';
      var kw = pg.params.get('q') || '';

      /* 分类 seg：最近生成/收藏/高库存匹配/临期优先 */
      var segBar = '<div class="plan-recipe-bar">' +
        ui.seg({
          act: 'plan:recipe-cat', value: cat,
          options: [
            { value: 'recent', label: '最近生成' },
            { value: 'fav', label: '收藏' },
            { value: 'stock', label: '高库存匹配' },
            { value: 'expire', label: '临期优先' }
          ]
        }) +
        '<button class="plan-search" data-act="plan:recipe-search">' +
          FS.icon('search') + '<span class="' + (kw ? '' : 'ph') + '">' + (kw ? FS.esc(kw) : '搜索菜谱…') + '</span></button>' +
        '</div>';

      /* 依分类 + 关键词过滤（不新造数据，仅重排/筛选 FS.data.recipes） */
      var list = FS.data.recipes.slice();
      if (cat === 'fav') list = list.filter(function (r) { return r.fav; });
      else if (cat === 'stock') list = list.sort(function (a, b) { return b.coverage - a.coverage; });
      else if (cat === 'expire') list = list.filter(function (r) { return /菠菜|番茄|卤牛肉/.test(r.use.join('')); });
      if (kw) list = list.filter(function (r) { return r.name.indexOf(kw) >= 0; });

      var grid = list.length
        ? '<div class="plan-recipe-grid">' + list.map(function (r) { return ui.recipeCard(r, { act: 'plan:recipe' }); }).join('') + '</div>'
        : ui.emptyState({
            icon: 'recipe',
            title: '还没有菜谱，让 Agent 生成一个吧',
            desc: '根据冰箱现有库存与临期食材，一键生成可执行的菜谱',
            action: ui.btn({ label: '让 Agent 生成', act: 'nav', href: '#/p/CHAT-01' })
          });

      var asyncHtml = segBar + grid;

      return '<div class="pg">' +
        ui.pageHead({
          title: '菜谱库', id: 'PLAN-03',
          sub: '共 ' + FS.data.recipes.length + ' 个菜谱 · 数据来自生成与收藏',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' }) +
                   ui.iconBtn({ icon: 'plus', label: '生成菜谱', act: 'nav', href: '#/p/CHAT-01' })
        }) +
        '<div class="pg-body">' + FS.states.wrap(pg, asyncHtml, {
          emptyTitle: '还没有菜谱，让 Agent 生成一个吧',
          emptyDesc: '根据冰箱现有库存与临期食材，一键生成可执行的菜谱',
          emptyAction: ui.btn({ label: '让 Agent 生成', act: 'nav', href: '#/p/CHAT-01' })
        }) + '</div></div>';
    }
  });

  /* ==================== PLAN-04 菜谱详情（二级页） ==================== */
  FS.router.register({
    id: 'PLAN-04', name: '菜谱详情', module: '计划', level: '二级页', ctx: 'CTX-06',
    states: ['loading', 'error'],
    parent: 'PLAN-03',

    render: function (pg) {
      ready();
      var r = curRecipe(pg);

      /* 头部：大图占位 + 元信息 + 库存覆盖率 */
      var head = '<div class="plan-detail-head">' +
        ui.imgPh({ w: 200, h: 200, icon: 'food', label: r.name.slice(0, 2), cls: 'plan-detail-img' }) +
        '<div class="plan-detail-meta">' +
          '<div class="plan-detail-name">' + FS.esc(r.name) +
            (r.fav ? '<span class="rcp-fav">' + FS.icon('star') + '</span>' : '') + '</div>' +
          '<div class="plan-detail-tags">' +
            ui.chip(r.servings, '') + ui.chip(r.time, 'info') + ui.chip(r.difficulty, r.difficulty === '简单' ? 'ok' : 'warn') +
          '</div>' +
          '<div class="plan-coverage"><span class="plan-coverage-label">库存覆盖率 ' + r.coverage + '%</span>' +
            ui.progress(r.coverage, r.coverage >= 90 ? 'ok' : r.coverage >= 70 ? 'warn' : 'danger') + '</div>' +
          (r.missing.length
            ? '<div class="plan-conflict">' + FS.icon('warn') + '缺：' + FS.esc(r.missing.join('、')) +
                ' <a data-act="plan:missing-shop" data-arg="' + FS.esc(r.missing[0]) + '">加入购物清单</a></div>'
            : '<div class="plan-ok-line">' + FS.icon('check') + '食材齐全，可直接开始</div>') +
        '</div></div>';

      /* 将消耗的库存（foodRow 风格，可点进 INV-03） */
      var useItems = q.itemsIn({}).filter(function (it) {
        return r.use.some(function (u) { return u.indexOf(it.name) >= 0; });
      }).slice(0, 3);
      var useRows = useItems.length
        ? useItems.map(function (it) { return ui.foodRow(it, { act: 'inv:detail' }); }).join('')
        : '<div class="plan-sub-line">本菜谱食材均需另行采购</div>';
      var useCard = ui.card({ title: '将消耗的库存', side: '点击进详情', body: useRows, cls: 'plan-use' });

      /* 步骤摘要（编号列表） */
      var steps = r.steps.map(function (s, i) {
        return '<div class="plan-step-row"><span class="plan-step-no">' + (i + 1) + '</span>' +
          '<span class="plan-step-text">' + FS.esc(s) + '</span></div>';
      }).join('');
      var stepCard = ui.card({ title: '步骤摘要', side: '共 ' + r.steps.length + ' 步', body: '<div class="plan-steps">' + steps + '</div>', cls: 'plan-steps-card' });

      /* 替代方案与备注 */
      var noteCard = ui.card({
        title: '替代方案与备注', cls: 'plan-note',
        body: '<div class="plan-sub-line">· 缺黑胡椒可用少许生抽+姜替代（香煎类）。</div>' +
              '<div class="plan-sub-line">· 减脂需求可将食用油减半，用不粘锅小火。</div>' +
              '<div class="plan-sub-line">· 份量按 ' + FS.esc(r.servings) + ' 设计，可底部「调整份量」重新配比。</div>'
      });

      var asyncHtml = head + '<div class="plan-detail-cols">' + useCard + stepCard + '</div>' + noteCard;

      return '<div class="pg">' +
        ui.pageHead({
          title: '菜谱详情', id: 'PLAN-04',
          sub: FS.esc(r.name) + ' · 库存覆盖 ' + r.coverage + '%',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body">' + FS.states.wrap(pg, asyncHtml) + '</div>' +
        /* 底部固定操作条 */
        '<div class="pg-foot">' +
          ui.btn({ label: '开始烹饪', kind: 'primary', size: 'big', icon: 'play', act: 'nav', href: '#/p/PLAN-05?recipe=' + r.id }) +
          ui.btn({ label: '加入菜单', kind: 'ghost', act: 'nav', href: '#/p/PLAN-02' }) +
          '<span class="spacer"></span>' +
          ui.btn({ label: '调整份量', kind: 'ghost', act: 'plan:adjust-servings' }) +
          ui.btn({ label: '和 Agent 讨论', kind: 'ghost', act: 'nav', href: '#/p/CHAT-01' }) +
        '</div></div>';
    }
  });

  /* ==================== PLAN-05 烹饪模式（任务页） ==================== */
  FS.router.register({
    id: 'PLAN-05', name: '烹饪模式', module: '计划', level: '任务页', ctx: 'CTX-06',
    states: [],
    parent: function (pg) { return '#/p/PLAN-04?recipe=' + encodeURIComponent(pg.params.get('recipe') || 'r1'); },

    render: function (pg) {
      ready();
      var r = curRecipe(pg);
      /* 演示锚定第 3 步（与 CTX-06 / 流程一致） */
      var stepIdx = Math.min(2, r.steps.length - 1);
      var stepNo = stepIdx + 1;
      var total = r.steps.length;
      var pct = Math.round((stepNo / total) * 100);
      var stepText = r.steps[stepIdx];

      /* 本步所需食材（演示：取菜谱 use 前 2 项） */
      var ingRow = '<div class="plan-cook-ings">' +
        r.use.slice(0, 2).map(function (u) { return ui.chip(u, 'info', 'food'); }).join('') + '</div>';

      var body =
        '<div class="plan-cook-top">' +
          '<div class="plan-cook-progress-info"><span>第 ' + stepNo + ' / ' + total + ' 步</span><span>' + pct + '%</span></div>' +
          ui.progress(pct, 'ok') +
        '</div>' +
        '<div class="plan-cook-stage">' +
          '<div class="plan-cook-no">' + stepNo + '</div>' +
          '<div class="plan-cook-text">' + FS.esc(stepText) + '</div>' +
          ingRow +
          '<button class="plan-cook-timerbtn" data-act="plan:create-timer">' + FS.icon('timer') +
            '<span>创建关联定时器</span></button>' +
          '<div class="plan-cook-hint">' + FS.icon('mic') + '说「下一步」「上一步」可语音控制</div>' +
        '</div>';

      return '<div class="pg plan-cook">' +
        ui.pageHead({
          title: '烹饪模式', id: 'PLAN-05',
          sub: FS.esc(r.name) + ' · 单步模式，减少干扰',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body">' + body + '</div>' +
        '<div class="pg-foot plan-cook-foot">' +
          ui.btn({ label: '上一步', kind: 'ghost', size: 'big', icon: 'back', act: 'plan:cook-prev', disabled: stepIdx === 0 }) +
          ui.btn({ label: '下一步', kind: 'primary', size: 'big', icon: 'fwd', act: 'plan:cook-next' }) +
          '<span class="spacer"></span>' +
          ui.btn({ label: '暂停', kind: 'ghost', act: 'plan:cook-pause' }) +
          ui.btn({ label: '结束烹饪', kind: 'danger', act: 'plan:cook-finish' }) +
        '</div></div>';
    }
  });

  /* ==================== PLAN-06 购物清单（二级页） ==================== */
  FS.router.register({
    id: 'PLAN-06', name: '购物清单', module: '计划', level: '二级页', ctx: 'CTX-06',
    states: ['loading', 'empty', 'offline', 'error'],
    parent: 'PLAN-01',

    render: function (pg) {
      ready();
      var list = FS.data.shopping;
      var sortMode = pg.params.get('sort') || 'default';

      function rowOf(s) {
        var bought = s.group === '已购买';
        var right;
        if (bought) right = ui.btn({ label: '转为入库', kind: 'ghost', size: 'sm', act: 'plan:shop-to-inv', arg: s.id });
        else if (s.group === '建议购买') right = ui.btn({ label: '加入待买', kind: 'primary', size: 'sm', act: 'plan:shop-add-pending', arg: s.id });
        else right = '';
        return '<div class="plan-shop-row' + (bought ? ' bought' : '') + '">' +
          '<button class="plan-check' + (s.checked ? ' on' : '') + '" data-act="plan:shop-toggle" data-arg="' + s.id + '" role="checkbox" aria-checked="' + !!s.checked + '">' +
            (s.checked ? FS.icon('check') : '') + '</button>' +
          '<div class="plan-shop-main"><div class="plan-shop-name">' + FS.esc(s.name) + '</div>' +
            '<div class="plan-shop-sub">' + FS.esc(s.qty) + FS.esc(s.unit || '') + ' · 关联：' + (s.source === '菜谱缺失' ? '周三菜单' : s.source === '菜单计划' ? '本周菜单' : '—') + '</div></div>' +
          ui.chip(s.source, s.source === '菜谱缺失' ? 'warn' : s.source === '低库存' ? 'info' : '') +
          '<div class="plan-shop-side">' + right + '</div>' +
          '</div>';
      }

      var pending = list.filter(function (s) { return s.group === '待购买'; });
      var suggest = list.filter(function (s) { return s.group === '建议购买'; });
      var bought = list.filter(function (s) { return s.group === '已购买'; });

      /* 排序 seg 仅作演示（不改变数据本身） */
      var sortBar = '<div class="plan-shop-sort">' +
        '<span class="plan-sort-label">排序</span>' +
        ui.seg({
          act: 'plan:shop-sort', value: sortMode,
          options: [{ value: 'default', label: '默认' }, { value: 'cat', label: '按分类' }, { value: 'route', label: '按超市动线' }]
        }) + '</div>';

      var groups =
        ui.sectionTitle('待购买（' + pending.length + '）') +
        '<div class="plan-shop-group">' + (pending.map(rowOf).join('') || '<div class="plan-sub-line">没有待买物品</div>') + '</div>' +
        ui.sectionTitle('建议购买（' + suggest.length + '）', 'Agent / 低库存建议') +
        '<div class="plan-shop-group">' + (suggest.map(rowOf).join('') || '<div class="plan-sub-line">暂无建议</div>') + '</div>' +
        ui.sectionTitle('已购买（' + bought.length + '）') +
        '<div class="plan-shop-group">' + (bought.map(rowOf).join('') || '<div class="plan-sub-line">还没有已完成的物品</div>') + '</div>';

      var syncLine = '<div class="plan-sync-line">' + ui.statusDot('ok') +
        '<span class="sd-label">已与小程序同步 · 今天 12:30</span></div>';

      var asyncHtml = syncLine + sortBar + groups;

      return '<div class="pg">' +
        ui.pageHead({
          title: '购物清单', id: 'PLAN-06',
          sub: pending.length + ' 项待买 · 与小程序实时同步',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body">' + FS.states.wrap(pg, asyncHtml, {
          emptyTitle: '购物清单是空的',
          emptyDesc: '缺料、低库存或手动添加的物品会显示在这里',
          emptyAction: ui.btn({ label: '添加物品', act: 'plan:shop-add' })
        }) + '</div>' +
        '<div class="pg-foot">' +
          ui.btn({ label: '添加物品', kind: 'primary', icon: 'plus', act: 'plan:shop-add' }) +
          '<span class="spacer"></span>' +
          ui.btn({ label: '清空已完成', kind: 'ghost', act: 'plan:shop-clear-done' }) +
        '</div></div>';
    }
  });

  /* ==================== 模块内动作 ==================== */

  /* PLAN-01：采纳 / 忽略 Agent 计划 */
  FS.action('plan:adopt-menu', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已采纳周四菜单（演示）' });
  });
  FS.action('plan:dismiss-agent', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已忽略该计划（演示）' });
  });

  /* PLAN-02：日/周视图与日期切换（写回 hash 参数） */
  FS.action('plan:menu-view', function (el) {
    var p = FS.router.currentParams();
    FS.router.goPage('PLAN-02', { view: el.getAttribute('data-arg'), day: p.get('day') || String(curDayIdx({ params: p })) });
  });
  FS.action('plan:menu-day', function (el) {
    var p = FS.router.currentParams();
    var view = p.get('view') || 'day';
    /* 周视图点单元格 → 进入该日日视图 */
    FS.router.goPage('PLAN-02', { view: view === 'week' ? 'day' : view, day: el.getAttribute('data-arg') });
  });
  FS.action('plan:move-meal', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：选择目标日期（OV-12 日期选择）' });
  });
  FS.action('plan:missing-shop', function (el) {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已把「' + el.getAttribute('data-arg') + '」加入购物清单（演示）' });
  });

  /* PLAN-03：分类 / 搜索 */
  FS.action('plan:recipe-cat', function (el) {
    FS.router.goPage('PLAN-03', { cat: el.getAttribute('data-arg') });
  });
  FS.action('plan:recipe-search', function () {
    /* 演示：弹软键盘后按关键词「蛋」过滤 */
    FS.overlays.show('OV-12', { tab: '键盘', preedit: 'dan', cands: ['蛋', '番茄炒蛋', '虾仁滑蛋', '菠菜鸡蛋汤', '蛋糕'] });
  });

  /* PLAN-04：调整份量 */
  FS.action('plan:adjust-servings', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：份量调整为 3 人份，用量已重算' });
  });

  /* PLAN-05：烹饪控制 */
  FS.action('plan:cook-prev', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：上一步' });
  });
  FS.action('plan:cook-next', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '演示：下一步' });
  });
  FS.action('plan:cook-pause', function () {
    FS.overlays.show('OV-01', { tone: 'warn', text: '演示：已暂停，步骤进度保留' });
  });
  /* 创建关联定时器：toast + 侧舱切到 CTX-07（定时器情境） */
  FS.action('plan:create-timer', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已创建定时器 煮蛋 10:00' });
    if (FS.cabin && FS.cabin.setMode) {
      FS.cabin.setMode('CTX-07');
      var sel = document.getElementById('tb-ctx');
      if (sel) sel.value = 'CTX-07';
    }
    /* 提供恢复自动跟随的提示（侧舱平时为 auto） */
    setTimeout(function () {
      FS.overlays.show('OV-01', { tone: 'ok', text: '侧舱已切到定时器；工具栏「侧舱 → 自动跟随」可恢复' });
    }, 2600);
  });
  /* 结束烹饪：弹 OV-03 确认实际消耗写回库存 */
  FS.action('plan:cook-finish', function () {
    FS.overlays.show('OV-03', {
      title: '结束并记录实际消耗？',
      desc: '将按菜谱用量把番茄×2、鸡蛋×3 从库存中扣减，写回库存。此操作需要确认。',
      ok: '结束并写回', danger: false
    });
  });

  /* PLAN-06：购物清单操作 */
  FS.action('plan:shop-toggle', function (el) {
    var s = FS.data.shopping.find(function (x) { return x.id === el.getAttribute('data-arg'); });
    if (s) s.checked = !s.checked;
    FS.shell.renderPage();
  });
  FS.action('plan:shop-add-pending', function (el) {
    var s = FS.data.shopping.find(function (x) { return x.id === el.getAttribute('data-arg'); });
    if (s) { s.group = '待购买'; s.checked = false; }
    FS.shell.renderPage();
    FS.overlays.show('OV-01', { tone: 'ok', text: '已加入待购买' });
  });
  FS.action('plan:shop-to-inv', function (el) {
    FS.router.go('#/p/INV-04?from=' + encodeURIComponent(el.getAttribute('data-arg')));
  });
  FS.action('plan:shop-sort', function (el) {
    FS.router.goPage('PLAN-06', { sort: el.getAttribute('data-arg') });
  });
  FS.action('plan:shop-add', function () {
    /* 添加物品：弹 OV-12 软键盘输入名称 */
    FS.overlays.show('OV-12', { tab: '键盘', preedit: 'mian tiao', cands: ['面条', '面包', '米饭', '面粉', '馒头'] });
  });
  FS.action('plan:shop-clear-done', function () {
    FS.overlays.show('OV-03', {
      title: '清空已完成物品？',
      desc: '将把「已购买」分组的 1 项从清单中移除，此操作不可恢复。',
      ok: '清空', danger: true
    });
  });

  /* ==================== 流程：烹饪 + 计时到时 ==================== */
  FS.flows.register('cook', {
    run: function (api) {
      (async function () {
        api.go('#/p/PLAN-04?recipe=r1');
        await api.wait(1200); if (api.cancelled()) return;
        api.go('#/p/PLAN-05?recipe=r1');
        api.toast('已创建关联定时器 煮蛋 10:00');
        await api.wait(2600); if (api.cancelled()) return;
        api.show('OV-09', { name: '煮蛋', src: '菜谱·番茄炒蛋' });
      })();
    }
  });
})();
