import {
  ArchiveRestore, ChevronRight, Download, File, FileAudio, FileImage, FileText,
  Folder, FolderPlus, Gauge, HardDrive, MoreHorizontal, RefreshCw, Save, Search,
  ShieldAlert, Trash2, Upload, Wrench,
} from "lucide-react";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type {
  DeviceCommand, DeviceResponse, TfAccessMode, TfCardStatus, TfFileEntry,
  TfHttpSession, TfListResponse, TfTransferSession,
} from "./types";

type SendCommand = <T = unknown>(command: DeviceCommand, payload?: unknown) => Promise<DeviceResponse<T>>;
type Tab = "files" | "trash" | "maintenance";
type QueueItem = { id: string; session?: number; name: string; direction: "upload" | "download"; progress: number; state: "running" | "paused" | "done" | "error" | "cancelled"; detail: string };

const SERIAL_LIMIT = 512 * 1024;
const SERIAL_CHUNK = 1024;
const MAX_FAT_FILE = 0xffff_ffff;

function formatBytes(value = 0) {
  if (!Number.isFinite(value) || value <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  const index = Math.min(Math.floor(Math.log(value) / Math.log(1024)), units.length - 1);
  return `${(value / 1024 ** index).toFixed(index > 1 ? 1 : 0)} ${units[index]}`;
}

function crc32(bytes: Uint8Array) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function bytesToBase64(bytes: Uint8Array) {
  let binary = "";
  for (let offset = 0; offset < bytes.length; offset += 8192) binary += String.fromCharCode(...bytes.subarray(offset, offset + 8192));
  return btoa(binary);
}

function base64ToBytes(value: string) {
  const binary = atob(value);
  return Uint8Array.from(binary, (character) => character.charCodeAt(0));
}

function joinPath(parent: string, name: string) {
  return parent ? `${parent}/${name}` : name;
}

function previewKind(entry: TfFileEntry | null) {
  const extension = entry?.name.split(".").pop()?.toLowerCase() ?? "";
  if (["txt", "log", "json", "md", "csv", "ini", "yaml", "yml"].includes(extension)) return "text";
  if (["png", "jpg", "jpeg", "gif", "webp", "bmp"].includes(extension)) return "image";
  if (["wav", "mp3", "ogg", "m4a"].includes(extension)) return "audio";
  if (["mp4", "webm", "mov"].includes(extension)) return "video";
  return "hex";
}

function previewMime(entry: TfFileEntry) {
  const extension = entry.name.split(".").pop()?.toLowerCase() ?? "";
  const types: Record<string, string> = {
    png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp", bmp: "image/bmp",
    wav: "audio/wav", mp3: "audio/mpeg", ogg: "audio/ogg", m4a: "audio/mp4",
    mp4: "video/mp4", webm: "video/webm", mov: "video/quicktime",
  };
  return types[extension] ?? "application/octet-stream";
}

export function TfCardPage({ connected, sendCommand }: { connected: boolean; sendCommand: SendCommand }) {
  const [tab, setTab] = useState<Tab>("files");
  const [status, setStatus] = useState<TfCardStatus | null>(null);
  const [path, setPath] = useState("");
  const [entries, setEntries] = useState<TfFileEntry[]>([]);
  const [cursor, setCursor] = useState<number | undefined>();
  const [hasMore, setHasMore] = useState(false);
  const [query, setQuery] = useState("");
  const [sort, setSort] = useState<"name" | "size" | "time">("name");
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [inspected, setInspected] = useState<TfFileEntry | null>(null);
  const [preview, setPreview] = useState("");
  const [previewUrl, setPreviewUrl] = useState("");
  const [editing, setEditing] = useState(false);
  const [busy, setBusy] = useState(false);
  const [notice, setNotice] = useState("");
  const [access, setAccess] = useState<TfAccessMode>({ expertMode: false });
  const [queue, setQueue] = useState<QueueItem[]>([]);
  const uploadRef = useRef<HTMLInputElement>(null);
  const cancelledQueue = useRef(new Set<string>());
  const pausedQueue = useRef(new Set<string>());
  const httpControllers = useRef(new Map<string, AbortController>());

  const command = useCallback(async <T,>(name: DeviceCommand, payload?: unknown) => {
    if (!connected) throw new Error("请先连接设备");
    const response = await sendCommand<T>(name, payload);
    if (!response.ok) throw new Error(response.error || `${name} 执行失败`);
    return response.payload;
  }, [connected, sendCommand]);

  const refreshStatus = useCallback(async () => {
    if (!connected) return;
    const [card, mode] = await Promise.all([
      command<TfCardStatus>("get_tfcard_status"),
      command<TfAccessMode>("tf_get_access_mode").catch(() => ({ expertMode: false })),
    ]);
    const totalBytes = card.totalBytes ?? (card.capacityMb ?? 0) * 1024 * 1024;
    const freeBytes = card.freeBytes ?? (card.freeMb ?? 0) * 1024 * 1024;
    setStatus({ ...card, totalBytes: totalBytes || 0, freeBytes: freeBytes || 0 });
    setAccess(mode);
  }, [command, connected]);

  const loadDirectory = useCallback(async (target = path, nextCursor?: number, append = false) => {
    if (!connected) return;
    setBusy(true);
    try {
      const payload = await command<TfListResponse>("tf_list", { path: target, cursor: nextCursor ?? 0, limit: 32 });
      const nextEntries = (payload.entries ?? [])
        .filter((entry) => !(tab === "trash" && entry.name.endsWith(".meta")))
        .map((entry) => ({
          ...entry,
          type: entry.isDir ? "directory" as const : "file" as const,
          modifiedAt: entry.mtime ?? entry.modifiedAt,
        }));
      setEntries((current) => append ? [...current, ...nextEntries] : nextEntries);
      setCursor(payload.nextCursor);
      setHasMore(Boolean(payload.hasMore));
      setPath(payload.path || target);
      if (!append) { setSelected(new Set()); setInspected(null); setPreview(""); setPreviewUrl(""); }
      setNotice("");
    } catch (error) { setNotice(error instanceof Error ? error.message : String(error)); }
    finally { setBusy(false); }
  }, [command, connected, path, tab]);

  useEffect(() => {
    if (!connected) { setStatus(null); setEntries([]); return; }
    void refreshStatus().catch((error) => setNotice(String(error)));
  }, [connected, refreshStatus]);

  useEffect(() => {
    if (connected && tab !== "maintenance") void loadDirectory(tab === "trash" ? ".fridge_spirit/trash" : "");
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [connected, tab]);

  useEffect(() => () => { if (previewUrl) URL.revokeObjectURL(previewUrl); }, [previewUrl]);

  const visible = useMemo(() => entries
    .filter((entry) => entry.name.toLocaleLowerCase().includes(query.toLocaleLowerCase()))
    .sort((a, b) => {
      if (a.type !== b.type) return a.type === "directory" ? -1 : 1;
      if (sort === "size") return b.size - a.size;
      if (sort === "time") return String(b.modifiedAt ?? "").localeCompare(String(a.modifiedAt ?? ""));
      return a.name.localeCompare(b.name, "zh-CN", { numeric: true });
    }), [entries, query, sort]);

  const used = Math.max(0, (status?.totalBytes ?? 0) - (status?.freeBytes ?? 0));
  const usage = status?.totalBytes ? Math.round(used / status.totalBytes * 100) : 0;

  async function mutate(name: DeviceCommand, payload: unknown, success: string) {
    setBusy(true);
    try { await command(name, payload); setNotice(success); await loadDirectory(path); await refreshStatus(); }
    catch (error) { setNotice(error instanceof Error ? error.message : String(error)); }
    finally { setBusy(false); }
  }

  async function createFolder() {
    const name = window.prompt("新目录名称");
    if (name?.trim()) await mutate("tf_mkdir", { path: joinPath(path, name.trim()) }, "目录已创建");
  }

  async function renameOrMove(entry: TfFileEntry) {
    const destination = window.prompt("输入新的 TF 根相对路径", entry.path);
    if (destination && destination !== entry.path) await mutate("tf_move", { from: entry.path, to: destination.replace(/^\/+/, ""), overwrite: false }, "移动完成");
  }

  async function deleteSelected() {
    const paths = selected.size ? [...selected] : inspected ? [inspected.path] : [];
    if (!paths.length || !window.confirm(`将 ${paths.length} 项移入回收站？`)) return;
    for (const target of paths) await command("tf_delete", { path: target, recursive: true });
    setNotice("已移入回收站"); await loadDirectory(path); await refreshStatus();
  }

  async function restoreSelected() {
    const paths = selected.size ? [...selected] : inspected ? [inspected.path] : [];
    for (const target of paths) {
      const name = target.split("/").at(-1) ?? target;
      const trashId = name.endsWith(".data") ? name.slice(0, -5) : name;
      await command("tf_restore", { trashId, overwrite: false });
    }
    setNotice("恢复完成"); await loadDirectory(path);
  }

  async function readBinary(entry: TfFileEntry, maxBytes?: number) {
    const wanted = Math.min(entry.size, maxBytes ?? entry.size);
    if (wanted === 0) return new Uint8Array();
    const useHttp = maxBytes === undefined && entry.size > SERIAL_LIMIT;
    const { session, http } = await beginTransfer("download", entry.path, entry.size, useHttp);
    let completed = false;
    try {
      if (http?.url && http.token) {
        const end = wanted ? wanted - 1 : 0;
        const response = await fetch(http.url, { headers: { Authorization: `Bearer ${http.token}`, Range: `bytes=0-${end}` } });
        if (!response.ok && response.status !== 206) throw new Error(`HTTP ${response.status}`);
        const bytes = new Uint8Array(await response.arrayBuffer());
        if (wanted === entry.size) await command("tf_transfer_end", { session: session.session });
        else await command("tf_transfer_abort", { session: session.session });
        completed = true;
        return bytes;
      }
      const parts: Uint8Array[] = [];
      let offset = 0;
      while (offset < wanted) {
        const result = await command<{ data_b64: string; crc32: number; length: number }>("tf_transfer_chunk", { session: session.session, offset });
        const chunk = base64ToBytes(result.data_b64).slice(0, wanted - offset);
        if (crc32(base64ToBytes(result.data_b64)) !== result.crc32) throw new Error("预览块 CRC32 校验失败");
        parts.push(chunk); offset += chunk.length;
      }
      if (wanted === entry.size) await command("tf_transfer_end", { session: session.session });
      else await command("tf_transfer_abort", { session: session.session });
      completed = true;
      const merged = new Uint8Array(parts.reduce((sum, item) => sum + item.length, 0));
      let cursor = 0; for (const part of parts) { merged.set(part, cursor); cursor += part.length; }
      return merged;
    } finally {
      if (!completed) await command("tf_transfer_abort", { session: session.session }).catch(() => undefined);
      if (http) await command("tf_http_disable").catch(() => undefined);
    }
  }

  async function readPreview(entry: TfFileEntry) {
    setInspected(entry); setEditing(false); setPreview("");
    if (previewUrl) { URL.revokeObjectURL(previewUrl); setPreviewUrl(""); }
    if (entry.type === "directory") return;
    try {
      const kind = previewKind(entry);
      if (kind === "text") {
        const result = await command<{ text: string }>("tf_text_read", { path: entry.path });
        setPreview(result.text ?? "");
      } else {
        const bytes = await readBinary(entry, kind === "hex" ? 64 * 1024 : undefined);
        if (kind === "hex") setPreview(bytesToBase64(bytes));
        else setPreviewUrl(URL.createObjectURL(new Blob([bytes], { type: previewMime(entry) })));
      }
    } catch (error) { setPreview(`读取失败：${error instanceof Error ? error.message : String(error)}`); }
  }

  async function saveText() {
    if (!inspected) return;
    const bytes = new TextEncoder().encode(preview);
    if (bytes.length > 256 * 1024) return;
    if (bytes.length < 900) {
      await mutate("tf_text_write", { path: inspected.path, text: preview, overwrite: true }, "文本已原子保存");
    } else {
      const session = await command<TfTransferSession>("tf_transfer_begin", {
        direction: "write", path: inspected.path, totalSize: bytes.length, overwrite: true,
      });
      let completed = false;
      try {
        for (let offset = 0; offset < bytes.length;) {
          const chunk = bytes.subarray(offset, Math.min(bytes.length, offset + SERIAL_CHUNK));
          const ack = await command<{ nextOffset: number }>("tf_transfer_chunk", {
            session: session.session, offset, data_b64: bytesToBase64(chunk), crc32: crc32(chunk),
          });
          offset = ack.nextOffset;
        }
        await command("tf_transfer_end", { session: session.session });
        completed = true;
      } finally {
        if (!completed) await command("tf_transfer_abort", { session: session.session }).catch(() => undefined);
      }
      setNotice("文本已通过分块传输原子保存");
      await loadDirectory(path);
    }
    setEditing(false);
  }

  function updateQueue(id: string, update: Partial<QueueItem>) { setQueue((items) => items.map((item) => item.id === id ? { ...item, ...update } : item)); }

  async function waitForQueue(id: string) {
    while (pausedQueue.current.has(id) && !cancelledQueue.current.has(id)) {
      await new Promise((resolve) => window.setTimeout(resolve, 100));
    }
    if (cancelledQueue.current.has(id)) throw new Error("用户已取消");
  }

  function toggleQueuePause(item: QueueItem) {
    if (item.state === "paused") {
      pausedQueue.current.delete(item.id);
      updateQueue(item.id, { state: "running", detail: "正在继续传输" });
    } else {
      pausedQueue.current.add(item.id);
      updateQueue(item.id, { state: "paused", detail: "已暂停，可继续当前会话" });
    }
  }

  function cancelQueue(item: QueueItem) {
    cancelledQueue.current.add(item.id);
    pausedQueue.current.delete(item.id);
    httpControllers.current.get(item.id)?.abort();
    if (item.session) void command("tf_transfer_abort", { session: item.session }).catch(() => undefined);
    updateQueue(item.id, { state: "cancelled", detail: "用户已取消" });
  }

  async function beginTransfer(direction: "upload" | "download", target: string, totalSize: number, useHttp: boolean) {
    const session = await command<TfTransferSession>("tf_transfer_begin", {
      direction: direction === "upload" ? "write" : "read",
      path: target,
      totalSize,
      overwrite: direction === "upload",
    });
    try {
      const http = useHttp ? await command<TfHttpSession>("tf_http_enable") : null;
      return { session, http };
    } catch (error) {
      await command("tf_transfer_abort", { session: session.session }).catch(() => undefined);
      throw error;
    }
  }

  async function uploadFile(file: File) {
    if (file.size >= MAX_FAT_FILE) { setNotice("FAT32 单文件必须小于 4 GiB"); return; }
    const id = crypto.randomUUID(); const target = joinPath(path, file.name);
    cancelledQueue.current.delete(id); pausedQueue.current.delete(id);
    setQueue((items) => [...items, { id, name: file.name, direction: "upload", progress: 0, state: "running", detail: "准备传输" }]);
    let activeSession: number | undefined;
    let httpEnabled = false;
    let completed = false;
    try {
      const useHttp = file.size > SERIAL_LIMIT;
      let transfer;
      try {
        transfer = await beginTransfer("upload", target, file.size, useHttp);
      } catch (error) {
        if (!useHttp) throw error;
        transfer = await beginTransfer("upload", target, file.size, false);
        setNotice("Wi-Fi HTTP 不可用，已降级为 COM15 串口传输");
      }
      const { session, http } = transfer;
      activeSession = session.session;
      httpEnabled = Boolean(http);
      updateQueue(id, { session: session.session });
      if (http?.url && http.token) {
        const controller = new AbortController();
        httpControllers.current.set(id, controller);
        for (let offset = session.offset || 0; offset < file.size;) {
          await waitForQueue(id);
          const end = Math.min(offset + 1024 * 1024, file.size);
          const response = await fetch(http.url, {
            method: "PUT",
            headers: { Authorization: `Bearer ${http.token}`, "Content-Type": "application/octet-stream", "X-Upload-Offset": String(offset) },
            body: file.slice(offset, end),
            signal: controller.signal,
          });
          if (!response.ok) throw new Error(`HTTP ${response.status}`);
          const ack = await response.json() as { nextOffset?: number };
          offset = ack.nextOffset ?? end;
          updateQueue(id, { progress: Math.round(offset / file.size * 100), detail: `${formatBytes(offset)} / ${formatBytes(file.size)} · Wi-Fi HTTP` });
        }
      } else {
        for (let offset = session.offset || 0; offset < file.size;) {
          await waitForQueue(id);
          const chunk = new Uint8Array(await file.slice(offset, Math.min(file.size, offset + SERIAL_CHUNK)).arrayBuffer());
          const ack = await command<{ nextOffset: number }>("tf_transfer_chunk", { session: session.session, offset, data_b64: bytesToBase64(chunk), crc32: crc32(chunk) });
          offset = ack.nextOffset;
          updateQueue(id, { progress: Math.round(offset / file.size * 100), detail: `${formatBytes(offset)} / ${formatBytes(file.size)} · COM15` });
        }
      }
      await command("tf_transfer_end", { session: session.session });
      completed = true;
      updateQueue(id, { progress: 100, state: "done", detail: `${http ? "Wi-Fi HTTP" : "COM 串口"} · 完成` });
      await loadDirectory(path); await refreshStatus();
    } catch (error) {
      if (!cancelledQueue.current.has(id)) updateQueue(id, { state: "error", detail: error instanceof Error ? error.message : String(error) });
    } finally {
      if (!completed && activeSession !== undefined) await command("tf_transfer_abort", { session: activeSession }).catch(() => undefined);
      if (httpEnabled) await command("tf_http_disable").catch(() => undefined);
      httpControllers.current.delete(id); pausedQueue.current.delete(id); cancelledQueue.current.delete(id);
    }
  }

  function uploadFiles(files: FileList | File[]) {
    [...files].forEach((file) => void uploadFile(file));
  }

  async function downloadEntry(entry: TfFileEntry) {
    const id = crypto.randomUUID();
    cancelledQueue.current.delete(id); pausedQueue.current.delete(id);
    setQueue((items) => [...items, { id, name: entry.name, direction: "download", progress: 0, state: "running", detail: "准备传输" }]);
    let activeSession: number | undefined;
    let httpEnabled = false;
    let completed = false;
    try {
      const useHttp = entry.size > SERIAL_LIMIT;
      let transfer;
      try {
        transfer = await beginTransfer("download", entry.path, entry.size, useHttp);
      } catch (error) {
        if (!useHttp) throw error;
        transfer = await beginTransfer("download", entry.path, entry.size, false);
        setNotice("Wi-Fi HTTP 不可用，已降级为 COM15 串口传输");
      }
      const { session, http } = transfer;
      activeSession = session.session;
      httpEnabled = Boolean(http);
      updateQueue(id, { session: session.session });
      let blob: Blob;
      if (http?.url && http.token) {
        const controller = new AbortController();
        httpControllers.current.set(id, controller);
        // 浏览器下载没有持久化旧前缀，始终从 0 开始，避免续传 offset 生成截断文件。
        const response = await fetch(http.url, { headers: { Authorization: `Bearer ${http.token}`, Range: "bytes=0-" }, signal: controller.signal });
        if (!response.ok && response.status !== 206) throw new Error(`HTTP ${response.status}`);
        const reader = response.body?.getReader();
        const chunks: Uint8Array[] = [];
        let received = 0;
        if (!reader) blob = await response.blob();
        else {
          while (true) {
            await waitForQueue(id);
            const { done, value } = await reader.read();
            if (done) break;
            chunks.push(value); received += value.length;
            updateQueue(id, { progress: Math.round(received / entry.size * 100), detail: `${formatBytes(received)} / ${formatBytes(entry.size)} · Wi-Fi HTTP` });
          }
          blob = new Blob(chunks as BlobPart[]);
        }
      } else {
        const chunks: Uint8Array[] = [];
        for (let offset = session.offset || 0; offset < entry.size;) {
          await waitForQueue(id);
          const result = await command<{ data_b64: string; crc32: number; length: number }>("tf_transfer_chunk", { session: session.session, offset });
          const chunk = base64ToBytes(result.data_b64);
          if (result.crc32 !== undefined && crc32(chunk) !== result.crc32) throw new Error("下载块 CRC32 校验失败");
          chunks.push(chunk); offset += result.length;
          updateQueue(id, { progress: Math.round(offset / entry.size * 100), detail: `${formatBytes(offset)} / ${formatBytes(entry.size)} · COM15` });
        }
        blob = new Blob(chunks as BlobPart[]);
      }
      await command("tf_transfer_end", { session: session.session });
      completed = true;
      const url = URL.createObjectURL(blob); const anchor = document.createElement("a"); anchor.href = url; anchor.download = entry.name; anchor.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
      updateQueue(id, { progress: 100, state: "done", detail: "下载完成" });
    } catch (error) {
      if (!cancelledQueue.current.has(id)) updateQueue(id, { state: "error", detail: error instanceof Error ? error.message : String(error) });
    } finally {
      if (!completed && activeSession !== undefined) await command("tf_transfer_abort", { session: activeSession }).catch(() => undefined);
      if (httpEnabled) await command("tf_http_disable").catch(() => undefined);
      httpControllers.current.delete(id); pausedQueue.current.delete(id); cancelledQueue.current.delete(id);
    }
  }

  async function toggleExpert() {
    if (!access.expertMode) {
      const confirm = window.prompt("输入 ENABLE_TF_EXPERT_MODE 启用专家模式");
      if (confirm !== "ENABLE_TF_EXPERT_MODE") return;
    }
    const next = await command<TfAccessMode>("tf_set_access_mode", { enabled: !access.expertMode, confirm: access.expertMode ? "" : "ENABLE_TF_EXPERT_MODE" });
    setAccess(next); setNotice(next.expertMode ? "专家模式已持久启用" : "已恢复保护模式");
  }

  async function maintenance(name: DeviceCommand, payload?: unknown) {
    setBusy(true); try { const result = await command<Record<string, unknown>>(name, payload); setNotice(JSON.stringify(result)); await refreshStatus(); }
    catch (error) { setNotice(error instanceof Error ? error.message : String(error)); } finally { setBusy(false); }
  }

  const crumbs = path.split("/").filter(Boolean);
  return (
    <div className="tf-workbench">
      {access.expertMode && <div className="tf-expert-banner"><ShieldAlert size={18} /><strong>专家模式已启用</strong><span>系统目录保护已解除，操作会持久生效。</span><button onClick={() => void toggleExpert()}>退出专家模式</button></div>}
      <div className="tf-summary">
        <div><span className={`tf-drive-icon ${status?.mounted ? "ready" : ""}`}><HardDrive /></span><div><strong>{status?.mounted ? "TF 卡已挂载" : "TF 卡未挂载"}</strong><small>{status?.filesystem || "FAT32"} · {status?.mountPoint || "/sdcard"}</small></div></div>
        <div className="tf-capacity"><span>{formatBytes(used)} / {formatBytes(status?.totalBytes)}</span><strong>{usage}%</strong><div><i style={{ width: `${usage}%` }} /></div></div>
        <div className="tf-summary-actions"><span>{formatBytes(status?.freeBytes)} 可用</span><button className="icon-button" disabled={!connected || busy} onClick={() => void Promise.all([refreshStatus(), loadDirectory(path)])}><RefreshCw size={17} className={busy ? "spin" : ""} /></button></div>
      </div>
      {notice && <div className="tf-notice">{notice}<button onClick={() => setNotice("")}>×</button></div>}
      <div className="tf-tabs">
        <button className={tab === "files" ? "active" : ""} onClick={() => setTab("files")}>文件</button>
        <button className={tab === "trash" ? "active" : ""} onClick={() => setTab("trash")}>回收站</button>
        <button className={tab === "maintenance" ? "active" : ""} onClick={() => setTab("maintenance")}>维护</button>
      </div>

      {tab !== "maintenance" ? <>
        <div className="tf-toolbar">
          <div className="tf-breadcrumb"><button onClick={() => void loadDirectory(tab === "trash" ? ".fridge_spirit/trash" : "")}>TF</button>{crumbs.map((crumb, index) => <span key={`${crumb}-${index}`}><ChevronRight size={14}/><button onClick={() => void loadDirectory(crumbs.slice(0, index + 1).join("/"))}>{crumb}</button></span>)}</div>
          <label className="tf-search"><Search size={16}/><input value={query} onChange={(event) => setQuery(event.target.value)} placeholder="筛选当前目录" /></label>
          <select value={sort} onChange={(event) => setSort(event.target.value as typeof sort)}><option value="name">按名称</option><option value="size">按大小</option><option value="time">按时间</option></select>
          {tab === "files" && <><button onClick={() => void createFolder()}><FolderPlus size={16}/>新建目录</button><button className="primary" onClick={() => uploadRef.current?.click()}><Upload size={16}/>上传</button><input ref={uploadRef} type="file" multiple hidden onChange={(event) => { if (event.target.files) uploadFiles(event.target.files); event.target.value = ""; }} /></>}
          {selected.size > 0 && <button className="danger" onClick={() => void (tab === "trash" ? restoreSelected() : deleteSelected())}>{tab === "trash" ? <ArchiveRestore size={16}/> : <Trash2 size={16}/>} {tab === "trash" ? "恢复" : "删除"} {selected.size} 项</button>}
        </div>
        <div className="tf-browser" onDragOver={(event) => { if (tab === "files") event.preventDefault(); }} onDrop={(event) => { if (tab !== "files") return; event.preventDefault(); uploadFiles(event.dataTransfer.files); }} title={tab === "files" ? "可将文件拖放到此区域上传" : undefined}>
          <div className="tf-file-list">
            <div className="tf-file-head"><span></span><span>名称</span><span>大小</span><span>修改时间</span><span></span></div>
            {visible.map((entry) => <div className={`tf-file-row ${inspected?.path === entry.path ? "active" : ""}`} key={entry.path} onDoubleClick={() => entry.type === "directory" ? void loadDirectory(entry.path) : void readPreview(entry)}>
              <input type="checkbox" checked={selected.has(entry.path)} onChange={() => setSelected((current) => { const next = new Set(current); next.has(entry.path) ? next.delete(entry.path) : next.add(entry.path); return next; })}/>
              <button className="tf-file-name" onClick={() => void readPreview(entry)}>{entry.type === "directory" ? <Folder size={19}/> : previewKind(entry) === "image" ? <FileImage size={19}/> : previewKind(entry) === "audio" ? <FileAudio size={19}/> : previewKind(entry) === "text" ? <FileText size={19}/> : <File size={19}/>}<span>{entry.name}{entry.protected && <small>受保护</small>}</span></button>
              <span>{entry.type === "directory" ? "—" : formatBytes(entry.size)}</span><span>{entry.modifiedAt ? new Date(typeof entry.modifiedAt === "number" ? entry.modifiedAt * 1000 : entry.modifiedAt).toLocaleString("zh-CN") : "—"}</span>
              <button className="icon-button" onClick={() => setInspected(entry)}><MoreHorizontal size={16}/></button>
            </div>)}
            {!visible.length && <div className="tf-empty">{busy ? "正在读取目录…" : "当前目录为空"}</div>}
            {hasMore && <button className="tf-load-more" onClick={() => void loadDirectory(path, cursor, true)}>加载下一页</button>}
          </div>
          <aside className="tf-inspector">
            {inspected ? <><div className="tf-inspector-title"><div>{inspected.type === "directory" ? <Folder/> : <File/>}<strong>{inspected.name}</strong></div><small>{inspected.path}</small></div>
              <dl><dt>类型</dt><dd>{inspected.type === "directory" ? "目录" : previewKind(inspected)}</dd><dt>大小</dt><dd>{formatBytes(inspected.size)}</dd><dt>保护</dt><dd>{inspected.protected ? "是" : "否"}</dd></dl>
              {inspected.type === "file" && <div className="tf-preview">{previewKind(inspected) === "text" ? <textarea value={preview} readOnly={!editing} onChange={(event) => setPreview(event.target.value)} /> : previewUrl && previewKind(inspected) === "image" ? <img src={previewUrl} alt={inspected.name} /> : previewUrl && previewKind(inspected) === "audio" ? <audio controls src={previewUrl} /> : previewUrl && previewKind(inspected) === "video" ? <video controls src={previewUrl} /> : preview ? <pre>{base64ToBytes(preview).slice(0, 64 * 1024).reduce((text, byte, index) => `${text}${index % 16 === 0 ? `\n${index.toString(16).padStart(6, "0")}  ` : ""}${byte.toString(16).padStart(2, "0")} `, "")}</pre> : <span>点击预览读取内容</span>}</div>}
              <div className="tf-inspector-actions">{inspected.type === "file" && <><button onClick={() => void readPreview(inspected)}>预览</button><button onClick={() => void downloadEntry(inspected)}><Download size={15}/>下载</button>{previewKind(inspected) === "text" && (editing ? <button className="primary" onClick={() => void saveText()}><Save size={15}/>保存</button> : <button onClick={() => setEditing(true)}>编辑</button>)}</>}<button onClick={() => void renameOrMove(inspected)}>移动/重命名</button><button className="danger" onClick={() => void (tab === "trash" ? restoreSelected() : deleteSelected())}>{tab === "trash" ? "恢复" : "删除"}</button></div>
            </> : <div className="tf-empty">选择文件查看属性和预览</div>}
          </aside>
        </div>
        {tab === "trash" && <div className="tf-trash-footer"><span>删除项目会先进入此目录，恢复冲突默认自动重命名。</span><button className="danger" onClick={() => { if (window.prompt("输入 EMPTY_TF_TRASH 永久清空") === "EMPTY_TF_TRASH") void mutate("tf_trash_empty", { confirm: "EMPTY_TF_TRASH" }, "回收站已清空"); }}>永久清空回收站</button></div>}
      </> : <div className="tf-maintenance">
        <section><HardDrive/><div><h3>挂载控制</h3><p>仅释放 TF 使用的 SDMMC Slot0，不触碰 C6 Hosted Slot1。</p></div><button onClick={() => void maintenance(status?.mounted ? "tf_unmount" : "tf_mount")}>{status?.mounted ? "安全卸载" : "重新挂载"}</button></section>
        <section><Gauge/><div><h3>读写健康与性能</h3><p>健康结果代表挂载和 I/O，不代表存储卡寿命或 SMART。</p></div><button onClick={() => void maintenance("tfcard_selftest")}>快速自检</button><button onClick={() => void maintenance("tf_health_test")}>完整自检</button><button onClick={() => void maintenance("tf_benchmark", { bytes: 4 * 1024 * 1024 })}>4 MiB 测速</button></section>
        <section><Save/><div><h3>导出诊断包</h3><p>将设备、Hosted、TF 状态和非敏感日志写入 `.fridge_spirit/exports`，不会导出密码或 API Key。</p></div><button onClick={() => void maintenance("tf_export_diagnostics")}>一键导出</button></section>
        <section><Wrench/><div><h3>访问与服务</h3><p>专家模式持久保存；HTTP 服务只在 Wi-Fi 和 TF 就绪时开放。</p></div><button onClick={() => void toggleExpert()}>{access.expertMode ? "恢复保护模式" : "启用专家模式"}</button><button onClick={() => void maintenance("tf_http_status")}>HTTP 状态</button><button onClick={() => void maintenance("tf_http_enable")}>启用 HTTP</button><button onClick={() => void maintenance("tf_http_disable")}>关闭 HTTP</button></section>
        <section className="danger"><Trash2/><div><h3>格式化 TF 卡</h3><p>将永久删除整卡数据。格式化后固件应重建 `.fridge_spirit` 目录并执行自检。</p></div><button onClick={() => { if (window.prompt("输入 ERASE_TF_CARD 格式化整卡") === "ERASE_TF_CARD") void maintenance("format_tfcard", { confirm: "ERASE_TF_CARD" }); }}>格式化</button></section>
      </div>}

      {queue.length > 0 && <div className="tf-transfer-drawer"><div><strong>传输队列</strong><span>{queue.filter((item) => item.state === "running" || item.state === "paused").length} 个活动任务</span></div>{queue.map((item) => <div className="tf-transfer-item" key={item.id}><span>{item.direction === "upload" ? <Upload size={16}/> : <Download size={16}/>}</span><div><strong>{item.name}</strong><small>{item.detail}</small><i><b style={{ width: `${item.progress}%` }}/></i></div><em className={item.state}>{item.state === "running" ? `${item.progress}%` : item.state === "paused" ? "已暂停" : item.state === "done" ? "完成" : item.state === "cancelled" ? "已取消" : "失败"}</em>{(item.state === "running" || item.state === "paused") && <><button onClick={() => toggleQueuePause(item)}>{item.state === "paused" ? "继续" : "暂停"}</button><button onClick={() => cancelQueue(item)}>取消</button></>}</div>)}</div>}
    </div>
  );
}
