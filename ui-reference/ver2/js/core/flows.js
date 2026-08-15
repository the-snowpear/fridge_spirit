/**
 * 冰箱小精灵 ver2 UI 预览 —— 关键流程演示编排器
 * ------------------------------------------------------------------
 * 核心只提供注册/启动/停止与编排助手（wait/go/show/toast）；
 * 各页面模块在自己的 pages-*.js 里 FS.flows.register() 注册具体流程：
 *   photo  拍照登记（INV-05→OV-07→INV-06→确认→toast→返回）
 *   voice  语音登记（OV-05→OV-06→INV-04 草稿）
 *   agent  Agent 高风险确认（CHAT-01→OV-06→OV-04）
 *   cook   烹饪+计时到时（PLAN-04→PLAN-05→OV-09）
 *   safety 关键安全告警（OV-10 全屏接管）
 * 「停止演示」会取消定时器链并关闭全部浮层。
 */
window.FS = window.FS || {};

FS.flows = (function () {
  var runners = {};
  var token = null;

  function register(id, def) { runners[id] = def; }
  function has(id) { return !!runners[id]; }

  function start(id) {
    /* 竖屏隐私模式：禁止拍照登记流程（摄像头关闭） */
    if (id === 'photo' && FS.mode && FS.mode.isPrivacy()) {
      if (FS.mode.privacyCameraToast) FS.mode.privacyCameraToast();
      else FS.overlays.show('OV-01', { tone: 'warn', text: '隐私模式已关闭摄像头，无法演示拍照登记' });
      return;
    }
    stop();
    var def = runners[id];
    if (!def) {
      FS.overlays.show('OV-01', { tone: 'warn', text: '流程「' + id + '」尚未注册' });
      return;
    }
    var tk = { cancelled: false, timers: [] };
    token = tk;
    var api = {
      /* 等待 ms 毫秒（可被打断；打断后 promise 不 resolve，流程自然终止） */
      wait: function (ms) {
        return new Promise(function (resolve) {
          var t = setTimeout(resolve, ms);
          tk.timers.push(t);
        });
      },
      go: function (hash) { if (!tk.cancelled) FS.router.go(hash); },
      show: function (ovl, data) { if (!tk.cancelled) FS.overlays.show(ovl, data); },
      close: function (ovl) { if (!tk.cancelled) FS.overlays.close(ovl); },
      toast: function (text, tone) { if (!tk.cancelled) FS.overlays.show('OV-01', { tone: tone || 'ok', text: text }); },
      cancelled: function () { return tk.cancelled; }
    };
    def.run(api);
  }

  function stop() {
    if (token) {
      token.cancelled = true;
      token.timers.forEach(clearTimeout);
      token = null;
    }
    FS.overlays.closeAll();
  }

  return { register: register, start: start, stop: stop, has: has };
})();
