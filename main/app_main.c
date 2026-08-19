/*
 * app_main.c - ESP32-C5 Web 配网固件入口
 *
 * 流程：
 *   1. 初始化 NVS / Wi-Fi / Web 服务器
 *   2. 若有已保存配置 -> 直接连接 STA（可选关闭 SoftAP）
 *   3. 若无配置或连接失败 -> 进入 SoftAP 配网模式（192.168.4.1）
 *
 * 复位按键（可选）：长按 CONFIG_PROV_RESET_GPIO 3 秒
 *   清除已保存配置并重启进入配网模式。
 */
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"   /* esp_restart() (v6.0: 由 esp_restart.h 迁移至此) */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"

#include "config_store.h"
#include "modbus_gw.h"
#include "modbus_rtu.h"
#include "mb_device.h"
#include "rgb_led.h"
#include "uart_io.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "app_main";

#ifndef CONFIG_PROV_RESET_GPIO
#define CONFIG_PROV_RESET_GPIO (-1)
#endif

#if CONFIG_PROV_RESET_GPIO >= 0

#define RESET_BTN_PRESS_MS 3000
#define RESET_BTN_POLL_MS  50

static void reset_btn_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_PROV_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    uint32_t pressed_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RESET_BTN_POLL_MS));
        /* BOOT 按键按下为低电平 */
        if (gpio_get_level(CONFIG_PROV_RESET_GPIO) == 0) {
            pressed_ms += RESET_BTN_POLL_MS;
            if (pressed_ms >= RESET_BTN_PRESS_MS) {
                ESP_LOGW(TAG, "reset button long-pressed, clearing config...");
                config_store_clear();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            pressed_ms = 0;
        }
    }
}

static void start_reset_btn_task(void)
{
    xTaskCreate(reset_btn_task, "reset_btn", 3072, NULL, 5, NULL);
}

#endif /* CONFIG_PROV_RESET_GPIO >= 0 */

#if CONFIG_PROV_LED_GPIO >= 0

#define ESTOP_LED_POLL_MS 300

/* 急停 LED 监控任务：轮询急停综合状态，触发时红灯闪烁（覆盖网络状态色），
 * 解除锁存后由 rgb_led 自动恢复网络状态色（联网=绿 / 未联网=橙）。 */
static void estop_led_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ESTOP_LED_POLL_MS));
        rgb_led_set_estop(mb_device_estop_active());
    }
}

static void start_estop_led_task(void)
{
    xTaskCreate(estop_led_task, "estop_led", 2048, NULL, 3, NULL);
}

#endif /* CONFIG_PROV_LED_GPIO >= 0 */

/* Web 服务器常驻运行：SoftAP 开启时可经 192.168.4.1 访问；
 * 热点关闭（ap_off）后仍可经路由器分配的 IP 访问，方便再次配网。
 * 服务器在 app_main 中启动一次，不随 AP 开关启停。 */
static void ensure_web_server(void)
{
    esp_err_t ret = web_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "web server start failed: %s", esp_err_to_name(ret));
    }
}

/* mDNS：局域网内可通过 http://esp32c5.local 访问配网页面 */
static void init_mdns(void)
{
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mdns init failed: %s", esp_err_to_name(ret));
        return;
    }
    mdns_hostname_set("esp32c5");
    mdns_instance_name_set("ESP32-C5 Web Provision");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS ready: http://esp32c5.local");
}

/* UART1 接收回调：打印收到的数据（RTU 从站未启用时的调试用） */
#if !CONFIG_MB_RTU_ENABLED
static void uart_rx_log_cb(const uint8_t *data, size_t len)
{
    /* 文本内容直接打印；含控制字符时退化为十六进制 */
    bool printable = true;
    for (size_t i = 0; i < len; i++) {
        if (data[i] < 0x20 || data[i] > 0x7E) {
            printable = false;
            break;
        }
    }
    if (printable) {
        ESP_LOGI(TAG, "UART1 recv %uB: %.*s", len, (int)len, (const char *)data);
    } else {
        char hex[3 * 64 + 1] = {0};
        size_t show = len < 64 ? len : 64;
        for (size_t i = 0; i < show; i++) {
            snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", data[i]);
        }
        ESP_LOGI(TAG, "UART1 recv %uB: %s%s", len, hex, len > show ? "..." : "");
    }
}
#endif /* !CONFIG_MB_RTU_ENABLED */

/* UART1 每秒发送测试：周期发出测试字符串（验证发送通道，默认关闭） */
#if CONFIG_UART_TEST_SEND_EN
static void uart_send_test_task(void *arg)
{
    static const char msg[] = "xieliang takes test！ \r\n";
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uart_io_send((const uint8_t *)msg, sizeof(msg) - 1);
    }
}
#endif /* CONFIG_UART_TEST_SEND_EN */

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C5 web provisioning firmware starting");

    esp_err_t ret = config_store_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = wifi_mgr_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Modbus 从站必须在 wifi_mgr_init（esp_netif_init/lwIP）之后启动，
     * 否则 socket() 会因 lwIP 未初始化而断言崩溃 */
    ret = mb_device_init();     /* 初始化 DI/DO GPIO 与寄存器模型 */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mb device init failed: %s", esp_err_to_name(ret));
    }
    modbus_gw_init();

    ret = uart_io_init();       /* 通用串口 UART1（TX=GPIO4 / RX=GPIO5） */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_io init failed: %s", esp_err_to_name(ret));
    } else {
#if CONFIG_MB_RTU_ENABLED
        modbus_rtu_init();      /* UART1 上的 Modbus RTU 从站（接管接收） */
#else
        uart_io_set_rx_cb(uart_rx_log_cb);   /* 调试：接收数据打印到控制台 */
#endif
#if CONFIG_UART_TEST_SEND_EN
        xTaskCreate(uart_send_test_task, "uart_send_test", 2048, NULL, 3, NULL);
        ESP_LOGI(TAG, "UART1 send test started: every 1s -> geekplus");
#endif
    }

#if CONFIG_PROV_LED_GPIO >= 0
    rgb_led_init();
    rgb_led_set_state(RGB_STATE_DEFAULT);   /* 初始橙色 */
#endif

#if CONFIG_PROV_LED_GPIO >= 0
    start_estop_led_task();   /* 急停触发 -> 红灯闪烁，解除后恢复网络状态色 */
#endif

    /* Web 服务器常驻：SoftAP 开启时可经 192.168.4.1 访问，
     * 热点关闭（ap_off）后仍可经路由器分配的 IP 访问，方便再次配网。 */
    ensure_web_server();
    init_mdns();

#if CONFIG_PROV_RESET_GPIO >= 0
    start_reset_btn_task();
#endif

    wifi_mgr_start();   /* 有配置 -> 连接；无配置 -> SoftAP 配网 */
}
