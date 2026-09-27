//! 仪表盘上位机 · GPUI 版
//!
//! 5 个下拉框选 5 路表头指标（6 选 5），1 个字符串输入框取字模渲染到 32×8 灯阵。
//! 后台按 60fps 发像素、10Hz 发表头、10s 授时。协议见 docs/07-host-integration.md。

mod audio;
mod discovery;
mod font5x7;
mod metrics;
mod protocol;
mod render;
mod spectrum;
#[cfg(target_os = "macos")]
mod statusbar;

use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use gpui::{
    div, prelude::*, px, rgb, Application, Bounds, Context, Div, FocusHandle, InteractiveElement,
    IntoElement, KeyDownEvent, MouseButton, Render, SharedString, Styled, Window, WindowBounds,
    WindowOptions,
};

use audio::{AudioStatus, AudioTap};
use discovery::{Device, Discovery};
use metrics::{Collector, Metric, Ranges};
use protocol::Sender;
use render::{render_spectrum, render_text, scroll_for, COLS, RGB_BYTES, ROWS};
use spectrum::{Spectrum, FFT_SIZE};

const PIXEL_COLOR: (u8, u8, u8) = (0, 220, 160);
const BRIGHTNESS: u8 = 60;
const SCROLL_MS_PER_COL: u32 = 80;

/// 灯阵画面来源
///
/// 注意：灯阵和表头是**两路独立的数据源**，互不绑定。
/// 可以「灯阵走频谱 + 表头走系统指标」，也可以反过来。
#[derive(Clone, Copy, PartialEq, Eq)]
enum DisplayMode {
    /// 输入框文字取字模横向滚动（原行为）
    Text,
    /// 系统音频频谱
    Spectrum,
    /// 不推灯阵：像素帧一个都不发，设备端 5 s 后自己转彩虹时钟。
    /// 表头是另一路，不受影响。
    Off,
}

impl DisplayMode {
    fn label(&self) -> &'static str {
        match self {
            Self::Text => "文字",
            Self::Spectrum => "频谱",
            Self::Off => "无",
        }
    }
}

/// 5 路表头的数据来源
#[derive(Clone, Copy, PartialEq, Eq)]
enum MeterSource {
    /// 系统指标，5 路各自选（CPU / 内存 / 网速 / 磁盘 …）
    System,
    /// 音频频段，5 路固定对应 低 / 中低 / 中 / 中高 / 高频
    Audio,
}

impl MeterSource {
    fn label(&self) -> &'static str {
        match self {
            Self::System => "系统指标",
            Self::Audio => "音频频段",
        }
    }
}

/// 表头走音频时 5 路各自代表的频段
const METER_BAND_LABELS: [&str; 5] = ["低频", "中低频", "中频", "中高频", "高频"];

struct DashboardApp {
    discovery: Discovery,
    /// 已选中设备的 fullname。mDNS 发现到第一台就自动选中，
    /// 但**选中不等于推送** —— 必须显式点「开始推送」才建立 sender。
    selected: Option<String>,
    /// 是否正在推送。这是唯一的发送开关：false 时 sender 恒为 None，
    /// 一个包都不发（固件侧 5 s 收不到会自己转演示扫针）。
    pushing: bool,
    sender: Option<Sender>,
    send_err: Option<String>,
    dev_dropdown: bool, // 设备下拉框是否展开
    metrics: Collector,
    meter_sel: [Metric; 5],
    dropdown: Option<usize>, // 哪个表头下拉框展开（0..4）
    input: String,
    input_focused: bool,
    focus_handle: FocusHandle,
    elapsed_ms: u32,
    last_pixel_frame: [u8; RGB_BYTES],
    last_sample: [u16; 6],
    sample_ts: Instant,
    fps: f32,
    frame_count: u32,
    last_fps_ts: Instant,
    /// 灯阵画面来源
    mode: DisplayMode,
    /// 5 路表头的数据来源（与灯阵独立）
    meter_src: MeterSource,
    /// 音频采集。懒启动：只在频谱模式下存在，切回文字模式就停掉，
    /// 这样 macOS 的录制指示灯不会一直亮着。
    audio: Option<AudioTap>,
    spectrum: Spectrum,
    audio_buf: Vec<f32>,
    /// 频谱模式下的 5 路表头值（0..1），供 UI 显示
    audio_meters: [f32; 5],
}

fn rgb_of(c: (u8, u8, u8)) -> gpui::Rgba {
    rgb(((c.0 as u32) << 16) | ((c.1 as u32) << 8) | (c.2 as u32))
}

impl DashboardApp {
    fn new(cx: &mut Context<Self>) -> Self {
        Self {
            discovery: Discovery::new(),
            selected: None,
            pushing: false,
            sender: None,
            send_err: None,
            dev_dropdown: false,
            metrics: Collector::new(Ranges::default()),
            meter_sel: [
                Metric::Cpu,
                Metric::Mem,
                Metric::NetTx,
                Metric::NetRx,
                Metric::DiskRead,
            ],
            dropdown: None,
            input: "HELLO".into(),
            input_focused: false,
            focus_handle: cx.focus_handle(),
            elapsed_ms: 0,
            last_pixel_frame: [0; RGB_BYTES],
            last_sample: [0; 6],
            sample_ts: Instant::now(),
            fps: 0.0,
            frame_count: 0,
            last_fps_ts: Instant::now(),
            mode: DisplayMode::Text,
            meter_src: MeterSource::System,
            audio: None,
            spectrum: Spectrum::new(audio::SAMPLE_RATE as f32),
            audio_buf: vec![0.0; FFT_SIZE],
            audio_meters: [0.0; 5],
        }
    }

    /// 灯阵或表头任一路用到音频，就得开着采集。
    fn audio_needed(&self) -> bool {
        self.mode == DisplayMode::Spectrum || self.meter_src == MeterSource::Audio
    }

    /// 按当前数据源启停音频采集。懒启动：两边都不用音频就停掉，
    /// 这样 macOS 的录制指示灯不会一直亮着。
    fn sync_audio(&mut self) {
        if self.audio_needed() {
            if self.audio.is_none() {
                self.audio = Some(AudioTap::start());
            }
        } else if self.audio.is_some() {
            // Drop 会 join 采集线程并 stop_capture，录制指示灯随之熄灭
            self.audio = None;
            self.audio_meters = [0.0; 5];
        }
    }

    fn tick_pixel(&mut self) {
        let dt = 16u32;
        self.elapsed_ms = self.elapsed_ms.wrapping_add(dt);

        // 跑 FFT 的条件是「灯阵走频谱 **或** 表头走音频频段」——两路都要用频段值，
        // 只按灯阵模式判断的话，表头选了音频频段却会一直读到 0。
        let need_audio = self.mode == DisplayMode::Spectrum || self.meter_src == MeterSource::Audio;
        if need_audio {
            match &self.audio {
                Some(a) => {
                    // 取最新 FFT_SIZE 个样本；没数据就是全 0，频谱自然落到地板
                    a.ring().latest(FFT_SIZE, &mut self.audio_buf);
                    a.refresh_status();
                }
                None => self.audio_buf.fill(0.0),
            }
            self.spectrum.update(&self.audio_buf, dt as f32 / 1000.0);
            self.audio_meters = self.spectrum.meter_levels();
        }

        match self.mode {
            DisplayMode::Text => {
                let scroll = scroll_for(&self.input, self.elapsed_ms, SCROLL_MS_PER_COL);
                render_text(
                    &self.input,
                    scroll,
                    PIXEL_COLOR.0,
                    PIXEL_COLOR.1,
                    PIXEL_COLOR.2,
                    &mut self.last_pixel_frame,
                );
            }
            DisplayMode::Spectrum => {
                render_spectrum(
                    self.spectrum.levels(),
                    self.spectrum.peaks(),
                    &mut self.last_pixel_frame,
                );
            }
            // 「无」：灯阵一路不渲染也不发送，预览保持全黑。
            // 表头是另一路，照常走自己的数据源。
            DisplayMode::Off => self.last_pixel_frame.fill(0),
        }

        // 画面照常渲染（界面上的预览要看），但只有 pushing 才真的发出去。
        // 「无」模式下帧内容是全 0，这里要连包一起省掉 —— 设备端 5 s 后转彩虹时钟。
        if self.pushing && self.mode != DisplayMode::Off {
            if let Some(s) = &mut self.sender {
                s.send_pixels(&self.last_pixel_frame, BRIGHTNESS);
            }
        }
        self.frame_count += 1;
        let now = Instant::now();
        let d = now.duration_since(self.last_fps_ts).as_secs_f64();
        if d >= 1.0 {
            self.fps = self.frame_count as f32 / d as f32;
            self.frame_count = 0;
            self.last_fps_ts = now;
        }
    }

    /// 表头数据源与灯阵无关：只认 `meter_src`
    fn tick_meters(&mut self) {
        let mut m = [0u16; 5];
        match self.meter_src {
            MeterSource::Audio => {
                for i in 0..5 {
                    m[i] = (self.audio_meters[i].clamp(0.0, 1.0) * 65535.0) as u16;
                }
            }
            MeterSource::System => {
                let v = self.metrics.sample();
                self.last_sample = v;
                self.sample_ts = Instant::now();
                for i in 0..5 {
                    m[i] = v[self.meter_sel[i] as usize];
                }
            }
        }
        if self.pushing {
            if let Some(s) = &mut self.sender {
                s.send_meters(&m);
            }
        }
    }

    fn tick_time(&mut self) {
        if !self.pushing {
            return;
        }
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs() as u32;
        if let Some(s) = &mut self.sender {
            s.send_time(now);
        }
    }

    /// 按当前「选中设备 + 推送开关」同步 sender。端口取自 SRV，不写死。
    ///
    /// 不变量：**`pushing == false` 时 `sender` 恒为 None**，一个包都发不出去。
    /// 设备端 5 s 收不到包会自己转演示扫针，所以「停止推送」不需要额外协议。
    fn sync_sender(&mut self) {
        if !self.pushing {
            self.sender = None;
            return;
        }
        self.send_err = None;
        let Some(fullname) = self.selected.clone() else {
            self.sender = None;
            return;
        };
        let Some(dev) = self.discovery.get(&fullname).cloned() else {
            // 选中的设备下线了：停推送，等下一次发现重新选中一台
            self.sender = None;
            self.selected = None;
            self.pushing = false;
            self.send_err = Some("选中的设备已离线，已停止推送".into());
            return;
        };
        match Sender::new(dev.addr) {
            Ok(s) => self.sender = Some(s),
            Err(e) => {
                self.sender = None;
                self.pushing = false;
                self.send_err = Some(e.to_string());
            }
        }
    }

    /// 「开始推送 / 停止推送」按钮
    fn toggle_push(&mut self) {
        self.input_focused = false;
        if self.pushing {
            self.pushing = false;
            self.sender = None;
            self.send_err = None;
            return;
        }
        if self.selected.is_none() {
            self.send_err = Some("还没有可用设备，等 mDNS 发现后再试".into());
            return;
        }
        self.pushing = true;
        self.sync_sender();
    }

    /// 拉取发现事件。返回 true 表示设备表变了，UI 要重画。
    ///
    /// 发现到设备且当前没有选中目标时，**默认选中第一台**（按名字排序，顺序稳定）。
    /// 只选中、不推送 —— 发不发由 `pushing` 决定。
    fn tick_discovery(&mut self) -> bool {
        let changed = self.discovery.poll();
        if !changed {
            return false;
        }
        let need_pick = match &self.selected {
            None => true,
            Some(f) => self.discovery.get(f).is_none(), // 选中的那台下线了
        };
        if need_pick {
            self.selected = self.discovery.devices().first().map(|d| d.fullname.clone());
        }
        // 地址可能随 DHCP 变了，重连一次让目标跟上；未推送时这步只是把 sender 置空
        self.sync_sender();
        changed
    }

    fn handle_key(&mut self, ev: &KeyDownEvent, _window: &mut Window, cx: &mut Context<Self>) {
        if !self.input_focused {
            return;
        }
        let ks = ev.keystroke.key.clone();
        if ev.keystroke.modifiers.control || ev.keystroke.modifiers.platform || ev.keystroke.modifiers.alt {
            return;
        }
        match ks.as_str() {
            "escape" => self.input_focused = false,
            "backspace" => { self.input.pop(); }
            "enter" => self.input_focused = false,
            "space" => self.input.push(' '),
            "tab" => {}
            other => {
                if other.chars().count() == 1 {
                    if let Some(ch) = other.chars().next() {
                        if (ch as u32) < 256 && ch != '\u{0}' {
                            self.input.push(ch);
                        }
                    }
                }
            }
        }
        cx.notify();
    }
}

impl Render for DashboardApp {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let bg = rgb(0x1e1e22);
        let panel = rgb(0x2a2a30);
        let accent = rgb(0x3a3a44);
        let text = rgb(0xe8e8e8);
        let muted = rgb(0x8a8a92);
        let ok = rgb(0x4ade80);
        let warn = rgb(0xf87171);

        let (conn_color, conn_text) = match (&self.sender, &self.send_err) {
            (Some(_), _) => (
                ok,
                SharedString::from(format!(
                    "推送中 → {}",
                    self.selected.as_deref().unwrap_or("?")
                )),
            ),
            (None, Some(e)) => (warn, SharedString::from(format!("错误：{}", e))),
            (None, None) => (
                muted,
                SharedString::from(if self.pushing { "连接中…" } else { "未推送" }),
            ),
        };

        // 底部推送行要用的目标描述
        let target_desc: SharedString = match &self.selected {
            Some(f) => match self.discovery.get(f) {
                Some(d) => format!("{} · {}", d.name, d.addr).into(),
                None => "设备已离线".into(),
            },
            None => "未选择设备".into(),
        };

        // ---- 设备选择下拉框 ----
        // 设备列表先收集成独立 Vec，避免闭包里再借用 self.discovery 打架
        let devs: Vec<Device> = self.discovery.devices();
        let dev_open = self.dev_dropdown;
        let sel_label: SharedString = match &self.selected {
            Some(f) => match self.discovery.get(f) {
                Some(d) => format!(
                    "{} · {} · v{}",
                    d.name,
                    d.addr,
                    d.ver.as_deref().unwrap_or("?")
                )
                .into(),
                None => "设备已离线".into(),
            },
            None => {
                if devs.is_empty() {
                    "未发现设备".into()
                } else {
                    format!("选择设备（{} 台）", devs.len()).into()
                }
            }
        };

        let mut dev_box = div()
            .flex()
            .flex_col()
            .gap(px(2.0))
            .child(
                div()
                    .id("devbtn")
                    .px_3()
                    .py_1()
                    // 绿色只表示「已选中目标」，不代表在推送（推送看底部按钮）
                    .bg(if self.selected.is_some() { ok } else { accent })
                    .text_color(if self.selected.is_some() { bg } else { text })
                    .text_sm()
                    .child(SharedString::from(format!("{} ▼", sel_label)))
                    .on_mouse_down(MouseButton::Left, cx.listener(|this, _ev, _w, cx| {
                        this.dev_dropdown = !this.dev_dropdown;
                        this.input_focused = false;
                        cx.notify();
                    })),
            );

        if dev_open {
            let mut items = div().flex().flex_col().gap(px(1.0));
            for (di, d) in devs.into_iter().enumerate() {
                let is_sel = self.selected.as_deref() == Some(d.fullname.as_str());
                // 带上规格摘要：一眼能看出不同批次的设备是不是同一种
                let spec = match (d.leds, d.meters) {
                    (Some(l), Some(m)) => format!(" · {}灯{}表", l, m),
                    (Some(l), None) => format!(" · {}灯", l),
                    _ => String::new(),
                };
                let label = SharedString::from(format!(
                    "{}{} · {}{} · v{}",
                    if is_sel { "● " } else { "  " },
                    d.name,
                    d.addr,
                    spec,
                    d.ver.as_deref().unwrap_or("?"),
                ));
                let fname = d.fullname.clone();
                items = items.child(
                    div()
                        .id(("dev", di))
                        .px_3()
                        .py_1()
                        .bg(panel)
                        .text_color(text)
                        .text_sm()
                        .child(label)
                        .on_mouse_down(MouseButton::Left, cx.listener(move |this, _ev, _w, cx| {
                            // 再点一次已选中的 = 取消选择。
                            // 正在推送就顺带停下，否则会卡在「按钮显示停止推送、
                            // 但没有目标、一个包也发不出去」的怪状态。
                            if this.selected.as_deref() == Some(fname.as_str()) {
                                this.selected = None;
                                this.pushing = false;
                            } else {
                                this.selected = Some(fname.clone());
                            }
                            this.dev_dropdown = false;
                            // 换设备只重建目标，不自动开始推送
                            this.sync_sender();
                            cx.notify();
                        })),
                );
            }
            dev_box = dev_box.child(items);
        }
        // mDNS 自身出错（无可用网卡、守护进程起不来）必须显式暴露，
        // 否则用户面对一个空设备列表完全无从下手。
        if let Some(e) = self.discovery.err() {
            dev_box = dev_box.child(
                div()
                    .text_xs()
                    .text_color(warn)
                    .child(SharedString::from(e.to_string())),
            );
        }

        // ---- 表头设置 ----
        // 系统指标 → 5 个下拉框各选一路；音频频段 → 5 路固定绑频段，只做实时显示
        let mut rows: Vec<Div> = Vec::with_capacity(5);
        for i in 0..5usize {
            let mut dd = div()
                .flex()
                .flex_col()
                .gap(px(4.0))
                .child(
                    div()
                        .flex()
                        .items_center()
                        .gap(px(8.0))
                        .child(
                            div()
                                .text_color(muted)
                                .text_sm()
                                .child(SharedString::from(format!("表头{}", i + 1))),
                        ),
                );

            if self.meter_src == MeterSource::System {
                let cur = self.meter_sel[i];
                let open = self.dropdown == Some(i);
                dd = dd.child(
                    div()
                        .id(("ddbtn", i))
                        .px_3()
                        .py_1()
                        .bg(accent)
                        .text_color(text)
                        .text_sm()
                        .child(SharedString::from(format!("{} ▼", cur.label())))
                        .on_mouse_down(MouseButton::Left, cx.listener(move |this, _ev, _w, cx| {
                            this.dropdown = if this.dropdown == Some(i) { None } else { Some(i) };
                            this.input_focused = false;
                            cx.notify();
                        })),
                );
                if open {
                    let mut list = div().flex().flex_col().gap(px(1.0));
                    for (mi, m) in Metric::ALL.iter().enumerate() {
                        list = list.child(
                            div()
                                .id(("opt", i * 6 + mi))
                                .px_3()
                                .py_1()
                                .bg(panel)
                                .text_color(text)
                                .text_sm()
                                .child(SharedString::from(m.label()))
                                .on_mouse_down(MouseButton::Left, cx.listener(move |this, _ev, _w, cx| {
                                    this.meter_sel[i] = Metric::ALL[mi];
                                    this.dropdown = None;
                                    cx.notify();
                                })),
                        );
                    }
                    dd = dd.child(list);
                }
            } else {
                let v = self.audio_meters[i].clamp(0.0, 1.0);
                dd = dd.child(
                    div()
                        .flex()
                        .items_center()
                        .gap(px(8.0))
                        .child(
                            div()
                                .w(px(120.0))
                                .h(px(8.0))
                                .bg(accent)
                                .child(div().w(px(120.0 * v)).h_full().bg(ok)),
                        )
                        .child(
                            div()
                                .text_sm()
                                .text_color(text)
                                .child(SharedString::from(format!(
                                    "{} {:.0}%",
                                    METER_BAND_LABELS[i],
                                    v * 100.0
                                ))),
                        ),
                );
            }
            rows.push(dd);
        }

        // 像素预览
        let mut preview_rows: Vec<Div> = Vec::with_capacity(ROWS);
        for y in 0..ROWS {
            let mut row = div().flex().flex_row();
            for x in 0..COLS {
                let idx = (y * COLS + x) * 3;
                let r = self.last_pixel_frame[idx];
                let g = self.last_pixel_frame[idx + 1];
                let b = self.last_pixel_frame[idx + 2];
                let c = rgb(((r as u32) << 16) | ((g as u32) << 8) | b as u32);
                row = row.child(div().size(px(8.0)).bg(c));
            }
            preview_rows.push(row);
        }
        let mut preview = div().flex().flex_col().gap(px(1.0));
        for r in preview_rows {
            preview = preview.child(r);
        }

        let mode = self.mode;
        let fps = self.fps;

        // 音频状态：没授权、或系统压根没在放声音，都得让用户看得见，
        // 否则面对一条不动的频谱完全无从下手。
        let audio_status: SharedString = match &self.audio {
            None => "音频采集未启动".into(),
            Some(a) => match a.status() {
                AudioStatus::Idle => {
                    "等待音频…（首次会弹权限请求，请在系统设置里允许「屏幕与系统音频录制」）".into()
                }
                AudioStatus::Running => "正在采集系统音频".into(),
                AudioStatus::Silent => "已连接，但没收到声音（系统当前是否在播放？）".into(),
                AudioStatus::Failed(e) => format!("采集失败：{e}").into(),
            },
        };
        let audio_ok = matches!(
            self.audio.as_ref().map(|a| a.status()),
            Some(AudioStatus::Running)
        );

        // ---- 两条独立的数据源切换条 ----
        // 灯阵走什么画面、表头走什么数据，互不影响；两边任一用到音频才开采集。
        let mut mode_bar = div().flex().flex_row().items_center().gap(px(6.0));
        for m in [DisplayMode::Text, DisplayMode::Spectrum, DisplayMode::Off] {
            let active = mode == m;
            mode_bar = mode_bar.child(
                div()
                    .id(("mode", m as usize))
                    .px_3()
                    .py_1()
                    .bg(if active { ok } else { accent })
                    .text_color(if active { bg } else { text })
                    .text_sm()
                    .child(SharedString::from(m.label()))
                    .on_mouse_down(MouseButton::Left, cx.listener(move |this, _ev, _w, cx| {
                        this.mode = m;
                        this.sync_audio();
                        cx.notify();
                    })),
            );
        }

        let mut src_bar = div().flex().flex_row().items_center().gap(px(6.0));
        for s in [MeterSource::System, MeterSource::Audio] {
            let active = self.meter_src == s;
            src_bar = src_bar.child(
                div()
                    .id(("msrc", s as usize))
                    .px_3()
                    .py_1()
                    .bg(if active { ok } else { accent })
                    .text_color(if active { bg } else { text })
                    .text_sm()
                    .child(SharedString::from(s.label()))
                    .on_mouse_down(MouseButton::Left, cx.listener(move |this, _ev, _w, cx| {
                        this.meter_src = s;
                        this.dropdown = None;
                        this.sync_audio();
                        cx.notify();
                    })),
            );
        }

        // ---- 左列：灯阵 ----
        let mut col_matrix = div()
            .flex()
            .flex_col()
            .gap(px(8.0))
            .w(px(360.0))
            .child(
                div()
                    .text_color(muted)
                    .text_sm()
                    .child(SharedString::from("灯阵数据源（32×8 像素）")),
            )
            .child(mode_bar);

        // 输入区（文字）／音频状态（频谱）／关闭提示（无）：
        // 三个分支的子树不同，用 if 分派避免统一类型
        if mode == DisplayMode::Off {
            col_matrix = col_matrix.child(
                div()
                    .px_3()
                    .py_2()
                    .bg(panel)
                    .text_xs()
                    .text_color(muted)
                    .child(SharedString::from(
                        "不发像素帧。设备端 5 s 收不到会转彩虹时钟；表头不受影响，照常按自己的数据源推送",
                    )),
            );
        } else if mode == DisplayMode::Text {
            col_matrix = col_matrix
                .child(
                    div()
                        .id("input")
                        .flex()
                        .flex_row()
                        .items_center()
                        .px_3()
                        .py_2()
                        .bg(panel)
                        .child(
                            div()
                                .flex_1()
                                .text_color(if self.input_focused { rgb_of((180, 220, 255)) } else { text })
                                .child(SharedString::from(format!("{}{}", self.input, if self.input_focused { "▏" } else { "" }))),
                        )
                        .on_mouse_down(MouseButton::Left, cx.listener(|this, _ev, w, cx| {
                            this.input_focused = true;
                            this.dropdown = None;
                            this.focus_handle.focus(w);
                            cx.notify();
                        })),
                )
                .child(div().text_xs().text_color(muted).child(SharedString::from(format!("当前 {} 字符，位宽 {}", self.input.chars().count(), self.input.chars().count() * render::CHAR_PITCH))));
        } else {
            col_matrix = col_matrix
                .child(
                    div()
                        .flex()
                        .flex_row()
                        .items_center()
                        .gap(px(8.0))
                        .px_3()
                        .py_2()
                        .bg(panel)
                        .child(div().w(px(10.0)).h(px(10.0)).bg(if audio_ok { ok } else { warn }))
                        .child(div().text_xs().child(audio_status)),
                )
                .child(div().text_xs().text_color(muted).child(SharedString::from(
                    "系统音频 → FFT → 40 Hz–8 kHz 混合分频 32 段",
                )));
        }

        // ---- 右列：表头 ----
        let mut col_meter = div()
            .flex()
            .flex_col()
            .gap(px(8.0))
            .w(px(360.0))
            .child(
                div()
                    .text_color(muted)
                    .text_sm()
                    .child(SharedString::from("表头数据源（5 路动圈表）")),
            )
            .child(src_bar);
        for d in rows.into_iter() {
            col_meter = col_meter.child(d);
        }
        if self.meter_src == MeterSource::Audio {
            col_meter = col_meter.child(
                div()
                    .text_xs()
                    .text_color(muted)
                    .child(SharedString::from("5 路固定跟随 低/中低/中/中高/高频段，不再选系统指标")),
            );
        }

        let root = div()
            .id("root")
            .track_focus(&self.focus_handle)
            .bg(bg)
            .text_color(text)
            .size_full()
            .flex()
            .flex_col()
            .p_6()
            .gap(px(16.0))
            .on_key_down(cx.listener(Self::handle_key))
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .justify_between()
                    .child(
                        div()
                            .flex()
                            .flex_row()
                            .items_center()
                            .gap(px(16.0))
                            .child(div().text_xl().child(SharedString::from("仪表盘上位机"))),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_row()
                            .items_center()
                            .gap(px(12.0))
                            .child(div().text_sm().child(SharedString::from(format!("fps {:.0}", fps))))
                            .child(div().text_sm().text_color(conn_color).child(conn_text))
                            .child(dev_box),
                    ),
            )
            .child(
                div()
                    .flex()
                    .flex_row()
                    .gap(px(24.0))
                    .child(col_matrix)
                    .child(col_meter),
            );

        root.child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(8.0))
                    .child(div().text_sm().text_color(muted).child(SharedString::from("32×8 灯阵预览（实际发送内容）")))
                    .child(preview),
            )
            // ---- 推送开关：选中设备只是定目标，点这里才真的开始发 ----
            .child(
                div()
                    .flex()
                    .flex_row()
                    .items_center()
                    .gap(px(16.0))
                    .child(
                        div()
                            .id("pushbtn")
                            .px_4()
                            .py_2()
                            .bg(if self.pushing { warn } else { ok })
                            .text_color(bg)
                            .text_base()
                            .child(SharedString::from(if self.pushing { "停止推送" } else { "开始推送" }))
                            .on_mouse_down(MouseButton::Left, cx.listener(|this, _ev, _w, cx| {
                                this.toggle_push();
                                cx.notify();
                            })),
                    )
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(2.0))
                            .text_xs()
                            .text_color(muted)
                            .child(SharedString::from(if self.pushing {
                                format!("正在向 {} 发送像素 60fps / 表头 10Hz", target_desc.clone())
                            } else {
                                format!("已就绪：{}（点「开始推送」后才发包）", target_desc.clone())
                            }))
                            .child(SharedString::from(
                                "停止后设备 5 秒收不到包会自动转演示扫针，不需要额外协议",
                            )),
                    ),
            )
    }
}

/// 音频采集自检：`cargo run -- --probe-audio`
///
/// 不开界面，直接跑 5 秒采集并打印结果。用来确认三件事：
/// 权限给没给、PCM 是 f32 还是 i16、到底有没有声音进来。
/// 采集链路上任何一环出问题，先跑它。
fn probe_audio() {
    println!("系统音频采集自检（5 秒）—— 请先让 Mac 正在播放音乐\n");

    let tap = AudioTap::start();
    let t0 = Instant::now();
    let mut last = 0usize;

    while t0.elapsed() < Duration::from_secs(5) {
        std::thread::sleep(Duration::from_millis(250));
        tap.refresh_status();
        let n = tap.ring().received();
        if n != last {
            last = n;
            println!("  已收到 {} 个样本    状态：{:?}", n, tap.status());
        }
    }

    let mut buf = Vec::new();
    let got = tap.ring().latest(FFT_SIZE, &mut buf);
    let mut sum2 = 0.0f64;
    let mut peak = 0.0f32;
    for &s in &buf {
        sum2 += (s as f64) * (s as f64);
        peak = peak.max(s.abs());
    }
    let rms = (sum2 / buf.len().max(1) as f64).sqrt();

    println!("\n---- 结果 ----");
    println!("状态      ：{:?}", tap.status());
    println!("PCM 格式  ：{:?}", tap.pcm_kind().unwrap_or("未探测到"));
    println!("累计样本  ：{}", tap.ring().received());
    println!("本次取到  ：{} / {}", got, FFT_SIZE);
    println!("RMS       ：{:.6}   峰值：{:.6}", rms, peak);
    println!();

    match tap.status() {
        AudioStatus::Failed(e) => {
            println!("采集失败：{e}");
            println!("多半是没授权：系统设置 → 隐私与安全性 → 屏幕与系统音频录制，勾选本程序后重启。");
        }
        AudioStatus::Running => {
            if peak < 1e-5 {
                println!("收得到数据但全是静音。确认系统音量不为 0、且真的有声音在播。");
            } else {
                println!("采集正常。GUI 里切到「频谱」模式即可。");
            }
        }
        AudioStatus::Silent => {
            println!("已启动但没收到任何样本。检查：系统是否在播放？权限是否给了？");
        }
        AudioStatus::Idle => {
            println!("仍在等待首个样本，可再试一次（期间保持有声音在播）。");
        }
    }
}

fn main() {
    if std::env::args().any(|a| a == "--probe-audio") {
        probe_audio();
        return;
    }

    let app = Application::new();

    // 点 Dock 图标也要能把窗口叫回来（macOS 只在没有可见窗口时才发 reopen）
    app.on_reopen(|_cx| {
        #[cfg(target_os = "macos")]
        statusbar::show_window();
    });

    app.run(|cx: &mut gpui::App| {
        // 菜单栏常驻图标：窗口关掉之后还能把它叫回来、也能正经退出
        #[cfg(target_os = "macos")]
        statusbar::install();

        // 菜单栏动作在 App 级轮询（不是窗口级），窗口隐藏后照样收得到。
        // 100 ms 一次足够 —— 这是个人工触发的动作，没人能感知这 100 ms。
        #[cfg(target_os = "macos")]
        cx.spawn(async move |cx| {
            loop {
                gpui::Timer::after(Duration::from_millis(100)).await;
                if let Some(action) = statusbar::poll() {
                    let _ = cx.update(|app: &mut gpui::App| match action {
                        statusbar::StatusAction::ShowWindow => statusbar::show_window(),
                        statusbar::StatusAction::Quit => app.quit(),
                    });
                }
            }
        })
        .detach();

        let bounds = Bounds::centered(None, gpui::size(px(960.), px(720.0)), cx);
        let _ = cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                ..Default::default()
            },
            |window, cx| {
                let entity = cx.new(|cx| {
                    let app = DashboardApp::new(cx);
                    // 后台调度：像素 60fps 发送 / 表头 10Hz / 授时 10s
                    // UI notify 降到 ~10fps，避免 debug 模式下每 16ms 重渲染 256+ div 卡死事件循环
                    cx.spawn(async move |this, cx| {
                        let mut last_meter = Instant::now();
                        let mut last_time = Instant::now();
                        let mut last_notify = Instant::now();
                        loop {
                            gpui::Timer::after(Duration::from_millis(16)).await;
                            let _ = this.update(cx, |this: &mut DashboardApp, cx| {
                                this.tick_pixel();
                                // 发现事件必须勤拉取：mdns-sd 用的是容量为 10 的
                                // 有界 channel，不排空会堵住 daemon，之后再也发现不到新设备
                                let dev_changed = this.tick_discovery();
                                if last_meter.elapsed() >= Duration::from_millis(100) {
                                    last_meter = Instant::now();
                                    this.tick_meters();
                                }
                                if last_time.elapsed() >= Duration::from_secs(10) {
                                    last_time = Instant::now();
                                    this.tick_time();
                                }
                                // 设备上线/下限要立刻反应，不等 100ms 的常规重绘
                                if dev_changed || last_notify.elapsed() >= Duration::from_millis(100) {
                                    last_notify = Instant::now();
                                    cx.notify();
                                }
                            });
                        }
                    })
                    .detach();
                    app
                });
                // 关窗口 = 隐藏，不是销毁：窗口里的 entity 和挂在它上面的
                // 后台调度循环（60fps 像素 / 10Hz 表头）要继续活着推数据。
                // 返回 false 表示「别关」，真正的隐藏在 hide_window() 里做。
                #[cfg(target_os = "macos")]
                {
                    statusbar::remember_window(window);
                    window.on_window_should_close(cx, |_window, _cx| {
                        statusbar::hide_window();
                        false
                    });
                }

                // 初始聚焦到 root，让 on_key_down 能收到键盘事件
                let fh = entity.read(cx).focus_handle.clone();
                window.focus(&fh);
                entity
            },
        );
    });
}
