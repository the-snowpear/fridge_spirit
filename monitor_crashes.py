#!/usr/bin/env python3
"""
ESP32-P4 持续崩溃监测脚本
监控 COM15 串口，检测并记录所有崩溃事件
"""

import serial
import time
import sys
from datetime import datetime

PORT = 'COM17'
BAUD = 115200
CRASH_KEYWORDS = ['panic', 'Guru Meditation', 'abort', 'rst:', 'Core  0',
                  'Stack protection', 'assert', 'LoadProhibited', 'StoreProhibited']

def main():
    print(f'[MONITOR] Starting monitoring {PORT} @ {BAUD} baud...')
    print(f'[MONITOR] Keywords: {", ".join(CRASH_KEYWORDS)}')
    print('=' * 80)

    try:
        ser = serial.Serial(PORT, BAUD, timeout=1)
        line_count = 0
        crash_count = 0
        last_heartbeat = time.time()

        while True:
            if ser.in_waiting:
                try:
                    line = ser.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        line_count += 1
                        timestamp = datetime.now().strftime('%H:%M:%S.%f')[:-3]

                        # 检查是否包含崩溃关键字
                        is_crash = any(kw in line for kw in CRASH_KEYWORDS)

                        if is_crash:
                            crash_count += 1
                            print(f'[{timestamp}] [CRASH #{crash_count}] {line}')
                            sys.stdout.flush()
                        elif 'fridge_diag: boot' in line or 'stack_hwm' in line or 'panic=' in line:
                            print(f'[{timestamp}] [INFO] {line}')
                            sys.stdout.flush()
                except Exception as e:
                    pass  # 忽略单行解码错误

            # 每30秒输出一次心跳
            if time.time() - last_heartbeat > 30:
                timestamp = datetime.now().strftime('%H:%M:%S')
                print(f'[{timestamp}] [HEARTBEAT] Received {line_count} lines, detected {crash_count} crashes')
                sys.stdout.flush()
                last_heartbeat = time.time()

            time.sleep(0.01)

    except KeyboardInterrupt:
        print(f'\n\n[MONITOR] Monitoring ended. Total: {line_count} lines, {crash_count} crashes')
    except Exception as e:
        print(f'[ERROR] {e}', file=sys.stderr)
        return 1

    return 0

if __name__ == '__main__':
    sys.exit(main())
