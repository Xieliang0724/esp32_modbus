/*
 * modbus_rtu.c - Modbus RTU 从站（UART1）实现
 *
 * 接收 uart_io 的字节流，按 RTU 帧（静默间隔定界）解析：
 *   从站地址(1) + PDU + CRC16(2)
 * 校验地址与 CRC 后，复用 mb_device_handle_pdu 处理 PDU，组 RTU 响应发回。
 */
#include <string.h>

#include "esp_log.h"

#include "mb_device.h"
#include "modbus_rtu.h"
#include "uart_io.h"

static const char *TAG = "mb_rtu";

#define RTU_FRAME_MAX  260           /* 1 addr + 253 pdu + 2 crc */
#define RTU_MIN_FRAME  4             /* 1 addr + 1 fc + 2 crc */
#define RTU_BROADCAST  0             /* 广播地址：接收但不响应 */

static bool s_ready = false;
static uint8_t s_frame[RTU_FRAME_MAX];
static size_t s_frame_len = 0;

/* Modbus CRC16（poly 0x8005，反射 0xA001，初值 0xFFFF） */
static uint16_t rtu_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* uart_io 接收回调：帧缓冲 + 校验 + 处理 + 响应 */
static void rtu_rx_cb(const uint8_t *data, size_t len)
{
    if (!s_ready) {
        return;
    }
    /* 追加到帧缓冲；超长丢弃重建 */
    if (s_frame_len + len > RTU_FRAME_MAX) {
        s_frame_len = 0;
    }
    memcpy(s_frame + s_frame_len, data, len);
    s_frame_len += len;

    if (s_frame_len < RTU_MIN_FRAME) {
        return;
    }

    /* 校验 CRC16（RTU 规范：CRC 低字节在前） */
    uint16_t crc_rx = (uint16_t)(s_frame[s_frame_len - 1] << 8) | s_frame[s_frame_len - 2];
    uint16_t crc_calc = rtu_crc16(s_frame, s_frame_len - 2);
    if (crc_rx != crc_calc) {
        ESP_LOGW(TAG, "CRC mismatch, drop %uB frame", s_frame_len);
        s_frame_len = 0;
        return;
    }

    uint8_t addr = s_frame[0];
    size_t pdu_len = s_frame_len - 3;             /* 去掉 addr 和 crc */
    const uint8_t *pdu = s_frame + 1;

    /* 地址匹配（广播不响应） */
    if (addr != CONFIG_MB_RTU_ADDR) {
        if (addr != RTU_BROADCAST) {
            ESP_LOGI(TAG, "addr %u != %u, ignore", addr, CONFIG_MB_RTU_ADDR);
        }
        s_frame_len = 0;
        return;
    }

    if (pdu_len < 1) {
        s_frame_len = 0;
        return;
    }

    /* 复用寄存器模型处理 PDU */
    ESP_LOGI(TAG, "RTU req addr=%u fc=0x%02X len=%u", addr, pdu[0], (unsigned)pdu_len);
    uint8_t resp_pdu[RTU_FRAME_MAX];
    size_t resp_pdu_len = 0;
    mb_device_handle_pdu(addr, pdu, pdu_len, resp_pdu, &resp_pdu_len);

    /* 组 RTU 响应：addr + resp_pdu + crc（CRC 低字节在前） */
    if (resp_pdu_len > 0) {
        /* resp_pdu_len 上界：resp 数组固定为 3 + RTU_FRAME_MAX，
         * 但 addr(1) + pdu + crc(2) 需要 pdu <= RTU_FRAME_MAX - 3 = 257
         * （Modbus PDU spec 最大 253）。超出即丢弃，防止栈越界。 */
        if (resp_pdu_len > RTU_FRAME_MAX - 3) {
            ESP_LOGE(TAG, "resp pdu too long: %u, drop", (unsigned)resp_pdu_len);
            s_frame_len = 0;
            return;
        }
        uint8_t resp[RTU_FRAME_MAX];
        resp[0] = addr;
        memcpy(resp + 1, resp_pdu, resp_pdu_len);
        uint16_t crc = rtu_crc16(resp, 1 + resp_pdu_len);
        resp[1 + resp_pdu_len] = crc & 0xFF;         /* CRC_L */
        resp[2 + resp_pdu_len] = (crc >> 8) & 0xFF;  /* CRC_H */
        uart_io_send(resp, 3 + resp_pdu_len);
        ESP_LOGI(TAG, "RTU resp fc=0x%02X len=%u", resp_pdu[0], (unsigned)(3 + resp_pdu_len));
    }
    s_frame_len = 0;
}

esp_err_t modbus_rtu_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_frame_len = 0;
    s_ready = true;
    uart_io_set_rx_cb(rtu_rx_cb);
    ESP_LOGI(TAG, "Modbus RTU slave on UART1, addr=%d", CONFIG_MB_RTU_ADDR);
    return ESP_OK;
}
