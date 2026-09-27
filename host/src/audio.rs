//! 系统音频采集（macOS · ScreenCaptureKit）
//!
//! ## 为什么用 SCK 而不是虚拟声卡
//!
//! macOS 出于隐私沙盒，默认不允许任何 App 读到系统正在播放的声音。
//! 常规做法是装 BlackHole 之类的虚拟声卡做回环，但那要装内核扩展、重启、
//! 还要在「音频 MIDI 设置」里建多输出设备，且键盘音量键会失效。
//!
//! ScreenCaptureKit（macOS 13+）可以直接拿到系统音频，**零安装**，
//! 代价是需要在「系统设置 → 隐私与安全性 → 屏幕与系统音频录制」里授权本程序。
//! 这是纯粹的 TCC 权限门：没有任何 entitlement 能代替用户手动授权。
//!
//! ## 只抓音频不抓画面
//!
//! 配置里只设音频相关的几项（不开尺寸），并且**只注册 `SCStreamOutputType::Audio`
//! 回调、不注册 `Screen`**，这样就不会收到任何视频帧。注意 `SCContentFilter` 仍然
//! 必须给（SCK 的音频是挂在被捕获内容上的），这里挂第一个显示器。

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

#[cfg(target_os = "macos")]
use screencapturekit::cm::CMSampleBufferExt;
#[cfg(target_os = "macos")]
use screencapturekit::prelude::*;

/// 采集目标采样率。SCK 只支持 8000/16000/24000/48000，超出会被系统改回 48000。
pub const SAMPLE_RATE: u32 = 48_000;

/// 环形缓冲容量：48 kHz 下约 170 ms，够两个 FFT 窗口还富裕。
const RING_CAPACITY: usize = 8192;

/// 单声道样本的环形缓冲。采集线程写、UI 线程读，靠 Mutex 保护。
#[derive(Clone)]
pub struct AudioRing {
    inner: Arc<Mutex<VecDeque<f32>>>,
    capacity: usize,
    /// 累计收到的样本数。只增不减，用来判断「到底有没有声音进来」。
    received: Arc<AtomicUsize>,
}

impl AudioRing {
    fn new(capacity: usize) -> Self {
        Self {
            inner: Arc::new(Mutex::new(VecDeque::with_capacity(capacity))),
            capacity,
            received: Arc::new(AtomicUsize::new(0)),
        }
    }

    /// 追加一批单声道样本，超出容量时丢掉最旧的。
    pub fn push(&self, samples: &[f32]) {
        if samples.is_empty() {
            return;
        }
        if let Ok(mut q) = self.inner.lock() {
            for &s in samples {
                if q.len() >= self.capacity {
                    q.pop_front();
                }
                q.push_back(s);
            }
            self.received.fetch_add(samples.len(), Ordering::Relaxed);
        }
    }

    /// 取最近 n 个样本写入 out（不足则在前面补 0）。返回实际取到的有效样本数。
    pub fn latest(&self, n: usize, out: &mut Vec<f32>) -> usize {
        out.clear();
        out.resize(n, 0.0);
        let Ok(q) = self.inner.lock() else { return 0; };
        let avail = q.len().min(n);
        if avail == 0 {
            return 0;
        }
        // VecDeque 尾部是最新样本，把它们放到 out 的末尾
        let start = q.len() - avail;
        for (i, s) in q.iter().skip(start).enumerate() {
            out[n - avail + i] = *s;
        }
        avail
    }

    /// 累计收到的样本数。
    pub fn received(&self) -> usize {
        self.received.load(Ordering::Relaxed)
    }
}

/// 采集状态。UI 据此提示用户去授权或排查。
#[derive(Clone, PartialEq, Debug)]
pub enum AudioStatus {
    /// 还没启动
    Idle,
    /// 正在采集且已收到音频
    Running,
    /// 已启动但还没收到任何样本（多半是系统当前没在放声音，或授权给了但没内容）
    Silent,
    /// 启动失败，附带原因（通常是未授权）
    Failed(String),
}

pub struct AudioTap {
    ring: AudioRing,
    running: Arc<AtomicBool>,
    status: Arc<Mutex<AudioStatus>>,
    thread: Option<thread::JoinHandle<()>>,
    /// 启动时刻，用来把「还没收到样本」区分成「刚启动」和「确实没声音」
    started_at: Instant,
    /// 回调里探测出的 PCM 格式，与采集线程共享
    pcm: Arc<Mutex<Option<PcmKind>>>,
}

impl AudioTap {
    /// 启动采集。失败不会 panic，状态会写进 `status()`。
    pub fn start() -> Self {
        let ring = AudioRing::new(RING_CAPACITY);
        let running = Arc::new(AtomicBool::new(true));
        let status = Arc::new(Mutex::new(AudioStatus::Idle));

        let t_ring = ring.clone();
        let t_running = Arc::clone(&running);
        let t_status = Arc::clone(&status);
        let pcm = Arc::new(Mutex::new(None));
        let t_pcm = Arc::clone(&pcm);

        let thread = thread::spawn(move || {
            run_capture(t_ring, t_running, t_status, t_pcm);
        });

        Self { ring, running, status, thread: Some(thread), started_at: Instant::now(), pcm }
    }

    /// 回调里探测出的 PCM 格式（"f32" / "i16"），还没收到音频时是 None。
    pub fn pcm_kind(&self) -> Option<&'static str> {
        let g = self.pcm.lock().ok()?;
        (*g).map(|k| match k {
            PcmKind::F32 => "f32",
            PcmKind::I16 => "i16",
        })
    }

    pub fn ring(&self) -> &AudioRing {
        &self.ring
    }

    pub fn status(&self) -> AudioStatus {
        self.status.lock().map(|s| s.clone()).unwrap_or(AudioStatus::Idle)
    }

    /// 由 UI 线程定期调用：把启动后的 Idle 细化成 Running 或 Silent。
    ///
    /// 启动后 3 秒还没收到任何样本，基本可以判定为「系统没在放声音」或
    /// 「授权给了但抓不到」，再显示「等待音频…」就会误导用户。
    pub fn refresh_status(&self) {
        let mut s = match self.status.lock() {
            Ok(s) => s,
            Err(_) => return,
        };
        if *s != AudioStatus::Idle {
            return;
        }
        if self.ring.received() > 0 {
            *s = AudioStatus::Running;
        } else if self.started_at.elapsed() >= Duration::from_secs(3) {
            *s = AudioStatus::Silent;
        }
    }

    pub fn stop(&mut self) {
        self.running.store(false, Ordering::Relaxed);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl Drop for AudioTap {
    fn drop(&mut self) {
        self.stop();
    }
}

// ---------------------------------------------------------------------
//  PCM 格式：SCK 一般给 float32，但保险起见做一次探测
// ---------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Debug)]
enum PcmKind {
    F32,
    I16,
}

/// 猜一下是 f32 还是 i16。
///
/// 依据：f32 音频样本幅值在 ±1 附近，字节的指数位落在很窄的范围；
/// i16 样本则能到 ±32767。静音时两者都是全 0（无法区分，但那时解析成什么都是 0，
/// 不影响结果），所以默认给 F32。
fn guess_kind(bytes: &[u8]) -> PcmKind {
    let n_f32 = (bytes.len() / 4).min(64);
    for i in 0..n_f32 {
        let o = i * 4;
        let v = f32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]]);
        // 落在合理音频幅度内且不是 0 → 基本可以认定是 f32
        if v.is_finite() && v.abs() > 1e-7 && v.abs() < 8.0 {
            return PcmKind::F32;
        }
    }
    let n_i16 = (bytes.len() / 2).min(128);
    for i in 0..n_i16 {
        let o = i * 2;
        let v = i16::from_le_bytes([bytes[o], bytes[o + 1]]);
        if v != 0 {
            return PcmKind::I16;
        }
    }
    PcmKind::F32
}

/// 把一段原始 PCM 字节按指定格式解成 f32（归一化到 ±1）。
fn decode_pcm(bytes: &[u8], kind: PcmKind, out: &mut Vec<f32>) {
    out.clear();
    match kind {
        PcmKind::F32 => {
            let n = bytes.len() / 4;
            out.reserve(n);
            for i in 0..n {
                let o = i * 4;
                out.push(f32::from_le_bytes([bytes[o], bytes[o + 1], bytes[o + 2], bytes[o + 3]]));
            }
        }
        PcmKind::I16 => {
            let n = bytes.len() / 2;
            out.reserve(n);
            for i in 0..n {
                let o = i * 2;
                let v = i16::from_le_bytes([bytes[o], bytes[o + 1]]);
                out.push(v as f32 / 32768.0);
            }
        }
    }
}

// ---------------------------------------------------------------------
//  采集线程
// ---------------------------------------------------------------------

#[cfg(target_os = "macos")]
fn run_capture(
    ring: AudioRing,
    running: Arc<AtomicBool>,
    status: Arc<Mutex<AudioStatus>>,
    pcm: Arc<Mutex<Option<PcmKind>>>,
) {
    use screencapturekit::stream::configuration::audio::{AudioChannelCount, AudioSampleRate};

    macro_rules! fail {
        ($msg:expr) => {{
            if let Ok(mut s) = status.lock() {
                *s = AudioStatus::Failed($msg);
            }
            return;
        }};
    }

    // 1) 枚举可捕获内容。没授权时这一步就会失败。
    let content = match SCShareableContent::get() {
        Ok(c) => c,
        Err(e) => fail!(format!("无法枚举屏幕内容（多半未授权「屏幕与系统音频录制」）：{e}")),
    };
    let displays = content.displays();
    let Some(display) = displays.first() else {
        fail!("没有找到可捕获的显示器".into());
    };

    // 2) 过滤器：音频挂在被捕获内容上，这里挂主显示器
    let filter = SCContentFilter::create()
        .with_display(display)
        .with_excluding_windows(&[])
        .build();

    // 3) 只配音频。注意不设 width/height —— 我们不要画面。
    let config = SCStreamConfiguration::new()
        .with_captures_audio(true)
        .with_sample_rate(AudioSampleRate::Rate48000)
        .with_channel_count(AudioChannelCount::Stereo)
        // 排除本进程自己的声音，避免反馈
        .with_excludes_current_process_audio(true);

    let mut stream = SCStream::new(&filter, &config);

    // 4) 只注册 Audio 回调，不注册 Screen → 不会收到视频帧
    let handler = AudioHandler {
        ring: ring.clone(),
        kind: pcm,
    };
    stream.add_output_handler(handler, SCStreamOutputType::Audio);

    if let Err(e) = stream.start_capture() {
        fail!(format!("启动采集失败：{e}"));
    }
    if let Ok(mut s) = status.lock() {
        *s = AudioStatus::Idle; // 已启动，等 refresh_status() 判 Silent/Running
    }

    // 5) stream 必须活到循环结束，否则回调立刻停
    while running.load(Ordering::Relaxed) {
        thread::sleep(Duration::from_millis(100));
    }
    let _ = stream.stop_capture();
}

#[cfg(not(target_os = "macos"))]
fn run_capture(
    _ring: AudioRing,
    _running: Arc<AtomicBool>,
    status: Arc<Mutex<AudioStatus>>,
    _pcm: Arc<Mutex<Option<PcmKind>>>,
) {
    if let Ok(mut s) = status.lock() {
        *s = AudioStatus::Failed("系统音频采集只支持 macOS".into());
    }
}

// ---------------------------------------------------------------------
//  回调：拿 PCM → 混单声道 → 写环形缓冲
// ---------------------------------------------------------------------

#[cfg(target_os = "macos")]
struct AudioHandler {
    ring: AudioRing,
    /// 首次回调时探测出的格式，之后沿用
    kind: Arc<Mutex<Option<PcmKind>>>,
}

#[cfg(target_os = "macos")]
impl SCStreamOutputTrait for AudioHandler {
    fn did_output_sample_buffer(&self, sample_buffer: CMSampleBuffer, of_type: SCStreamOutputType) {
        if !matches!(of_type, SCStreamOutputType::Audio) {
            return;
        }
        let Some(list) = sample_buffer.audio_buffer_list() else {
            return;
        };

        // 解析出的声道数据（临时缓冲，避免每次分配）
        let mut ch: Vec<f32> = Vec::new();
        let mut mono: Vec<f32> = Vec::new();

        let n = list.num_buffers();
        if n == 0 {
            return;
        }

        // 确定 PCM 格式（只探测一次）
        let mut kind_slot = match self.kind.lock() {
            Ok(s) => s,
            Err(_) => return,
        };
        let kind = match *kind_slot {
            Some(k) => k,
            None => {
                let k = list
                    .get(0)
                    .map(|b| guess_kind(b.data()))
                    .unwrap_or(PcmKind::F32);
                *kind_slot = Some(k);
                k
            }
        };
        drop(kind_slot);

        // 双声道：non-interleaved（每 buffer 一声道）时取平均
        if n >= 2 {
            decode_pcm(list.get(0).unwrap().data(), kind, &mut ch);
            let frames = ch.len();
            decode_pcm(list.get(1).unwrap().data(), kind, &mut mono);
            let other = mono.clone();
            mono.clear();
            mono.reserve(frames);
            for i in 0..frames {
                let l = ch[i];
                let r = other.get(i).copied().unwrap_or(l);
                mono.push((l + r) * 0.5);
            }
        } else {
            // 单 buffer：可能已是单声道，也可能是交错立体声
            let b = list.get(0).unwrap();
            decode_pcm(b.data(), kind, &mut ch);
            if b.number_channels() >= 2 {
                mono.reserve(ch.len() / 2);
                for pair in ch.chunks_exact(2) {
                    mono.push((pair[0] + pair[1]) * 0.5);
                }
            } else {
                mono.extend_from_slice(&ch);
            }
        }

        self.ring.push(&mono);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ring_keeps_latest_and_drops_old() {
        let r = AudioRing::new(8);
        r.push(&[1.0, 2.0, 3.0]);
        r.push(&[4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0]); // 超限，最旧的被丢
        assert_eq!(r.received(), 10);

        let mut out = Vec::new();
        let n = r.latest(4, &mut out);
        assert_eq!(n, 4);
        assert_eq!(out, vec![7.0, 8.0, 9.0, 10.0]);
    }

    #[test]
    fn ring_pads_with_zero_when_short() {
        let r = AudioRing::new(16);
        r.push(&[1.0, 2.0]);
        let mut out = Vec::new();
        let n = r.latest(5, &mut out);
        assert_eq!(n, 2);
        assert_eq!(out, vec![0.0, 0.0, 0.0, 1.0, 2.0]);
    }

    #[test]
    fn decodes_f32_and_i16() {
        let mut v = Vec::new();
        let bytes = 0.5f32.to_le_bytes();
        decode_pcm(&bytes, PcmKind::F32, &mut v);
        assert!((v[0] - 0.5).abs() < 1e-6);

        decode_pcm(&16384i16.to_le_bytes(), PcmKind::I16, &mut v);
        assert!((v[0] - 0.5).abs() < 1e-3);
    }

    #[test]
    fn guesses_f32_from_bytes() {
        assert_eq!(guess_kind(&0.25f32.to_le_bytes()), PcmKind::F32);
        assert_eq!(guess_kind(&(-12345i16).to_le_bytes()), PcmKind::I16);
    }
}
