# 07 · 上位机对接接口文档

> **面向读者**：要写常驻上位机程序的人。
> 本文就是那张「照着它就能把画面打到屏上」的纸，协议设计原理见 [04-udp-protocol.md](04-udp-protocol.md)，
> 两者冲突时**以本文为准**（04 是设计稿，本文是实现契约）。

---

## 0. 三十秒速查

| 项 | 值 |
|---|---|
| 传输 | **UDP，数据面单向**（上位机 → 模块；像素/表头无握手、无 ACK、无重传） |
| 目标地址 | **不要再写死 IP** —— 固件默认 DHCP，用 §0.1 的 mDNS 发现，手动填 IP 仅作兜底 |
| 字节序 | **全部小端（little-endian）** —— C#/Java 侧记得显式处理 |
| 最小保活 | 像素包 **≥ 1 Hz**（推荐 60 fps）；表头包 **≥ 1 Hz**（推荐 10 Hz） |
| 三种包 | `type=1` 表头 18 B / `type=2` 像素帧 776 B / `type=3` 授时 12 B（可选） |
| 超时 | 灯阵、表头各自独立计 5 s |
| 固件诊断 | 串口 115200（TX 可用，**RX 不可用**，不要指望串口命令或回读） |

**上位机只要做三件事**：按 60 fps 发像素帧、按 10 Hz 发表头值、**画面哪怕没变化也要继续发**。

### 0.1 服务发现（先看这里，别硬编码 IP）

固件以 **DHCP** 拿 IP，并注册标准 DNS-SD 服务。上位机靠 browse 自动发现，**不需要知道 IP**。

| 项 | 值 |
|---|---|
| 服务类型 | `_dashboard._udp.local.` |
| 端口 | SRV 记录里给出（默认 8551），**以 SRV 为准，别写死** |
| 主机名 | `<host>.local`，见下表命名规则 |
| 实例名 | 同主机名，用于 UI 列表显示 |

**主机名规则**：`dash-` + ChipId 后 6 位十六进制大写（`WiFi.hostname()` 同时设置）。
例：`dash-A1B2C3.local`。用 ChipId 而非手写名字，保证**多机同网不撞名**。

**TXT 记录**（静态元数据，列设备时直接显示，避免为此扩协议）：

| key | 值 | 说明 |
|---|---|---|
| `ver` | 例如 `1.0.0` | 固件版本，排错先问它 |
| `proto` | `1` | **协议**版本 = 包头里的 `ver` 字段 |
| `mac` | MAC 字符串 | 区分多台设备的最终依据 |
| `leds` | `256` | 灯珠总数 = 像素包 payload ÷ 3 |
| `meters` | `5` | 表头路数 = `type=1` 的 uint16 个数 |
| `cols` | `32` | 逻辑宽度，配 `leds` 反推行数 |

> 刻意**不放** fps / 丢包 / 电流这类实时值到 TXT —— TXT 缓存期长且每次变更要重新通告，
> 实时数据后面用查询机制拿（尚未实现）。

**命令行验证（不用开上位机，强烈建议第一步先做这个）**：

```bash
dns-sd -B _dashboard._udp          # 持续列出上线/离线的设备
dns-sd -L dash-A1B2C3 _dashboard._udp local.   # 看某台的 SRV + TXT
ping dash-A1B2C3.local             # 确认名字可解析
```

Linux 用 `avahi-browse -r _dashboard._udp`，Windows 需装 Bonjour。

**上位机侧参考实现**（Rust，`mdns-sd` crate）：

```rust
use mdns_sd::{ServiceDaemon, ServiceEvent};

let daemon = ServiceDaemon::new()?;
let rx = daemon.browse("_dashboard._udp.local.")?;
while let Ok(ev) = rx.recv() {
    if let ServiceEvent::ServiceResolved(info) = ev {
        // 端口以 SRV 为准，别硬编码 8551
        let addr = SocketAddr::new(info.get_addresses().iter().next().copied().unwrap(), info.get_port());
        let ver  = info.get_properties().get("ver").map(|v| v.to_string());
        println!("{} @ {} ver={:?}", info.get_fullname(), addr, ver);
    }
}
```

> **要缓存 SRV 里的地址，不要缓存 IP**。IP 会随 DHCP 租期变化；
> 监听 `ServiceRemoved` / 重新 resolve 才能在变更后跟上。

**参考实现已落地**：`host/src/discovery.rs`（`Discovery::poll()` 拉取事件 + 维护设备表），
UI 在右上角设备下拉框单选，选中才推送，取消即停止（设备端自动转演示）。
可用 `cargo test discover_probe -- --ignored --nocapture` 单独验证发现链路。

> **mdns-sd 0.20 的两个 API 陷阱**（换版本时会编译不过，照这里改）：
> ① `ServiceEvent::ServiceResolved` 装的是 `Box<ResolvedService>`，**字段直接访问**
> （`fullname`/`host`/`port`/`addresses`/`txt_properties`），不是 ServiceInfo 那套 getter；
> ② `addresses` 是 `HashSet<ScopedIp>`，要 `.to_ip_addr()` 才是 `IpAddr`，
> TXT 取值是 `properties.get("ver")?.val_str()`。

**mDNS 不通时的兜底**（这三个方向依次排查）：

1. AP 开了「客户端隔离 / AP Isolation」或跨了 VLAN —— **这是最常见的原因**，关掉隔离即可
2. 固件 `config.h` 里 `kMdnsEnable` 被设成 0
3. 网络确实不适合多播 → 用静态 IP 退回手动寻址。两种方式都保留着：
   - 手机连配网热点 `dash-XXXXXX`，页面里展开「高级：使用静态 IP」填模块 IP 与网关（不用重新烧录）
   - 或把 `secrets.h` 的 `USE_STATIC_IP` 改回 `1`

> 模块的 WiFi 凭证现在**不写死**，由配网门户写进 EEPROM，
> 详见 [08-wifi-provisioning.md](08-wifi-provisioning.md)。
> 本节的"发现"发生在配网**之后**：先有 IP，才谈得上 mDNS。

---

## 1. 通用包头（8 字节，三类包共用）

```
 字节  0      1      2      3      4       5       6           7
     ┌──────┬──────┬──────┬──────┬────────────────┬────────┬────────┬───────────
     │ 0x4D │ 0x50 │ ver  │ type │     seq        │  brt   │  rsv   │ payload…
     │ 'M'  │ 'P'  │  1   │ 1/2/3│  uint16  LE    │ uint8  │  =0    │
     └──────┴──────┴──────┴──────┴────────────────┴────────┴────────┴───────────
```

| 字段 | 偏移 | 类型 | 说明 |
|---|---|---|---|
| magic | 0–1 | `0x4D 0x50` = `'M' 'P'` | 不等于这两个字节的包**直接丢弃**。用来防止网段里其它 UDP 流量污染画面 |
| ver | 2 | uint8 | **当前 `1`**。写错整包丢弃 |
| type | 3 | uint8 | `1` 表头 / `2` 像素帧 / `3` 授时 |
| seq | 4–5 | uint16 LE | 每类包**各自递增**，只用于固件统计丢包，不触发任何重传 |
| brt | 6 | uint8 | 全局亮度 0–255，**仅 type=2 生效**；固件侧硬上限 **128**，写 255 会被压到 128 |
| rsv | 7 | uint8 | **置 0**（保留，未来若启用会按位定义，非 0 值将来可能被解释为开关） |
| payload | 8– | — | 见 §2~§4 |

> **`seq` 强烈建议按包类型各自维护一个计数器。**
> 两类包共用一个计数器不会导致功能故障（固件只是用相邻包的差值估丢包），
> 但会让串口里的 `loss=` 变得毫无意义（虚高到 50% 左右），排查时被误导。

---

## 2. type=1 · 表头值（总长 18 B）

```c
payload: uint16 ch[5];   // 小端，依次为通道 1..5
         0x0000 = 零位，0xFFFF = 满偏，线性
```

| 项 | 值 |
|---|---|
| 建议频率 | **10 Hz**（恒定，不要按值变化发） |
| 链路 | 上位机值 → 固件 “latest-wins” 覆盖 → 指数缓动 τ≈200 ms → PWM 10 bit @1 kHz → 20 kΩ 限流 → 表头线圈 |
| 超时 | **5 s** 未收到 → 进入**演示模式**：5 路相位错开的三角波扫针，缓动过渡（不瞬跳） |
| 上电默认值 | 没收到过任何 type=1 包时，固件自己跑 demo 三角波；**收到第一个包起立刻接管** |
| 标定微调 | 固件侧 `include/config.h` 里 `kMeterTrim[5]`（默认全 1.00，出厂不动） |

**电学换算（上位机不用管，但要知道边界）：**

```
duty = value / 65535 × 1023
电流 = duty / 1023 × 3.3 V / (20 kΩ + 1.2 kΩ) ≈ duty/1023 × 155.6 µA
```
即 `65535` 对应**机械满偏**（约 155.6 µA）。这块板子的 20 kΩ 串联电阻就是这么定的 —— **别加大部的软件输出去“补”**，指针撞到止档会让游丝永久变形。

---

## 3. type=2 · 像素帧（总长 776 B）

```c
payload: uint8 rgb[768];   // 256 像素 × 3 字节（R,G,B）
索引:     i = y * 32 + x     // 行优先，row-major，x 先变
         rgb[3i]=R, rgb[3i+1]=G, rgb[3i+2]=B
原点:     左上角 (0,0)，y 向下增大（与 PIL/numpy 的 image 坐标一致）
```

| 项 | 值 |
|---|---|
| 分辨率 | **32 × 8**，共 256 像素，payload **必须正好 768 B**（少一字节整包丢弃） |
| 建议频率 | **60 fps**；固件是“来一帧推一帧”，喂得快就显示得快，物理硬顶约 129 fps |
| 端到端延迟 | 约 7.7 ms（线上传输）+ 50 µs（锁存），恒定，不随负载累积 |
| 旧帧处理 | latest-wins 单缓冲，**不排队、不补帧**。丢一帧就是跳过去，肉眼几乎无感 |
| 亮度 | 包头 `brt` 字段，0–255，固件侧**硬压到 128** |
| 超时 | **5 s** 未收到 → 降级到彩虹时钟（见 §6） |

### 3.1 坐标系：上位机按逻辑坐标发，硬件重映射由固件做

**这是整个项目最容易写错的一环。** 物理上 4 块 8×8 模块是「每块左旋 90°」焊接的，硬件串号顺序和逻辑 XY 完全不是一回事：

```
硬件索引: index = (x/8)*64 + (x%8)*8 + (7-y)      ← 固件里做，上位机不要碰
```

**上位机只需要保证「按行优先、(0,0) 在左上角」打包即可**，别自作聪明去加这个变换。
快速自检：发 `tools/udp_sender.py --pattern corners`，应当**左上红、右上绿、左下蓝、右下黄、底行四个洋红点**。

### 3.2 从常见图像格式打包（Python 参考）

```python
# PIL.Image img 已 resize 到 (32, 8)，模式 RGB
payload = img.tobytes()            # 行优先 RGB，768 字节 —— 直接就是 payload

# numpy ndarray arr shape (8, 32, 3) uint8
payload = arr.tobytes()            # 注意顺序是 [y][x][channel]

# 手写
def put(buf, x, y, r, g, b):
    if 0 <= x < 32 and 0 <= y < 8:
        i = (y * 32 + x) * 3
        buf[i], buf[i+1], buf[i+2] = r, g, b
```

> C/C#/Rust 侧若用二维 uchar 数组 `frame[y][x]`，内存天然就是行优先，`memcpy` 直接用。
> OpenCV 的 `Mat`(BGR) 记得**先 `cvtColor(BGR2RGB)`**，否则红蓝互换。

### 3.3 功耗：固件会强制封顶，画面会被整体压暗

固件每帧计算 `Σ = 所有像素的 (R+G+B)`（已乘过 `brt`）：

```
估算电流 mA = 0.0784 × Σ                （保守模型，实测值通常更低）
若 Σ > 12755（≈1000 mA）→ 整帧等比缩放直到达标
```

**结果是整个画面均匀地变暗，而不是某几颗灯不亮。** 如果上位机发现自己画的亮色变成了灰蒙蒙的，
第一嫌疑就是这个（`cap=` 计数在涨）。真正的解法是**自己先把内容压暗**，而不是赖 Firmware 帮忙缩：

```python
def estimate_ma(payload: bytes, brt: int) -> float:
    return 0.0784 * min(sum(payload) * brt / 255, 12755)
```

预算建议：USB 演示 ≤ 800 mA，外接 5 V/2 A ≤ 1500 mA。详见 [05-power-budget.md](05-power-budget.md)。
另外，**降饱和度不能省电**（趋近白色会让 Σ 翻倍），唯一有效手段是降明度 V。

---

## 4. type=3 · 授时（总长 12 B，可选）

```c
payload: uint32 unix;    // 小端，Unix 时间戳（UTC 秒，不是本地时间）
```

- 模块**主时间源是自己跑的 NTP**（`CST-8`，每 2 小时重同步），这个包**只用来纠偏差**。
- 生效条件严格：`unix > 1700000000`（2023-11 之后）**且** `|unix − 本地| > 5 s`，否则静默忽略。
- 设计意图：NTP 挂了（无外网）而局域网内有可信时间源时才有用。**正常情况完全可以不发**，发了也不会有坏处。
- 建议频率：≥ 10 s 一次，或干脆不发。

> 注意：**传 UTC 秒**。固件内部用 POSIX 时区串 `CST-8` 转换成本地时间，
> 上位机若图省事把东八区的本地时间塞进来，时钟会快 8 小时。

---

## 5. 参考发送端（Python，可直接抄）

```python
import socket, struct, time

HOST = ("192.168.5.50", 8551)
HDR  = "<BBBBHBB"                      # magic(2) ver type seq(LE) brt rsv

class Dashboard:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.seq_pix = 0                # ★ 每类包各自计数
        self.seq_mtr = 0

    def send_frame(self, rgb768: bytes, brightness: int = 60):
        assert len(rgb768) == 768
        hdr = struct.pack(HDR, 0x4D, 0x50, 1, 2, self.seq_pix & 0xFFFF,
                          min(brightness, 128), 0)
        self.sock.sendto(hdr + rgb768, HOST)
        self.seq_pix += 1

    def send_meters(self, values5):
        """values5: 5 个 0..65535"""
        hdr = struct.pack(HDR, 0x4D, 0x50, 1, 1, self.seq_mtr & 0xFFFF, 0, 0)
        self.sock.sendto(hdr + struct.pack("<5H", *values5), HOST)
        self.seq_mtr += 1

    def send_time(self, unix_utc: int):
        hdr = struct.pack(HDR, 0x4D, 0x50, 1, 3, 0, 0, 0)
        self.sock.sendto(hdr + struct.pack("<I", unix_utc), HOST)
```

**主循环骨架**（像素线程 60 fps + 表头线程 10 Hz，两者都要「无条件持续发」）：

```python
def loop_pixels(d: Dashboard):
    interval = 1 / 60
    while True:
        t0 = time.monotonic()
        d.send_frame(render_frame(), brightness=60)   # 内容不变也要发
        dt = interval - (time.monotonic() - t0)
        if dt > 0:
            time.sleep(dt)
```

---

## 6. 状态机与超时（上位机视角）

```
        上位机持续发 type=2
        ┌──────────────────────────┐
        ▼                          │
   ┌─────────┐  停止发 type=2 5s   │
   │  LIVE   │─────────────────────┼──▶ ┌──────────┐
   │ 上位机画面│                     │    │  CLOCK   │ 彩虹 HH:MM:SS
   └─────────┘◀──── 收到 type=2 ───┘    │ (需 NTP) │ + 底行秒进度
                                        └────┬─────┘
                                    时间未同步│
                                             ▼
                                        ┌──────────┐
                                        │ WAITING  │ "WAITING" 滚动
                                        └──────────┘
```

| 状态 | 灯阵显示 | 进入条件 |
|---|---|---|
| **LIVE** | 上位机像素帧 | 5 s 内收到过 type=2 |
| **CLOCK** | 彩虹 **HH:MM:SS**（4×7 窄体字模 + 底行秒进度） | type=2 超时 5 s **且** NTP 已同步 |
| **WAITING** | 琥珀色 "WAITING" 横向滚动 | type=2 超时 5 s **且** NTP 未同步 |

**灯阵和表头的两条超时各自独立计价**，所以会出现组合态：上位机还在推画面但停推表头值 → **画面照常显示，指针跑演示扫针**（5 路相位错开的三角波）。这是设计预期，不是 bug。

### 6.1 上位机的三条硬性纪律

1. **必须持续发。** 最常踩的坑是做「内容没变就跳过这一帧」的优化 —— 结果模块每隔 5 秒就闪一次时钟。
2. **退出时不用发再见。** 直接关程序即可，5 秒后模块自己切时钟。
3. **别在发送前做压缩/差分。** 776 B @60 fps 只有 47 KB/s，压缩省下的带宽远不值损失的延迟和复杂度。

---

## 7. 固件侧诊断输出（排错的第一现场）

串口 **115200** 每秒一行（`Serial` 的 TX 可用，**RX 已被灯阵 DMA 占用，收不了命令**）：

```
[LIVE] fps=60.0  pkt=18234  loss=0.3%  brt=60  clkV=48  mA=410  cap=0  bad=0  secrow=进度条  rssi=-48  heap=28320
[Meter] LIVE   %:  50.0  0.0 100.0  0.0  0.0   duty: 512    0 1023    0    0   pkt=1200  loss=0.2%
```

| 字段 | 含义 | 异常时的含义 |
|---|---|---|
| `state` | LIVE / CLOCK / WAIT / + Meter 侧的 DEMO / LIVE / TIMEOUT | 见 §6 |
| `fps` | 实测推帧帧率 | 远低于发送帧率 → WiFi 丢包或 CPU 被占 |
| `loss` | 由 seq 断号估的丢包率 | 持续 >2% → 查 RSSI、信道；**若两类包共用 seq 会虚高** |
| `brt` | 上位机要求的亮度（已被压到 ≤128） | 与预期不符 → 上位机写 >128 了 |
| `mA` | 当前帧估算电流 | 见 §3.3 |
| `cap` | 触发软件封顶的帧数（每秒清零） | **>0 说明画面被整体压暗了** |
| `bad` | magic / 版本 / 长度 / type 非法的包数 | **>0 说明上位机的包格式写错了** ← 对接期最重要的一个数字 |
| `rssi` | WiFi 信号强度 | 持续 < −75 dBm → 天线/位置问题 |
| `heap` | 剩余堆 | 长时间跑应基本恒定，单调下降=内存泄漏 |

> **对接期先盯 `bad`：** 只要它在涨，就别去查网络和电源 —— 一定是包头写错了（magic 顺序、`ver`、 `type`、payload 长度，四选一）。

---

## 8. 排错对照表

| 现象 | 最可能原因 | 怎么确认 |
|---|---|---|
| **`dns-sd -B _dashboard._udp` 查不到设备** | AP 开了客户端隔离 / 跨 VLAN；或 `kMdnsEnable=0` | 手机连同一 WiFi 试试；串口看有没有 `[mDNS] 已注册` 那行 |
| **能发现设备但连不上** | browsed 到的 IP 已过期（DHCP 换了租） | 重新 `dns-sd -L` 拿 SRV；**上位机别缓存 IP**（§0.1） |
| **`.local` 名字 ping 不通，IP 却能通** | 系统 mDNS resolver 缓存了旧记录 | `dscacheutil -flushcache; killall -HUP mDNSResponder` |
| 灯阵一直显示时钟，上位机在发 | 包被判非法 / 端口错 / **目标 IP 是 DHCP 换过的旧值** | 看串口 `bad=` 是否增长；改用主机名或重 browsed |
| 灯阵全黑且不确定是不是自己的错 | `brt` 写了 0，或 payload 全 0 | 同上工具先坐实链路，再怀疑自己的打包 |
| 画面整体发灰、没有期望的亮 | 触发了 1000 mA 软件封顶 | `cap=` 在涨；用 §3.3 的公式自估 mA |
| 颜色不对（红蓝互换） | OpenCV BGR 未转 RGB | 发 `corners` 图案：`tools/udp_sender.py --pattern corners` |
| 镜像 / 上下颠倒 / 四块错位 | 上位机自己做了硬件映射 | **删掉那段代码**，固件已经做了（§3.1） |
| 每 5 秒闪一次时钟 | 做了“内容没变就不发”的优化 | 改成恒定频率发 |
| 指针走不到位 / 一路不动 | 硬件接线（历史根因就是这个） | 用 `tools/meter_test.py --mode hold --value 100` 逐路打满验证 |
| 指针抖动 | 上位机发包频率过低 | 提到 10 Hz；固件侧 τ≈200 ms 缓动已把台阶磨平 |
| 一串丢包/卡顿 | WiFi 掉线重连中、或 RSSI 太差 | `rssi`；模块静态 IP + `WIFI_NONE_SLEEP` 已开 |
| `loss` 常年虚高 40~50% | 两类包共用一个 seq | 各自独立计数（§1 提醒） |

---

## 9. 版本演进与扩展预留

| 字段 | 当前 | 变更规则 |
|---|---|---|
| `ver` | `1` | 破坏性变更才升位；固件只接受与自己一致的 `ver` |
| `rsv`(byte 7) | 恒 `0` | 未来若启用会按**位**定义开关，非 0 值将被解释为新语义，**对接时务必置 0** |
| `type` | 1/2/3 | 4 及以上目前会被计数到 `bad` 后丢弃 |

**当前协议的数据面是单向的，上位机无法查询模块实时状态**（屏幕当前是 LIVE 还是时钟、丢包率多少）。
若后续要做：建议新增 `type=4` 查询 + 模块回 `type=5` 状态包，**不必升 `ver`**（加包类型是纯增量扩展，
旧固件收到未知 `type` 会丢弃，上位机查询超时降级即可）。真正的拦路虎不在协议，而在于
**上位机侧要把纯发送 socket 改成可收发**，并保留发送时的源端口。此事需同时改两侧，**现阶段未实现**。

> 注意区分两类「版本」：`ver` 是**协议**版本（包头字段），本文 §0.1 里 TXT 的 `ver` 是**固件**版本，两者独立。

---

## 10. 动手顺序建议

0. **`dns-sd -B _dashboard._udp`** —— 先确认 mDNS 能发现到设备。这步不过先去查 AP 隔离（见 §0.1 兜底）
1. `tools/udp_sender.py --ip <host>.local --pattern corners` —— 确认链路 + 映射（这步不过，后面全是白费）

   > `--ip` 支持填主机名，脚本会帮你解析；拿不到名字就退回 `--ip 192.168.5.x`
2. `--pattern gradient --fps 60` —— 确认帧率与 `loss` 可接受（目标 <1%）
3. `tools/meter_test.py --mode stairs` —— 确认 5 路表头全通
4. 停掉发送，等 5 秒 —— 确认自动降级到彩虹 HH:MM:SS，表头转演示扫针
5. 这时再开始写自己的上位机：先用参考脚本的协议代码起手，通了再换渲染内容
