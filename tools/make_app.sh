#!/bin/bash
# ============================================================================
# make_app.sh - 生成 ModbusTool.app（macOS 双击即用的网页调试工具）
#
# 用法：
#   ./tools/make_app.sh                    # 生成到桌面 ~/Desktop/ModbusTool.app
#   ./tools/make_app.sh /任意/输出目录      # 生成到指定目录
#
# 说明：
#   - 纯 shell 脚本，无需安装任何工具（Appify 方式：Info.plist + 启动脚本）
#   - 双击 .app 会弹出终端窗口并启动工具，浏览器自动打开页面
#   - 依赖系统已有 python3（macOS 自带或 homebrew 安装均可）
# ============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_NAME="ModbusTool"
DEST_DIR="${1:-$HOME/Desktop}"
DEST="$DEST_DIR/$APP_NAME.app"

if [ ! -f "$SCRIPT_DIR/modbus_tool.py" ]; then
    echo "错误：未找到 $SCRIPT_DIR/modbus_tool.py" >&2
    exit 1
fi

rm -rf "$DEST"
mkdir -p "$DEST/Contents/MacOS" "$DEST/Contents/Resources"
cp "$SCRIPT_DIR/modbus_tool.py" "$DEST/Contents/Resources/"

# ---- Info.plist ----
cat > "$DEST/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>ModbusTool</string>
    <key>CFBundleIdentifier</key>
    <string>com.esp32modbus.tool</string>
    <key>CFBundleName</key>
    <string>ModbusTool</string>
    <key>CFBundleDisplayName</key>
    <string>Modbus 调试工具</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>10.13</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
EOF

# ---- 启动脚本（双击 .app 时执行） ----
cat > "$DEST/Contents/MacOS/$APP_NAME" <<'EOF'
#!/bin/bash
# ModbusTool 启动器：找到 python3 -> 在终端窗口中运行 modbus_tool.py
RES="$(cd "$(dirname "$0")/../Resources" && pwd)"
APP="$RES/modbus_tool.py"

PY=""
for c in python3 /usr/bin/python3 /opt/homebrew/bin/python3 /usr/local/bin/python3; do
    if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done

if [ -z "$PY" ]; then
    osascript -e 'display dialog "未找到 python3" with title "Modbus 调试工具" message "请先安装 Python3：brew install python3" buttons {"好"} default button 1 with icon stop' 2>/dev/null || echo "未找到 python3"
    exit 1
fi

# 在终端窗口中运行（显示日志；关掉终端即退出工具）
osascript -e "tell application \"Terminal\" to do script \"cd '$RES' && exec '$PY' '$APP'\"" 2>/dev/null

# 若 osascript 不可用（无 GUI 会话），直接后台运行
if [ $? -ne 0 ]; then
    nohup "$PY" "$APP" >/dev/null 2>&1 &
fi
exit 0
EOF
chmod +x "$DEST/Contents/MacOS/$APP_NAME"

echo "✅ 已生成: $DEST"
echo "   双击图标即可启动（会弹出终端窗口 + 浏览器自动打开 http://127.0.0.1:8000）"
