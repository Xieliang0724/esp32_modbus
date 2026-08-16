/*
 * mb_device.h - Modbus TCP 从站设备模型（ESP32 本体寄存器）
 *
 * ESP32 作为 Modbus TCP 从站（Server），寄存器直接映射到本机外设：
 *
 *   | 区域          | 功能码         | 地址范围        | 映射                          |
 *   |---------------|----------------|-----------------|-------------------------------|
 *   | 线圈 COIL     | 01/05/0F      | 0x0000-0x0003   | DO0-DO3（GPIO 数字输出）       |
 *   | 离散输入 DI   | 02            | 0x1000-0x1006   | DI0-DI6（见下方 DI 表）         |
 *   | 输入寄存器 IR | 04            | 0x3000-0x3003   | 设备信息（版本/状态/运行时间）   |
 *   | 保持寄存器 HR | 03/06/10     | 0x4000-0x4003   | 用户参数（RAM 暂存，掉电清零）   |
 *
 * 离散输入 DI 表（0x1000 起）：
 *   | 地址    | 通道  | 语义                                                        |
 *   |---------|-------|-------------------------------------------------------------|
 *   | 0x1000  | DI0   | 通用数字输入（GPIO，内部上拉，实时电平）                      |
 *   | 0x1001  | DI1   | 通用数字输入                                                |
 *   | 0x1002  | DI2   | 通用数字输入                                                |
 *   | 0x1003  | DI3   | 通用数字输入                                                |
 *   | 0x1004  | DI4   | 急停1（常闭 NC，正常=1，按下=0，**锁存**）                    |
 *   | 0x1005  | DI5   | 急停2（常闭 NC，正常=1，按下=0，**锁存**）                    |
 *   | 0x1006  | DI6   | 复位按钮（按下=1，用于解除急停锁存）                          |
 *   | 0x1007  | DI7   | 未使用（恒 0）                                               |
 *
 * 急停锁存语义：急停按下 → 寄存器 0 并锁存；必须满足
 * "急停已松开" 且 "按一下复位按钮（上升沿）" 才解除锁存回到 1。
 *
 * 支持功能码：01/02/03/04/05/06/0F/10，非法请求返回标准异常码。
 * GPIO 引脚在 menuconfig 中配置（CONFIG_MB_DI_GPIO_x / CONFIG_MB_DO_GPIO_x /
 * CONFIG_MB_ESTOPx_GPIO / CONFIG_MB_RESET_BTN_GPIO），
 * 引脚设为 -1 表示该路未使用（急停禁用时寄存器恒为 1=正常，复位按钮恒为 0）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* 寄存器区域基地址（地址为 0 起始，Modbus 协议线上加 1 表示） */
#define MB_COIL_BASE     0x0000   /* 线圈：DO0-DO3 */
#define MB_DI_BASE       0x1000   /* 离散输入：DI0-DI7 */
#define MB_INREG_BASE    0x3000   /* 输入寄存器：设备信息 */
#define MB_HOLDREG_BASE  0x4000   /* 保持寄存器：用户参数 */

#define MB_MAX_DI        8
#define MB_MAX_DO        4
#define MB_MAX_HOLD      4
#define MB_MAX_INREG     4

/* 急停/复位在 DI 表中的下标 */
#define MB_DI_ESTOP1     4
#define MB_DI_ESTOP2     5
#define MB_DI_RESET_BTN  6

/* 初始化 GPIO 与寄存器状态（幂等，应用启动时调用一次） */
esp_err_t mb_device_init(void);

/* 处理一条 Modbus PDU（不含 MBAP/RTU 封装），生成响应 PDU。
 * 成功：resp[0]=功能码，resp_len 为响应长度；
 * 异常：resp[0]=功能码|0x80，resp[1]=异常码，resp_len=2。
 * resp 缓冲区需 >= 255 字节。 */
void mb_device_handle_pdu(uint8_t uid, const uint8_t *pdu, size_t pdu_len,
                          uint8_t *resp, size_t *resp_len);
