//! macOS 菜单栏常驻图标（NSStatusItem）+ 主窗口显隐
//!
//! ## 为什么需要它
//!
//! 关掉主窗口之后要让 UDP 推送继续跑，进程就必须活着；但进程活着又没有任何入口
//! 能把它叫回来，就变成「关了就只能 kill」的幽灵进程。菜单栏图标是 macOS 上
//! 最标准的常驻入口（LSUIElement 那种纯后台形态连窗口都显示不了，不适合这里）。
//!
//! ## 两个关键决定
//!
//! 1. **关窗口 = 隐藏（`orderOut:`），不是销毁**。
//!    gpui 的窗口一旦真正关闭，窗口里的 entity 会被 drop，挂在它上面的后台调度
//!    循环（60fps 像素 / 10Hz 表头）随之停摆 —— 那正是我们要保住的东西。
//!    所以 `on_window_should_close` 返回 **false** 拦下关闭，再把 NSWindow 藏起来。
//!
//! 2. **菜单点击只投递动作，不在 AppKit 回调里碰 gpui 状态**。
//!    AppKit 的回调发生在主线程，但那一刻 gpui 可能正持有 app 的可变借用。
//!    这里用一个 mpsc channel 把动作丢出去，由 gpui 自己的 tick 去 poll。

use std::ffi::c_void;
use std::ptr::null_mut;
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::Mutex;

use objc2::runtime::AnyObject;
use objc2::{class, define_class, msg_send, sel, ClassType};
use objc2_foundation::{NSObject, NSString};
use raw_window_handle::{HasWindowHandle, RawWindowHandle};

/// 菜单栏菜单能触发的动作。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StatusAction {
    /// 把主窗口叫回来
    ShowWindow,
    /// 退出程序
    Quit,
}

/// 动作投递通道。菜单回调在 AppKit 侧，gpui 侧需要自己来取。
static TX: Mutex<Option<Sender<StatusAction>>> = Mutex::new(None);
static RX: Mutex<Option<Receiver<StatusAction>>> = Mutex::new(None);

/// 主窗口的 NSWindow 指针，用于 orderOut / makeKeyAndOrderFront。
/// gpui 没有暴露「隐藏窗口」的跨平台 API，只能自己拿原生句柄。
static NS_WINDOW: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

/// 状态栏图标本体（retain 过，永不释放 —— 本来就希望它活到进程结束）。
static NS_ITEM: AtomicPtr<c_void> = AtomicPtr::new(null_mut());

// 菜单项的 target。AppKit 的 `NSMenuItem.target` 是 **assign**（不持有），
// 所以必须自己 retain 住，否则点一下就野指针。
// 这里必须用普通注释：`define_class!` 宏展开后会丢弃文档注释，写成 /// 会触发
// "unused doc comment" 警告。
define_class!(
    #[unsafe(super(NSObject))]
    #[name = "DashboardStatusTarget"]
    struct StatusTarget;

    impl StatusTarget {
        #[unsafe(method(onItem:))]
        fn on_item(&self, sender: &AnyObject) {
            let tag: isize = unsafe { msg_send![sender, tag] };
            let action = if tag == 0 {
                StatusAction::ShowWindow
            } else {
                StatusAction::Quit
            };
            if let Some(tx) = TX.lock().unwrap().as_ref() {
                let _ = tx.send(action);
            }
        }
    }
);

/// 创建菜单栏图标。整个进程只需要调一次。
pub fn install() {
    let (tx, rx) = mpsc::channel();
    *TX.lock().unwrap() = Some(tx);
    *RX.lock().unwrap() = Some(rx);
    unsafe { build_status_item() };
}

unsafe fn build_status_item() {
    // NSVariableStatusItemLength = -1.0：宽度跟着内容走
    let bar: *mut AnyObject = msg_send![class!(NSStatusBar), systemStatusBar];
    let item: *mut AnyObject = msg_send![bar, statusItemWithLength: -1.0f64];
    let _: () = msg_send![item, retain];
    NS_ITEM.store(item as *mut c_void, Ordering::SeqCst);

    let button: *mut AnyObject = msg_send![item, button];
    let title = NSString::from_str("DASH");
    let _: () = msg_send![button, setTitle: &*title];

    // 每一步都显式标注成 *mut AnyObject：嵌套 msg_send 会让返回类型推不出来
    let menu_alloc: *mut AnyObject = msg_send![class!(NSMenu), alloc];
    let menu_title = NSString::from_str("");
    let menu: *mut AnyObject = msg_send![menu_alloc, initWithTitle: &*menu_title];
    let _: () = msg_send![menu, retain];

    let target_alloc: *mut AnyObject = msg_send![StatusTarget::class(), alloc];
    let target: *mut AnyObject = msg_send![target_alloc, init];
    let _: () = msg_send![target, retain];

    for (tag, label) in [(0isize, "打开仪表盘"), (1isize, "退出")] {
        let t = NSString::from_str(label);
        let empty = NSString::from_str("");
        let mi_alloc: *mut AnyObject = msg_send![class!(NSMenuItem), alloc];
        let mi: *mut AnyObject = msg_send![
            mi_alloc,
            initWithTitle: &*t,
            action: sel!(onItem:),
            keyEquivalent: &*empty
        ];
        let _: () = msg_send![mi, setTag: tag];
        let _: () = msg_send![mi, setTarget: target];
        let _: () = msg_send![menu, addItem: mi];
    }

    let _: () = msg_send![item, setMenu: menu];
}

/// 记住主窗口的原生句柄。必须在窗口创建后、可能隐藏之前调用。
pub fn remember_window(window: &gpui::Window) {
    // 注意：gpui 的 `Window` 自己也有个 `window_handle()` 固有方法（返回 AnyWindowHandle），
    // 会遮蔽 trait 方法，所以这里显式写全 `HasWindowHandle::window_handle`。
    let Ok(handle) = HasWindowHandle::window_handle(window) else {
        return;
    };
    if let RawWindowHandle::AppKit(h) = handle.as_raw() {
        // raw-window-handle 0.6 的 AppKit 句柄只给 ns_view，窗口要再问视图要一次
        let view = h.ns_view.as_ptr() as *mut AnyObject;
        let win: *mut AnyObject = unsafe { msg_send![view, window] };
        NS_WINDOW.store(win as *mut c_void, Ordering::SeqCst);
    }
}

/// 取回一个待处理动作（不阻塞）。gpui 的 tick 里每帧调一次即可。
pub fn poll() -> Option<StatusAction> {
    RX.lock().unwrap().as_ref()?.try_recv().ok()
}

/// 隐藏主窗口。窗口对象本身仍然存活，后台调度不受影响。
pub fn hide_window() {
    let p = NS_WINDOW.load(Ordering::SeqCst);
    if p.is_null() {
        return;
    }
    unsafe {
        let w = p as *mut AnyObject;
        let _: () = msg_send![w, orderOut: null_mut::<AnyObject>()];
    }
}

/// 把主窗口叫回来，并让程序成为前台应用。
pub fn show_window() {
    let p = NS_WINDOW.load(Ordering::SeqCst);
    if p.is_null() {
        return;
    }
    unsafe {
        let w = p as *mut AnyObject;
        let _: () = msg_send![w, makeKeyAndOrderFront: null_mut::<AnyObject>()];
        let app: *mut AnyObject = msg_send![class!(NSApplication), sharedApplication];
        let _: () = msg_send![app, activateIgnoringOtherApps: true];
    }
}

/// 改状态栏图标上的文字（比如标出「未连接设备」）。
#[allow(dead_code)]
pub fn set_title(text: &str) {
    let p = NS_ITEM.load(Ordering::SeqCst);
    if p.is_null() {
        return;
    }
    unsafe {
        let item = p as *mut AnyObject;
        let button: *mut AnyObject = msg_send![item, button];
        let title = NSString::from_str(text);
        let _: () = msg_send![button, setTitle: &*title];
    }
}

// 让编译器知道这几个 import 是给 declare_class 生成的 impl 用的
#[allow(unused_imports)]
use objc2::runtime::NSObject as _NSObjectUnused;
#[allow(unused_imports)]
use objc2::Message as _MessageUnused;
