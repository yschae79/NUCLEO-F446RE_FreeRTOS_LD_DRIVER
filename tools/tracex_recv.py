#!/usr/bin/env python3
"""
tracex_recv.py — TraceX UART 덤프 수신 및 .trx 파일 저장

사용법:
    python tracex_recv.py <COM포트> [baud]

예시:
    python tracex_recv.py COM3 230400
    python tracex_recv.py /dev/ttyUSB0

동작:
    UART2 스트림을 수신하면서 TraceX 프레임 패킷을 추출합니다.
    패킷 외 바이트는 UTF-8 로그(printf 출력)로 터미널에 출력합니다.
    수신된 TraceX 프레임은 dump_NNNN.trx 파일로 저장됩니다.

프레임 프로토콜 (MCU 측 TraceDumpEntry 기준):
    [MAGIC_START 8B][size 4B LE][raw trace buffer][MAGIC_END 8B]

설치 필요:
    pip install pyserial
"""

import sys
import os
import serial
import struct
from datetime import datetime

# ── 프레임 마직 ──────────────────────────────────────────────────────────
MAGIC_START = bytes([0x54, 0x52, 0x43, 0x58, 0xFF, 0xFE, 0xFD, 0xFC])
MAGIC_END   = bytes([0xFC, 0xFD, 0xFE, 0xFF, 0x54, 0x52, 0x43, 0x58])
HEADER_LEN  = len(MAGIC_START) + 4          # MAGIC_START + size(4B)


def recv_loop(port: str, baud: int, outdir: str) -> None:
    os.makedirs(outdir, exist_ok=True)
    dump_count = 0
    buf = bytearray()
    log_line = bytearray()

    print(f"[tracex_recv] {port} @ {baud} bps  →  출력 디렉터리: {outdir}")
    print("[tracex_recv] 수신 대기 중... (Ctrl+C 로 종료)\n")

    with serial.Serial(port, baud, timeout=0.1) as ser:
        while True:
            chunk = ser.read(256)
            if not chunk:
                continue

            buf.extend(chunk)

            # 버퍼에서 ASCII 로그와 TraceX 프레임을 분리
            while True:
                start_idx = buf.find(MAGIC_START)

                # MAGIC_START 이전 바이트는 ASCII 로그
                if start_idx > 0:
                    log_bytes = buf[:start_idx]
                    _print_log(log_bytes, log_line)
                    buf = buf[start_idx:]
                    start_idx = 0

                if start_idx == -1:
                    # 마직 없음 — 전부 로그, 단 MAGIC_START 접두사가 split됐을 수 있음
                    keep = len(MAGIC_START) - 1
                    if len(buf) > keep:
                        _print_log(buf[:-keep], log_line)
                        buf = buf[-keep:]
                    break

                # 헤더 완성 대기
                if len(buf) < HEADER_LEN:
                    break

                # size 파싱 (little-endian 4B)
                size_bytes = buf[len(MAGIC_START):HEADER_LEN]
                frame_size = struct.unpack_from('<I', size_bytes)[0]
                total_frame = HEADER_LEN + frame_size + len(MAGIC_END)

                # 프레임 완성 대기
                if len(buf) < total_frame:
                    break

                # MAGIC_END 검증
                end_idx = HEADER_LEN + frame_size
                if buf[end_idx:end_idx + len(MAGIC_END)] != MAGIC_END:
                    # 손상된 프레임 — MAGIC_START 한 바이트 건너뜀
                    print("[WARN] MAGIC_END 불일치, 프레임 skip")
                    buf = buf[1:]
                    continue

                # 유효한 TraceX 프레임 추출
                raw_trace = bytes(buf[HEADER_LEN:end_idx])
                buf = buf[total_frame:]

                dump_count += 1
                ts = datetime.now().strftime("%Y%m%d_%H%M%S")
                filename = os.path.join(outdir, f"dump_{dump_count:04d}_{ts}.trx")

                with open(filename, 'wb') as f:
                    f.write(raw_trace)

                print(f"\n[TRACE] #{dump_count:04d}  {len(raw_trace):,} bytes  →  {filename}")


def _print_log(data: bytearray, line_buf: bytearray) -> None:
    """바이너리 데이터를 UTF-8 로그로 터미널에 출력 (줄 단위)"""
    line_buf.extend(data)
    while b'\n' in line_buf:
        idx = line_buf.index(b'\n')
        line = line_buf[:idx].decode('utf-8', errors='replace').rstrip('\r')
        print(f"[LOG] {line}")
        line_buf[:] = line_buf[idx + 1:]


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    port = sys.argv[1]
    baud = int(sys.argv[2]) if len(sys.argv) >= 3 else 230400
    outdir = os.path.join(os.path.dirname(__file__), "tracex_dumps")

    try:
        recv_loop(port, baud, outdir)
    except KeyboardInterrupt:
        print("\n[tracex_recv] 종료")
    except serial.SerialException as e:
        print(f"[ERROR] 시리얼 포트 오류: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
