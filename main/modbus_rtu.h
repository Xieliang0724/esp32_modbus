/*
 * modbus_rtu.h - Modbus RTU 从站（UART1 串口）
 *
 * 用途：外部主站经"TTL 转以太网模组"（内置 Modbus TCP<->RTU 转换）或
 * 直接经串口访问本机 Modbus 寄存器。与 TCP 从站共用同一套寄存器模型
 * （mb_device），地址映射一致。
 *
 *   - UART1（GPIO4 TX / GPIO5 RX，9600 8N1）由 uart_io 提供
 *   - 帧格式：从站地址(1) + PDU + CRC16(2)
 *   - 从站地址在 menuconfig 配置（CONFIG_MB_RTU_ADDR，默认 1）
 */
#pragma once

#include "esp_err.h"

/* 初始化并注册 UART1 接收（幂等） */
esp_err_t modbus_rtu_init(void);
