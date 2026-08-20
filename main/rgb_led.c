/*
 * rgb_led.c - RGB 状态灯实现（espressif/led_strip 托管组件, RMT 驱动）
 *            急停状态（RGB_STATE_ESTOP）红色闪烁，优先级高于网络状态色。
 */
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "rgb_led.h"

static const char *TAG = "rgb_led";

#define ESTOP_BLINK_MS 300   /* 急停红色闪烁周期（亮/灭各 300ms） */

static led_strip_handle_t s_strip = NULL;
static esp_timer_handle_t s_blink_timer = NULL;
static bool s_blink_on = false;
static bool s_estop_active = false;        /* 急停是否激活 */
static rgb_state_t s_applied = (rgb_state_t)0xFF;   /* 哨兵：未应用任何灯效，保证首次 apply 执行 */
static rgb_state_t s_base_state = RGB_STATE_DEFAULT;   /* wifi 最后请求的网络色 */
static SemaphoreHandle_t s_rmt_mutex = NULL;   /* 保护 led_strip_refresh 不被并发调用 */

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip || !s_rmt_mutex) {
        return;
    }
    /* RMT 通道同一时刻只能有一次传输：mutex 串行化 blink_cb 与
     * set_estop(false) 路径的 set_rgb，避免 RMT "channel not in init state"。
     * 短暂等待：blink_cb 持锁时间约几 ms（一次 WS2812 编码 + 发送）。 */
    if (xSemaphoreTake(s_rmt_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;   /* 拿不到锁说明 RMT 正忙且超时，丢弃这次刷新 */
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
    xSemaphoreGive(s_rmt_mutex);
}

/* 急停闪烁回调：红/灭交替 */
static void blink_cb(void *arg)
{
    (void)arg;
    if (!s_estop_active) {
        return;   /* timer stop 异步生效期间的尾事件，已 ESTOP 解除 */
    }
    s_blink_on = !s_blink_on;
    if (s_blink_on) {
        set_rgb(255, 0, 0);          /* 红 */
    } else {
        set_rgb(0, 0, 0);            /* 灭 */
    }
}

/* 停止闪烁（若在运行） */
static void blink_stop(void)
{
    if (s_blink_timer && esp_timer_is_active(s_blink_timer)) {
        esp_timer_stop(s_blink_timer);
    }
}

/* 应用一种灯效：ESTOP 启动闪烁，其他颜色常亮。
 * 关键：先无条件 blink_stop，再切灯，最后如需 ESTOP 再 restart timer。
 * RMT 传输由 s_rmt_mutex 串行化，避免 blink_cb 与 set_rgb 并发导致
 * "channel not in init state" 错误（错误日志：rmt_tx_enable failed）。 */
static void apply_state(rgb_state_t st)
{
    if (st == RGB_STATE_ESTOP) {
        blink_stop();
        s_blink_on = false;
        s_applied = st;
        esp_timer_start_periodic(s_blink_timer, ESTOP_BLINK_MS * 1000);
        ESP_LOGI(TAG, "LED state -> ESTOP (blink red)");
        return;
    }

    if (st == s_applied) {
        return;
    }
    blink_stop();
    s_blink_on = false;
    s_applied = st;

    uint8_t r = 0, g = 0, b = 0;
    switch (st) {
    case RGB_STATE_DEFAULT:      /* 橙色 */
        r = 255; g = 100; b = 0;
        break;
    case RGB_STATE_AP_CLIENT:    /* 蓝色 */
        r = 0;   g = 0;   b = 255;
        break;
    case RGB_STATE_CONNECTED:    /* 绿色 */
        r = 0;   g = 255; b = 0;
        break;
    default:
        return;
    }
    set_rgb(r, g, b);
    ESP_LOGI(TAG, "LED state -> %s (%u,%u,%u)",
             st == RGB_STATE_DEFAULT ? "ORANGE" :
             st == RGB_STATE_AP_CLIENT ? "BLUE" : "GREEN",
             r, g, b);
}

void rgb_led_init(void)
{
    /* 灯珠配置 */
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_PROV_LED_GPIO,
        .max_leds = 1,                              /* 单颗灯珠 */
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    /* RMT 驱动配置 */
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,          /* 10 MHz, 1 tick = 0.1us */
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);

    /* RMT 互斥锁：保护 led_strip_refresh 并发访问 */
    if (!s_rmt_mutex) {
        s_rmt_mutex = xSemaphoreCreateMutex();
    }

    /* 急停闪烁定时器（懒创建，仅 ESTOP 状态启用） */
    esp_timer_create_args_t targs = {
        .callback = blink_cb,
        .name = "rgb_blink",
    };
    esp_timer_create(&targs, &s_blink_timer);
    ESP_LOGI(TAG, "RGB LED init on GPIO%d", CONFIG_PROV_LED_GPIO);
}

void rgb_led_set_state(rgb_state_t st)
{
    if (st == RGB_STATE_ESTOP) {
        return;   /* 网络模块不应直接请求急停色 */
    }
    s_base_state = st;                    /* 记住网络色，急停解除后恢复 */
    if (s_estop_active) {
        return;                           /* 急停激活：忽略覆盖 */
    }
    apply_state(st);
}

void rgb_led_set_estop(bool active)
{
    if (active == s_estop_active) {
        return;   /* 状态未变，不刷 */
    }
    s_estop_active = active;
    if (active) {
        apply_state(RGB_STATE_ESTOP);
    } else {
        /* apply_state 内部已统一 stop timer + 立即刷目标色，
         * 消除 blink_cb tail event 与 set_estop 的 race */
        apply_state(s_base_state);
        ESP_LOGI(TAG, "ESTOP LED cleared, restore base state");
    }
}
