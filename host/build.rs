//! 把 Info.plist 嵌进二进制的 __TEXT/__info_plist 段。
//!
//! 目的：`cargo run` 跑出来的裸可执行文件没有 bundle，macOS 的 TCC 拿不到
//! NSScreenCaptureUsageDescription，于是在弹出授权对话框时**直接终止程序**
//! （不是"拒绝"，是崩溃）。嵌入这段之后，裸二进制也能正常申请屏幕与系统
//! 音频录制权限。
//!
//! 打包成 .app 时用的是 package_macos.sh 生成的 Contents/Info.plist，
//! 两者内容保持一致。

fn main() {
    #[cfg(target_os = "macos")]
    {
        // 1) 嵌入 Info.plist（TCC 权限必须，见文件内注释）
        println!("cargo:rustc-link-arg=-Wl,-sectcreate,__TEXT,__info_plist,Info.plist");
        println!("cargo:rerun-if-changed=Info.plist");

        // 2) screencapturekit 的桥接层是 Swift 写的，静态链接进来后二进制会
        //    依赖 @rpath/libswift_Concurrency.dylib 等。不加 rpath 的话
        //    启动时直接 dyld 报错：Library not loaded ... no LC_RPATH's found。
        //    macOS 的 Swift 运行时在 dyld 共享缓存里，指到 /usr/lib/swift 即可，
        //    不需要随包分发任何 dylib。
        println!("cargo:rustc-link-arg=-Wl,-rpath,/usr/lib/swift");

        // 装了 Xcode 时，其 toolchain 里也有一份 Swift 运行时，作为第二候选
        if let Ok(p) = std::env::var("DEVELOPER_DIR") {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{p}/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift/macosx");
        }
    }
}
