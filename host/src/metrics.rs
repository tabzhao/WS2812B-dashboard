//! 系统监控采集：6 个指标，归一化到 0..65535
//!
//! - CPU 使用率、内存使用率：直接来自 sysinfo。
//! - 网络发送/接收速率：两次刷新间 bytes 差 / dt，按可配量程归一化。
//! - 硬盘读/写速率：累加所有进程 sysinfo 的 disk_usage（since last refresh），
//!   再按量程归一化。macOS 上没有现成的"系统级 IO 速率"接口，进程级累加够用。

use std::time::{Duration, Instant};
use sysinfo::{Disks, Networks, ProcessesToUpdate, System};

/// 6 个可选指标。
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Metric {
    Cpu,
    Mem,
    NetTx,
    NetRx,
    DiskRead,
    DiskWrite,
}

impl Metric {
    pub const ALL: [Metric; 6] = [
        Metric::Cpu, Metric::Mem, Metric::NetTx, Metric::NetRx,
        Metric::DiskRead, Metric::DiskWrite,
    ];
    pub fn label(self) -> &'static str {
        match self {
            Metric::Cpu => "整体CPU使用率",
            Metric::Mem => "内存使用率",
            Metric::NetTx => "网络发送速率",
            Metric::NetRx => "网络接收速率",
            Metric::DiskRead => "硬盘读取速率",
            Metric::DiskWrite => "硬盘写入速率",
        }
    }
}

/// 可配量程（B/s）。超量程就满偏，不超量程线性。
#[derive(Clone, Copy)]
pub struct Ranges {
    pub net_bps: u64,    // 默认 10 MB/s 满偏
    pub disk_bps: u64,   // 默认 50 MB/s 满偏
}

impl Default for Ranges {
    fn default() -> Self {
        Self { net_bps: 10_000_000, disk_bps: 50_000_000 }
    }
}

pub struct Collector {
    sys: System,
    nets: Networks,
    disks: Disks,
    net_prev_tx: u64,
    net_prev_rx: u64,
    disk_prev_read: u64,
    disk_prev_write: u64,
    last: Instant,
    ready: bool, // CPU 首次刷新返回 0，需两次间隔才有值
    ranges: Ranges,
}

impl Collector {
    pub fn new(ranges: Ranges) -> Self {
        let mut s = Self {
            sys: System::new(),
            nets: Networks::new(),
            disks: Disks::new(),
            net_prev_tx: 0,
            net_prev_rx: 0,
            disk_prev_read: 0,
            disk_prev_write: 0,
            last: Instant::now(),
            ready: false,
            ranges,
        };
        s.refresh_once();
        // 第二次刷新建立基线（CPU 才有真实值）
        s.sys.refresh_cpu_usage();
        s
    }

    fn refresh_once(&mut self) {
        self.sys.refresh_processes(ProcessesToUpdate::All, true);
        self.sys.refresh_cpu_usage();
        self.sys.refresh_memory();
        self.nets.refresh();
        self.disks.refresh();
    }

    /// 采集一次，返回 6 个归一化值（0..65535）。
    /// 建议 100ms ~ 1s 调一次（表头协议 10Hz，1s 太慢，用 200ms）。
    pub fn sample(&mut self) -> [u16; 6] {
        self.refresh_once();
        self.sys.refresh_cpu_usage();

        let now = Instant::now();
        let dt = now.duration_since(self.last).as_secs_f64().max(0.001);
        self.last = now;

        // CPU：sysinfo global_cpu_usage 在 refresh_cpu_usage 后给瞬时值（0..100）
        let cpu_pct = self.sys.global_cpu_usage() as f64;
        // 内存：used/total（sysinfo 的 used = total - available）
        let mem_pct = if self.sys.total_memory() > 0 {
            self.sys.used_memory() as f64 / self.sys.total_memory() as f64 * 100.0
        } else { 0.0 };

        // 网络：累加所有接口
        let (tx, rx) = self.nets.iter().fold((0u64, 0u64), |(t, r), (_, n)| {
            (t + n.total_transmitted(), r + n.total_received())
        });
        let tx_rate = bytes_per_sec(tx, self.net_prev_tx, dt);
        let rx_rate = bytes_per_sec(rx, self.net_prev_rx, dt);
        self.net_prev_tx = tx;
        self.net_prev_rx = rx;

        // 硬盘：累加所有进程的 disk_usage（since last refresh 的增量）
        let (dr, dw) = self.sys.processes().values().fold((0u64, 0u64), |(r, w), p| {
            let d = p.disk_usage();
            (r + d.read_bytes, w + d.written_bytes)
        });
        let read_rate = bytes_per_sec(dr, self.disk_prev_read, dt);
        let write_rate = bytes_per_sec(dw, self.disk_prev_write, dt);
        self.disk_prev_read = dr;
        self.disk_prev_write = dw;

        self.ready = true;

        let norm = |pct: f64| -> u16 {
            if pct >= 100.0 { 65535 } else if pct <= 0.0 { 0 } else { (pct * 655.35) as u16 }
        };
        let norm_rate = |rate_bps: f64, full: u64| -> u16 {
            let f = full as f64;
            if rate_bps >= f { 65535 } else { (rate_bps / f * 65535.0) as u16 }
        };

        let cpu_val = if self.ready { norm(cpu_pct) } else { 0 };
        [
            cpu_val,
            norm(mem_pct),
            norm_rate(tx_rate, self.ranges.net_bps),
            norm_rate(rx_rate, self.ranges.net_bps),
            norm_rate(read_rate, self.ranges.disk_bps),
            norm_rate(write_rate, self.ranges.disk_bps),
        ]
    }
}

fn bytes_per_sec(cur: u64, prev: u64, dt: f64) -> f64 {
    if cur < prev { 0.0 } else { (cur - prev) as f64 / dt }
}

/// 给个简单的演示值生成器（在 sysinfo 不可用时退化用，这里不用，留作参考）。
#[allow(dead_code)]
pub fn demo_values(t: Duration, which: Metric) -> u16 {
    let s = t.as_secs_f64();
    let v = match which {
        Metric::Cpu | Metric::Mem => 0.5 + 0.4 * (s * 0.3).sin(),
        Metric::NetTx | Metric::DiskRead => 0.5 + 0.4 * (s * 0.7).sin(),
        Metric::NetRx | Metric::DiskWrite => 0.5 + 0.4 * (s * 0.5).sin(),
    };
    (v * 65535.0) as u16
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn sample_runs() {
        let mut c = Collector::new(Ranges::default());
        let _ = c.sample();
        // 不做值断言，只确保不 panic
    }
}
