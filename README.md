<div align="center">

# ESP32-C5 Modbus TCP 从站固件

**Wi-Fi 配网 · Modbus TCP 从站 · 急停安全回路 · 网页调试工具**

![version](https://img.shields.io/badge/版本-v1.1.1-blue)
![chip](https://img.shields.io/badge/芯片-ESP32--C5-brightgreen)
![protocol](https://img.shields.io/badge/协议-Modbus--TCP-important)
![tls](https://img.shields.io/badge/TLS-502%20%2F%20802-blueviolet)
![idf](https://img.shields.io/badge/ESP--IDF-v6.0.1-yellow)
![tool](https://img.shields.io/badge/测试工具-零依赖-9cf)

ESP32-C5 本体作为 **Modbus TCP 从站（Server）**：寄存器直接映射到本机外设
（DI / DO / 急停锁存 / 复位按钮），支持**明文 502** 与 **TLS 802** 双监听，
配网页一键启用，自带零依赖网页调试工具。

</div>

---

## 🚀 快速上手

```bash
git clone git@github.com:Xieliang0724/esp32_modbus.git
cd esp32_modbus
idf.py set-target esp32c5 && idf.py build
idf.py -p /dev/cu.usbmodem* flash          # 烧录（Mac 串口名）

python3 tools/modbus_tool.py               # 启动调试工具，浏览器自动打开
```

烧录后设备开机进入配网热点 `ESP32C5-XXXX` → 手机连上访问 `http://192.168.4.1` →
**⚙️ 高级设置** 勾选"启用 Modbus TCP 从站" → 保存，即可用任意 Modbus 客户端读写。

## ✨ 功能亮点

| 能力 | 说明 |
|---|---|
| 🎛️ **Modbus TCP 从站** | ESP32 本体响应请求，寄存器直连本机外设，8 个功能码全支持 |
| 🔒 **TLS 加密** | 明文 502 + Modbus Security 802 双监听，单向验证 |
| 🛑 **急停安全回路** | 2 路急停（常闭 NC）+ 锁存 + 复位按钮解除，fail-safe 设计 |
| 🔌 **DI / DO 外设** | 4 路通用输入 + 4 路输出，GPIO 全部 menuconfig 可配 |
| 🌐 **Web 配网** | SoftAP + 网页双频扫描，DHCP / 静态 IP，断网自动兜底 |
| 🧰 **零依赖调试工具** | 单文件 Python 网页工具，预设本项目寄存器映射 |
| 💾 **配置持久化** | NVS 存储，断电重启自动重连 |
| 🚦 **RGB 状态灯** | 板载 WS2812：橙=配网 / 蓝=热点有客户端 / 绿=已联网 |

## 📋 寄存器映射速览

| 区域 | 地址 | 功能码 | 内容 |
|---|---|---|---|
| 线圈 COIL | `0x0000` | 01 / 05 / 0F | DO0–DO3 数字输出 |
| 离散输入 DI | `0x1000` | 02 | DI0-3 通用输入 · DI4/5 急停（锁存）· DI6 复位 |
| 输入寄存器 IR | `0x3000` | 04 | 急停状态 / 急停实时 IO / 复位 / 设备信息 |
| 保持寄存器 HR | `0x4000` | 03 / 06 / 10 | 用户参数（RAM 暂存） |

> 急停为**常闭 NC**：正常闭合 = 1，按下 = 0 并**锁存**；急停松开后按一下复位按钮（上升沿）才回到 1。回路断开（断线/未接）一律视为急停触发（fail-safe）。

## 📖 目录

- [功能特性](#功能特性)
- [环境要求](#环境要求)
- [目录结构](#目录结构)
- [编译与烧录](#编译与烧录)
- [使用流程](#使用流程)
- [配置项（menuconfig）](#配置项menuconfig-sdkconfigdefaults)
- [REST API](#rest-api)
- [Modbus TCP 从站（详细）](#modbus-tcp-从站esp32-本体高级设置)
  - [寄存器映射](#寄存器映射表)
  - [配置项](#配置项)
  - [本开发板接线](#本开发板接线排针-32-脚)
  - [客户端测试示例](#客户端测试示例)
  - [Modbus 测试工具](#🧰-modbus-测试工具网页版maclinux-通用)
  - [Modbus TLS](#modbus-tlsv110-保留自基版本)
- [版本管理（git tag）](#版本管理git-tag)
- [常见问题](#常见问题)

---

## 功能特性

- 🌐 **Web 配网**：设备开机进入 SoftAP 热点，手机/电脑连接后访问 `http://192.168.4.1` 打开配网页面
- 🔎 **mDNS**：联网后局域网内可直接访问 `http://esp32c5.local`（无需记 IP）
- 📶 **SSID 下拉选择**：自动双频（2.4G + 5G）扫描，从列表选择 Wi-Fi，也支持手动输入（含隐藏网络）
- 🌐 **IP 方式可选**：
  - **DHCP 自动获取**（默认）
  - **静态 IP**：可自定义 IP / 子网掩码 / 网关 / DNS（留空则掩码默认 `255.255.255.0`、DNS 默认用网关）
- 📴 **SoftAP 可关**：配网时勾选"连接成功后关闭热点"，STA 连上后 AP 自动关闭（省电、更安全）；不勾选则 AP 保持开启，随时可重新配网
- 🔁 **自动回退**：STA 连接失败超过 `CONFIG_PROV_CONNECT_RETRY_MAX` 次（默认 5 次）后自动重新打开 SoftAP 进入配网模式
- 💾 **配置持久化**：配置保存在 NVS，断电重启自动连接
- 🔘 **复位按键**：长按 GPIO9（BOOT 键）3 秒清除配置并重启进入配网模式

### 🛡️ 失联兜底（AP 与 STA 不允许同时死掉）

C5 为**单射频**芯片，STA 连接/扫描时 AP 信号会变弱；且部分板子（如本项目的）没有复位按钮，因此固件保证**任何时刻 AP 和 STA 至少有一个可用**：

| 状态 | AP | STA | 可达方式 |
|---|---|---|---|
| 配网模式 | 🟢 开 | 空闲 | 走热点 `192.168.4.1` |
| 连接中/重试中 | 🟢 开 | 连接中 | 走热点 |
| 已联网 + 关闭热点 | 🔴 关 | 🟢 在线 | 走路由器 |
| 已联网 + 保持热点 | 🟢 开 | 🟢 在线 | 双通道 |

**兜底规则**：STA 断开后**不立即**开 AP（避免单射频频繁跳变），而是**连续断开超过 N 秒**（默认 15 秒）才自动开启热点。N 可在配网表单"断网后开启热点兜底延迟（秒）"设置（0~3600，0 = 立即开启，随配置持久化）。重试耗尽后自动回退配网模式（AP 常开）。

> ⚠️ 唯一残留窗口：路由器"静默死亡"时（信号还在但路由中断），驱动需等 beacon 超时（约 10~60 秒）才触发断开，之后 AP 兜底才启动。属 WiFi 协议固有延迟。

## 环境要求

- ESP32-C5 开发板（如 ESP32-C5-DevKitC-1，N4=4MB / N8R8=8MB 闪存）
- **ESP-IDF v5.4+**（本工程基于 v6.0.1 编译验证；v5.4 为 C5 技术预览支持）
- VS Code + [ESP-IDF 扩展](https://marketplace.visualstudio.com/items?itemName=espressif.esp-idf-extension)（推荐）

## 目录结构

```
esp32_modbus/
├── CMakeLists.txt
├── sdkconfig.defaults        # 目标/闪存大小等默认配置
├── .vscode/settings.json     # 项目级 IDF 路径（不影响全局设置）
├── tools/
│   └── modbus_tool.py        # Modbus TCP 调试工具（零依赖，网页版）
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild     # 配网 + Modbus 从站 menuconfig 选项
    ├── idf_component.yml     # 托管组件依赖（espressif/cjson）
    ├── app_main.c            # 入口：初始化 + 启动流程
    ├── config_store.[ch]     # NVS 配置持久化
    ├── wifi_mgr.[ch]         # Wi-Fi 状态机（AP/STA/扫描/静态IP/回退）
    ├── modbus_gw.[ch]        # Modbus TCP 从站（Server）：TCP/TLS 监听、MBAP 组帧
    ├── mb_device.[ch]        # 从站设备模型：寄存器映射 + 功能码 + DI/DO/急停 GPIO
    ├── rgb_led.[ch]          # RGB 状态灯（GPIO27 WS2812）
    ├── web_server.[ch]       # HTTP 配网服务器（REST API）
    └── www/index.html        # 内嵌配网网页（EMBED_FILES）
```

## 编译与烧录

### 方式一：VS Code（推荐）

1. VS Code 打开本工程文件夹（`File > Open Folder`）
2. 确认左下角显示目标芯片 **ESP32-C5**
3. 点击底部状态栏 **Build**（或 `Ctrl+E B`）编译
4. 插上开发板，点击 **Flash**（或 `Ctrl+E F`）烧录
5. 点击 **Monitor**（或 `Ctrl+E M`）查看串口日志

> 若未识别目标芯片，执行 `Ctrl+Shift+P` → `ESP-IDF: Set Espressif Device Target` → 选择 `esp32c5`。

### 方式二：命令行

```bash
. ~/.espressif/v6.0.1/esp-idf/export.sh        # 按你的 IDF 路径调整
cd esp32_modbus
idf.py set-target esp32c5
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor      # macOS 串口设备名
```

## 使用流程

**首次使用（配网）：**

1. 给设备上电，等待日志出现 `SoftAP ssid=ESP32C5-XXXX`
2. 手机/电脑连接 Wi-Fi 热点 **`ESP32C5-XXXX`**（默认开放网络；如需密码见下文配置）
3. 浏览器访问 **`http://192.168.4.1`**，打开配网页面
4. 点击 **刷新** 扫描 Wi-Fi → 下拉选择你的路由器 SSID（或手动输入）
5. 输入密码；按需选择 **静态 IP** 并填写 IP/网关等；按需勾选 **连接成功后关闭热点**
6. 点击 **保存并连接**，等待状态变为"已连接"

**再次使用：** 上电后自动按已保存配置连接。若勾选了关闭热点，如需重新配网：

- 长按开发板 **BOOT 键（GPIO9）3 秒** 清除配置重启，或
- 烧录前 `idf.py erase-flash` 清空

## 配置项（menuconfig / sdkconfig.defaults）

| 配置 | 默认值 | 说明 |
|---|---|---|
| `CONFIG_PROV_AP_SSID_PREFIX` | `ESP32C5` | 热点名前缀，实际为 `<前缀>-<MAC后4位>` |
| `CONFIG_PROV_AP_PASSWORD` | 空 | 热点密码，留空=开放网络；设置需 ≥8 位 |
| `CONFIG_PROV_AP_CHANNEL` | 1 | 热点 2.4G 信道 |
| `CONFIG_PROV_CONNECT_RETRY_MAX` | 5 | STA 失败重试次数，超过后回到配网模式 |
| `CONFIG_PROV_STA_TIMEOUT_MS` | 15000 | 单次连接超时（毫秒） |
| `CONFIG_PROV_RESET_GPIO` | 9 | 复位配网按键 GPIO（-1 禁用） |
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` | y | 闪存 8MB（实测 N8R4；N4 板改 4MB） |
| `CONFIG_MB_DI_GPIO_0~3` | 4/5/6/7 | 通用输入 GPIO（-1 禁用该路） |
| `CONFIG_MB_DO_GPIO_0~3` | 23/24/25/26 | 数字输出 GPIO（-1 禁用该路） |
| `CONFIG_MB_ESTOP1/2_GPIO` | 8 / 10 | 急停1/急停2 GPIO（-1 禁用，恒为正常） |
| `CONFIG_MB_RESET_BTN_GPIO` | 15 | 复位按钮 GPIO（-1 禁用，急停只能断电解除） |

## REST API

| 接口 | 方法 | 说明 |
|---|---|---|
| `/` | GET | 配网页面 |
| `/api/status` | GET | `{state: config/connecting/connected, ssid, ip, ap_on}` |
| `/api/scan` | GET | 触发/查询双频扫描，返回网络列表 |
| `/api/config` | POST | 提交 `{ssid, password, ip_mode, ip, netmask, gateway, dns, ap_off}` |
| `/api/reset` | POST | 清除配置并回到配网模式 |
| `/api/gw` | GET/POST | 读写 Modbus TCP 从站配置 `{enabled, port, client_ip, tls_enabled, tls_port}` |

## Modbus TCP 从站（ESP32 本体，高级设置）

配网页面底部 **⚙️ 高级设置** 中可配置并启用。**ESP32 本体作为 Modbus TCP 从站（Server）**，寄存器直接映射到本机外设，客户端（上位机/SCADA/Modbus Poll）通过 TCP 直连读写，**不再依赖外部从站**（v1.1.0 起移除 GD32 RTU 透传，UART1 已释放）。

### 寄存器映射表

| 区域 | 功能码 | 地址 | 数量 | 映射 |
|---|---|---|---|---|
| 线圈 COIL | 01/05/0F | 0x0000–0x0003 | 4 | DO0–DO3（GPIO 数字输出，上电默认断开） |
| 离散输入 DI | 02 | 0x1000–0x1006 | 7 | DI0-3 通用输入 / DI4-5 急停 / DI6 复位（见下表） |
| 输入寄存器 IR | 04 | 0x3000–0x3007 | 8 | 急停状态 / 急停实时 IO / 复位 / 设备信息（见下表） |
| 保持寄存器 HR | 03/06/10 | 0x4000–0x4003 | 4 | 用户参数（RAM 暂存，掉电清零） |

> 地址为 0 起始（Modbus 协议线上 +1）。GPIO 引脚在 menuconfig（`Modbus Slave Configuration`）中配置，`-1` = 该路未使用。

**离散输入 DI 表（0x1000 起，只读）：**

| 地址 | 通道 | 语义 |
|---|---|---|
| 0x1000–0x1003 | DI0–DI3 | 通用数字输入（GPIO，内部上拉，实时电平） |
| 0x1004 | DI4 | **急停1**（常闭 NC：正常=1，按下=0，**锁存**） |
| 0x1005 | DI5 | **急停2**（常闭 NC：正常=1，按下=0，**锁存**） |
| 0x1006 | DI6 | **复位按钮**（常开 NO：按下=1） |
| 0x1007 | DI7 | 未使用（恒 0） |

**急停锁存语义**（安全回路）：急停按下 → 对应寄存器变 **0** 并锁存（松开不回 1）；必须满足 **急停已松开** 且 **按一下复位按钮**（上升沿，一次同时解除两路），寄存器才回到 1。急停只上报状态，不联动 DO。复位按钮禁用（-1）时，急停锁存只能断电重启解除。

> ⚠️ **Fail-safe**：急停回路**断开即视为急停触发**（未接线/断线/按下时寄存器均为 0）。接线后正常状态应为：NC 触点闭合接 GND → 寄存器 1。

**输入寄存器（0x3000 起，只读）：**

| 地址 | 内容 |
|---|---|
| 0x3000 | **设备急停状态**：1 = 系统正常，0 = 急停触发（**锁存**，需复位解除） |
| 0x3001 | 急停1 实时 IO 状态（1 = 触点闭合正常，0 = 按下/断开） |
| 0x3002 | 急停2 实时 IO 状态（同上） |
| 0x3003 | 复位按钮实时状态（1 = 按下） |
| 0x3004 | 固件版本号 `(主版本<<8) | 次版本` |
| 0x3005 | WiFi STA 状态（0 = 未联网，1 = 已联网） |
| 0x3006/0x3007 | 设备运行秒数（低 16 位 / 高 16 位） |

> 设备急停状态（0x3000）：任一路急停按下 → 0 并锁存；必须**急停均已松开**且**按一下复位按钮**才恢复 1。触发急停时板载 LED 变**红色闪烁**，解除后恢复（联网=绿 / 未联网=橙）。

**支持功能码**：01（读线圈）、02（读离散输入）、03（读保持寄存器）、04（读输入寄存器）、05（写单线圈）、06（写单寄存器）、0F（写多线圈）、10（写多寄存器）；非法请求返回标准 Modbus 异常码（01/02/03）。

### 配置项

| 配置项 | 默认 | 说明 |
|---|---|---|
| 启用从站 | 关 | 勾选后生效 |
| 本地 TCP 端口 | 502 | Modbus 明文标准端口 |
| 允许客户端 IP | 空 | 留空 = 允许所有客户端；填写后仅该 IP 可连 |
| 启用 TLS | 关 | 单向 TLS（服务端证书），与明文 502 并存 |
| TLS 端口 | 802 | Modbus Security 标准端口 |
| 通用 DI/DO GPIO | DI=GPIO4-7 / DO=GPIO23-26 | menuconfig 配置 |
| 急停1/急停2/复位 GPIO | GPIO8 / GPIO10 / GPIO15 | menuconfig 配置 |

### 本开发板接线（排针 32 脚）

| Modbus 通道 | 排针引脚 | GPIO | 接线 |
|---|---|---|---|
| DI0 / DI1 / DI2 / DI3 | IO4 / IO5 / IO6 / IO7 | GPIO4/5/6/7 | 通用干接点，一端接 GND |
| DI4 急停1 | IO8 | GPIO8 | 常闭 NC 触点，一端接 GND |
| DI5 急停2 | IO10 | GPIO10 | 常闭 NC 触点，一端接 GND |
| DI6 复位按钮 | IO15 | GPIO15 | 常开 NO 按钮，一端接 GND |
| DO0 / DO1 / DO2 / DO3 | IO23 / IO24 / IO25 / IO26 | GPIO23/24/25/26 | 继电器/指示灯，上电默认断开 |

> ⚠️ 本开发板避开的引脚：**IO11/12**（串口 U0TXD/RXD）、**IO13/14**（USB_D-/D+，板载 USB 口）、**IO27**（RGB LED）、**IO28**（下载模式 strapping）、**IO9**（BOOT 键）。IO16-22 未引出排针，不可用。如需调整接线，在 menuconfig（`Modbus Slave Configuration`）中修改对应 GPIO。

### 客户端测试示例

```bash
# 用 modpoll（Linux/macOS）
modpoll -m tcp -a 1 -t 0 -r 0x1001 -c 7 <设备IP>     # 读全部 7 路离散输入（DI0-6）
modpoll -m tcp -a 1 -r 0x3000 -c 3 <设备IP>           # 读输入寄存器（设备信息）
# 写 DO0 线圈（ON）
modpoll -m tcp -a 1 -t 0 -r 0x0001 -c 1 -1 <设备IP>   # 需 modpoll 0.x 具体参数，见其文档
```

**急停/复位验证流程**：
1. 正常状态：读 DI4/DI5 = `1`（触点闭合）
2. 按下急停 → DI4（或 DI5）= `0`；松开急停 → 仍为 `0`（已锁存）
3. 按一下复位按钮（DI6 读到 1）→ DI4/DI5 回到 `1`（解除锁存）

### 🧰 Modbus 测试工具（网页版，Mac/Linux 通用）

项目自带单文件测试工具 `tools/modbus_tool.py`，仅用 Python 标准库（无需 pip 安装任何依赖），启动后自动打开浏览器：

```bash
python3 tools/modbus_tool.py                 # 默认 http://127.0.0.1:8000，自动开浏览器
python3 tools/modbus_tool.py --port 9000     # 换端口
python3 tools/modbus_tool.py --no-browser    # 不自动开浏览器
```

**Mac 双击版（可选）**：一键生成 `ModbusTool.app`，双击图标即用（弹出终端 + 浏览器自动打开）：

```bash
./tools/make_app.sh                          # 生成到桌面 ~/Desktop/ModbusTool.app
./tools/make_app.sh /任意目录                 # 生成到指定目录
```

功能：

- **连接配置**：设备 IP / 端口（502 明文、802 TLS 可勾选）/ 单元号，一键"测试连接"
- **读取**：功能码 01/02/03/04，支持任意起始地址与数量，结果表格带本项目寄存器语义标注
- **预设按钮**：一键读"全部 DI（含急停/复位）"、"DO 线圈"、"设备信息"、"保持寄存器"
- **写入**：05 写单线圈（ON/OFF）、06 写单寄存器、0F 写多线圈、10 写多寄存器；DO 快速测试按钮
- **自动刷新**：定时轮询读区（对急停/复位等输入监控特别有用）
- **TLS 支持**：对自签名证书自动忽略校验（仅测试用途）

### Modbus TLS（v1.1.0+ 保留自基版本）

- 启用后设备同时监听 **明文 502** 和 **TLS 802** 两个端口
- **单向 TLS**：客户端验证设备证书（`CN=esp32c5.local`，自签名，10 年），设备不验证客户端
- 测试握手：`openssl s_client -connect <设备IP>:802 -servername esp32c5.local`
- 自签名证书客户端会提示"不受信任"，属正常

**替换为自己的证书**：把 `main/certs/server_cert.pem` 和 `server_key.pem` 换成你们自己的（重新编译烧录）。生成自签名证书命令：

```bash
cd main/certs
openssl req -x509 -newkey rsa:2048 -keyout server_key.pem -out server_cert.pem \
  -days 3650 -nodes -subj "/CN=esp32c5.local/O=YourOrg" \
  -addext "subjectAltName=DNS:esp32c5.local,IP:192.168.4.1"
```

## 版本管理（git tag）

当前版本 **v1.1.1** 已打标签，固件内置版本号（网页状态面板 / `/api/status` / 串口日志 `App version:` 均可查看）。

**发布新版本**（改完代码后）：

```bash
git add -A
git commit -m "v1.2.0: 新功能描述"
git tag -a v1.2.0 -m "v1.2.0"
idf.py build && idf.py -p /dev/cu.usbserial-5C310834821 flash
git push && git push --tags
```

**回退到旧版本**（出问题时一键回到上个可用版本）：

```bash
git checkout v1.0.0
idf.py build && idf.py -p /dev/cu.usbserial-5C310834821 flash
git checkout main   # 回退完切回最新代码继续开发
```

> 提示：版本号由 `git describe` 自动生成（即 `PROJECT_VER`）；`build/`、`sdkconfig` 等已加入 `.gitignore` 不入库。若需**运行时自动回退**（OTA 升级失败自动回滚旧固件），可后续基于 IDF 的 OTA + `esp_ota_mark_app_valid_cancel_rollback` 机制扩展。

## 常见问题

- **连不上热点**：确认热点名是 `ESP32C5-XXXX`（日志中会打印）；若设了密码，确认密码 ≥8 位
- **扫描不到 5G 网络**：确认路由器 5G 开启且设备处于 5G 覆盖范围（5G 穿墙弱）
- **配网后想换网络**：长按 BOOT 3 秒，或连接热点（若未关闭）重新配置
- **Modbus 连接被拒绝**：设备正在重启/重连（等几秒）或从站未启用（网页高级设置勾选"启用"）；用 `tools/modbus_tool.py` 的错误提示定位（会显示实际连接地址）
- **IDF 版本兼容性**：本工程按 IDF v6.0.1 API 编写（`ESP_ERR_WIFI_CONN`、`esp_system.h`、cjson 托管组件等）；如用 v5.4/v5.5 请留意 API 差异

---

<div align="center">

**项目沿革**：本工程由 [`esp32c5_web_provision`](https://github.com/Xieliang0724/esp32c5_web_provision)（v1.2.0）全量复制而来，v1.1.0 起从"RTU↔TCP 网关"升级为"ESP32 本体 Modbus TCP 从站"，版本号从 v1.0.0 重新计数。

</div>
