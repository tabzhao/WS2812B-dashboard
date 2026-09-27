#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
阶段 1 · UDP 像素帧测试发送端

用于验证「上位机 → 模块」链路是否打通：能不能收到、掉不掉帧、亮度对不对。
协议见 docs/04-udp-protocol.md。

用法：
    python3 udp_sender.py --ip dash-A1B2C3.local --pattern gradient --fps 60
    python3 udp_sender.py --ip dash-A1B2C3.local --pattern corners   # 验证映射
    python3 udp_sender.py --ip 192.168.5.50 --pattern list           # 退回写静态 IP
    dns-sd -B _dashboard._udp                                        # 先看看有哪些设备

按 Ctrl+C 停止。
"""

import argparse
import math
import socket
import struct
import sys
import time

COLS = 32
ROWS = 8
NPIX = COLS * ROWS
PAYLOAD_BYTES = NPIX * 3          # 768
HEADER_FMT = "<BBBBHBB"           # magic(2) ver type seq(LE u16) brt rsv


# ---------------------------------------------------------------- 颜色工具
def hsv_to_rgb(h, s, v):
    """h, s, v 均为 0.0~1.0，返回 (r, g, b) 各 0~255"""
    if s <= 0.0:
        c = int(v * 255.0)
        return c, c, c
    sector = int(h * 6.0) % 6
    f = h * 6.0 - int(h * 6.0)
    vv = int(255.0 * v)
    p = int(255.0 * v * (1.0 - s))
    q = int(255.0 * v * (1.0 - s * f))
    t = int(255.0 * v * (1.0 - s * (1.0 - f)))
    return (
        (vv, t, p), (q, vv, p), (p, vv, t),
        (p, q, vv), (t, p, vv), (vv, p, q),
    )[sector]


# ---------------------------------------------------------------- 图案
def _put(buf, x, y, r, g, b):
    if 0 <= x < COLS and 0 <= y < ROWS:
        i = (y * COLS + x) * 3
        buf[i] = r
        buf[i + 1] = g
        buf[i + 2] = b


def pattern_gradient(t):
    """横向彩虹渐变并整体滚动：检查色彩连续性与整体亮度"""
    buf = bytearray(PAYLOAD_BYTES)
    for x in range(COLS):
        r, g, b = hsv_to_rgb(((x / COLS) + t * 0.08) % 1.0, 1.0, 1.0)
        for y in range(ROWS):
            _put(buf, x, y, r, g, b)
    return buf


def pattern_sweep(t):
    """带拖尾的竖条从左向右扫：检查帧率是否跟得上、有没有卡顿"""
    buf = bytearray(PAYLOAD_BYTES)
    head = (t * 24.0) % (COLS + 8)
    for k in range(8):
        x = int(head) - k
        if x < 0 or x >= COLS:
            continue
        v = 255 - k * 30
        for y in range(ROWS):
            _put(buf, x, y, 0, v, v)
    return buf


def pattern_solid(t):
    """整屏单色缓慢变色：最容易看出丢帧和闪烁"""
    r, g, b = hsv_to_rgb((t * 0.05) % 1.0, 1.0, 1.0)
    return bytearray(bytes((r, g, b)) * NPIX)


def pattern_checker(t):
    """棋盘：一格错就看得出来"""
    buf = bytearray(PAYLOAD_BYTES)
    phase = int(t * 2) & 1
    for y in range(ROWS):
        for x in range(COLS):
            on = ((x + y + phase) & 1) == 0
            v = 200 if on else 10
            _put(buf, x, y, v, v, v)
    return buf


def pattern_tiles(t):
    """四块配色：从左到右 红/绿/蓝/黄，验证块序"""
    colors = ((200, 0, 0), (0, 200, 0), (0, 0, 200), (200, 200, 0))
    buf = bytearray(PAYLOAD_BYTES)
    for x in range(COLS):
        r, g, b = colors[(x // 8) & 3]
        for y in range(ROWS):
            _put(buf, x, y, r, g, b)
    return buf


def pattern_corners(t):
    """关键点标记（与固件阶段 0 模式 8 一致）：最直接的映射复核
    期望：左上红、右上绿、左下蓝、右下黄、四块起点（底行）洋红"""
    buf = bytearray(PAYLOAD_BYTES)
    for x in range(COLS):
        for y in range(ROWS):
            _put(buf, x, y, 6, 6, 6)
    for k in range(4):
        _put(buf, k * 8, ROWS - 1, 255, 0, 255)
    _put(buf, 0, 0, 255, 0, 0)
    _put(buf, COLS - 1, 0, 0, 255, 0)
    _put(buf, 0, ROWS - 1, 0, 0, 255)
    _put(buf, COLS - 1, ROWS - 1, 255, 255, 0)
    return buf


def pattern_plasma(t):
    """动态波纹：综合压力测试，画面变化剧烈"""
    buf = bytearray(PAYLOAD_BYTES)
    for y in range(ROWS):
        for x in range(COLS):
            v = (math.sin(x * 0.35 + t * 2.0)
                 + math.sin(y * 0.8 + t * 1.3)
                 + math.sin((x + y) * 0.25 + t * 0.7)) / 3.0
            r, g, b = hsv_to_rgb((v + 1.0) / 2.0 % 1.0, 1.0, 1.0)
            _put(buf, x, y, r, g, b)
    return buf


PATTERNS = {
    "gradient": pattern_gradient,
    "sweep": pattern_sweep,
    "solid": pattern_solid,
    "checker": pattern_checker,
    "tiles": pattern_tiles,
    "corners": pattern_corners,
    "plasma": pattern_plasma,
}


# ---------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser(description="仪表盘 UDP 像素帧测试发送端")
    ap.add_argument("--ip", default="dash-000000.local",
                    help="模块 IP 或主机名（支持 mDNS 的 dash-XXXXXX.local）")
    ap.add_argument("--port", type=int, default=8551, help="模块 UDP 端口")
    ap.add_argument("--fps", type=float, default=60.0, help="发送帧率")
    ap.add_argument("--brightness", type=int, default=60,
                    help="全局亮度 0~255（固件侧硬上限 128）")
    ap.add_argument("--pattern", default="gradient", help="图案名，或 list 列出全部")
    ap.add_argument("--duration", type=float, default=0.0, help="发送时长（秒），0=一直发")
    args = ap.parse_args()

    if args.pattern == "list":
        print("可用图案：")
        for name, fn in PATTERNS.items():
            print("  {:<10} {}".format(name, (fn.__doc__ or "").split("\n")[0]))
        return 0

    if args.pattern not in PATTERNS:
        print("未知图案：{}（用 --pattern list 查看）".format(args.pattern), file=sys.stderr)
        return 1

    # --ip 支持主机名（含 mDNS 的 dash-XXXXXX.local），由系统 resolver 解析。
    # 解析失败优先怀疑多播/AP 隔离，而不是地址写错。
    is_ip = True
    try:
        socket.inet_aton(args.ip)
    except OSError:
        is_ip = False
    if not is_ip:
        try:
            resolved = socket.gethostbyname(args.ip)
            print("主机名 {} → {}".format(args.ip, resolved))
            args.ip = resolved
        except socket.gaierror as e:
            print("无法解析主机名 '{}'：{}".format(args.ip, e), file=sys.stderr)
            print("", file=sys.stderr)
            print("排查：", file=sys.stderr)
            print("  1. dns-sd -B _dashboard._udp   看看设备有没有注册出来", file=sys.stderr)
            print("  2. 若查不到：AP 开了「客户端隔离」？config.h 的 kMdnsEnable 是否为 1？", file=sys.stderr)
            print("  3. 或退回写静态 IP 地址", file=sys.stderr)
            return 1

    render = PATTERNS[args.pattern]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.ip, args.port)

    print("发送 {} → {}:{}  fps={} brightness={}  Ctrl+C 停止".format(
        args.pattern, args.ip, args.port, args.fps, args.brightness))

    interval = 1.0 / args.fps if args.fps > 0 else 0.0
    seq = 0
    sent = 0
    t0 = time.monotonic()
    last_report = t0
    sent_at_report = 0

    try:
        while True:
            now = time.monotonic()
            t = now - t0
            if args.duration > 0 and t >= args.duration:
                break

            frame = render(t)
            header = struct.pack(HEADER_FMT, 0x4D, 0x50, 1, 2,
                                 seq & 0xFFFF, args.brightness & 0xFF, 0)
            sock.sendto(header + bytes(frame), dest)
            seq += 1
            sent += 1

            if interval > 0:
                sleep_for = (t0 + sent * interval) - time.monotonic()
                if sleep_for > 0:
                    time.sleep(sleep_for)

            now = time.monotonic()
            if now - last_report >= 1.0:
                n = sent - sent_at_report
                dt = now - last_report
                kbps = n * (8 + PAYLOAD_BYTES) * 8 / 1000.0 / dt
                print("  {} 帧  实测 {:.1f} fps  {:.0f} kbps  seq={}".format(
                    n, n / dt, kbps, seq))
                last_report = now
                sent_at_report = sent
    except KeyboardInterrupt:
        print("\n已停止")
    finally:
        sock.close()

    elapsed = time.monotonic() - t0
    print("共发送 {} 帧，用时 {:.1f}s，平均 {:.1f} fps".format(
        sent, elapsed, sent / elapsed if elapsed > 0 else 0.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
