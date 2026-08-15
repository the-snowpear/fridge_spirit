// 智谱 GLM Realtime 设备端调试页。
// 浏览器仅通过 USB JSON Lines 展示对话、配置设备；不会连接智谱或保存 API Key。

import { FormEvent, useCallback, useEffect, useRef, useState } from "react";
import {
  Bot,
  KeyRound,
  Mic,
  Play,
  RefreshCw,
  Save,
  Send,
  Square,
  Trash2,
  Volume2,
  Wrench,
  X,
} from "lucide-react";
import type {
  ConnectionState,
  DeviceCommand,
  RealtimeConfig,
  RealtimeEventPayload,
  RealtimeStatus,
} from "./types";

const defaultConfig: RealtimeConfig = {
  url: "wss://open.bigmodel.cn/api/paas/v4/realtime",
  model: "glm-realtime-flash",
  voice: "tongtong",
  instructions: "你是冰箱小精灵，厨房语音助手。用简短中文回答。",
  vadMode: "server_vad",
  timeoutMs: 30000,
  enabled: true,
  hasApiKey: false,
  ready: false,
  apiKeyPreview: "",
  lastError: "",
};

const realtimeInstructionsMaxBytes = 2048;
const maxConversationEntries = 60;

type LogLine = { id: string; at: string; text: string; level: "info" | "warn" | "error" };
type ConversationEntry = {
  id: string;
  role: "user" | "assistant";
  text: string;
  at: string;
  source: "voice" | "text";
  status: "pending" | "streaming" | "done" | "interrupted" | "error";
};

type Props = {
  connection: ConnectionState;
  sendCommand: <T>(command: DeviceCommand, payload?: unknown) => Promise<{ ok: boolean; payload: T; error?: string }>;
};

function nowTime() {
  return new Intl.DateTimeFormat("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  }).format(new Date());
}

function appendBounded(current: ConversationEntry[], entry: ConversationEntry) {
  return [...current, entry].slice(-maxConversationEntries);
}

function formatBytes(value = 0) {
  if (value < 1024) return `${value} B`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
  return `${(value / 1024 / 1024).toFixed(1)} MiB`;
}

function realtimeStateLabel(state?: string) {
  switch (state) {
    case "connecting": return "连接中";
    case "session_ready": return "会话就绪";
    case "listening": return "正在聆听";
    case "responding": return "正在思考";
    case "tool_call": return "调用工具";
    case "speaking": return "正在回复";
    case "error": return "异常";
    case "idle": return "空闲";
    default: return state || "等待设备";
  }
}

export function GlmRealtimePage({ connection, sendCommand }: Props) {
  const [config, setConfig] = useState<RealtimeConfig>(defaultConfig);
  const [status, setStatus] = useState<RealtimeStatus | null>(null);
  const [apiKeyInput, setApiKeyInput] = useState("");
  const [textInput, setTextInput] = useState("");
  const [busy, setBusy] = useState(false);
  const [conversation, setConversation] = useState<ConversationEntry[]>([]);
  const [toolLog, setToolLog] = useState<string[]>([]);
  const [events, setEvents] = useState<LogLine[]>([]);
  const refreshInFlightRef = useRef(false);
  const activeAssistantIdRef = useRef<string | null>(null);
  const assistantSourceRef = useRef<"text" | "audio" | null>(null);
  const pendingVoiceUserIdRef = useRef<string | null>(null);
  const conversationEndRef = useRef<HTMLDivElement | null>(null);

  const pushEvent = useCallback((text: string, level: LogLine["level"] = "info") => {
    setEvents((current) => [
      { id: crypto.randomUUID(), at: nowTime(), text, level },
      ...current,
    ].slice(0, 80));
  }, []);

  const updateConversation = useCallback((id: string, update: Partial<ConversationEntry>) => {
    setConversation((current) => current.map((entry) => entry.id === id ? { ...entry, ...update } : entry));
  }, []);

  const ensureAssistantEntry = useCallback(() => {
    if (activeAssistantIdRef.current) return activeAssistantIdRef.current;
    const id = crypto.randomUUID();
    activeAssistantIdRef.current = id;
    assistantSourceRef.current = null;
    setConversation((current) => appendBounded(current, {
      id,
      role: "assistant",
      text: "正在生成回复…",
      at: nowTime(),
      source: "voice",
      status: "pending",
    }));
    return id;
  }, []);

  const ensureVoiceUserEntry = useCallback(() => {
    if (pendingVoiceUserIdRef.current) return pendingVoiceUserIdRef.current;
    const id = crypto.randomUUID();
    pendingVoiceUserIdRef.current = id;
    setConversation((current) => appendBounded(current, {
      id,
      role: "user",
      text: "正在识别语音…",
      at: nowTime(),
      source: "voice",
      status: "pending",
    }));
    return id;
  }, []);

  const finishAssistant = useCallback((statusText?: string) => {
    const id = activeAssistantIdRef.current;
    if (!id) return;
    setConversation((current) => current.map((entry) => {
      if (entry.id !== id) return entry;
      const hasContent = entry.text && entry.text !== "正在生成回复…";
      const interrupted = statusText?.includes("cancel") ?? false;
      return {
        ...entry,
        text: hasContent ? entry.text : interrupted ? "本轮回复已打断" : "本轮已结束，服务端未返回可显示的文字转写",
        status: interrupted ? "interrupted" : "done",
      };
    }));
  }, []);

  const refresh = useCallback(async () => {
    if (connection !== "connected" || refreshInFlightRef.current) return;
    refreshInFlightRef.current = true;
    try {
      const cfg = await sendCommand<RealtimeConfig>("get_realtime_config");
      if (cfg.ok && cfg.payload) setConfig({ ...defaultConfig, ...cfg.payload });
      const currentStatus = await sendCommand<RealtimeStatus>("realtime_status");
      if (currentStatus.ok && currentStatus.payload) setStatus(currentStatus.payload);
    } catch (error) {
      pushEvent(`刷新失败：${error instanceof Error ? error.message : String(error)}`, "warn");
    } finally {
      refreshInFlightRef.current = false;
    }
  }, [connection, pushEvent, sendCommand]);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  useEffect(() => {
    conversationEndRef.current?.scrollIntoView({ behavior: "smooth", block: "nearest" });
  }, [conversation]);

  // Realtime 事件由 App 从 USB JSON Lines 转为 window 自定义事件。
  useEffect(() => {
    const onRealtimeEvent = (event: Event) => {
      const detail = (event as CustomEvent<RealtimeEventPayload & { event?: string }>).detail;
      if (!detail) return;
      const name = detail.event || "realtime";

      if (name === "realtime_state" && detail.summary === "speech_started") {
        ensureVoiceUserEntry();
      }
      if (name === "realtime_state" &&
          (detail.summary === "speech_stopped" || detail.summary === "audio_committed") &&
          pendingVoiceUserIdRef.current) {
        updateConversation(pendingVoiceUserIdRef.current, {
          text: detail.summary === "audio_committed"
            ? "语音已提交，等待模型回复…"
            : "语音结束，正在识别…",
          status: "streaming",
        });
      }
      if (name === "realtime_state" && detail.summary === "response.created") {
        // 智谱不同事件类型之间不保证先后顺序。上一轮 done 后仍保留活动项，
        // 以接住迟到的 transcript.done；只有新响应创建时才切到新的助手消息。
        activeAssistantIdRef.current = null;
        assistantSourceRef.current = null;
        ensureAssistantEntry();
      }

      if (detail.text && detail.role === "user") {
        const id = pendingVoiceUserIdRef.current || ensureVoiceUserEntry();
        updateConversation(id, {
          text: detail.text,
          status: detail.isFinal ? "done" : "streaming",
        });
        if (detail.isFinal) pendingVoiceUserIdRef.current = null;
      } else if (detail.text && (detail.role === "assistant" || name.includes("text"))) {
        const id = ensureAssistantEntry();
        const audioTranscript = name === "realtime_transcript";
        const nextSource: "text" | "audio" = audioTranscript ? "audio" : "text";

        // 音频转写更接近用户实际听到的内容；一旦到达，就替代并忽略并行文本流，避免重复。
        if (nextSource === "text" && assistantSourceRef.current === "audio") {
          // 已切换到音频转写时忽略迟到的文本事件。
        } else {
          const sourceChanged = assistantSourceRef.current !== nextSource;
          assistantSourceRef.current = nextSource;
          setConversation((current) => current.map((entry) => {
            if (entry.id !== id) return entry;
            const replace = detail.isFinal || sourceChanged || entry.status === "pending";
            return {
              ...entry,
              text: replace ? detail.text! : `${entry.text}${detail.text}`,
              source: nextSource === "audio" ? "voice" : "text",
              status: detail.isFinal ? "done" : "streaming",
            };
          }));
        }
      }

      if (detail.tool) {
        setToolLog((current) => [
          `${nowTime()} · ${detail.tool} · ${detail.status || detail.summary || detail.arguments || "处理中"}`,
          ...current,
        ].slice(0, 40));
      }

      if (name === "realtime_done") finishAssistant(detail.status);
      if (name === "realtime_error" && pendingVoiceUserIdRef.current) {
        updateConversation(pendingVoiceUserIdRef.current, {
          text: detail.summary || detail.text || "语音识别失败",
          status: "error",
        });
        pendingVoiceUserIdRef.current = null;
      }

      pushEvent(
        `${name}: ${detail.state || ""} ${detail.summary || detail.text || detail.tool || detail.code || detail.status || ""}`.trim(),
        name.includes("error") ? "error" : "info",
      );
    };

    window.addEventListener("fridge-realtime-event", onRealtimeEvent as EventListener);
    const timer = window.setInterval(() => {
      if (connection === "connected") void refresh();
    }, 3000);
    return () => {
      window.removeEventListener("fridge-realtime-event", onRealtimeEvent as EventListener);
      window.clearInterval(timer);
    };
  }, [connection, ensureAssistantEntry, ensureVoiceUserEntry, finishAssistant, pushEvent, refresh, updateConversation]);

  useEffect(() => {
    if (connection === "connected") return;
    setConversation((current) => current.map((entry) =>
      entry.status === "pending" || entry.status === "streaming"
        ? { ...entry, status: "interrupted" as const }
        : entry,
    ));
    activeAssistantIdRef.current = null;
    pendingVoiceUserIdRef.current = null;
    assistantSourceRef.current = null;
  }, [connection]);

  const saveConfig = async (event: FormEvent) => {
    event.preventDefault();
    if (connection !== "connected") return;
    if (new TextEncoder().encode(config.instructions).length > realtimeInstructionsMaxBytes) {
      pushEvent(`系统指令不能超过 ${realtimeInstructionsMaxBytes} 个 UTF-8 字节`, "error");
      return;
    }
    setBusy(true);
    try {
      const payload: Record<string, unknown> = {
        url: config.url,
        model: config.model,
        voice: config.voice,
        instructions: config.instructions,
        timeoutMs: config.timeoutMs,
        enabled: config.enabled,
        vadMode: "server_vad",
      };
      if (apiKeyInput.trim()) payload.apiKey = apiKeyInput.trim();
      const result = await sendCommand<RealtimeConfig>("set_realtime_config", payload);
      if (result.ok && result.payload) {
        setConfig({ ...defaultConfig, ...result.payload });
        setApiKeyInput("");
        pushEvent("Realtime 配置已保存（Key 仅存设备 NVS）");
      } else {
        pushEvent(result.error || "保存失败", "error");
      }
    } catch (error) {
      pushEvent(error instanceof Error ? error.message : String(error), "error");
    } finally {
      setBusy(false);
    }
  };

  const clearKey = async () => {
    if (connection !== "connected") return;
    setBusy(true);
    try {
      const result = await sendCommand<RealtimeConfig>("clear_realtime_key");
      if (result.ok && result.payload) {
        setConfig({ ...defaultConfig, ...result.payload });
        pushEvent("已清除设备端 Realtime API Key");
      } else {
        pushEvent(result.error || "清除失败", "error");
      }
    } catch (error) {
      pushEvent(error instanceof Error ? error.message : String(error), "error");
    } finally {
      setBusy(false);
    }
  };

  const runAsync = async (command: DeviceCommand, payload?: unknown, label?: string) => {
    if (connection !== "connected") return false;
    setBusy(true);
    try {
      const result = await sendCommand<{ accepted?: boolean }>(command, payload);
      pushEvent(`${label || command}: ${result.ok ? "accepted" : result.error || "failed"}`, result.ok ? "info" : "error");
      if (result.ok && command === "realtime_stop") {
        finishAssistant("response.cancelled");
      }
      await refresh();
      return result.ok;
    } catch (error) {
      pushEvent(error instanceof Error ? error.message : String(error), "error");
      return false;
    } finally {
      setBusy(false);
    }
  };

  const sendText = async (event: FormEvent) => {
    event.preventDefault();
    const text = textInput.trim();
    if (!text || busy || connection !== "connected") return;
    const id = crypto.randomUUID();
    setConversation((current) => appendBounded(current, {
      id,
      role: "user",
      text,
      at: nowTime(),
      source: "text",
      status: "done",
    }));
    setTextInput("");
    const ok = await runAsync("realtime_send_text", { text }, "realtime_send_text");
    if (!ok) updateConversation(id, { status: "error" });
  };

  const clearConversation = () => {
    setConversation([]);
    activeAssistantIdRef.current = null;
    pendingVoiceUserIdRef.current = null;
    assistantSourceRef.current = null;
  };

  const connected = connection === "connected";
  const sessionActive = Boolean(status?.sessionActive);
  const instructionBytes = new TextEncoder().encode(config.instructions).length;

  return (
    <div className="realtime-page">
      <section className="realtime-overview">
        <div className="realtime-title-row">
          <div>
            <span className="realtime-eyebrow">GLM Realtime · ESP32-P4</span>
            <h2>实时对话</h2>
            <p>语音和文本都由设备直连模型；本页只通过 USB 展示逐轮内容和运行状态。</p>
          </div>
          <div className={`realtime-live-state ${status?.state === "error" ? "error" : sessionActive ? "active" : ""}`}>
            <span className="status-dot" />
            <strong>{realtimeStateLabel(status?.state)}</strong>
            <small>{sessionActive ? `${config.model} · ${config.voice}` : connected ? "设备已连接" : "等待 USB"}</small>
          </div>
        </div>
        <div className="realtime-status-grid">
          <div><span>会话</span><strong>{sessionActive ? "进行中" : "空闲"}</strong></div>
          <div><span>音频</span><strong>{status?.audioUpActive ? "MIC" : "—"} / {status?.audioDownActive ? "SPK" : "—"}</strong></div>
          <div><span>传输</span><strong>↑ {formatBytes(status?.audioUpBytes)} / ↓ {formatBytes(status?.audioDownBytes)}</strong></div>
          <div><span>工具</span><strong>{status?.toolsExported ?? 0} 个 · {status?.toolCallsOk ?? 0} 成功</strong></div>
          <div><span>API Key</span><strong>{config.hasApiKey ? config.apiKeyPreview || "已保存" : "未配置"}</strong></div>
        </div>
        {status?.lastError ? <div className="realtime-alert">最近错误：{status.lastError}</div> : null}
      </section>

      <section className="realtime-workspace">
        <div className="realtime-conversation-panel">
          <div className="realtime-section-head">
            <div>
              <h3>对话预览</h3>
              <p>用户语音转写与助手实际播报文本会在这里实时更新。</p>
            </div>
            <button className="action-button secondary" type="button" disabled={conversation.length === 0} onClick={clearConversation}>
              <Trash2 size={16} /> 清空预览
            </button>
          </div>

          <div className="realtime-conversation-stream" aria-live="polite">
            {conversation.map((entry) => (
              <article className={`realtime-message ${entry.role} ${entry.status}`} key={entry.id}>
                <div className="realtime-message-meta">
                  <span>{entry.role === "user" ? "你" : "小精灵"}</span>
                  <small>{entry.source === "voice" ? "语音" : "文本"} · {entry.at}</small>
                </div>
                <p>{entry.text}</p>
                {(entry.status === "pending" || entry.status === "streaming") && <i aria-label="内容生成中" />}
              </article>
            ))}
            {conversation.length === 0 && (
              <div className="realtime-empty">
                <Bot size={28} />
                <strong>等待第一轮对话</strong>
                <span>启动会话后直接对着板载麦克风说话，或在下方发送诊断文本。</span>
              </div>
            )}
            <div ref={conversationEndRef} />
          </div>

          <div className="realtime-session-controls">
            <button className="action-button" type="button" disabled={busy || !connected || sessionActive || !config.ready} onClick={() => void runAsync("realtime_start", undefined, "realtime_start")}>
              <Play size={16} /> 启动会话
            </button>
            <button className="action-button secondary" type="button" disabled={busy || !connected || !sessionActive} onClick={() => void runAsync("realtime_cancel", undefined, "realtime_cancel")}>
              <X size={16} /> 打断回复
            </button>
            <button className="action-button secondary danger-soft" type="button" disabled={busy || !connected || !sessionActive} onClick={() => void runAsync("realtime_stop", undefined, "realtime_stop")}>
              <Square size={15} /> 停止会话
            </button>
          </div>

          <form className="realtime-composer" onSubmit={(event) => void sendText(event)}>
            <input value={textInput} onChange={(event) => setTextInput(event.target.value)} maxLength={512} placeholder={sessionActive ? "输入诊断文本…" : "请先启动会话"} />
            <button className="action-button" type="submit" disabled={busy || !connected || !sessionActive || !textInput.trim()}>
              <Send size={16} /> 发送
            </button>
          </form>
        </div>

        <aside className="realtime-side-panel">
          <section>
            <div className="realtime-side-title"><Mic size={17} /><h3>音频链路</h3></div>
            <dl className="realtime-facts">
              <div><dt>麦克风上行</dt><dd>{status?.audioUpActive ? "运行中" : "未启动"}</dd></div>
              <div><dt>扬声器下行</dt><dd>{status?.audioDownActive ? "播放中" : "等待音频"}</dd></div>
              <div><dt>最近事件</dt><dd>{status ? `${status.lastEventAgeMs} ms` : "—"}</dd></div>
            </dl>
          </section>
          <section>
            <div className="realtime-side-title"><Wrench size={17} /><h3>工具调用</h3></div>
            <div className="realtime-tool-list">
              {toolLog.length ? toolLog.slice(0, 8).map((line, index) => <p key={`${line}-${index}`}>{line}</p>) : <span>本次会话尚未调用工具</span>}
            </div>
            <small>成功 {status?.toolCallsOk ?? 0} / 失败 {status?.toolCallsFail ?? 0}；危险工具不会导出。</small>
          </section>
          <section>
            <div className="realtime-side-title"><Volume2 size={17} /><h3>当前模型</h3></div>
            <dl className="realtime-facts">
              <div><dt>模型</dt><dd>{status?.model || config.model}</dd></div>
              <div><dt>音色</dt><dd>{status?.voice || config.voice}</dd></div>
            </dl>
          </section>
        </aside>
      </section>

      <details className="realtime-config-panel">
        <summary>
          <span><KeyRound size={17} /><strong>设备端配置</strong></span>
          <small>NVS fridge_rt · API Key 不回显</small>
        </summary>
        <form onSubmit={(event) => void saveConfig(event)}>
          <div className="form-grid">
            <label>WSS URL<input value={config.url} onChange={(event) => setConfig((current) => ({ ...current, url: event.target.value }))} /></label>
            <label>模型<input value={config.model} onChange={(event) => setConfig((current) => ({ ...current, model: event.target.value }))} placeholder="glm-realtime-flash" /></label>
            <label>音色<input value={config.voice} onChange={(event) => setConfig((current) => ({ ...current, voice: event.target.value }))} placeholder="tongtong" /></label>
            <label>超时 ms<input type="number" min={10000} max={120000} value={config.timeoutMs} onChange={(event) => setConfig((current) => ({ ...current, timeoutMs: Number(event.target.value) }))} /></label>
          </div>
          <label>系统指令<textarea rows={4} value={config.instructions} onChange={(event) => setConfig((current) => ({ ...current, instructions: event.target.value }))} /></label>
          <div className={`form-hint ${instructionBytes > realtimeInstructionsMaxBytes ? "danger" : ""}`}>{instructionBytes} / {realtimeInstructionsMaxBytes} UTF-8 bytes</div>
          <label>API Key（仅写入设备）<input type="password" value={apiKeyInput} placeholder={config.hasApiKey ? "已保存，留空表示不修改" : "粘贴智谱 API Key"} onChange={(event) => setApiKeyInput(event.target.value)} autoComplete="off" /></label>
          <label className="checkbox-line"><input type="checkbox" checked={config.enabled} onChange={(event) => setConfig((current) => ({ ...current, enabled: event.target.checked }))} />启用 Realtime</label>
          <div className="button-row">
            <button className="action-button" type="submit" disabled={busy || !connected || instructionBytes > realtimeInstructionsMaxBytes}><Save size={16} /> 保存配置</button>
            <button className="action-button secondary danger-soft" type="button" disabled={busy || !connected || !config.hasApiKey} onClick={() => void clearKey()}><Trash2 size={16} /> 清除 Key</button>
            <button className="action-button secondary" type="button" disabled={busy || !connected} onClick={() => void refresh()}><RefreshCw size={16} /> 刷新状态</button>
          </div>
        </form>
      </details>

      <details className="realtime-events-panel">
        <summary><strong>协议事件</strong><small>{events.length} 条，仅用于诊断</small></summary>
        <ul>
          {events.map((line) => <li key={line.id} className={line.level}><time>{line.at}</time><span>{line.text}</span></li>)}
          {events.length === 0 && <li><span>尚无 Realtime 事件</span></li>}
        </ul>
      </details>
    </div>
  );
}
