/*
 * uart_io.c - 通用串口（UART1）收发实现
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/uart.h"
#include "esp_log.h"

#include "uart_io.h"

static const char *TAG = "uart_io";

#define UART_IO_NUM       UART_NUM_1
#define UART_RX_BUF_SIZE  1024
#define UART_TX_BUF_SIZE  256
#define UART_EVT_QUEUE_LEN 16
#define UART_RX_TIMEOUT   10        /* 帧间隙（符号数） */

static bool s_ready = false;
static uart_io_rx_cb_t s_rx_cb = NULL;
static QueueHandle_t s_rx_queue = NULL;   /* UART 事件队列 */

static void uart_rx_task(void *arg)
{
    uart_event_t evt;
    uint8_t *buf = malloc(UART_RX_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "rx buf malloc failed");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        if (xQueueReceive(s_rx_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (evt.type) {
        case UART_DATA: {
            size_t avail = 0;
            uart_get_buffered_data_len(UART_IO_NUM, &avail);
            if (avail > 0) {
                size_t to_read = avail < UART_RX_BUF_SIZE ? avail : UART_RX_BUF_SIZE;
                int n = uart_read_bytes(UART_IO_NUM, buf, to_read, 0);
                if (n > 0 && s_rx_cb) {
                    s_rx_cb(buf, (size_t)n);
                }
            }
            break;
        }
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            uart_flush_input(UART_IO_NUM);
            break;
        default:
            break;
        }
    }
    free(buf);
    vTaskDelete(NULL);
}

esp_err_t uart_io_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (CONFIG_UART_IO_TX_GPIO < 0 || CONFIG_UART_IO_RX_GPIO < 0 ||
        CONFIG_UART_IO_TX_GPIO == CONFIG_UART_IO_RX_GPIO) {
        ESP_LOGE(TAG, "invalid uart gpio tx=%d rx=%d", CONFIG_UART_IO_TX_GPIO, CONFIG_UART_IO_RX_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    uart_config_t cfg = {
        .baud_rate = CONFIG_UART_IO_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_driver_install(UART_IO_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE,
                                        UART_EVT_QUEUE_LEN, &s_rx_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    uart_param_config(UART_IO_NUM, &cfg);
    uart_set_pin(UART_IO_NUM, CONFIG_UART_IO_TX_GPIO, CONFIG_UART_IO_RX_GPIO, -1, -1);
    uart_set_rx_timeout(UART_IO_NUM, UART_RX_TIMEOUT);
    uart_flush_input(UART_IO_NUM);

    if (xTaskCreate(uart_rx_task, "uart_io_rx", 3072, NULL, 7, NULL) != pdPASS) {
        ESP_LOGE(TAG, "rx task create failed");
        uart_driver_delete(UART_IO_NUM);
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "UART1 %d baud, TX=GPIO%d RX=GPIO%d", CONFIG_UART_IO_BAUD,
             CONFIG_UART_IO_TX_GPIO, CONFIG_UART_IO_RX_GPIO);
    return ESP_OK;
}

int uart_io_send(const uint8_t *data, size_t len)
{
    if (!s_ready) {
        return -1;
    }
    int written = uart_write_bytes(UART_IO_NUM, data, len);
    uart_wait_tx_done(UART_IO_NUM, pdMS_TO_TICKS(200));
    return written;
}

void uart_io_set_rx_cb(uart_io_rx_cb_t cb)
{
    s_rx_cb = cb;
}

bool uart_io_is_ready(void)
{
    return s_ready;
}
