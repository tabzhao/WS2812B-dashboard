//! 仪表盘 UDP 协议发送端
//!
//! 包格式见 docs/07-host-integration.md：8 字节头 + 载荷，全小端。
//!   type=1 表头 18 B / type=2 像素帧 776 B / type=3 授时 12 B
//! seq 每类各自递增（这样固件的丢包统计才有意义）。

use std::net::{SocketAddr, UdpSocket};

pub const MAGIC0: u8 = b'M';
pub const MAGIC1: u8 = b'P';
pub const VER: u8 = 1;
pub const TYPE_METERS: u8 = 1;
pub const TYPE_PIXELS: u8 = 2;
pub const TYPE_CLOCK: u8 = 3;

pub const HEADER: usize = 8;
pub const METER_PAYLOAD: usize = 10; // 5 × u16
pub const PIXEL_PAYLOAD: usize = 768; // 256 × 3
pub const CLOCK_PAYLOAD: usize = 4;

pub const METER_PACKET: usize = HEADER + METER_PAYLOAD; // 18
pub const PIXEL_PACKET: usize = HEADER + PIXEL_PAYLOAD; // 776
pub const CLOCK_PACKET: usize = HEADER + CLOCK_PAYLOAD; // 12

/// 固件侧亮度硬上限：写 >128 会被压到 128，上位机最好自己也别超。
pub const MAX_BRIGHTNESS: u8 = 128;

/// 软件功耗封顶阈值：Σ(R+G+B)（已乘亮度）超过此值固件会整帧等比压暗。
/// 12755 ≈ 1000 mA。上位机先自己压，比被固件压要可控。
pub const MAX_CHANNEL_SUM: u32 = 12755;

pub struct Sender {
    sock: UdpSocket,
    dest: SocketAddr,
    seq_pix: u16,
    seq_mtr: u16,
}

impl Sender {
    pub fn new(dest: SocketAddr) -> std::io::Result<Self> {
        let sock = UdpSocket::bind("0.0.0.0:0")?;
        // 局域网单播，无需特殊 buffer 设置
        Ok(Self { sock, dest, seq_pix: 0, seq_mtr: 0 })
    }

    #[allow(dead_code)]
    pub fn dest(&self) -> SocketAddr { self.dest }

    /// 发送像素帧。rgb 必须正好 768 字节（行优先，(0,0) 左上）。
    /// 内部做软件功耗封顶：超过 MAX_CHANNEL_SUM 则整帧等比压暗。
    pub fn send_pixels(&mut self, rgb: &[u8], brightness: u8) {
        assert_eq!(rgb.len(), PIXEL_PAYLOAD);
        let brt = brightness.min(MAX_BRIGHTNESS);

        // 1) 乘亮度 + 求和
        let mut scaled = [0u8; PIXEL_PAYLOAD];
        let mut sum: u32 = 0;
        for i in 0..PIXEL_PAYLOAD {
            let v = ((rgb[i] as u32) * (brt as u32)) >> 8; // /256 近似 /255
            scaled[i] = v as u8;
            sum += v;
        }
        // 2) 功耗封顶：等比缩放，保持画面整体比例
        if sum > MAX_CHANNEL_SUM {
            let scale = (MAX_CHANNEL_SUM * 256) / sum; // 8.8 定点
            for i in 0..PIXEL_PAYLOAD {
                scaled[i] = ((scaled[i] as u32) * scale >> 8) as u8;
            }
        }

        // 3) 打包发送
        let mut buf = [0u8; PIXEL_PACKET];
        buf[0] = MAGIC0;
        buf[1] = MAGIC1;
        buf[2] = VER;
        buf[3] = TYPE_PIXELS;
        buf[4] = (self.seq_pix & 0xFF) as u8;
        buf[5] = ((self.seq_pix >> 8) & 0xFF) as u8;
        buf[6] = brt;
        buf[7] = 0;
        buf[8..].copy_from_slice(&scaled);
        let _ = self.sock.send_to(&buf, self.dest);
        self.seq_pix = self.seq_pix.wrapping_add(1);
    }

    /// 发送 5 路表头值，0..65535 = 零位..满偏。
    pub fn send_meters(&mut self, v: &[u16; 5]) {
        let mut buf = [0u8; METER_PACKET];
        buf[0] = MAGIC0;
        buf[1] = MAGIC1;
        buf[2] = VER;
        buf[3] = TYPE_METERS;
        buf[4] = (self.seq_mtr & 0xFF) as u8;
        buf[5] = ((self.seq_mtr >> 8) & 0xFF) as u8;
        buf[6] = 0;
        buf[7] = 0;
        for i in 0..5 {
            buf[8 + i * 2] = (v[i] & 0xFF) as u8;
            buf[8 + i * 2 + 1] = ((v[i] >> 8) & 0xFF) as u8;
        }
        let _ = self.sock.send_to(&buf, self.dest);
        self.seq_mtr = self.seq_mtr.wrapping_add(1);
    }

    /// 发送授时（UTC 秒）。固件只在偏差 >5s 时才采纳。
    pub fn send_time(&mut self, unix_utc: u32) {
        let mut buf = [0u8; CLOCK_PACKET];
        buf[0] = MAGIC0;
        buf[1] = MAGIC1;
        buf[2] = VER;
        buf[3] = TYPE_CLOCK;
        buf[4] = 0;
        buf[5] = 0;
        buf[6] = 0;
        buf[7] = 0;
        buf[8] = (unix_utc & 0xFF) as u8;
        buf[9] = ((unix_utc >> 8) & 0xFF) as u8;
        buf[10] = ((unix_utc >> 16) & 0xFF) as u8;
        buf[11] = ((unix_utc >> 24) & 0xFF) as u8;
        let _ = self.sock.send_to(&buf, self.dest);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packet_sizes() {
        assert_eq!(METER_PACKET, 18);
        assert_eq!(PIXEL_PACKET, 776);
        assert_eq!(CLOCK_PACKET, 12);
    }
}
