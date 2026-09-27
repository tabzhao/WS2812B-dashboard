//! 频谱分析：加窗 FFT → 混合分频 → dB 归一化 → 倾斜补偿 → 时间平滑
//!
//! ## 为什么不是纯对数分频
//!
//! 音乐能量集中在低频，线性均分会让右边一片死寂；但**纯对数分频在低频会崩**：
//! 每段宽度 = f0 × (r−1)，最低段只有 40×(1.206−1) ≈ 7.7 Hz，而 48 kHz/1024 的
//! bin 宽是 46.9 Hz —— 最左边 4 段被 `round()` 压进同一个 bin，读数**一模一样**，
//! 看上去就是「左边四根柱子焊在一起」。这是 FFT 分辨率决定的硬限制，提高平滑
//! 参数救不了，只能：① 加大 FFT 点数把 bin 变窄；② 低频改用线性分频。
//!
//! 所以这里用**低频线性 + 高频对数**的混合分频：
//! - 40~400 Hz：线性分 12 段，每段 30 Hz。段宽 > bin 宽（23.4 Hz @FFT2048），
//!   保证每段至少落在不同 bin 上，底鼓/贝斯的层次能分开。
//! - 400 Hz~8 kHz：对数分 20 段，每段宽度随频率等比增宽，符合听觉。
//! - 8 kHz 以上整体砍掉：音乐在这个区间的能量极低，给列也是常年黑着。

use std::f32::consts::PI;

use rustfft::num_complex::Complex;
use rustfft::FftPlanner;

/// FFT 点数。2048 点 @48 kHz → 频率分辨率 23.4 Hz、时间窗 42.7 ms。
///
/// 1024 点的 bin 宽 46.9 Hz 太粗，撑不起低频 30 Hz/段的线性分频（会导致相邻段
/// 落进同一个 bin，读数重复）。2048 是「低频能分开」与「时延可接受」的平衡点：
/// 42.7 ms 只比人眼两三帧多一点，配合 30 ms 上升时间常数察觉不出额外延迟。
pub const FFT_SIZE: usize = 2048;

/// 灯阵列数，也就是频段数。
pub const BANDS: usize = 32;

/// 表头频段数（5 路动圈表头）。
pub const METER_BANDS: usize = 5;

/// 分频下限/上限。低于 40 Hz 的 bin 基本是直流与呼吸噪声，
/// 高于 8 kHz 的音乐能量太低，给列也常年不亮。
const F_LO: f32 = 40.0;
const F_HI: f32 = 8_000.0;

/// 线性段的上边界，以及线性段占多少列。低频用线性、高频用对数的分界点。
///
/// 约束：线性段宽 (F_LIN_HI − F_LO) / LINEAR_BANDS 必须 **大于** bin 宽，
/// 否则又退化成「相邻列共用一个 bin」的死结。400/12 = 30 Hz > 23.4 Hz ✅
const F_LIN_HI: f32 = 400.0;
const LINEAR_BANDS: usize = 12;

/// 高频倾斜补偿（dB / 倍频程）。**当前为 0 = 关闭补偿**（2026-09-26 定）。
///
/// 背景：音乐频谱天然以约 −3 dB/oct 衰减，不补偿的话右半边柱子容易趴在地板上。
/// 1.5 dB/oct 时 8 kHz 相对 200 Hz 约 +8 dB，镲片、齿音能亮起来。
/// 但实测下来补偿会把底噪一起抬上来，观感反而不实，所以关掉。
/// **想开回来就把它改成 1.5 左右**，别超过 3 dB/oct（底噪会变成常亮）；
/// 低于参考频率的段不做负增益（`max(0.0)`），免得把底鼓压矮。
const TILT_DB_PER_OCTAVE: f32 = 0.0;
const TILT_REF_HZ: f32 = 200.0;

/// dB 显示范围。低于下限归零，高于上限顶格。
const DB_MIN: f32 = -70.0;
const DB_MAX: f32 = -8.0;

/// 上升/下降时间常数（秒）。上升快、下降慢，柱子才有「弹跳感」而不是抽搐。
const TAU_ATTACK: f32 = 0.030;
const TAU_RELEASE: f32 = 0.220;

/// 峰值白点的行为三件套。
///
/// 观感目标：柱子在往上跳的时候，白点**始终贴在柱顶**（所以上升阶段一定看得见白点）；
/// 柱子开始往下掉之后，白点**先原地停一会儿**再掉，而且**掉得比柱子慢**，
/// 一路落到最底一行就停住，不再继续消失。
const PEAK_HOLD_SEC: f32 = 0.8;    // 柱子开掉后白点先停多久（「比频谱晚一点点」）。
                                   // 柱子自己约 460 ms 就到底了，所以 0.8 s 意味着
                                   // **柱子掉完之后白点还会在空中多悬约 0.34 s** 才开始落。
const PEAK_FALL_PER_SEC: f32 = 1.6; // 白点下落速度。单位是**满量程/秒**（不是格/秒）：
                                    // 1.0 = 每秒掉掉一整个 0..1（也就是 8 格），
                                    // 所以 1.6 ≈ 12.8 格/秒，从满格掉到底约 550 ms。
                                    // **必须慢于柱子**：柱子是 τ=220 ms 的指数衰减，
                                    // 从满格掉到底约 460 ms（平均 ≈1.9 满量程/秒）。
                                    // 1.6 离临界 1.9 已经很近，再往上调到 2.0 就会追上柱子。
                                    // 慢的代价是白点始终悬在柱子上方慢慢飘落（经典频谱仪观感）；
                                    // 反过来若快于 1.9，白点会追上柱子被顶住，
                                    // 变成「贴着柱子一起掉」，就看不出是峰值了。
/// 白点的落脚点：最底一行（1/8 格），掉到这里就停住，不归零。
/// 归零的话白点会闪一下再消失，看起来像丢帧。
const PEAK_MIN_LEVEL: f32 = 1.0 / 8.0;

pub struct Spectrum {
    planner: FftPlanner<f32>,
    window: Vec<f32>,
    scratch: Vec<Complex<f32>>,
    /// 每个频段对应的 bin 区间 [lo, hi)
    band_bins: Vec<(usize, usize)>,
    /// 每个频段的中心频率，用于倾斜补偿
    band_center_hz: Vec<f32>,
    /// 表头 5 路各自覆盖的频段区间
    meter_bands: [(usize, usize); METER_BANDS],
    levels: [f32; BANDS],
    peaks: [f32; BANDS],
    /// 白点的「停留计时」：柱子开始掉之后累积，超过 PEAK_HOLD_SEC 才允许下落
    peak_hold: [f32; BANDS],
}

impl Spectrum {
    pub fn new(sample_rate: f32) -> Self {
        let mut planner = FftPlanner::new();
        // 预生成计划，之后 process() 不再分配
        let _ = planner.plan_fft_forward(FFT_SIZE);

        // Hann 窗：不加窗的话频谱泄漏会让相邻频段互相污染
        let window: Vec<f32> = (0..FFT_SIZE)
            .map(|i| 0.5 * (1.0 - (2.0 * PI * i as f32 / FFT_SIZE as f32).cos()))
            .collect();

        let bin_hz = sample_rate / FFT_SIZE as f32;
        let max_bin = FFT_SIZE / 2;

        // 分频边界（Hz）：低频线性、高频对数
        let mut edges_hz = Vec::with_capacity(BANDS + 1);
        for k in 0..=BANDS {
            edges_hz.push(Self::band_edge_hz(k));
        }

        let mut band_bins = Vec::with_capacity(BANDS);
        let mut band_center_hz = Vec::with_capacity(BANDS);
        for k in 0..BANDS {
            let f0 = edges_hz[k];
            let f1 = edges_hz[k + 1];
            let b0 = ((f0 / bin_hz).round() as usize).max(1).min(max_bin - 1);
            // 至少给一个 bin，否则低频段会全是空区间
            let b1 = ((f1 / bin_hz).round() as usize).max(b0 + 1).min(max_bin);
            band_bins.push((b0, b1));
            band_center_hz.push((f0 * f1).sqrt()); // 几何中心，对数刻度上才是中点
        }

        // 5 路表头：低 / 中低 / 中 / 中高 / 高。
        // 边界按新分频取，前 4 路落在 40~400 Hz 线性段内，第 5 路覆盖整个高频段。
        let edges = [0usize, 4, 9, 16, 24, BANDS];
        let mut meter_bands = [(0usize, 0usize); METER_BANDS];
        for i in 0..METER_BANDS {
            meter_bands[i] = (edges[i], edges[i + 1]);
        }

        Self {
            planner,
            window,
            scratch: vec![Complex::new(0.0, 0.0); FFT_SIZE],
            band_bins,
            band_center_hz,
            meter_bands,
            levels: [0.0; BANDS],
            peaks: [0.0; BANDS],
            peak_hold: [0.0; BANDS],
        }
    }

    /// 第 k 个分频边界的频率（Hz），共 BANDS+1 个。
    /// 低频段线性等分，高频段对数等分，在 F_LIN_HI 处接缝。
    fn band_edge_hz(k: usize) -> f32 {
        if k <= LINEAR_BANDS {
            F_LO + (F_LIN_HI - F_LO) * (k as f32 / LINEAR_BANDS as f32)
        } else {
            let n_log = (BANDS - LINEAR_BANDS) as f32;
            F_LIN_HI * (F_HI / F_LIN_HI).powf((k - LINEAR_BANDS) as f32 / n_log)
        }
    }

    /// 喂入最新的单声道样本（长度不足会补零），按 dt 推进平滑与峰值衰减。
    /// 返回的 levels/peaks 都是 0..1。
    pub fn update(&mut self, samples: &[f32], dt: f32) {
        // 1) 加窗填实部
        for i in 0..FFT_SIZE {
            let s = if i < samples.len() { samples[i] } else { 0.0 };
            self.scratch[i] = Complex::new(s * self.window[i], 0.0);
        }

        // 2) FFT
        let fft = self.planner.plan_fft_forward(FFT_SIZE);
        fft.process(&mut self.scratch);

        // 3) 分频求幅度。每段取 bin 幅度的最大值 —— 用均值会让鼓点被抹平。
        let half = FFT_SIZE as f32 * 0.5;
        let mut raw = [0f32; BANDS];
        for (k, &(b0, b1)) in self.band_bins.iter().enumerate() {
            let mut mag = 0f32;
            for b in b0..b1 {
                let c = self.scratch[b];
                let m = (c.re * c.re + c.im * c.im).sqrt() / half;
                if m > mag {
                    mag = m;
                }
            }
            // 幅度 → dB → 倾斜补偿 → 归一化
            let db = 20.0 * (mag + 1e-9).log10();
            let db = db + self.tilt_gain_db(k);
            let v = ((db - DB_MIN) / (DB_MAX - DB_MIN)).clamp(0.0, 1.0);
            raw[k] = v;
        }

        // 4) 时间平滑：上升快、下降慢
        let dt = dt.clamp(0.001, 0.1);
        for k in 0..BANDS {
            let target = raw[k];
            let tau = if target > self.levels[k] { TAU_ATTACK } else { TAU_RELEASE };
            let alpha = 1.0 - (-dt / tau).exp();
            self.levels[k] += (target - self.levels[k]) * alpha;

            // 5) 峰值白点：上升立刻贴住柱顶；柱子掉下去之后先停 PEAK_HOLD_SEC，
            //    再按固定速度往下掉（比柱子快），掉到最底一行就停住。
            if self.levels[k] >= self.peaks[k] {
                // 上升（或被柱子顶住）：白点跟着柱顶走，停留计时清零
                self.peaks[k] = self.levels[k];
                self.peak_hold[k] = 0.0;
            } else {
                self.peak_hold[k] += dt;
                if self.peak_hold[k] >= PEAK_HOLD_SEC {
                    self.peaks[k] -= PEAK_FALL_PER_SEC * dt;
                }
            }
            if self.peaks[k] < PEAK_MIN_LEVEL {
                self.peaks[k] = PEAK_MIN_LEVEL;
            }
        }
    }

    /// 第 k 段的倾斜补偿增益（dB）：高频抬一点，低频不动。
    fn tilt_gain_db(&self, k: usize) -> f32 {
        let oct = (self.band_center_hz[k] / TILT_REF_HZ).log2();
        (TILT_DB_PER_OCTAVE * oct).max(0.0)
    }

    pub fn levels(&self) -> &[f32; BANDS] {
        &self.levels
    }

    pub fn peaks(&self) -> &[f32; BANDS] {
        &self.peaks
    }

    /// 5 路表头值：各频段的平均能量，0..1。
    pub fn meter_levels(&self) -> [f32; METER_BANDS] {
        let mut out = [0f32; METER_BANDS];
        for (i, &(lo, hi)) in self.meter_bands.iter().enumerate() {
            let mut sum = 0f32;
            let mut n = 0;
            for k in lo..hi {
                sum += self.levels[k];
                n += 1;
            }
            out[i] = if n > 0 { sum / n as f32 } else { 0.0 };
        }
        out
    }

    /// 分频边界（bin 索引），调试用。
    #[allow(dead_code)]
    pub fn band_bins(&self) -> &[(usize, usize)] {
        &self.band_bins
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 造一段指定频率的正弦波
    fn tone(freq: f32, sample_rate: f32, n: usize) -> Vec<f32> {
        (0..n)
            .map(|i| (2.0 * PI * freq * i as f32 / sample_rate).sin() * 0.8)
            .collect()
    }

    #[test]
    fn low_tone_lands_in_left_bands() {
        let sr = 48_000.0;
        let mut sp = Spectrum::new(sr);
        let s = tone(100.0, sr, FFT_SIZE);
        for _ in 0..60 {
            sp.update(&s, 1.0 / 60.0);
        }
        let lv = sp.levels();
        // 100 Hz 落在线性段的第 2 段（70~100 Hz 边界附近）
        let left: f32 = lv[0..4].iter().sum();
        let right: f32 = lv[24..32].iter().sum();
        assert!(left > right, "低频能量应在左侧，实际 left={left} right={right}");
        assert!(lv[0] > 0.05, "最低频段应有明显能量，实际 {}", lv[0]);
    }

    #[test]
    fn high_tone_lands_in_right_bands() {
        let sr = 48_000.0;
        let mut sp = Spectrum::new(sr);
        // 用 6 kHz：8 kHz 以上已被裁掉，拿 10 kHz 测会全部落空
        let s = tone(6_000.0, sr, FFT_SIZE);
        for _ in 0..60 {
            sp.update(&s, 1.0 / 60.0);
        }
        let lv = sp.levels();
        let left: f32 = lv[0..4].iter().sum();
        let right: f32 = lv[24..32].iter().sum();
        assert!(right > left, "高频能量应在右侧，实际 left={left} right={right}");
    }

    /// 回归测试：最左边几段**不能共用同一个 bin**。
    /// 曾经的纯对数分频下，列 0~3 的 bin 区间全是 (1,2)，四根柱子读数一模一样。
    #[test]
    fn low_bands_do_not_share_bins() {
        for sr in [44_100.0, 48_000.0] {
            let sp = Spectrum::new(sr);
            let b = sp.band_bins();
            for i in 1..LINEAR_BANDS {
                let same = b[i] == b[i - 1];
                assert!(
                    !same,
                    "{sr} Hz 下第 {i} 段与第 {} 段共用 bin {:?}，低频会糊成一片",
                    i - 1,
                    b[i]
                );
            }
        }
    }

    /// 回归测试：白点「上升贴顶 → 停一下 → 掉得**比柱子慢** → 停在最底一行」。
    #[test]
    fn peak_holds_then_falls_slower_than_level() {
        let sr = 48_000.0;
        let mut sp = Spectrum::new(sr);
        let s = tone(800.0, sr, FFT_SIZE);
        for _ in 0..120 {
            sp.update(&s, 1.0 / 60.0);
        }
        let k = sp
            .levels()
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.partial_cmp(b.1).unwrap())
            .unwrap()
            .0;
        let top = sp.peaks()[k];
        assert!(top > 0.8, "稳定音应把白点顶到接近满格，实际 {top}");

        let z = vec![0f32; FFT_SIZE];
        // ① 静音后 0.2 s（< PEAK_HOLD_SEC = 0.25）：白点应原地不动
        for _ in 0..12 {
            sp.update(&z, 1.0 / 60.0);
        }
        let held = sp.peaks()[k];
        assert!(held > top - 0.02, "hold 期间白点不该掉：top={top} held={held}");

        // ② 比「谁先落到底」：白点必须比柱子**慢**（柱子 ≈460 ms，白点 ≈730 ms）。
        //    注意不能拿某一段窗口内的位移来比 —— 柱子是指数衰减，前快后慢，
        //    而白点是匀速，窗口取在不同位置会得出相反结论。比总耗时才稳。
        let mut frames_level = 0usize;
        let mut frames_peak = 0usize;
        for i in 1..=600 {
            sp.update(&z, 1.0 / 60.0);
            if frames_level == 0 && sp.levels()[k] < 1.0 / 8.0 {
                frames_level = i;
            }
            if frames_peak == 0 && sp.peaks()[k] <= 1.0 / 8.0 + 1e-6 {
                frames_peak = i;
            }
            if frames_level != 0 && frames_peak != 0 {
                break;
            }
        }
        assert!(frames_level > 0, "柱子应该会落到底");
        assert!(
            frames_peak > frames_level,
            "白点要掉得比柱子慢：柱子 {frames_level} 帧到底，白点 {frames_peak} 帧"
        );

        // ③ 长时间静音：白点停在最底一行（1/8），不归零
        for _ in 0..120 {
            sp.update(&z, 1.0 / 60.0);
        }
        let p = sp.peaks()[k];
        assert!((p - 1.0 / 8.0).abs() < 1e-6, "白点应停在最底一行，实际 {p}");
        assert!(sp.levels()[k] < 0.01, "柱子本身应该已经归零");
    }

    /// 用「仿音乐」信号跑一遍并打印 ASCII 柱状图，肉眼核对观感：
    /// `cargo test preview_music_like_signal -- --nocapture`
    ///
    /// 信号按真实音乐的「粉噪式」能量比例给：8 个谐波分量自 60 Hz 排到 6 kHz，
    /// 幅度随频率衰减（60 Hz 最厚、6 kHz 最薄 −18 dB），另加一点底噪。
    /// 判据：左边不该出现连续等高的方块，右边 6 kHz 附近应该亮得起来。
    #[test]
    fn preview_music_like_signal() {
        let sr = 48_000.0;
        let mut sp = Spectrum::new(sr);
        let mut s = Vec::with_capacity(FFT_SIZE);
        let mut rng = 12_345u32;
        for i in 0..FFT_SIZE {
            let t = i as f32 / sr;
            let parts = [
                (60.0, 0.80),
                (150.0, 0.50),
                (300.0, 0.40),
                (500.0, 0.35),
                (800.0, 0.25),
                (1_500.0, 0.18),
                (3_000.0, 0.12),
                (6_000.0, 0.10),
            ];
            let mut v = 0f32;
            for (f, a) in parts {
                v += a * (2.0 * PI * f * t).sin();
            }
            // 简易 LCG 底噪，模拟真实录音的信噪比
            rng = rng.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            v += 0.02 * ((rng >> 16) as f32 / 32_768.0 - 1.0);
            s.push(v);
        }
        for _ in 0..90 {
            sp.update(&s, 1.0 / 60.0);
        }
        let lv = sp.levels();
        println!("\n  仿音乐信号频谱（8 行 = 8 像素高，左=40Hz 右=8kHz）");
        for row in (0..8).rev() {
            let line: String = (0..BANDS)
                .map(|k| if (lv[k] * 8.0).round() as i32 > row { '█' } else { '·' })
                .collect();
            println!("  |{line}|");
        }
        println!("   {}", (0..BANDS).map(|k| if k % 8 == 0 { '^' } else { ' ' }).collect::<String>());
        println!("   0(40Hz)      8(280Hz)      16(730Hz)     24(2.4kHz)    31(8kHz)");
    }

    /// 打印「静音之后柱子和白点各自掉多快」的时序轨迹：
    /// `cargo test dump_peak_trace -- --nocapture`
    ///
    /// 预期：前 250 ms 白点停住不动（柱子已经在掉），之后白点开始掉但**掉得比柱子慢**，
    /// 一路悬在柱子上方，最后停在第 1 格（最底一行）不再往下。
    #[test]
    fn dump_peak_trace() {
        let sr = 48_000.0;
        let mut sp = Spectrum::new(sr);
        let s = tone(800.0, sr, FFT_SIZE);
        for _ in 0..120 {
            sp.update(&s, 1.0 / 60.0);
        }
        let k = sp
            .levels()
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.partial_cmp(b.1).unwrap())
            .unwrap()
            .0;
        let z = vec![0f32; FFT_SIZE];
        println!("\n  静音后柱高 / 白点高度（格，满格 8）");
        println!("   t(ms)   柱高   白点");
        let mut t = 0.0f32;
        for step in 0..=66 {
            if step % 6 == 0 {
                println!(
                    "  {:6.0}   {:>4}   {:>4}",
                    t * 1000.0,
                    (sp.levels()[k] * 8.0).round() as i32,
                    (sp.peaks()[k] * 8.0).round() as i32
                );
            }
            sp.update(&z, 1.0 / 60.0);
            t += 1.0 / 60.0;
        }
    }

    /// 打印分频映射表，人眼核对用：`cargo test dump_band_map -- --nocapture`
    #[test]
    fn dump_band_map() {
        let sr = 48_000.0;
        let sp = Spectrum::new(sr);
        let bin_hz = sr / FFT_SIZE as f32;
        println!("\n列  频率范围(Hz)      bin 区间   bin数  倾斜(dB)");
        for k in 0..BANDS {
            let f0 = Spectrum::band_edge_hz(k);
            let f1 = Spectrum::band_edge_hz(k + 1);
            let (b0, b1) = sp.band_bins()[k];
            println!(
                "{k:2}  {:7.1}~{:<8.1} {:3}..{:<5} {:4}   {:+.1}",
                f0,
                f1,
                b0,
                b1,
                b1 - b0,
                sp.tilt_gain_db(k)
            );
        }
        println!("bin 宽 = {bin_hz:.2} Hz, 时间窗 = {:.1} ms", FFT_SIZE as f32 / sr * 1000.0);
    }

    #[test]
    fn silence_is_flat_zero() {
        let mut sp = Spectrum::new(48_000.0);
        let s = vec![0f32; FFT_SIZE];
        for _ in 0..30 {
            sp.update(&s, 1.0 / 60.0);
        }
        assert!(sp.levels().iter().all(|&v| v < 0.01));
    }

    #[test]
    fn meter_levels_are_five_normalized() {
        let sr = 48_000.0;
        let mut sp = Spectrum::new(sr);
        let s = tone(1000.0, sr, FFT_SIZE);
        for _ in 0..60 {
            sp.update(&s, 1.0 / 60.0);
        }
        let m = sp.meter_levels();
        assert_eq!(m.len(), 5);
        assert!(m.iter().all(|&v| v >= 0.0 && v <= 1.0));
    }

    #[test]
    fn bands_cover_increasing_frequency() {
        let sp = Spectrum::new(48_000.0);
        let b = sp.band_bins();
        assert_eq!(b.len(), BANDS);
        for i in 1..BANDS {
            assert!(b[i].0 >= b[i - 1].0, "频段 bin 必须单调不减");
            assert!(b[i].1 > b[i].0, "每个频段至少要有一个 bin");
        }
    }
}
