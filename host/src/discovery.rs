//! 局域网仪表盘设备发现（mDNS / DNS-SD）
//!
//! 固件注册 `_dashboard._udp`，这里 browse 发现它们，上位机因此不必知道 IP。
//! 协议与字段含义见 docs/07-host-integration.md §0.1。
//!
//! 设计要点：
//! - **端口从 SRV 取，不写死**。固件将来改端口，上位机无需改代码。
//! - **不要缓存 IP**。DHCP 换个租就变了；这里每次 `ServiceResolved` 都刷新地址，
//!   设备下线靠 `ServiceRemoved` 剔除。
//! - `poll()` 必须被周期性调用。mdns-sd 用的是容量为 10 的有界 channel，
//!   不排空会堵住 daemon 线程，导致后续再也发现不到新设备。

use mdns_sd::{ServiceDaemon, ServiceEvent};
use std::collections::HashMap;
use std::net::SocketAddr;

/// 固件 advertise 的服务类型。带 `.local.` 后缀（mDNS 要求）。
pub const SERVICE_TYPE: &str = "_dashboard._udp.local.";

/// 一台被发现的仪表盘。
#[derive(Clone, Debug)]
pub struct Device {
    /// 服务全名，唯一键。形如 `dash-A292CE._dashboard._udp.local.`
    pub fullname: String,
    /// 实例名，形如 `dash-A292CE`（ChipId 后 6 位十六进制）
    pub name: String,
    /// mDNS 主机名，形如 `dash-A292CE.local.`，可直接 ping 来排查网络。
    /// UI 暂未展示，保留在结构里供命令行/日志使用。
    #[allow(dead_code)]
    pub host: String,
    /// 实际地址：IP 来自 A/AAAA 记录，**端口来自 SRV**
    pub addr: SocketAddr,
    pub ver: Option<String>,
    /// MAC 是区分设备的最终依据（名字可能撞），暂未在 UI 展示
    #[allow(dead_code)]
    pub mac: Option<String>,
    pub leds: Option<u32>,
    pub meters: Option<u32>,
    /// 逻辑宽度，配 leds 可反推行数；暂未在 UI 展示
    #[allow(dead_code)]
    pub cols: Option<u32>,
}

pub struct Discovery {
    // 必须一直持有：drop 掉会让 daemon 线程退出，发现随之停止
    #[allow(dead_code)]
    daemon: Option<ServiceDaemon>,
    rx: Option<mdns_sd::Receiver<ServiceEvent>>,
    devices: HashMap<String, Device>,
    err: Option<String>,
}

impl Discovery {
    pub fn new() -> Self {
        match ServiceDaemon::new() {
            Ok(daemon) => match daemon.browse(SERVICE_TYPE) {
                Ok(rx) => Self { daemon: Some(daemon), rx: Some(rx), devices: HashMap::new(), err: None },
                Err(e) => Self {
                    daemon: Some(daemon),
                    rx: None,
                    devices: HashMap::new(),
                    err: Some(format!("订阅 {} 失败：{}", SERVICE_TYPE, e)),
                },
            },
            Err(e) => Self {
                daemon: None,
                rx: None,
                devices: HashMap::new(),
                err: Some(format!("mDNS 守护进程启动失败：{}", e)),
            },
        }
    }

    /// 拉取待处理的发现事件。返回 true 表示设备表有变化，UI 应该刷新。
    pub fn poll(&mut self) -> bool {
        // 先把事件排干再统一处理：否则 rx 的不可变借用会一直活着，
        // 后面 self.handle() 的可变借用就跟它冲突了。
        let mut events = Vec::new();
        if let Some(rx) = self.rx.as_ref() {
            while let Ok(ev) = rx.try_recv() {
                events.push(ev);
            }
        }
        let mut changed = false;
        for ev in events {
            if self.handle(ev) {
                changed = true;
            }
        }
        changed
    }

    fn handle(&mut self, ev: ServiceEvent) -> bool {
        match ev {
            ServiceEvent::ServiceResolved(info) => {
                if !info.is_valid() {
                    return false;
                }
                // 多网卡时可能给多个地址，优先 IPv4（固件是 2.4G WiFi）
                let Some(ip) = info
                    .addresses
                    .iter()
                    .find(|a| a.is_ipv4())
                    .or_else(|| info.addresses.iter().next())
                    .map(|a| a.to_ip_addr())
                else {
                    return false;
                };

                let txt = &info.txt_properties;
                let num = |k: &str| txt.get(k).and_then(|p| p.val_str().parse::<u32>().ok());
                let s = |k: &str| txt.get(k).map(|p| p.val_str().to_string());

                // fullname 形如 "dash-A292CE._dashboard._udp.local."，首段即实例名
                let name = info
                    .fullname
                    .split('.')
                    .next()
                    .unwrap_or(&info.fullname)
                    .to_string();

                self.devices.insert(
                    info.fullname.clone(),
                    Device {
                        fullname: info.fullname.clone(),
                        name,
                        host: info.host.clone(),
                        addr: SocketAddr::new(ip, info.port),
                        ver: s("ver"),
                        mac: s("mac"),
                        leds: num("leds"),
                        meters: num("meters"),
                        cols: num("cols"),
                    },
                );
                true // 地址或元数据可能已更新，让 UI 重画
            }
            ServiceEvent::ServiceRemoved(_, fullname) => self.devices.remove(&fullname).is_some(),
            _ => false,
        }
    }

    /// 按名字排序的设备列表（顺序稳定，避免 UI 里跳来跳去）
    pub fn devices(&self) -> Vec<Device> {
        let mut v: Vec<Device> = self.devices.values().cloned().collect();
        v.sort_by(|a, b| a.name.cmp(&b.name));
        v
    }

    pub fn get(&self, fullname: &str) -> Option<&Device> {
        self.devices.get(fullname)
    }

    /// 启动阶段的错误（例如没有可用网卡）。None 表示正常。
    pub fn err(&self) -> Option<&str> {
        self.err.as_deref()
    }
}

impl Default for Discovery {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    /// 对着真实网络跑一次，验证确实能发现固件。
    /// 依赖局域网上有一台在线的仪表盘，CI 里不该跑，因此标 #[ignore]：
    ///
    ///   cargo test discover_probe -- --ignored --nocapture
    #[test]
    #[ignore]
    fn discover_probe() {
        let mut d = Discovery::new();
        assert!(d.err().is_none(), "mDNS 启动失败：{:?}", d.err());

        for _ in 0..60 {
            d.poll();
            std::thread::sleep(Duration::from_millis(100));
        }

        let devices = d.devices();
        println!("\n发现 {} 台设备：", devices.len());
        for dev in &devices {
            println!(
                "  {}  host={}  addr={}  ver={:?}  leds={:?} meters={:?}",
                dev.name, dev.host, dev.addr, dev.ver, dev.leds, dev.meters
            );
        }
        assert!(!devices.is_empty(), "没发现设备——固件在线吗？多播通吗？");

        let d0 = &devices[0];
        assert_eq!(d0.addr.port(), 8551, "端口应来自 SRV");
        assert!(d0.ver.is_some(), "TXT 里应有固件版本");
    }
}
