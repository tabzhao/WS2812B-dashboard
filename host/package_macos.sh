#!/bin/bash
#
# 把 release 二进制打包成 macOS .app。
#
# 为什么要打包：Finder 双击裸 Unix 可执行文件时，系统不知道它是图形程序，
# 会用 Terminal.app 当宿主来运行 —— 于是先弹终端窗口，主界面再从终端里冒出来。
# 打包成 .app 后由 WindowServer 直接拉起，不再有终端窗口。
#
# 用法：
#   ./package_macos.sh            # 编译并打包
#   ./package_macos.sh --no-build # 只打包（不重新编译）
#
set -euo pipefail

cd "$(dirname "$0")"

APP_NAME="Dashboard"
DISPLAY_NAME="仪表盘上位机"
BIN="dashboard_host"
IDENT="com.local.dashboard-host"
VERSION="0.1.0"
APP_DIR="${APP_NAME}.app"

# cargo 装在 ~/.cargo/bin 时通常不在非交互 shell 的 PATH 里，
# 直接用 `cargo` 会报 command not found。这里兜底找一下。
CARGO="${CARGO:-$(command -v cargo || true)}"
if [[ -z "${CARGO}" && -x "${HOME}/.cargo/bin/cargo" ]]; then
    CARGO="${HOME}/.cargo/bin/cargo"
fi
[[ -n "${CARGO}" ]] || { echo "找不到 cargo，设置 CARGO=/path/to/cargo 或把它加进 PATH"; exit 1; }

if [[ "${1:-}" != "--no-build" ]]; then
    echo "编译 release ..."
    "${CARGO}" build --release
fi

[[ -x "target/release/${BIN}" ]] || { echo "找不到 target/release/${BIN}，先编译"; exit 1; }

echo "组装 ${APP_DIR} ..."
rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}/Contents/MacOS"
mkdir -p "${APP_DIR}/Contents/Resources"

cp "target/release/${BIN}" "${APP_DIR}/Contents/MacOS/${BIN}"

cat > "${APP_DIR}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>${APP_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${DISPLAY_NAME}</string>
    <key>CFBundleExecutable</key>
    <string>${BIN}</string>
    <key>CFBundleIdentifier</key>
    <string>${IDENT}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
    <key>NSSupportsAutomaticTermination</key>
    <true/>
    <!-- 频谱功能抓系统音频走 ScreenCaptureKit，这是唯一的授权入口。
         缺了它，触发 TCC 提示时程序会被系统终止（不是简单拒绝）。 -->
    <key>NSScreenCaptureUsageDescription</key>
    <string>需要「屏幕与系统音频录制」权限，把 Mac 正在播放的音乐转成频谱显示到灯阵上。不会录制或保存任何画面。</string>
</dict>
</plist>
PLIST

# ad-hoc 签名：不是给 Gatekeeper 发证书，而是让系统把"用户确认过"这件事
# 记在这个 bundle 上，避免每次打开都重新警告。
if command -v codesign >/dev/null 2>&1; then
    codesign --force --deep --sign - "${APP_DIR}" 2>/dev/null && echo "已 ad-hoc 签名" || echo "签名跳过（不影响运行）"
fi

# 让 LaunchServices 立刻认识这个新 app，否则图标可能不刷新
touch "${APP_DIR}"

echo
echo "完成：$(pwd)/${APP_DIR}"
echo "双击它启动，不会再有终端窗口。"
echo
echo "提示：首次打开若被 Gatekeeper 拦下，右键 → 打开 即可（自己编译的本地程序都这样）。"
