/**
 * 冰箱小精灵 ver2 UI 预览 —— 全量 Mock 数据（唯一数据源）
 * ------------------------------------------------------------------
 * 说明：
 *  - 演示"今天"锚定为 2026-07-22（TODAY）。食材 daysLeft 均为字面整数，
 *    expire 日期字符串已与该日期对齐；固件侧由 SNTP 当前日期动态计算。
 *  - 食材字段对齐固件库存模型（components/storage）：
 *    name / qty / unit / expire / daysLeft / loc{zone,layer,cells} / source / confidence ...
 *  - 所有页面 / 侧舱 / 浮层只读 FS.data，查询助手集中在 FS.dataQ。
 */
window.FS = window.FS || {};

FS.data = {
  TODAY: '2026-07-22',

  /* 设备与连接总览（侧舱固定区 / MORE-04 / 设置页数据源） */
  device: {
    time: '14:32',
    date: '7月22日 周三',   /* 兼容整串；侧舱优先用下面拆分字段 */
    dateDay: '7月22日',
    weekday: '周三',
    temp: '4.2°C',
    hum: '38%',
    wifi: { ssid: 'Home_2.4G', rssi: -58, ok: true },
    cloud: true,                 // 云端（MQTT）在线
    tf: { free: '26.4G', total: '29.1G', ok: true },
    c6: { ok: true, ver: 'v1.4.2' },
    cameraOk: true, micOk: true, speakerOk: true,
    permission: '标准模式',       // Agent 权限：确认模式 / 标准模式 / 自动模式
    online: true,                // 网络在线标志（状态切换器"离线"时联动）
    version: 'v0.9.3-P4'
  },

  /* 侧舱健康图标行（顺序即显示顺序） */
  health: [
    { key: 'wifi',   icon: 'wifi',   label: 'Wi-Fi',  status: 'ok' },
    { key: 'cloud',  icon: 'cloud',  label: '云端',   status: 'ok' },
    { key: 'tf',     icon: 'tf',     label: 'TF卡',   status: 'ok' },
    { key: 'camera', icon: 'camera', label: '摄像头', status: 'ok' },
    { key: 'mic',    icon: 'mic',    label: '麦克风', status: 'warn' }
  ],

  /* 冰箱空间布局：L1 分区（3×3 可合并宫格，rect=x,y,w,h）→ L2 层 → L3 固定九宫格
   * 说明：L3 恒为 A1–C3 九个等大单位格，不存在"相邻格合并位"概念。
   *       每个格最多放一项食材（一格一物），因此每项库存的 loc.cells 必须且只有一个格号；
   *       "只记到层"的旧精度已取消，位置未知的食材 loc 为 null，走 INV-07 未定位清单。 */
  layout: {
    zones: [
      { id: 'freezer', name: '冷冻区', type: '冷冻', rect: { x: 0, y: 0, w: 3, h: 1 }, layers: [
        { id: 'f1', name: '第1层', on: true },
        { id: 'f2', name: '第2层', on: true }
      ]},
      { id: 'left', name: '左冷藏', type: '冷藏', rect: { x: 0, y: 1, w: 1, h: 2 }, layers: [
        { id: 'l1', name: '第1层', on: true },
        { id: 'l2', name: '第2层', on: true },
        { id: 'l3', name: '第3层', on: true }
      ]},
      { id: 'right', name: '右冷藏', type: '冷藏', rect: { x: 1, y: 1, w: 2, h: 1 }, layers: [
        { id: 'r1', name: '第1层', on: true },
        { id: 'r2', name: '第2层', on: true }
      ]},
      { id: 'door', name: '门架', type: '门架', rect: { x: 1, y: 2, w: 1, h: 1 }, layers: [
        { id: 'd1', name: '上格', on: true },
        { id: 'd2', name: '下格', on: true }
      ]},
      { id: 'drawer', name: '果蔬抽屉', type: '抽屉', rect: { x: 2, y: 2, w: 1, h: 1 }, layers: [
        { id: 'v1', name: '抽屉', on: true }
      ]}
    ]
  },

  /* 库存食材（20 条；覆盖：过期2 / 今天2 / 明天2 / 2-5天若干 / 冷冻长保 / 未定位2 / 低置信+估算1） */
  items: [
    { id: 'it01', name: '鸡蛋',       cat: '蛋奶',   qty: '12',        unit: '个', added: '07-18', expire: '07-27', daysLeft: 5,   estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'right',   layer: 'r2', cells: ['B2'] },          synced: true,  note: '' },
    { id: 'it02', name: '鲜牛奶',     cat: '蛋奶',   qty: '950',       unit: 'ml', added: '07-20', expire: '07-24', daysLeft: 2,   estimated: false, source: '拍照',   confidence: 0.93, loc: { zone: 'right',   layer: 'r2', cells: ['B3'] },          synced: true,  note: '全脂' },
    { id: 'it03', name: '原味酸奶',   cat: '蛋奶',   qty: '4',         unit: '杯', added: '07-15', expire: '07-22', daysLeft: 0,   estimated: true,  source: '拍照',   confidence: 0.87, loc: { zone: 'right',   layer: 'r2', cells: ['C1'] },          synced: true,  note: '日期为估算' },
    { id: 'it04', name: '番茄',       cat: '蔬菜',   qty: '3',         unit: '个', added: '07-19', expire: '07-25', daysLeft: 3,   estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'drawer',  layer: 'v1', cells: ['A1'] },            synced: true,  note: '' },
    { id: 'it05', name: '菠菜',       cat: '蔬菜',   qty: '1',         unit: '把', added: '07-21', expire: '07-23', daysLeft: 1,   estimated: false, source: '语音',   confidence: 1,    loc: { zone: 'drawer',  layer: 'v1', cells: ['A2'] },            synced: true,  note: '' },
    { id: 'it06', name: '剩米饭',     cat: '主食',   qty: '1',         unit: '盒', added: '07-20', expire: '07-21', daysLeft: -1,  estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'left',    layer: 'l1', cells: ['A1'] },            synced: true,  note: '建议丢弃' },
    { id: 'it07', name: '卤牛肉',     cat: '肉类',   qty: '200',       unit: 'g',  added: '07-19', expire: '07-22', daysLeft: 0,   estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'left',    layer: 'l2', cells: ['A1'] },            synced: true,  note: '今天优先吃' },
    { id: 'it08', name: '速冻水饺',   cat: '速冻',   qty: '半袋(约20)', unit: '个', added: '07-10', expire: '08-21', daysLeft: 30,  estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'freezer', layer: 'f1', cells: ['A1'] },            synced: true,  note: '' },
    { id: 'it09', name: '鸡胸肉',     cat: '肉类',   qty: '400',       unit: 'g',  added: '07-12', expire: '10-20', daysLeft: 90,  estimated: true,  source: '小程序', confidence: 1,    loc: { zone: 'freezer', layer: 'f2', cells: ['A1'] },            synced: true,  note: '' },
    { id: 'it10', name: '黄油',       cat: '蛋奶',   qty: '1',         unit: '块', added: '07-08', expire: '08-05', daysLeft: 14,  estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'door',    layer: 'd1', cells: ['A1'] },            synced: true,  note: '227g装' },
    { id: 'it11', name: '蚝油',       cat: '调味',   qty: '半瓶',      unit: '',   added: '06-20', expire: '09-20', daysLeft: 60,  estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'door',    layer: 'd2', cells: ['A1'] },            synced: true,  note: '' },
    { id: 'it12', name: '可口可乐',   cat: '饮品',   qty: '3',         unit: '罐', added: '07-15', expire: '11-19', daysLeft: 120, estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'door',    layer: 'd2', cells: ['A2'] },            synced: true,  note: '' },
    { id: 'it13', name: '吐司面包',   cat: '主食',   qty: '6',         unit: '片', added: '07-14', expire: '07-19', daysLeft: -3,  estimated: false, source: '拍照',   confidence: 0.91, loc: { zone: 'left',    layer: 'l1', cells: ['A2'] },            synced: false, note: '已过期，待确认处理' },
    { id: 'it14', name: '芝士片',     cat: '蛋奶',   qty: '8',         unit: '片', added: '07-16', expire: '07-29', daysLeft: 7,   estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'right',   layer: 'r1', cells: ['A1'] },            synced: true,  note: '' },
    { id: 'it15', name: '胡萝卜',     cat: '蔬菜',   qty: '2',         unit: '根', added: '07-18', expire: '07-28', daysLeft: 6,   estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'drawer',  layer: 'v1', cells: ['A3'] },            synced: true,  note: '' },
    { id: 'it16', name: '苹果',       cat: '水果',   qty: '5',         unit: '个', added: '07-17', expire: '08-01', daysLeft: 10,  estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'left',    layer: 'l3', cells: ['A1'] },            synced: true,  note: '' },
    { id: 'it17', name: '某绿叶菜',   cat: '蔬菜',   qty: '1',         unit: '把', added: '07-21', expire: '07-24', daysLeft: 2,   estimated: true,  source: '拍照',   confidence: 0.61, loc: null,                                        synced: false, note: '低置信度识别，名称待确认' },
    { id: 'it18', name: '内酯豆腐',   cat: '豆制品', qty: '1',         unit: '盒', added: '07-21', expire: '07-23', daysLeft: 1,   estimated: false, source: '语音',   confidence: 1,    loc: null,                                        synced: false, note: '位置待补充' },
    { id: 'it19', name: '午餐肉罐头', cat: '罐头',   qty: '1',         unit: '罐', added: '07-05', expire: '01-18', daysLeft: 180, estimated: false, source: '手动',   confidence: 1,    loc: { zone: 'right',   layer: 'r1', cells: ['A2'] },            synced: true,  note: '' },
    { id: 'it20', name: '虾仁',       cat: '水产',   qty: '250',       unit: 'g',  added: '07-19', expire: '09-05', daysLeft: 45,  estimated: false, source: '拍照',   confidence: 0.95, loc: { zone: 'freezer', layer: 'f2', cells: ['A2'] },            synced: true,  note: '' }
  ],

  /* 对话模块：历史会话 + 当前会话消息流 + 待确认行动 */
  chat: {
    sessions: [
      { id: 's1', title: '今晚吃什么',         time: '今天 12:08',  summary: '根据临期食材推荐了番茄炒蛋', tasks: '菜谱×1',   synced: true },
      { id: 's2', title: '登记了一袋蔬菜',     time: '昨天 19:42',  summary: '语音登记菠菜/胡萝卜，位置待补充', tasks: '库存×2', synced: true },
      { id: 's3', title: '周末采购清单',       time: '07-20 10:15', summary: '生成购物清单 6 项', tasks: '购物×1',           synced: true },
      { id: 's4', title: '冰箱温度异常咨询',   time: '07-18 22:03', summary: '解释温度波动原因并给出建议', tasks: '',          synced: false }
    ],
    /* 消息类型：user=用户气泡 agent=Agent气泡 tool=工具调用卡 result=结果卡 */
    messages: [
      { type: 'user',  text: '帮我看看冰箱里有什么快过期的？', time: '14:20' },
      { type: 'tool',  name: 'inventory_query', label: '查询库存', risk: '低', status: 'ok',      detail: '筛选 剩余≤3天，共 6 项', latency: '0.4s' },
      { type: 'agent', text: '有 2 样今天到期：原味酸奶和卤牛肉，菠菜明天也该吃了。建议今晚优先安排卤牛肉，酸奶可以当餐后。', time: '14:20' },
      { type: 'user',  text: '那今晚做什么吃比较好？', time: '14:24' },
      { type: 'tool',  name: 'menu_generate',   label: '生成菜谱', risk: '中', status: 'running', detail: '结合库存与口味偏好生成中…', latency: '' },
      { type: 'result', kind: 'recipe', title: '推荐：番茄炒蛋', desc: '用掉番茄×2、鸡蛋×3 · 约15分钟 · 库存覆盖率 90%', refId: 'r1' },
      { type: 'agent', text: '推荐番茄炒蛋，能消耗 2 个番茄和 3 个鸡蛋。菠菜明天到期，可以再来个菠菜鸡蛋汤。', time: '14:25' },
      { type: 'tool',  name: 'inventory_delete', label: '删除食材', risk: '高', status: 'pending',  detail: '删除「吐司面包（已过期3天）」批次', latency: '' }
    ]
  },

  /* CHAT-04 待确认行动列表 */
  pendingActions: [
    { id: 'pa1', title: '删除过期批次「吐司面包」', risk: '高', desc: '该批次已过期 3 天，删除后不可恢复', objects: '吐司面包 ×6片（左冷藏/第1层）', irreversible: true },
    { id: 'pa2', title: '把 2 件未定位食材移到右冷藏', risk: '中', desc: '某绿叶菜、内酯豆腐 → 右冷藏/第1层', objects: '某绿叶菜、内酯豆腐', irreversible: false },
    { id: 'pa3', title: '上传 1 张照片用于识别归档', risk: '中', desc: '照片将上传至云端视觉模型，本地不留存', objects: 'IMG_0722_1431.jpg', irreversible: false }
  ],

  /* 菜谱库（PLAN-03/04/05 数据源；steps 供烹饪单步模式） */
  recipes: [
    { id: 'r1', name: '番茄炒蛋',     servings: '2人份', time: '15分钟', difficulty: '简单', coverage: 90,  fav: true,  missing: [],           use: ['番茄×2', '鸡蛋×3'], steps: ['番茄切块，鸡蛋加少许盐打散', '热锅凉油，倒入蛋液炒至半熟盛出', '下番茄块中火炒出汁', '加半勺糖、少许盐调味', '倒回鸡蛋翻炒均匀', '出锅装盘'] },
    { id: 'r2', name: '菠菜鸡蛋汤',   servings: '2人份', time: '10分钟', difficulty: '简单', coverage: 100, fav: false, missing: [],           use: ['菠菜×1把', '鸡蛋×1'], steps: ['菠菜洗净切段', '水烧开下菠菜', '淋入蛋液搅成蛋花', '加盐、香油出锅'] },
    { id: 'r3', name: '香煎鸡胸肉',   servings: '1人份', time: '20分钟', difficulty: '中等', coverage: 70,  fav: false, missing: ['黑胡椒'],   use: ['鸡胸肉×200g'], steps: ['鸡胸肉解冻，片成两片', '盐、黑胡椒腌制10分钟', '平底锅少油中小火煎', '每面4分钟至金黄', '静置2分钟切片'] },
    { id: 'r4', name: '卤牛肉拌面',   servings: '1人份', time: '12分钟', difficulty: '简单', coverage: 60,  fav: false, missing: ['面条', '青菜'], use: ['卤牛肉×100g'], steps: ['烧水煮面', '卤牛肉切片', '面熟捞出加卤汁拌匀', '铺上牛肉片'] },
    { id: 'r5', name: '虾仁滑蛋',     servings: '2人份', time: '15分钟', difficulty: '中等', coverage: 85,  fav: true,  missing: [],           use: ['虾仁×150g', '鸡蛋×3'], steps: ['虾仁解冻去虾线', '蛋清抓匀虾仁腌5分钟', '鸡蛋加牛奶打散', '虾仁滑炒至变色', '倒入蛋液小火推炒', '凝固即出锅'] },
    { id: 'r6', name: '苹果酸奶杯',   servings: '1人份', time: '5分钟',  difficulty: '简单', coverage: 100, fav: false, missing: [],           use: ['苹果×1', '原味酸奶×1杯'], steps: ['苹果切小块', '倒入酸奶拌匀即可'] }
  ],

  /* 本周菜单（PLAN-01/02；含 2 处缺失食材冲突） */
  menu: {
    week: '7月21日 – 7月27日',
    days: [
      { date: '周一 21', meals: { '早餐': '苹果酸奶杯', '午餐': '卤牛肉拌面', '晚餐': '番茄炒蛋', '加餐': '' }, conflict: false },
      { date: '周二 22', meals: { '早餐': '苹果酸奶杯', '午餐': '虾仁滑蛋',   '晚餐': '番茄炒蛋+菠菜鸡蛋汤', '加餐': '酸奶' }, conflict: false, today: true },
      { date: '周三 23', meals: { '早餐': '吐司(需购买)', '午餐': '香煎鸡胸肉', '晚餐': '菠菜鸡蛋汤', '加餐': '' }, conflict: true },
      { date: '周四 24', meals: { '早餐': '', '午餐': '卤牛肉拌面', '晚餐': '虾仁滑蛋', '加餐': '' }, conflict: false },
      { date: '周五 25', meals: { '早餐': '苹果酸奶杯', '午餐': '速冻水饺', '晚餐': '香煎鸡胸肉', '加餐': '' }, conflict: true },
      { date: '周六 26', meals: { '早餐': '', '午餐': '速冻水饺', '晚餐': '番茄炒蛋', '加餐': '' }, conflict: false },
      { date: '周日 27', meals: { '早餐': '苹果酸奶杯', '午餐': '', '晚餐': '虾仁滑蛋', '加餐': '' }, conflict: false }
    ]
  },

  /* 购物清单（PLAN-06 三组） */
  shopping: [
    { id: 'sp1', name: '面条',     qty: '1',   unit: '把', group: '待购买', source: '菜谱缺失', checked: false },
    { id: 'sp2', name: '黑胡椒',   qty: '1',   unit: '瓶', group: '待购买', source: '菜谱缺失', checked: false },
    { id: 'sp3', name: '吐司面包', qty: '1',   unit: '袋', group: '待购买', source: '菜单计划', checked: false },
    { id: 'sp4', name: '鲜牛奶',   qty: '2',   unit: '盒', group: '建议购买', source: '低库存',  checked: false },
    { id: 'sp5', name: '鸡蛋',     qty: '10',  unit: '个', group: '建议购买', source: 'Agent建议', checked: false },
    { id: 'sp6', name: '青菜',     qty: '1',   unit: '把', group: '已购买', source: '手动添加', checked: true }
  ],

  /* 定时器 / 闹钟（MORE-02/03、CTX-07、OV-09） */
  timers: [
    { id: 't1', kind: '倒计时', name: '煮蛋',         remain: '08:00', total: '10:00', state: 'running', source: '菜谱·番茄炒蛋', linked: 'r1' },
    { id: 't2', kind: '倒计时', name: '焯排骨',       remain: '03:30', total: '05:00', state: 'running', source: '手动',          linked: '' },
    { id: 't3', kind: '闹钟',   name: '早餐提醒',     at: '07:30',     repeat: '每天', state: 'armed',   source: '手动',          linked: '' },
    { id: 't4', kind: '周期提醒', name: '检查临期食材', at: '每周日 20:00', state: 'armed',               source: 'Agent',         linked: '' },
    { id: 't5', kind: '倒计时', name: '蒸鱼',         remain: '00:00', total: '12:00', state: 'done',    source: '菜谱',          linked: '' },
    { id: 't6', kind: '倒计时', name: '解冻鸡胸肉',   remain: '00:00', total: '30:00', state: 'done',    source: '手动',          linked: '' }
  ],

  /* INV-06 拍照识别候选（演示置信度/日期估算/同名合并建议） */
  recognize: [
    { id: 'rc1', name: '西红柿',   qty: '3', unit: '个', confidence: 0.93, estimated: false, keep: true,  suggestLoc: '果蔬抽屉/抽屉', mergeWith: '',           warnings: [] },
    { id: 'rc2', name: '原味酸奶', qty: '2', unit: '杯', confidence: 0.87, estimated: true,  keep: true,  suggestLoc: '右冷藏/第2层/B3', mergeWith: '原味酸奶(库存4杯)', warnings: ['日期为估算', '存在同名批次，建议合并'] },
    { id: 'rc3', name: '某绿叶菜', qty: '1', unit: '把', confidence: 0.61, estimated: true,  keep: false, suggestLoc: '稍后补充', mergeWith: '',           warnings: ['置信度较低，请确认名称', '日期为估算'] }
  ],

  /* MORE-04 设备与传感器 */
  devices: [
    { name: 'ESP32-P4 主控', status: 'ok',  desc: 'rev1.3 · 360MHz · 已运行 3天4小时' },
    { name: 'ESP32-C6 协处理器', status: 'ok', desc: 'esp-hosted v1.4.2 · SDIO 连接正常' },
    { name: 'TF 存储卡',    status: 'ok',   desc: '29.1G 总量 · 剩余 26.4G' },
    { name: 'OV5647 摄像头', status: 'ok',  desc: 'MIPI-CSI · 1080p 抓拍可用' },
    { name: 'ES8311 音频编解码', status: 'ok', desc: '麦克采集 / 功放播放正常' },
    { name: '麦克风阵列',   status: 'warn', desc: 'FY-SP703 未插入，使用板载麦克风' }
  ],
  sensors: [
    { name: '冷藏温度', value: '4.2°C',  status: 'ok' },
    { name: '内部湿度', value: '38%',    status: 'ok' },
    { name: '环境光照', value: '320lux', status: 'ok' },
    { name: '门体姿态', value: '关闭',   status: 'ok' },
    { name: '人体存在', value: '2.1m',   status: 'ok' }
  ],
  safetyEvents: [
    { level: 'warn', title: '门未关严提醒',       time: '昨天 21:14', confirmed: false, desc: '门磁检测门体未完全闭合，持续 3 分钟' },
    { level: 'ok',   title: '冷藏温度恢复正常',   time: '07-20 08:02', confirmed: true,  desc: '温度由 7.8°C 回落到 4.2°C' }
  ],

  /* 设置中心分组（MORE-05） */
  wifiList: [
    { ssid: 'Home_2.4G',  rssi: -58, secured: true, connected: true },
    { ssid: 'Kitchen_5G', rssi: -72, secured: true, connected: false },
    { ssid: 'Fridge_IoT', rssi: -80, secured: true, connected: false },
    { ssid: 'Campus_IoT', rssi: -85, secured: true, connected: false }
  ],
  settings: [
    { group: '网络与同步', rows: [
      { label: 'Wi-Fi 网络',   type: 'value',  value: 'Home_2.4G · -58dBm', desc: '点击重新配置' },
      { label: '云端同步',     type: 'switch', value: true,  desc: '库存/菜单/会话与家庭云端同步' },
      { label: 'MQTT 推送',    type: 'switch', value: true,  desc: '状态变化实时推送到小程序' },
      { label: '离线模式',     type: 'switch', value: false, desc: '仅本地运行，不发起任何网络请求' }
    ]},
    { group: 'AI Provider 与模型', rows: [
      { label: '对话模型',     type: 'value',  value: 'GLM-4-Air · 在线', desc: '服务状态正常' },
      { label: '视觉识别模型', type: 'value',  value: 'GLM-4V · 在线',    desc: '服务状态正常' },
      { label: '语音合成',     type: 'value',  value: 'MOSS-TTSD',        desc: '' },
      { label: '测试连接',     type: 'btn',    value: '立即测试',          desc: '检测 API Key 与网络连通性' }
    ]},
    { group: 'Agent 权限', rows: [
      { label: '权限模式',     type: 'seg',    value: '标准模式', options: ['确认模式', '标准模式', '自动模式'], desc: '标准模式：低风险自动执行，高影响需确认' },
      { label: '允许自动建提醒', type: 'switch', value: true,  desc: '' },
      { label: '允许自动更新库存', type: 'switch', value: false, desc: '开启后低风险库存变更无需确认' }
    ]},
    { group: '语音与显示', rows: [
      { label: '语音唤醒',     type: 'switch', value: true,  desc: '唤醒词「小冰小冰」' },
      { label: '屏幕亮度',     type: 'slider', value: 80,    desc: '' },
      { label: '夜间模式',     type: 'switch', value: true,  desc: '23:00-6:00 自动降低亮度并熄屏' },
      { label: '待机颜文字',   type: 'switch', value: true,  desc: '无人时显示低功耗待机画面' }
    ]},
    { group: '隐私与家庭', rows: [
      { label: '拍照本地留存', type: 'switch', value: false, desc: '识别完成后删除本地照片' },
      { label: '家庭成员',     type: 'value',  value: '3 人已绑定', desc: '通过小程序管理' },
      { label: '设备绑定',     type: 'value',  value: '已绑定「我家厨房」', desc: '' }
    ]}
  ],

  /* MORE-06 维护中心 */
  plugins: [
    { name: '临期提醒',     ver: '1.2.0', on: true,  perm: '读库存 / 发通知' },
    { name: '菜谱生成器',   ver: '0.9.1', on: true,  perm: '读库存 / 联网' },
    { name: '冰箱布局向导', ver: '1.0.3', on: false, perm: '写布局' }
  ],
  logs: [
    { time: '14:31:02', level: 'INFO', text: 'inventory sync ok, 20 items' },
    { time: '14:25:47', level: 'INFO', text: 'tool menu_generate done in 2.3s' },
    { time: '14:20:11', level: 'INFO', text: 'tool inventory_query ok (0.4s)' },
    { time: '13:58:30', level: 'WARN', text: 'fy_sp703 not found, fallback to onboard mic' },
    { time: '13:02:14', level: 'INFO', text: 'wake word detected (小冰小冰)' },
    { time: '12:40:09', level: 'ERR',  text: 'mqtt publish retry 1/3 (timeout)' }
  ],
  ota: { current: 'v0.9.3-P4', latest: 'v0.9.4-P4', state: '发现新版本', slot: 'ota_0', size: '2.9MB' },
  storageDirs: [
    { path: '/sdcard/agent/memory',    size: '12MB',  status: 'ok' },
    { path: '/sdcard/agent/inventory', size: '3MB',   status: 'ok' },
    { path: '/sdcard/agent/workspace', size: '128MB', status: 'ok' },
    { path: '/sdcard/agent/tasks',     size: '2MB',   status: 'ok' },
    { path: '/sdcard/agent/plugins',   size: '45MB',  status: 'ok' },
    { path: '/sdcard/agent/logs',      size: '210MB', status: 'warn' },
    { path: '/sdcard/agent/cache',     size: '1.8G',  status: 'ok' }
  ],

  /* SYS-01 开机自检清单 */
  bootChecks: [
    { name: '屏幕显示',     status: 'ok' },
    { name: '触摸',         status: 'ok' },
    { name: 'TF 存储卡',    status: 'ok' },
    { name: 'ESP32-C6 网络', status: 'ok' },
    { name: '音频编解码',   status: 'ok' },
    { name: '摄像头',       status: 'ok' },
    { name: '传感器',       status: 'warn', note: 'FY-SP703 未插入，已切换板载麦克风' }
  ],

  /* SYS-03 待机颜文字（纯文本，不依赖图片资源） */
  kaomoji: ['^_^', '-_-', 'o_o', '=w=', '^o^', 'u_u', '>_<'],
  standbyHint: '叫我「小冰小冰」，或轻触屏幕',

  /* HOME 页 Agent 主动建议 */
  agentTips: [
    { id: 'tip1', text: '卤牛肉今天到期，晚餐推荐「卤牛肉拌面」，缺少面条和青菜，要加入购物清单吗？', act: '加入购物清单', page: 'PLAN-06' },
    { id: 'tip2', text: '吐司面包已过期 3 天，需要我帮你从库存中移除吗？', act: '去确认', page: 'CHAT-04' }
  ]
};

/* ------------------------------------------------------------------
 * FS.dataQ —— 数据查询助手（页面/侧舱统一使用，避免各处重复实现）
 * ------------------------------------------------------------------ */
FS.dataQ = {
  zone: function (id) {
    return FS.data.layout.zones.find(function (z) { return z.id === id; }) || null;
  },
  zoneName: function (id) {
    var z = this.zone(id);
    return z ? z.name : '';
  },
  layer: function (zoneId, layerId) {
    var z = this.zone(zoneId);
    if (!z) return null;
    return z.layers.find(function (l) { return l.id === layerId; }) || null;
  },
  layerName: function (zoneId, layerId) {
    var l = this.layer(zoneId, layerId);
    return l ? l.name : '';
  },
  /* 位置全路径文案，如 "右冷藏/第2层/B3"；未定位返回占位文案 */
  locPath: function (item) {
    if (!item.loc) return '位置待补充';
    var parts = [this.zoneName(item.loc.zone)];
    if (item.loc.layer) parts.push(this.layerName(item.loc.zone, item.loc.layer));
    /* L3 恒为单位格，直接取格号（历史数据可能存多个，按读序拼接） */
    if (item.loc.cells && item.loc.cells.length) {
      parts.push(item.loc.cells.join('+'));
    }
    return parts.join('/');
  },
  /* 按范围取食材：{} 全部 | {zone} | {zone,layer} | {zone,layer,cell} */
  itemsIn: function (scope) {
    scope = scope || {};
    return FS.data.items.filter(function (it) {
      if (!scope.zone) return true;
      if (!it.loc || it.loc.zone !== scope.zone) return false;
      if (!scope.layer) return true;
      if (it.loc.layer !== scope.layer) return false;
      if (!scope.cell) return true;
      return it.loc.cells && it.loc.cells.indexOf(scope.cell) >= 0;
    });
  },
  item: function (id) {
    return FS.data.items.find(function (it) { return it.id === id; }) || null;
  },
  /* 临期（含今天，默认 3 天内，未过期）按剩余天数升序 */
  expiring: function (n) {
    n = (n === undefined) ? 3 : n;
    return FS.data.items
      .filter(function (it) { return it.daysLeft >= 0 && it.daysLeft <= n; })
      .sort(function (a, b) { return a.daysLeft - b.daysLeft; });
  },
  expired: function () {
    return FS.data.items.filter(function (it) { return it.daysLeft < 0; });
  },
  unlocated: function () {
    return FS.data.items.filter(function (it) { return !it.loc; });
  },
  /* 剩余天数文案 */
  daysText: function (d) {
    if (d < 0) return '已过期' + (-d) + '天';
    if (d === 0) return '今天到期';
    if (d === 1) return '明天到期';
    return d + '天后';
  },
  /* 剩余天数语义色：danger=过期/今天 warn=明天起3天内 ok=更久 */
  daysTone: function (d) {
    if (d <= 0) return 'danger';
    if (d <= 3) return 'warn';
    return 'ok';
  },
  recipe: function (id) {
    return FS.data.recipes.find(function (r) { return r.id === id; }) || null;
  },
  /* 某范围内食材统计：总数 / 临期数 / 未定位数（INV-01、CTX-02 用） */
  stats: function (scope) {
    var list = this.itemsIn(scope);
    return {
      total: list.length,
      expiring: list.filter(function (it) { return it.daysLeft >= 0 && it.daysLeft <= 3; }).length,
      expired: list.filter(function (it) { return it.daysLeft < 0; }).length,
      unlocated: scope.zone ? 0 : this.unlocated().length
    };
  }
};
