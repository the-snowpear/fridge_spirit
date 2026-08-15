/**
 * 冰箱小精灵 ver2 UI 预览 —— 首页模块（HOME-01 / HOME-02）
 * ------------------------------------------------------------------
 * HOME-01 首页（一级）：一屏回答"今天有什么值得处理 / 吃什么 / 系统是否正常"：
 *   库存与临期统计、临期 Top3、今日推荐、菜单/购物摘要、Agent 主动建议、最近同步。
 * HOME-02 今日摘要详情（二级）：临期/过期分组、推荐菜与理由、计划缺料、
 *   今日出入库流水、Agent 可执行建议。
 * 本文件同时是其他页面模块的参考实现（注册结构 / builder 用法 / 状态包裹）。
 */
window.FS = window.FS || {};

(function () {
  var ui, q;
  function ready() { ui = FS.ui; q = FS.dataQ; }

  /* ==================== HOME-01 首页 ==================== */
  FS.router.register({
    id: 'HOME-01', name: '首页', module: '首页', level: '一级页', ctx: 'CTX-01',
    states: ['loading', 'empty', 'offline', 'error'],

    render: function (pg) {
      ready();
      var d = FS.data;
      var expiring = q.expiring(3);
      var expired = q.expired();
      var unlocated = q.unlocated();

      /* 统计卡行 */
      var stats = '<div class="home-stats">' +
        ui.statCard({ icon: 'box', value: d.items.length, label: '库存食材', act: 'nav', href: '#/p/INV-01' }) +
        ui.statCard({ icon: 'clock', value: expiring.length, label: '3天内临期', tone: 'warn', act: 'nav', href: '#/p/HOME-02' }) +
        ui.statCard({ icon: 'warn', value: expired.length, label: '已过期', tone: expired.length ? 'danger' : '', act: 'nav', href: '#/p/HOME-02' }) +
        ui.statCard({ icon: 'location', value: unlocated.length, label: '待补充位置', act: 'nav', href: '#/p/INV-07?filter=unlocated' }) +
        '</div>';

      /* 临期 Top3 */
      var top3 = expiring.slice(0, 3).map(function (it) { return ui.foodRow(it); }).join('');
      var expCard = ui.card({
        title: '临期优先',
        side: '<a data-act="nav" data-href="#/p/HOME-02">全部 ' + expiring.length + ' 项 ›</a>',
        body: top3 || ui.emptyState({ icon: 'check', title: '没有临期食材', desc: '3 天内没有会过期的食材' }),
        cls: 'home-exp'
      });

      /* 今日推荐（菜谱 + 理由） */
      var r = q.recipe('r1');
      var recCard = ui.card({
        title: '今日推荐',
        side: '<a data-act="nav" data-href="#/p/PLAN-02">本周菜单 ›</a>',
        body: ui.recipeCard(r, { act: 'plan:recipe' }) +
          '<div class="home-why">' + FS.icon('info') + '理由：番茄 3 天后到期、鸡蛋充足；15 分钟快手菜。</div>' +
          '<div class="home-rec-btns">' +
            ui.btn({ label: '开始烹饪', kind: 'primary', size: 'sm', act: 'nav', href: '#/p/PLAN-05?recipe=r1' }) +
            ui.btn({ label: '换一个', kind: 'ghost', size: 'sm', act: 'nav', href: '#/p/PLAN-03' }) +
          '</div>',
        cls: 'home-rec'
      });

      /* Agent 主动建议（可执行） */
      var tips = d.agentTips.map(function (t) {
        return '<div class="tip-card">' +
          '<span class="tip-icon">' + FS.icon('zap') + '</span>' +
          '<div class="tip-text">' + FS.esc(t.text) + '</div>' +
          ui.btn({ label: t.act, kind: 'ghost', size: 'sm', act: 'nav', href: '#/p/' + t.page }) +
          ui.iconBtn({ icon: 'x', label: '忽略', act: 'home:tip-dismiss', arg: t.id }) +
        '</div>';
      }).join('');
      var tipCardBox = ui.card({ title: 'Agent 建议', side: '标准模式', body: tips, cls: 'home-tips' });

      /* 菜单/购物摘要 + 最近同步 */
      var planRow = '<div class="home-mini">' +
        '<div class="mini-card" data-act="nav" data-href="#/p/PLAN-02">' + FS.icon('plan') +
          '<div><div class="mini-title">今日菜单</div><div class="mini-sub">晚：番茄炒蛋 + 菠菜鸡蛋汤</div></div>' + FS.icon('fwd', 'lr-arrow') + '</div>' +
        '<div class="mini-card" data-act="nav" data-href="#/p/PLAN-06">' + FS.icon('cart') +
          '<div><div class="mini-title">购物清单</div><div class="mini-sub">3 项待买 · 2 项建议</div></div>' + FS.icon('fwd', 'lr-arrow') + '</div>' +
        '</div>';
      var syncLine = '<div class="sync-line">' + ui.statusDot('ok') +
        '<span class="sd-label">14:31 库存变更已同步到云端 · 语音识别新增「内酯豆腐」待补充位置</span></div>';

      /* 异步内容区：包进单一 scroll-area，避免 600 高被挤出裁切 */
      var asyncHtml =
        '<div class="home-scroll">' +
          stats +
          '<div class="home-cols">' + expCard + recCard + '</div>' +
          tipCardBox + planRow + syncLine +
        '</div>';

      return '<div class="pg">' +
        ui.pageHead({
          title: '首页', id: 'HOME-01',
          sub: d.device.date + ' · 冰箱一切正常',
          actions: ui.iconBtn({ icon: 'search', label: '搜索', act: 'nav', href: '#/p/INV-07' }) +
                   ui.iconBtn({ icon: 'plus', label: '快速添加', act: 'home:quick-add' })
        }) +
        '<div class="pg-body home-body">' +
          ui.scrollArea(FS.states.wrap(pg, asyncHtml, {
            emptyTitle: '还没有库存数据', emptyDesc: '先添加一些食材吧',
            emptyAction: ui.btn({ label: '添加食材', act: 'nav', href: '#/p/INV-04' })
          }), 'home-scroll-area') +
        '</div></div>';
    }
  });

  /* ==================== HOME-02 今日摘要详情 ==================== */
  FS.router.register({
    id: 'HOME-02', name: '今日摘要详情', module: '首页', level: '二级页', ctx: 'CTX-01',
    states: ['loading', 'offline', 'error'],
    parent: 'HOME-01',

    render: function (pg) {
      ready();
      var expired = q.expired();
      var today = q.expiring(0);
      var soon = q.expiring(3).filter(function (it) { return it.daysLeft > 0; });

      /* 过期/今天/3天内 分组 */
      var groups = '';
      if (expired.length) {
        groups += ui.sectionTitle('已过期（' + expired.length + '）', '<span class="danger-text">建议尽快处理</span>') +
          '<div class="food-group">' + expired.map(function (it) { return ui.foodRow(it); }).join('') + '</div>';
      }
      if (today.length) {
        groups += ui.sectionTitle('今天到期（' + today.length + '）') +
          '<div class="food-group">' + today.map(function (it) { return ui.foodRow(it); }).join('') + '</div>';
      }
      if (soon.length) {
        groups += ui.sectionTitle('3天内到期（' + soon.length + '）') +
          '<div class="food-group">' + soon.map(function (it) { return ui.foodRow(it); }).join('') + '</div>';
      }

      /* 今日推荐菜 + 理由 */
      var rec = ui.card({
        title: '今日推荐：番茄炒蛋 + 菠菜鸡蛋汤',
        body: '<div class="home-why">' + FS.icon('info') + '菠菜明天到期、番茄 3 天后到期，优先消耗；两道菜共用鸡蛋，20 分钟内可完成。</div>' +
          '<div class="home-rec-btns">' +
            ui.btn({ label: '生成完整菜谱', kind: 'primary', act: 'nav', href: '#/p/PLAN-04?recipe=r1' }) +
            ui.btn({ label: '继续和 Agent 讨论', kind: 'ghost', act: 'nav', href: '#/p/CHAT-01' }) +
          '</div>'
      });

      /* 计划缺料 */
      var missing = ui.card({
        title: '菜单计划缺料',
        body: ui.listRow({ icon: 'cart', title: '面条 ×1把', sub: '周三 · 卤牛肉拌面 缺料', side: ui.chip('缺', 'danger'), act: 'nav', href: '#/p/PLAN-06' }) +
              ui.listRow({ icon: 'cart', title: '黑胡椒 ×1瓶', sub: '周五 · 香煎鸡胸肉 缺料', side: ui.chip('缺', 'danger'), act: 'nav', href: '#/p/PLAN-06' }) +
              '<div class="home-add-all">' + ui.btn({ label: '全部加入购物清单', kind: 'primary', size: 'sm', act: 'home:add-all-shopping' }) + '</div>'
      });

      /* 今日出入库流水 */
      var log = ui.card({
        title: '今日库存动态',
        body: ui.listRow({ icon: 'plus', title: '新增 内酯豆腐 ×1盒', sub: '07-22 14:02 · 语音登记 · 位置待补充', arrow: false }) +
              ui.listRow({ icon: 'camera', title: '拍照识别 某绿叶菜', sub: '07-22 11:38 · 低置信度待确认', arrow: false }) +
              ui.listRow({ icon: 'minus', title: '消耗 鸡蛋 ×2', sub: '07-22 09:15 · 早餐', arrow: false })
      });

      var asyncHtml = '<div class="home-scroll">' + groups + rec + missing + log + '</div>';

      return '<div class="pg">' +
        ui.pageHead({
          title: '今日摘要详情', id: 'HOME-02',
          sub: '临期 · 推荐 · 缺料 · 动态一览',
          actions: ui.iconBtn({ icon: 'back', label: '返回', act: 'back' })
        }) +
        '<div class="pg-body home-body">' +
          ui.scrollArea(FS.states.wrap(pg, asyncHtml), 'home-scroll-area') +
        '</div></div>';
    }
  });

  /* ==================== 模块内动作 ==================== */
  FS.action('home:quick-add', function () { FS.overlays.show('OV-11'); });
  FS.action('home:tip-dismiss', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已忽略该建议（演示）' });
  });
  FS.action('home:add-all-shopping', function () {
    FS.overlays.show('OV-01', { tone: 'ok', text: '已加入购物清单（演示）' });
  });
  /* 食材行默认动作：进 INV-03 详情（库存模块注册同名片段前的通用实现） */
  FS.action('inv:detail', function (el) {
    var id = el.getAttribute('data-arg');
    var it = FS.dataQ && FS.dataQ.item ? FS.dataQ.item(id) : null;
    var hash = '#/p/INV-03?id=' + encodeURIComponent(id);
    /* 把位置参数带上，侧舱「当前位置」能显示层级 */
    if (it && it.loc && it.loc.zone) {
      hash += '&zone=' + encodeURIComponent(it.loc.zone);
      if (it.loc.layer) hash += '&layer=' + encodeURIComponent(it.loc.layer);
      if (it.loc.cells && it.loc.cells.length) hash += '&cell=' + encodeURIComponent(it.loc.cells.join('+'));
    }
    FS.router.go(hash);
  });
  FS.action('plan:recipe', function (el) {
    FS.router.go('#/p/PLAN-04?recipe=' + encodeURIComponent(el.getAttribute('data-arg')));
  });
})();
