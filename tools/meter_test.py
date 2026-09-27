#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
阶段 2 · UDP 表头值测试发送端

用于验证「上位机 → 模块 → PWM → 表头指针」这条链路：
5 路能不能独立控制、指针是否平滑、有没有卡针、超时会不会回落。

协议见 docs/04-udp-protocol.md（type=1，18 B：8 字节头 + 5×uint16 LE）
驱动原理见 docs/02-meter-driver.md

常用：
    # 最基础：5 路同时停在 50%，看指针是不是稳稳压在中间
    python3 meter_test.py --ip 192.168.5.50 --mode hold --value 50

    # 各路不同：确认没有串路、顺序没错
    python3 meter_test.py --ip 192.168.5.50 --mode per --values 10 30 50 70 90

    # 阶梯：每档停 2 秒，用来核对线性（每 10% 指针偏转是否等距）
    python3 meter_test.py --ip 192.168.5.50 --mode stairs

    # 三角波：看缓动是否平滑、有没有抖动
    python3 meter_test.py --ip 192.168.5.50 --mode sweep

    # 阶跃：0 ↔ 100 来回，观察过冲与稳定时间
    python3 meter_test.py --ip 192.168.5.50 --mode step --step-values 0 100

    # 随机游走：模拟真实数据，看 10 Hz 下指针跟不跟得上
    python3 meter_test.py --ip 192.168.5.50 --mode walk

按 Ctrl+C 停止。停止后模块会在 5 s 内把指针缓落回 0。
"""

import argparse
import random
import socket
import struct
import sys
import time

NCH = 5
HEADER_FMT = "<BBBBHBB"      # magic(2) ver type seq(LE u16) brt rsv
MAGIC = (0x4D, 0x50)         # 'M' 'P'
VER = 1
TYPE_METERS = 1
PAYLOAD_BYTES = NCH * 2      # 10
PACKET_BYTES = 8 + PAYLOAD_BYTES   # 18


def build_packet(values01, seq, brightness=0):
    """values01: 5 个 0.0~1.0 的归一化值"""
    vals = []
    for v in values01:
        v = 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)
        vals.append(int(v * 65535.0 + 0.5))
    header = struct.pack(HEADER_FMT, MAGIC[0], MAGIC[1], VER, TYPE_METERS,
                         seq & 0xFFFF, brightness & 0xFF, 0)
    return header + struct.pack("<5H", *vals)


# ---------------------------------------------------------------- 模式生成器
def gen_hold(args, t):
    v = args.value / 100.0
    return [v] * NCH


def gen_per(args, t):
    return [v / 100.0 for v in args.values]


def gen_sweep(args, t):
    """三角波 0→100→0"""
    half = args.period / 2.0
    phase = t % args.period
    f = phase / half if phase < half else 1.0 - (phase - half) / half
    return [f] * NCH


def gen_stairs(args, t):
    """每 10% 一档，到顶后回到底。用于逐点核对线性"""
    steps = 10
    idx = int(t / args.step_hold) % (steps * 2)
    level = idx if idx <= steps else steps * 2 - idx
    return [level / float(steps)] * NCH


def gen_step(args, t):
    """在两个值之间来回阶跃"""
    idx = int(t / args.step_period) & 1
    v = args.step_values[idx] / 100.0
    return [v] * NCH


def gen_walk(args, t):
    """每 0.5 s 随机游走一步 ±10%，各路独立"""
    return [state_walk_value(i, t) for i in range(NCH)]


_walk_state = [50.0] * NCH
_walk_last = [-1.0] * NCH


def state_walk_value(ch, t):
    slot = int(t / 0.5)
    if slot != _walk_last[ch]:
        _walk_last[ch] = slot
        v = _walk_state[ch] + random.uniform(-10.0, 10.0)
        _walk_state[ch] = max(0.0, min(100.0, v))
    return _walk_state[ch] / 100.0


MODES = {
    "hold":   (gen_hold,   "固定在某一百分比：验证静态位置与稳定性"),
    "per":    (gen_per,    "5 路各给一个值：验证通道顺序、有没有串路"),
    "sweep":  (gen_sweep,  "三角波连续扫描：观察缓动平滑度与抖动"),
    "stairs": (gen_stairs, "每 10% 停一档：逐点核对机械线性"),
    "step":   (gen_step,   "两值来回阶跃：观察过冲、反转与稳定时间"),
    "walk":   (gen_walk,   "随机游走：模拟真实数据，验证 10 Hz 跟随"),
}


# ---------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser(
        description="仪表盘 UDP 表头值测试发送端",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default="192.168.5.50", help="模块 IP")
    ap.add_argument("--port", type=int, default=8551, help="模块 UDP 端口")
    ap.add_argument("--hz", type=float, default=10.0,
                    help="发送频率（默认 10 Hz，与上位机约定一致）")
    ap.add_argument("--duration", type=float, default=0.0,
                    help="发送时长（秒），0=一直发")
    ap.add_argument("--mode", default="hold", help="模式名，或 list 列出全部")

    # 各模式的参数
    ap.add_argument("--value", type=float, default=50.0,
                    help="hold 模式的百分比 0~100")
    ap.add_argument("--values", type=float, nargs=5, default=[10, 30, 50, 70, 90],
                    help="per 模式的 5 个百分比")
    ap.add_argument("--period", type=float, default=8.0,
                    help="sweep 模式的完整周期（秒）")
    ap.add_argument("--step-hold", type=float, default=2.0,
                    help="stairs 模式每档停留秒数")
    ap.add_argument("--step-values", type=float, nargs=2, default=[0.0, 100.0],
                    help="step 模式的两个百分比")
    ap.add_argument("--step-period", type=float, default=3.0,
                    help="step 模式每次切换的间隔（秒）")
    ap.add_argument("--series-ohm", type=float, default=20000.0,
                    help="每路串联电阻(Ω)，需与固件 config.h 的 kMeterSeriesOhm 一致；"
                         "用于计算回路电流与固件限幅后的 duty 上限")
    ap.add_argument("--coil-ohm", type=float, default=1200.0,
                    help="表头内阻(Ω)，实测 1.2 kΩ")
    ap.add_argument("--ifs-ua", type=float, default=155.6,
                    help="表头满偏电流(µA)，2026-09-18 实测推算值")
    args = ap.parse_args()

    if args.mode == "list":
        print("可用模式：")
        for name, (fn, doc) in MODES.items():
            print("  {:<8} {}".format(name, doc))
        return 0

    if args.mode not in MODES:
        print("未知模式：{}（用 --mode list 查看）".format(args.mode), file=sys.stderr)
        return 1

    gen = MODES[args.mode][0]

    # 电参数：由串联电阻推算固件侧的实际行为
    #   100% 占空比电流  I100 = 3.3V / (R_series + R_coil)
    #   固件限幅上限    duty_max = 满偏电流 / I100
    r_coil = args.coil_ohm
    i_fullscale_ua = args.ifs_ua          # 表头额定满偏（2026-09-18 实测 155.6 µA）
    i100_ua = 3300000.0 / (args.series_ohm + r_coil)
    ratio = i100_ua / i_fullscale_ua      # >1 表示打得住
    ceiling_on = ratio > 1.0
    ceiling_duty = int(min(1.0, 1.0 / ratio) * 1023.0 + 0.5)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.ip, args.port)

    print("模式 {}  →  {}:{}  {} Hz   Ctrl+C 停止".format(
        args.mode, args.ip, args.port, args.hz))
    print("-" * 62)
    print("UDP 是无回执协议：脚本不会收到任何确认，一定会持续发下去。")
    print("判断有没有生效，看这三点：指针动没动 / 串口有没有包计数 / Ctrl+C 后的统计。")
    print("电参数：{} Ω 串联 + {} Ω 线圈 → 100% 占空比电流 {:.1f} µA = 满偏 {:.0f} µA 的 {:.1f}%".format(
        args.series_ohm, r_coil, i100_ua, i_fullscale_ua, i100_ua / i_fullscale_ua * 100.0))
    if ratio > 1.02:
        print("⚠️  过载：100% 占空比电流是额定满偏的 {:.1f} 倍，指针会撞止档。".format(ratio))
        print("    固件已限幅到 duty≤{}，有效行程只有 0~{:.1f}%。".format(ceiling_duty, 100.0 / ratio))
    elif ratio < 0.85:
        print("⚠️  行程不足：100% 占空比只能走到 {:.1f}%，要更满得减小串联电阻。".format(ratio * 100.0))
    print("-" * 62)

    interval = 1.0 / args.hz if args.hz > 0 else 0.0
    seq = 0
    sent = 0
    t0 = time.monotonic()
    last_print = t0

    try:
        while True:
            now = time.monotonic()
            t = now - t0
            if args.duration > 0 and t >= args.duration:
                break

            values01 = gen(args, t)
            packet = build_packet(values01, seq)
            sock.sendto(packet, dest)
            seq += 1
            sent += 1

            # 每秒必打一行心跳（hold 这类定值模式否则会被误认为卡死）
            pct = tuple(int(v * 100.0 + 0.5) for v in values01)
            if now - last_print >= 1.0:
                last_print = now
                duty_raw = tuple(int(v * 1023.0 + 0.5) for v in values01)
                if ceiling_on:
                    duty = tuple(min(d, ceiling_duty) for d in duty_raw)
                else:
                    duty = duty_raw
                ua = tuple(round(d / 1023.0 * i100_ua, 2) for d in duty)
                print("  {:>6.1f}s  %: {}   duty: {}   µA: {}   已发 {}".format(
                    t,
                    " ".join("{:>5}".format(p) for p in pct),
                    " ".join("{:>4}".format(d) for d in duty),
                    " ".join("{:>6}".format(a) for a in ua),
                    sent))

            if interval > 0:
                sleep_for = (t0 + sent * interval) - time.monotonic()
                if sleep_for > 0:
                    time.sleep(sleep_for)
    except KeyboardInterrupt:
        print("\n已停止（模块将在 5 s 内把指针缓落回 0）")
    finally:
        sock.close()

    elapsed = time.monotonic() - t0
    print("共发送 {} 包 / {:.1f}s，平均 {:.1f} Hz".format(
        sent, elapsed, sent / elapsed if elapsed > 0 else 0.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
