#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
32×8 彩虹时钟（HH:MM:SS）的离线预览 + 功耗估算。

镜像 firmware/src/main.cpp 里 drawClock() / drawSecRow() 的逻辑，
在不烧录的情况下确认排版、比较底行三种秒进度样式。

用法：
    python tools/clock_preview.py                          # 当前时刻，默认样式
    python tools/clock_preview.py --time 21:47:35          # 指定时间
    python tools/clock_preview.py --secrow ticks           # 刻度样式
    python tools/clock_preview.py --secrow all             # 三种全画一遍
    python tools/clock_preview.py --v 64                   # 明度 64
    python tools/clock_preview.py --scan                   # 扫描全部 HH:MM:SS 找最坏电流

说明：
    · 电流估算用 docs/05 的模型 I(mA) = 0.0784 × Σ(R+G+B)，
      且 HSV 饱和时必有一通道为 0，故 Σ(R+G+B) ≈ 1.5 × V。
    · 软件封顶阈值 Σ = 12755 ≈ 1000 mA，固件会对超出的帧整体缩放。
"""

import argparse
import datetime

COLS, ROWS = 32, 8
CLOCK_TOP = 0
DIGIT_W = 4
DIGIT_X = [1, 6, 12, 17, 23, 28]     # HH MM SS 六位
COLON_X = [10, 21]
PROGRESS_ROW = 7

# 4×7 窄体数字字模，MSB = 最左列（与固件 kDigitFont 保持一致）
# 32 列要放 6 位数字 + 2 个冒号，5 宽字模需 37 列放不下。
DIGIT_FONT = {
    0: [0x6, 0x9, 0x9, 0x9, 0x9, 0x9, 0x6],
    1: [0x2, 0x6, 0x2, 0x2, 0x2, 0x2, 0x7],
    2: [0x6, 0x9, 0x1, 0x2, 0x4, 0x8, 0xF],
    3: [0x6, 0x9, 0x1, 0x3, 0x1, 0x9, 0x6],
    4: [0x1, 0x3, 0x5, 0x9, 0xF, 0x1, 0x1],
    5: [0xF, 0x8, 0xE, 0x1, 0x1, 0x9, 0x6],
    6: [0x6, 0x8, 0xE, 0x9, 0x9, 0x9, 0x6],
    7: [0xF, 0x1, 0x2, 0x4, 0x8, 0x8, 0x8],
    8: [0x6, 0x9, 0x9, 0x6, 0x9, 0x9, 0x6],
    9: [0x6, 0x9, 0x9, 0x7, 0x1, 0x2, 0xC],
}

SECROWS = ["bar", "off", "ticks"]
SECROW_CN = {"bar": "秒进度条", "off": "关闭", "ticks": "刻度"}


def blank():
    return [["."] * COLS for _ in range(ROWS)]


def put(buf, weight, x, y, ch, w=None):
    """weight 是该像素相对 V 的明度系数，用于最后的功耗估算。"""
    if 0 <= x < COLS and 0 <= y < ROWS:
        buf[y][x] = ch
        weight[y][x] = 1.0 if w is None else w


def draw_secrow(buf, weight, sec, mode):
    if mode == "off":
        return
    if mode == "ticks":
        cur = int(sec / 5.0)                     # 0..11
        for x in range(COLS):
            tick = x * 12 // COLS
            f = 0.85 if tick == cur else (0.22 if tick <= cur else 0.06)
            put(buf, weight, x, PROGRESS_ROW, "=" if tick == cur else "-", f)
        return
    # bar：0..59 秒映射到 32 px
    for x in range(COLS):
        travelled = sec >= x * 60.0 / COLS
        if travelled:
            put(buf, weight, x, PROGRESS_ROW, "=",
                0.35 + 0.45 * x / COLS)
        else:
            put(buf, weight, x, PROGRESS_ROW, ".", 0.05)


def render(hour, minute, sec, secrow="bar"):
    buf, weight = blank(), [[0.0] * COLS for _ in range(ROWS)]

    digits = [hour // 10, hour % 10, minute // 10, minute % 10, sec // 10, sec % 10]
    for d, x0 in zip(digits, DIGIT_X):
        for row in range(7):
            bits = DIGIT_FONT[d][row]
            for col in range(DIGIT_W):
                if bits & (0x08 >> col):
                    put(buf, weight, x0 + col, CLOCK_TOP + row, "#")

    for cx in COLON_X:
        put(buf, weight, cx, 2, "o")
        put(buf, weight, cx, 4, "o")

    draw_secrow(buf, weight, sec, secrow)
    return buf, weight


def estimate_ma(weight, v=48):
    total = sum(sum(row) for row in weight) * v
    # Σ(R+G+B) ≈ 1.5 × V ；I(mA) = 0.0784 × Σ
    return 0.0784 * 1.5 * total


def show(buf, title):
    print(title)
    print("   " + "".join(str(x % 10) for x in range(COLS)))
    for y, row in enumerate(buf):
        print("%d |%s|" % (y, "".join(row)))


def _digits_weight(hh, mm, ss):
    """数字部分的明度权重之和（每位独立，与底行无关）。"""
    total = 0.0
    for d in [hh // 10, hh % 10, mm // 10, mm % 10, ss // 10, ss % 10]:
        total += sum(bin(b).count("1") for b in DIGIT_FONT[d])
    return total


def _secrow_weight(sec, mode):
    """底行的明度权重之和（只依赖秒）。"""
    if mode == "off":
        return 0.0
    if mode == "ticks":
        cur = int(sec / 5.0)
        return sum(0.85 if x * 12 // COLS == cur
                   else (0.22 if x * 12 // COLS <= cur else 0.06)
                   for x in range(COLS))
    # bar
    return sum((0.35 + 0.45 * x / COLS) if sec >= x * 60.0 / COLS else 0.05
               for x in range(COLS))


def scan(v, colon_v=None):
    """扫全部 HH:MM:SS，找出最坏电流组合。

    数字权重与 HH:MM 独立，底行只依赖秒，所以可以解析求解，
    不必真的渲染 24×60×60×3 遍。
    """
    print("扫描全部 24×60×60 组合，V=%d ..." % v)
    print("%-10s %12s %10s" % ("底行", "最坏组合", "电流"))
    for mode in SECROWS:
        # HH / MM 各自独立取最大
        bh = max(range(24), key=lambda h: _digits_weight(h, 0, 0))
        bm = max(range(60), key=lambda m: _digits_weight(0, m, 0))
        # SS 同时影响数字与底行，必须全扫
        bs = max(range(60),
                 key=lambda s: _digits_weight(0, 0, s) + _secrow_weight(s, mode))
        total = (_digits_weight(bh, bm, bs) + _secrow_weight(bs, mode)
                 + 2 * 2)          # 两个冒号各 2 颗
        ma = 0.0784 * 1.5 * total * v
        flag = "  ⚠ 接近 1000mA 封顶" if ma > 850 else ""
        print("%-10s %02d:%02d:%02d %8.0f mA%s"
              % (SECROW_CN[mode], bh, bm, bs, ma, flag))


def main():
    ap = argparse.ArgumentParser(description="32×8 彩虹时钟(HH:MM:SS)离线预览")
    ap.add_argument("--time", default=None, help="HH:MM:SS，默认取当前时间")
    ap.add_argument("--secrow", default="bar",
                    help="bar / off / ticks / all（默认 bar）")
    ap.add_argument("--v", type=int, default=48, help="明度 V，默认 48")
    ap.add_argument("--scan", action="store_true", help="扫描全部时间组合找最坏电流")
    args = ap.parse_args()

    if args.scan:
        scan(args.v)
        return

    now = datetime.datetime.now()
    if args.time:
        parts = [int(p) for p in args.time.split(":")]
        hh, mm = parts[0], parts[1]
        ss = parts[2] if len(parts) > 2 else 0
    else:
        hh, mm, ss = now.hour, now.minute, now.second

    modes = SECROWS if args.secrow == "all" else [args.secrow]

    print("=== %02d:%02d:%02d   V=%d ===" % (hh, mm, ss, args.v))
    for mode in modes:
        buf, weight = render(hh, mm, ss, mode)
        show(buf, "\n[%s] %s" % (mode, SECROW_CN[mode]))
        lit = sum(1 for row in buf for c in row if c != ".")
        ma = estimate_ma(weight, args.v)
        flag = "  ⚠ 接近 1000mA 封顶" if ma > 850 else ""
        print("   点亮 %d 颗   估算电流 ≈ %.0f mA%s" % (lit, ma, flag))
    print("\n( '#' 数字  'o' 冒号  '=' 底行已走过  '-' 刻度  '.' 未走 )")


if __name__ == "__main__":
    main()
