/**
 * 冰箱小精灵 ver2 UI 预览 —— 页面注册表 + hash 路由
 * ------------------------------------------------------------------
 * 路由格式：#/p/<页面ID>?k=v&k=v（file:// 安全、可刷新、可深链）。
 * 注册表是全量页面（30 页）的唯一事实源：工具栏下拉、页面总览、验收清单
 * 均由注册表生成。
 * 侧导航切模块：始终进入该模块一级页，不恢复上次子页/参数现场。
 * 返回顺序（规范）：关浮层 → 模块内上一级 → 模块根页（由 shell 统一调度）。
 */
window.FS = window.FS || {};

FS.router = (function () {
  var pages = {};            // id → 页面定义
  var order = [];            // 注册顺序（= 规范页面顺序，用于上一页/下一页）
  var listeners = [];
  var current = { id: null, params: {} };

  /* 页面定义：
   * { id, name, module, level, ctx, states[], system?, parent?: id|fn(params)→hash,
   *   gallery?: [{label, hash}], render(pg)→html, onMount?(root) }
   * pg = { id, params, viewState, page } */
  function register(def) {
    pages[def.id] = def;
    order.push(def.id);
  }

  function parse(hash) {
    var m = /^#\/p\/([A-Z]+-[0-9]+)(\?(.*))?$/.exec(hash || '');
    if (!m) return null;
    return { id: m[1], params: new URLSearchParams(m[3] || '') };
  }

  function toHash(id, params) {
    var q = params ? params.toString() : '';
    return '#/p/' + id + (q ? '?' + q : '');
  }

  /* 隐私模式禁止进入拍照取景 / 识别确认页 */
  function isCameraPage(id) {
    return id === 'INV-05' || id === 'INV-06';
  }

  function blockPrivacyCamera(id) {
    if (FS.mode && FS.mode.isPrivacy() && isCameraPage(id)) {
      if (FS.mode.privacyCameraToast) FS.mode.privacyCameraToast();
      return true;
    }
    return false;
  }

  /* 跳转（写 hash，由 hashchange 驱动渲染） */
  function go(hash) {
    var p = parse(hash);
    if (p && blockPrivacyCamera(p.id)) return;
    if (location.hash === hash) { sync(); return; }
    location.hash = hash;
  }
  function goPage(id, paramsObj) {
    if (blockPrivacyCamera(id)) return;
    go(toHash(id, new URLSearchParams(paramsObj || {})));
  }

  /* 侧导航切模块：始终进该模块一级页（不带参数、不恢复上次子页） */
  function goModule(module) {
    var rootId = order.find(function (id) { return pages[id].module === module && pages[id].level === '一级页'; });
    if (!rootId) return;
    /* 已在该模块一级页时也强制回根（清掉 URL 参数，如 INV-01 的 zone/view） */
    goPage(rootId);
  }

  /* 返回上一级：优先页面定义的 parent（可为函数），否则回模块根页 */
  function back() {
    var def = pages[current.id];
    if (!def) return;
    var target = null;
    if (typeof def.parent === 'function') target = def.parent(current.params);
    else if (def.parent) target = toHash(def.parent, null);
    if (target) { go(target); return; }
    var rootId = order.find(function (id) { return pages[id].module === def.module && pages[id].level === '一级页'; });
    if (rootId && rootId !== current.id) goPage(rootId);
  }

  function currentPage() { return pages[current.id] || null; }
  function currentId() { return current.id; }
  function currentParams() { return current.params; }

  function list() { return order.map(function (id) { return pages[id]; }); }
  function get(id) { return pages[id] || null; }

  function onChange(cb) { listeners.push(cb); }
  function emit() {
    listeners.forEach(function (cb) { cb(current, pages[current.id]); });
  }

  /* 解析当前 hash 并通知（shell 挂到 hashchange） */
  function sync() {
    var p = parse(location.hash);
    if (!p || !pages[p.id]) {
      /* 默认进首页 */
      p = { id: 'HOME-01', params: new URLSearchParams() };
      location.replace(toHash(p.id, p.params));
    }
    /* 深链进入拍照页时，隐私模式回落到库存浏览器 */
    if (p && FS.mode && FS.mode.isPrivacy() && isCameraPage(p.id)) {
      p = { id: 'INV-01', params: new URLSearchParams() };
      location.replace(toHash(p.id, p.params));
    }
    current = p;
    emit();
  }

  /* 模块根页 hash（侧导航高亮与返回用） */
  function moduleOf(id) {
    var def = pages[id];
    return def ? (def.navModule || def.module) : null;
  }

  /* 上一页/下一页（工具栏按注册顺序） */
  function sibling(step) {
    var i = order.indexOf(current.id);
    if (i < 0) return;
    var next = order[(i + step + order.length) % order.length];
    goPage(next);
  }

  return {
    register: register, parse: parse, toHash: toHash,
    go: go, goPage: goPage, goModule: goModule, back: back,
    currentPage: currentPage, currentId: currentId, currentParams: currentParams,
    list: list, get: get, onChange: onChange, sync: sync,
    moduleOf: moduleOf, sibling: sibling
  };
})();
