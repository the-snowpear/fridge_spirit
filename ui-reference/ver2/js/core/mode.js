/**
 * 冰箱小精灵 ver2 UI 预览 —— 布局模式（横屏标准 / 竖屏隐私）
 * ------------------------------------------------------------------
 * 横屏 landscape：1024×600，三栏 main | nav | cabin，摄像头可用。
 * 竖屏 portrait：600×1024，三区 上主区 / 中导航 / 下状态舱，绑定隐私模式（关摄像头）。
 * 切换入口：工具栏按钮、?layout=portrait|landscape、web iframe query。
 */
window.FS = window.FS || {};

FS.mode = (function () {
  var layout = 'landscape'; // landscape | portrait
  var listeners = [];

  function isPrivacy() {
    return layout === 'portrait';
  }

  function getLayout() {
    return layout;
  }

  function getSize() {
    return layout === 'portrait' ? { w: 600, h: 1024 } : { w: 1024, h: 600 };
  }

  function label() {
    /* 竖屏外壳：主区 672 + 导航 64 + 状态舱 288 */
    return layout === 'portrait' ? '竖屏隐私 · 600×1024' : '横屏标准 · 1024×600';
  }

  function applyDom() {
    var stage = document.getElementById('stage');
    if (!stage) return;
    stage.classList.toggle('mode-portrait', layout === 'portrait');
    stage.classList.toggle('mode-landscape', layout === 'landscape');
    stage.classList.toggle('mode-privacy', isPrivacy());
    document.body.classList.toggle('layout-portrait', layout === 'portrait');
    document.body.classList.toggle('layout-landscape', layout === 'landscape');
  }

  function writeQuery(next) {
    try {
      var url = new URL(location.href);
      url.searchParams.set('layout', next);
      history.replaceState(null, '', url.pathname + url.search + url.hash);
    } catch (e) {
      /* file:// 或旧环境忽略 query 同步 */
    }
  }

  function setLayout(next) {
    if (next !== 'portrait' && next !== 'landscape') return;
    var changed = layout !== next;
    layout = next;
    writeQuery(layout);
    try {
      localStorage.setItem('fs_ver2_layout', layout);
    } catch (e) { /* 无痕模式等 */ }
    applyDom();
    if (changed) {
      listeners.forEach(function (cb) {
        try { cb(layout); } catch (err) { /* 监听方异常不阻断模式切换 */ }
      });
    }
  }

  function onChange(cb) {
    if (typeof cb === 'function') listeners.push(cb);
  }

  function init() {
    var fromQuery = null;
    try {
      fromQuery = new URL(location.href).searchParams.get('layout');
    } catch (e) { /* ignore */ }
    var fromStore = null;
    try {
      fromStore = localStorage.getItem('fs_ver2_layout');
    } catch (e) { /* ignore */ }

    if (fromQuery === 'portrait' || fromQuery === 'landscape') {
      layout = fromQuery;
    } else if (fromStore === 'portrait' || fromStore === 'landscape') {
      layout = fromStore;
    } else {
      layout = 'landscape';
    }
    applyDom();
    writeQuery(layout);
  }

  /** 隐私模式拦截文案（舱按钮 / 路由 / 流程共用） */
  function privacyCameraToast() {
    if (FS.overlays) {
      FS.overlays.show('OV-01', {
        tone: 'warn',
        text: '隐私模式已关闭摄像头，无法拍照识别'
      });
    }
  }

  return {
    init: init,
    getLayout: getLayout,
    setLayout: setLayout,
    isPrivacy: isPrivacy,
    getSize: getSize,
    label: label,
    onChange: onChange,
    applyDom: applyDom,
    privacyCameraToast: privacyCameraToast
  };
})();
