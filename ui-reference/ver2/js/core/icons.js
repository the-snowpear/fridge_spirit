/**
 * 冰箱小精灵 ver2 UI 预览 —— 内联 SVG 图标表
 * ------------------------------------------------------------------
 * 约束：单色 currentColor 线性图标（stroke），对应固件侧 TF 卡图标资源；
 *       设备内所有图标位旁边均有文字标签或可回退为文字（LVGL 字形缺失策略）。
 * 用法：FS.icon('home') / FS.icon('mic', 'lg') → svg 字符串。
 */
window.FS = window.FS || {};

FS.icons = {
  /* ---- 一级导航 ---- */
  home:  '<path d="M3 11l9-8 9 8"/><path d="M5 10v10h14V10"/>',
  box:   '<path d="M21 8l-9-5-9 5v8l9 5 9-5V8z"/><path d="M3 8l9 5 9-5"/><path d="M12 13v8"/>',
  chat:  '<path d="M21 12a8 8 0 0 1-11.5 7.2L4 21l1.8-5.5A8 8 0 1 1 21 12z"/>',
  plan:  '<rect x="3" y="5" width="18" height="16" rx="2"/><path d="M3 10h18"/><path d="M8 3v4M16 3v4"/>',
  more:  '<circle cx="5" cy="12" r="1.6"/><circle cx="12" cy="12" r="1.6"/><circle cx="19" cy="12" r="1.6"/>',

  /* ---- 动作 ---- */
  mic:   '<rect x="9" y="3" width="6" height="11" rx="3"/><path d="M5 11a7 7 0 0 0 14 0"/><path d="M12 18v3"/>',
  camera:'<path d="M4 8h3l2-3h6l2 3h3v11H4z"/><circle cx="12" cy="13" r="3.5"/>',
  search:'<circle cx="11" cy="11" r="6"/><path d="M20 20l-4-4"/>',
  plus:  '<path d="M12 5v14M5 12h14"/>',
  minus: '<path d="M5 12h14"/>',
  edit:  '<path d="M4 20l4-1L20 7l-3-3L5 16l-1 4z"/>',
  trash: '<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"/>',
  move:  '<path d="M8 7l-5 5 5 5M16 7l5 5-5 5"/><path d="M12 4v16"/>',
  check: '<path d="M4 12l5 5L20 6"/>',
  x:     '<path d="M6 6l12 12M18 6L6 18"/>',
  undo:  '<path d="M8 5L3 10l5 5"/><path d="M3 10h11a6 6 0 0 1 0 12h-3"/>',
  retry: '<path d="M20 12a8 8 0 1 1-2.3-5.7"/><path d="M20 4v5h-5"/>',
  copy:  '<rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15H3V3h12v2"/>',
  send:  '<path d="M22 2L11 13"/><path d="M22 2l-7 20-4-9-9-4l20-7z"/>',

  /* ---- 方向 ---- */
  back:  '<path d="M15 5l-7 7 7 7"/>',
  fwd:   '<path d="M9 5l7 7-7 7"/>',
  down:  '<path d="M6 9l6 6 6-6"/>',
  up:    '<path d="M6 15l6-6 6 6"/>',

  /* ---- 状态 / 语义 ---- */
  warn:   '<path d="M12 3L2 20h20L12 3z"/><path d="M12 10v4M12 17.5v.5"/>',
  danger: '<circle cx="12" cy="12" r="9"/><path d="M12 8v5M12 16.5v.5"/>',
  info:   '<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 7.5V8"/>',
  sync:   '<path d="M20 12a8 8 0 1 1-2.3-5.7"/><path d="M20 4v5h-5"/>',
  lock:   '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
  star:   '<path d="M12 3l2.7 5.6 6.1.9-4.4 4.3 1 6.1-5.4-2.9-5.4 2.9 1-6.1L3.2 9.5l6.1-.9L12 3z"/>',
  clock:  '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 3"/>',
  history:'<path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/><path d="M12 8v4l3 2"/>',
  eye:    '<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7-10-7-10-7z"/><circle cx="12" cy="12" r="3"/>',

  /* ---- 设备 / 健康 ---- */
  wifi:    '<path d="M5 12a10 10 0 0 1 14 0"/><path d="M8.5 15.5a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1"/>',
  wifioff: '<path d="M5 12a10 10 0 0 1 14 0"/><path d="M8.5 15.5a5 5 0 0 1 7 0"/><path d="M3 3l18 18"/>',
  cloud:   '<path d="M6.5 18a4 4 0 0 1-.4-8A5.5 5.5 0 0 1 16.8 8.6 4.2 4.2 0 0 1 17.5 18h-11z"/>',
  cloudoff:'<path d="M6.5 18a4 4 0 0 1-.4-8A5.5 5.5 0 0 1 16.8 8.6"/><path d="M3 3l18 18"/>',
  tf:      '<path d="M6 3h9l3 3v15H6z"/><path d="M9 7h2M13 7h2M9 11h6"/>',
  bell:    '<path d="M18 9a6 6 0 1 0-12 0c0 6-2 7-2 7h16s-2-1-2-7"/><path d="M10 20a2.2 2.2 0 0 0 4 0"/>',
  timer:   '<circle cx="12" cy="13" r="8"/><path d="M12 9v4l3 2"/><path d="M9 2h6"/>',
  stopwatch:'<circle cx="12" cy="13" r="8"/><path d="M12 9v4"/><path d="M10 2h4M12 2v3"/>',
  settings:'<circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M19.1 4.9L17 7M7 17l-2.1 2.1"/>',
  tool:    '<path d="M21 6.5a4.5 4.5 0 0 1-6 4.2L7 18.7a2.1 2.1 0 0 1-3-3l8-8A4.5 4.5 0 0 1 16.5 2l-2.5 2.5 2 2L18.5 4 21 6.5z"/>',
  log:     '<path d="M6 2h9l4 4v16H6z"/><path d="M9 12h6M9 16h6M9 8h2"/>',
  power:   '<path d="M12 3v9"/><path d="M6.3 6.3a8 8 0 1 0 11.4 0"/>',
  shield:  '<path d="M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6l8-3z"/>',
  temp:    '<path d="M10 4a2 2 0 0 1 4 0v9a4 4 0 1 1-4 0V4z"/><path d="M12 13v4"/>',
  drop:    '<path d="M12 3s6 6.5 6 11a6 6 0 0 1-12 0c0-4.5 6-11 6-11z"/>',
  sun:     '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M19.1 4.9l-1.4 1.4M6.3 17.7l-1.4 1.4"/>',
  door:    '<rect x="6" y="3" width="12" height="18" rx="1"/><path d="M15 12h.5"/><path d="M3 21h18"/>',
  radar:   '<circle cx="12" cy="12" r="1.5"/><path d="M12 3a9 9 0 0 1 9 9"/><path d="M12 7a5 5 0 0 1 5 5"/><path d="M12 12l4-4"/>',
  globe:   '<circle cx="12" cy="12" r="9"/><path d="M3 12h18"/><path d="M12 3a14 14 0 0 1 0 18 14 14 0 0 1 0-18"/>',
  key:     '<circle cx="8" cy="14" r="4"/><path d="M11 11l9-9M17 5l2 2M14 8l2 2"/>',
  zap:     '<path d="M13 2L4 14h6l-1 8 9-12h-6l1-8z"/>',
  fridge:  '<rect x="7" y="2" width="10" height="20" rx="2"/><path d="M7 10h10"/><path d="M9 5v3M9 12v4"/>',
  keyboard:'<rect x="2" y="6" width="20" height="12" rx="2"/><path d="M6 10h.5M10 10h.5M14 10h.5M18 10h.5M7 14h10"/>',

  /* ---- 厨房业务 ---- */
  food:  '<path d="M7 3v8"/><path d="M4 3v4a3 3 0 0 0 6 0V3"/><path d="M7 11v10"/><path d="M17 3c-2 0-3 2.5-3 5.5S15 13 17 13v8"/><path d="M17 3v10"/>',
  recipe:'<path d="M4 4a2 2 0 0 1 2-2h13v18H6a2 2 0 0 0-2 2V4z"/><path d="M4 18a2 2 0 0 1 2-2h13"/><path d="M9 7h6"/>',
  cart:  '<path d="M3 4h3l3 12h10l3-8H7"/><circle cx="10" cy="20" r="1.5"/><circle cx="17" cy="20" r="1.5"/>',
  heart: '<path d="M12 20s-8-4.5-8-10a4.5 4.5 0 0 1 8-2.8A4.5 4.5 0 0 1 20 10c0 5.5-8 10-8 10z"/>',
  filter:'<path d="M3 5h18l-7 8v6l-4 2v-8L3 5z"/>',
  sort:  '<path d="M7 4v16M7 20l-3-3M7 20l3-3"/><path d="M17 20V4M17 4l-3 3M17 4l3 3"/>',
  volume:'<path d="M4 10v4h4l5 4V6l-5 4H4z"/><path d="M16 9a4 4 0 0 1 0 6"/>',
  list:  '<path d="M9 6h12M9 12h12M9 18h12"/><circle cx="4.5" cy="6" r="1"/><circle cx="4.5" cy="12" r="1"/><circle cx="4.5" cy="18" r="1"/>',
  grid:  '<rect x="4" y="4" width="7" height="7" rx="1"/><rect x="13" y="4" width="7" height="7" rx="1"/><rect x="4" y="13" width="7" height="7" rx="1"/><rect x="13" y="13" width="7" height="7" rx="1"/>',
  image: '<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="10" r="2"/><path d="M3 18l6-6 4 4 3-3 5 5"/>',
  upload:  '<path d="M12 16V4M6 10l6-6 6 6"/><path d="M4 20h16"/>',
  download:'<path d="M12 4v12M6 14l6 6 6-6"/><path d="M4 20h16"/>',
  play:  '<path d="M7 4l13 8-13 8V4z"/>',
  pause: '<path d="M7 4h4v16H7zM13 4h4v16h-4z"/>',
  stop:  '<rect x="6" y="6" width="12" height="12" rx="1"/>',
  user:  '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>',
  users: '<circle cx="9" cy="8" r="3.5"/><path d="M2.5 20a6.5 6.5 0 0 1 13 0"/><path d="M16 5a3.5 3.5 0 0 1 0 7"/><path d="M17.5 14a6.5 6.5 0 0 1 4 6"/>',
  location:'<path d="M12 21s-7-5.5-7-11a7 7 0 0 1 14 0c0 5.5-7 11-7 11z"/><circle cx="12" cy="10" r="2.5"/>'
};

/* 生成 svg 字符串；未知图标回退为空心方块，避免空白 */
FS.icon = function (name, cls) {
  var inner = FS.icons[name] || '<rect x="5" y="5" width="14" height="14" rx="2"/><path d="M9 9h6v6H9z"/>';
  return '<svg class="ic ' + (cls || '') + '" viewBox="0 0 24 24" aria-hidden="true">' + inner + '</svg>';
};
