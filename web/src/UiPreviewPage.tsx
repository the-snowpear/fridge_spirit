import { Monitor, Smartphone } from "lucide-react";
import { useMemo, useState } from "react";

/** 设备 UI 布局：横屏标准 / 竖屏隐私（与 ui-reference/ver2 的 FS.mode 对齐） */
export type UiPreviewLayout = "landscape" | "portrait";

const LAYOUT_META: Record<
  UiPreviewLayout,
  { label: string; size: string; hint: string; Icon: typeof Monitor }
> = {
  landscape: {
    label: "横屏标准",
    size: "1024×600",
    hint: "主区 | 导航 | 状态舱 · 摄像头可用（拍照登记等完整流程）",
    Icon: Monitor,
  },
  portrait: {
    label: "竖屏隐私",
    size: "600×1024",
    hint: "上功能区 / 中导航 / 下状态舱 · 隐私模式，摄像头关闭",
    Icon: Smartphone,
  },
};

/**
 * 运维面板内嵌 ver2 UI 参考预览。
 * 与设备串口无关：iframe 加载 /ui-ver2（Vite 托管 ui-reference/ver2）。
 */
export function UiPreviewPage() {
  const [layout, setLayout] = useState<UiPreviewLayout>("landscape");

  const src = useMemo(
    () => `/ui-ver2/index.html?layout=${layout}`,
    [layout],
  );

  const meta = LAYOUT_META[layout];

  return (
    <div className="ui-preview">
      <div className="ui-preview-toolbar">
        <div className="ui-preview-seg" role="group" aria-label="设备 UI 布局">
          {(Object.keys(LAYOUT_META) as UiPreviewLayout[]).map((key) => {
            const item = LAYOUT_META[key];
            const Icon = item.Icon;
            return (
              <button
                key={key}
                type="button"
                className={layout === key ? "on" : undefined}
                onClick={() => setLayout(key)}
                title={`${item.label} ${item.size}`}
              >
                <Icon size={16} />
                <span>
                  {item.label}
                  <small> {item.size}</small>
                </span>
              </button>
            );
          })}
        </div>
        <p className="ui-preview-hint">
          <strong>{meta.label}</strong>：{meta.hint}
          {" · "}
          纯 UI 参考，不依赖串口连接；iframe 内工具栏也可切换布局。
        </p>
      </div>
      <div className="ui-preview-frame-wrap">
        <iframe
          key={layout}
          className="ui-preview-frame"
          title={`冰箱小精灵 UI 预览 · ${meta.label}`}
          src={src}
        />
      </div>
    </div>
  );
}
