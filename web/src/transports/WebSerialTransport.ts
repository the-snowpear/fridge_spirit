import type { DeviceCommand, DeviceRequest, DeviceResponse } from "../types";
import { BaseTransport } from "./DeviceTransport";

interface PendingRequest {
  resolve: (value: DeviceResponse) => void;
  reject: (reason?: unknown) => void;
  timeout: number;
}

// Web Serial 传输层：通过 USB CDC/JTAG 串口收发 JSON Lines 协议。
export class WebSerialTransport extends BaseTransport {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private pending = new Map<string, PendingRequest>();
  private decoder = new TextDecoder();
  private encoder = new TextEncoder();
  private readBuffer = "";
  private readLoopActive = false;
  private disconnecting = false;
  private connectionLostHandled = false;
  private requestQueue: Promise<void> = Promise.resolve();
  private debugTxLogs = window.localStorage.getItem("fridge.debugSerial") === "1";

  constructor(private timeoutMs = 30000) {
    super();
  }

  async connect() {
    if (!navigator.serial) {
      throw new Error("当前浏览器不支持 Web Serial，请使用 Chrome 或 Edge 的 localhost 页面。");
    }
    this.port = await navigator.serial.requestPort();
    this.connectionLostHandled = false;
    this.disconnecting = false;
    this.readBuffer = "";
    this.decoder = new TextDecoder();
    this.emitLog("info", `已选择串口设备${this.formatPortInfo()}，正在打开 115200 波特率连接。`, "serial");
    try {
      await this.openPortWithTimeout(10000);

      if (!this.port.readable || !this.port.writable) {
        throw new Error("串口未提供可读写流。");
      }

      this.reader = this.port.readable.getReader();
      this.writer = this.port.writable.getWriter();
      this.readLoopActive = true;
      this.readLoop();
      this.emitLog("info", "USB 串口已连接，波特率 115200。", "serial");
    } catch (error) {
      await this.cleanupAfterFailedConnect();
      this.emitLog("error", String(error), "serial");
      throw error;
    }
  }

  async disconnect() {
    this.disconnecting = true;
    this.readLoopActive = false;
    this.rejectPending(new Error("串口已断开"));

    await this.reader?.cancel().catch(() => undefined);
    try {
      this.reader?.releaseLock();
    } catch {
      // 设备丢失时浏览器可能已自动释放锁，忽略重复释放。
    }
    this.reader = null;

    try {
      this.writer?.releaseLock();
    } catch {
      // 同上，断线清理必须保持幂等。
    }
    this.writer = null;

    await this.port?.close().catch(() => undefined);
    this.port = null;
    this.readBuffer = "";
    this.disconnecting = false;
    this.emitLog("info", "USB 串口已断开。", "serial");
  }

  async sendCommand<TPayload = unknown>(
    command: DeviceCommand,
    payload?: unknown,
  ): Promise<DeviceResponse<TPayload>> {
    const run = () => this.sendCommandNow<TPayload>(command, payload);
    const result = this.requestQueue.then(run, run);
    this.requestQueue = result.then(
      () => undefined,
      () => undefined,
    );
    return result;
  }

  private async sendCommandNow<TPayload = unknown>(
    command: DeviceCommand,
    payload?: unknown,
  ): Promise<DeviceResponse<TPayload>> {
    if (!this.writer) {
      throw new Error("串口未连接");
    }

    const request: DeviceRequest = {
      type: "request",
      request_id: crypto.randomUUID(),
      command,
      payload,
    };
    // Tool Broker 调试协议要求 tool/arguments 位于顶层，不能包在普通 payload 中。
    if (command === "tool_call" && payload && typeof payload === "object") {
      const toolCall = payload as {
        tool?: string;
        arguments?: Record<string, unknown>;
      };
      request.tool = toolCall.tool;
      request.arguments = toolCall.arguments;
      delete request.payload;
    }
    if (command === "kitchen_ai_generate" && payload && typeof payload === "object" && "page" in payload) {
      Object.assign(request, { page: (payload as { page?: unknown }).page });
    }
    const line = `${JSON.stringify(request)}\n`;

    const promise = new Promise<DeviceResponse<TPayload>>((resolve, reject) => {
      const timeout = window.setTimeout(() => {
        this.pending.delete(request.request_id);
        reject(new Error(`${command} 请求超时`));
      }, this.getCommandTimeoutMs(command));
      this.pending.set(request.request_id, {
        resolve: resolve as (value: DeviceResponse) => void,
        reject,
        timeout,
      });
    });

    try {
      await this.writer.write(this.encoder.encode(line));
    } catch (error) {
      const pending = this.pending.get(request.request_id);
      if (pending) {
        window.clearTimeout(pending.timeout);
        this.pending.delete(request.request_id);
        pending.reject(error);
      }
      await this.handleUnexpectedDisconnect(error);
      return promise;
    }
    if (this.debugTxLogs) {
      this.emitLog("debug", `TX ${command} ${request.request_id}`, "serial");
    }
    return promise;
  }

  private getCommandTimeoutMs(command: DeviceCommand) {
    if (command === "scan_wifi") {
      return Math.max(this.timeoutMs, 60000);
    }
    if (command === "voice_chat_stop" || command === "asr_transcribe") {
      return Math.max(this.timeoutMs, 150000);
    }
    if (command === "tts_play") {
      return Math.max(this.timeoutMs, 120000);
    }
    if (command === "mic_record_wav") {
      return Math.max(this.timeoutMs, 120000);
    }
    if (command === "test_ai_chat") {
      return Math.max(this.timeoutMs, 130000);
    }
    if (command === "agent_session_read" || command === "agent_session_clear") {
      return Math.max(this.timeoutMs, 60000);
    }
    if (command === "format_tfcard" || command === "tf_benchmark" || command === "tf_trash_empty" || command === "tf_text_read") {
      return Math.max(this.timeoutMs, 180000);
    }
    if (
      command === "set_network" ||
      command === "ai_assistant_chat" ||
      command === "camera_capture" ||
      command === "camera_jpeg_diag" ||
      command === "camera_rgb565_diag" ||
      command === "camera_analyze" ||
      command === "get_ai_context_preview" ||
      command === "test_ai_task" ||
      command === "kitchen_ai_generate"
    ) {
      return Math.max(this.timeoutMs, 90000);
    }
    return this.timeoutMs;
  }

  private async openPortWithTimeout(timeoutMs: number) {
    if (!this.port) {
      throw new Error("串口对象不存在");
    }
    let timeout: number | undefined;
    try {
      await Promise.race([
        this.port.open({ baudRate: 115200 }).catch((error) => {
          throw new Error(this.describeOpenFailure(error));
        }),
        new Promise<never>((_, reject) => {
          timeout = window.setTimeout(() => {
            reject(new Error("串口打开超时：请确认端口没有被 ESP-IDF monitor、串口助手或其他浏览器标签占用。"));
          }, timeoutMs);
        }),
      ]);
    } finally {
      if (timeout) {
        window.clearTimeout(timeout);
      }
    }
  }

  private describeOpenFailure(error: unknown) {
    const raw = error instanceof Error ? `${error.name}: ${error.message}` : String(error);
    if (raw.includes("NetworkError") || raw.includes("Failed to open serial port")) {
      return `串口打开失败：端口可能被 ESP-IDF monitor、串口助手、VS Code Serial Monitor 或其他浏览器标签占用。请关闭占用 COM 口的程序后重试。原始错误：${raw}`;
    }
    return `串口打开失败：${raw}`;
  }

  private formatPortInfo() {
    if (!this.port) {
      return "";
    }
    const portWithInfo = this.port as SerialPort & {
      getInfo?: () => { usbVendorId?: number; usbProductId?: number };
    };
    const info = portWithInfo.getInfo?.();
    if (!info) {
      return "";
    }
    const parts = [
      info.usbVendorId !== undefined ? `VID=0x${info.usbVendorId.toString(16).padStart(4, "0")}` : "",
      info.usbProductId !== undefined ? `PID=0x${info.usbProductId.toString(16).padStart(4, "0")}` : "",
    ].filter(Boolean);
    return parts.length > 0 ? `（${parts.join(", ")}）` : "";
  }

  private async cleanupAfterFailedConnect() {
    this.disconnecting = true;
    this.readLoopActive = false;
    this.rejectPending(new Error("串口连接失败"));
    try {
      this.reader?.releaseLock();
    } catch {
      // 失败路径可能没有取得 reader 锁。
    }
    this.reader = null;
    try {
      this.writer?.releaseLock();
    } catch {
      // 失败路径可能没有取得 writer 锁。
    }
    this.writer = null;
    await this.port?.close().catch(() => undefined);
    this.port = null;
    this.readBuffer = "";
    this.disconnecting = false;
  }

  private async readLoop() {
    let disconnectError: unknown = null;
    while (this.readLoopActive && this.reader) {
      try {
        const { value, done } = await this.reader.read();
        if (done) {
          if (this.readLoopActive) {
            disconnectError = new Error("串口读取流已结束");
          }
          break;
        }
        if (value) {
          this.handleChunk(value);
        }
      } catch (error) {
        if (this.readLoopActive) {
          disconnectError = error;
        }
        break;
      }
    }
    if (disconnectError) {
      await this.handleUnexpectedDisconnect(disconnectError);
    }
  }

  private rejectPending(reason: Error) {
    this.pending.forEach((pending) => {
      window.clearTimeout(pending.timeout);
      pending.reject(reason);
    });
    this.pending.clear();
  }

  private async handleUnexpectedDisconnect(error: unknown) {
    if (this.disconnecting || this.connectionLostHandled) {
      return;
    }
    this.connectionLostHandled = true;
    this.readLoopActive = false;
    const raw = error instanceof Error ? `${error.name}: ${error.message}` : String(error);
    const reason = new Error(`USB 串口连接已丢失：${raw}`);
    this.rejectPending(reason);

    await this.reader?.cancel().catch(() => undefined);
    try {
      this.reader?.releaseLock();
    } catch {
      // 浏览器报告 device lost 后可能已自动释放 reader。
    }
    this.reader = null;
    try {
      this.writer?.releaseLock();
    } catch {
      // 浏览器报告 device lost 后可能已自动释放 writer。
    }
    this.writer = null;
    await this.port?.close().catch(() => undefined);
    this.port = null;
    this.readBuffer = "";

    this.emitLog("error", reason.message, "serial");
    this.emitConnectionLost(reason);
  }

  private handleChunk(value: Uint8Array) {
    this.readBuffer += this.decoder.decode(value, { stream: true });
    const lines = this.readBuffer.split(/\r?\n/);
    this.readBuffer = lines.pop() ?? "";

    lines
      .map((line) => line.trim())
      .filter(Boolean)
      .forEach((line) => this.handleLine(line));
  }

  private handleLine(line: string) {
    try {
      let messageLine = line;
      try {
        JSON.parse(messageLine);
      } catch {
        // 原生 USB 在刚打开时可能先吐出一段未换行的启动日志，再紧接 JSON 响应。
        // 保留日志前缀，同时提取响应，避免首个 get_status 因无法解析而等待到超时。
        const jsonStart = line.indexOf('{"type":');
        if (jsonStart <= 0) {
          throw new Error("not a JSON protocol line");
        }
        const logPrefix = line.slice(0, jsonStart).trim();
        if (logPrefix) {
          this.emitLog("info", logPrefix, "serial-log");
        }
        messageLine = line.slice(jsonStart);
      }
      const message = JSON.parse(messageLine);
      if (message.type === "response" && message.request_id) {
        const pending = this.pending.get(message.request_id);
        if (pending) {
          window.clearTimeout(pending.timeout);
          this.pending.delete(message.request_id);
          if (message.ok === false && message.command !== "camera_probe") {
            pending.reject(new Error(message.error ?? `${message.command ?? "request"} 请求失败`));
          } else {
            pending.resolve(message);
          }
        }
      }
      this.emitMessage(message);
    } catch {
      this.emitLog("info", line, "serial-log");
    }
  }
}
