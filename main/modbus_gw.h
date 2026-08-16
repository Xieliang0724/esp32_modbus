/*
 * modbus_gw.h - Modbus TCP 从站（Server）
 *
 * 功能（本机从站，非透传）：
 *   - ESP32 本体作为 Modbus TCP 从站，监听本地端口（默认 502）
 *   - 寄存器直接映射到本机外设（DI/DO/设备信息，见 mb_device.h）
 *   - 支持客户端 IP 白名单（空 = 无限制）
 *   - 可选 TLS 监听（Modbus Security，端口 802，单向验证）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config_store.h"   /* gw_config_t */

/* 初始化：加载已保存配置，若启用则启动从站 */
void modbus_gw_init(void);

/* 应用新配置（停止旧服务并重启） */
esp_err_t modbus_gw_reconfigure(const gw_config_t *cfg);

/* 获取当前运行配置 */
void modbus_gw_get_config(gw_config_t *cfg);

bool modbus_gw_is_running(void);
