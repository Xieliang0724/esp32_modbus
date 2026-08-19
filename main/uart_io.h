/*
 * uart_io.h - 通用串口（UART1）收发模块
 *
 * 用途：普通串口收发（接仪表/传感器/其他 MCU）。
 *   - UART1：TX/RX GPIO 与波特率在 menuconfig 配置（默认 TX=GPIO4, RX=GPIO5, 9600 8N1）
 *   - 提供发送 API 与接收回调（接收数据在回调中给出，调用方自行处理）
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* 接收数据回调（在 uart 接收任务上下文中调用，勿阻塞） */
typedef void (*uart_io_rx_cb_t)(const uint8_t *data, size_t len);

/* 初始化 UART1（幂等）：配置 GPIO、波特率，启动接收任务 */
esp_err_t uart_io_init(void);

/* 发送数据（阻塞写入 UART FIFO） */
int uart_io_send(const uint8_t *data, size_t len);

/* 注册接收回调（NULL 取消） */
void uart_io_set_rx_cb(uart_io_rx_cb_t cb);

/* 是否已初始化 */
bool uart_io_is_ready(void);
