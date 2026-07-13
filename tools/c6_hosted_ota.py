#!/usr/bin/env python3
"""通过 P4 的 COM JSON Lines 把 APP-only 固件流式升级到板载 ESP32-C6。"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid
import zlib

import serial


MAX_IMAGE_BYTES = 0x180000
CHUNK_BYTES = 1024


class ProtocolError(RuntimeError):
    pass


class JsonLineClient:
    def __init__(self, port: str, baud: int = 115200) -> None:
        self.port = port
        self.baud = baud
        # CH343 打开端口时若默认拉动 DTR/RTS 会复位 P4；先设定控制线再 open。
        self.ser = serial.Serial(port=None, baudrate=baud, timeout=0.25)
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.port = port
        self.ser.open()
        self.ser.reset_input_buffer()

    def close(self) -> None:
        self.ser.close()

    def wait_ready(self, timeout: float = 12.0) -> None:
        """等待固件 ready；若打开串口未复位，也允许 get_network 探测成功。"""
        deadline = time.monotonic() + timeout
        next_probe = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            line = self.ser.readline()
            if line.lstrip().startswith(b"{"):
                try:
                    obj = json.loads(line.decode("utf-8", errors="strict"))
                    if obj.get("type") == "event" and obj.get("event") == "ready":
                        return
                except (UnicodeDecodeError, json.JSONDecodeError):
                    pass
            if time.monotonic() >= next_probe:
                try:
                    self.request("get_network", timeout=1.5)
                    return
                except (TimeoutError, ProtocolError):
                    next_probe = time.monotonic() + 0.5
        raise TimeoutError("timeout waiting for device ready")

    def request(self, command: str, payload: dict | None = None, timeout: float = 20.0) -> dict:
        request_id = f"c6-{uuid.uuid4().hex[:12]}"
        message = {"type": "request", "request_id": request_id, "command": command}
        if payload:
            message.update(payload)
        raw = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
        if len(raw) >= 2048:
            raise ProtocolError(f"JSON line too large: {len(raw)} bytes")
        self.ser.write(raw)
        self.ser.flush()

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.ser.readline()
            if not line or not line.lstrip().startswith(b"{"):
                continue
            try:
                obj = json.loads(line.decode("utf-8", errors="strict"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if obj.get("request_id") != request_id:
                continue
            if not obj.get("ok", False):
                raise ProtocolError(f"{command}: {obj.get('error', 'unknown device error')}")
            return obj.get("payload", {})
        raise TimeoutError(f"timeout waiting for {command}")


def validate_image(path: Path) -> tuple[bytes, str]:
    data = path.read_bytes()
    if not data or data[0] != 0xE9:
        raise ProtocolError("not an ESP app image: missing 0xE9 magic")
    if len(data) > MAX_IMAGE_BYTES:
        raise ProtocolError(f"image size 0x{len(data):x} exceeds legacy C6 OTA slot 0x{MAX_IMAGE_BYTES:x}")

    check = subprocess.run(
        [sys.executable, "-m", "esptool", "--chip", "esp32c6", "image-info", str(path)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if check.returncode != 0 or "ESP32-C6" not in check.stdout:
        raise ProtocolError(f"esptool rejected ESP32-C6 image:\n{check.stdout}")
    return data, hashlib.sha256(data).hexdigest()


def query_status(client: JsonLineClient) -> dict:
    return client.request("c6_ota_status", timeout=5.0)


def transfer(client: JsonLineClient, image: bytes, sha256: str, activation_mode: str) -> None:
    current = query_status(client)
    if (current.get("state") == "writing" and
            int(current.get("expectedSize", -1)) == len(image) and
            current.get("expectedSha256", "").lower() == sha256.lower()):
        session = int(current["session"])
        next_offset = int(current["nextOffset"])
        print(f"Resuming matching OTA session={session} at offset={next_offset}")
    else:
        if current.get("state") in {"writing", "failed", "wait_activate"}:
            raise ProtocolError(f"device has incompatible active OTA state: {current}")
        begin = client.request(
            "c6_ota_begin",
            {
                "image_size": len(image),
                "sha256": sha256,
                "activation_mode": activation_mode,
                "confirm": "UPDATE_C6_FIRMWARE",
            },
            timeout=30.0,
        )
        session = int(begin["session"])
        next_offset = int(begin.get("nextOffset", begin.get("receivedBytes", 0)))
        if next_offset != 0:
            raise ProtocolError(f"new OTA session has unexpected offset {next_offset}")

    started = time.monotonic()
    while next_offset < len(image):
        offset = next_offset
        chunk = image[offset : offset + CHUNK_BYTES]
        payload = {
            "session": session,
            "offset": offset,
            "crc32": zlib.crc32(chunk) & 0xFFFFFFFF,
            "data_b64": base64.b64encode(chunk).decode("ascii"),
        }
        try:
            ack = client.request("c6_ota_chunk", payload, timeout=15.0)
            next_offset = int(ack["nextOffset"])
        except TimeoutError:
            status = query_status(client)
            device_offset = int(status.get("nextOffset", -1))
            if device_offset == offset + len(chunk):
                next_offset = device_offset
            elif device_offset == offset and status.get("state") == "writing":
                # 设备明确尚未推进时才允许重发；写入错误会把状态锁为 failed。
                continue
            else:
                raise ProtocolError(f"uncertain OTA offset after timeout: device={device_offset}, host={offset}")

        if next_offset != offset + len(chunk):
            raise ProtocolError(f"unexpected nextOffset {next_offset}, expected {offset + len(chunk)}")
        if next_offset == len(image) or next_offset % (64 * 1024) == 0:
            elapsed = max(time.monotonic() - started, 0.001)
            print(f"C6 OTA {next_offset}/{len(image)} bytes ({next_offset * 100 / len(image):.1f}%), "
                  f"{next_offset / elapsed / 1024:.1f} KiB/s")

    end = client.request("c6_ota_end", {"session": session}, timeout=30.0)
    if end.get("state") not in {"complete", "wait_activate"}:
        raise ProtocolError(f"unexpected OTA end state: {end}")
    if activation_mode == "explicit_activate":
        client.request("c6_ota_activate", {"session": session}, timeout=20.0)


def wait_for_reboot(port: str, baud: int) -> dict:
    deadline = time.monotonic() + 40.0
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            client = JsonLineClient(port, baud)
            try:
                client.wait_ready(timeout=12.0)
                status = client.request("get_network", timeout=8.0)
                # end响应发出后P4还会延迟7秒重启，不能把重启前仍处于OTA保护态的旧进程误判为升级结果。
                if status.get("c6OtaGuard", False):
                    time.sleep(1.0)
                    continue
                return status
            finally:
                client.close()
        except (OSError, serial.SerialException, TimeoutError, ProtocolError) as exc:
            last_error = exc
            time.sleep(1.0)
    raise ProtocolError(f"device did not return after C6/P4 reboot: {last_error}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM15")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument(
        "--activation-mode",
        choices=("legacy_end_reboots", "explicit_activate"),
        default="legacy_end_reboots",
        help="Use legacy_end_reboots for the factory Hosted 0.0.6 image",
    )
    parser.add_argument("--confirm", action="store_true", help="Required safety acknowledgement")
    args = parser.parse_args()
    if not args.confirm:
        parser.error("--confirm is required to update C6 firmware")

    image, sha256 = validate_image(args.firmware.resolve())
    print(f"Firmware: {args.firmware} size=0x{len(image):x} sha256={sha256}")
    client = JsonLineClient(args.port, args.baud)
    try:
        client.wait_ready()
        transfer(client, image, sha256, args.activation_mode)
    finally:
        client.close()

    print("C6 image accepted; waiting for C6 and P4 coordinated reboot...")
    status = wait_for_reboot(args.port, args.baud)
    print(json.dumps(status, ensure_ascii=False, indent=2))
    if status.get("hostedSlaveVersion") != "2.12.11":
        raise ProtocolError("device returned, but C6 Hosted version is not 2.12.11")
    print("C6 Hosted upgrade verified: 2.12.11")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ProtocolError, TimeoutError, serial.SerialException, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
