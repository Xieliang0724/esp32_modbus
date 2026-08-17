/*
 * rgb_led.c - RGB 状态灯实现（espressif/led_strip 托管组件, RMT 驱动）
 *            急停状态（RGB_STATE_ESTOP）红色闪烁，其余状态常亮。
 */
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "rgb_led.h"

static const char *TAG = "rgb_led";

#define ESTOP_BLINK_MS 300   /* 急停红色闪烁周期（亮/灭各 300ms） */

static led_strip_handle_t s_strip = NULL;
static esp_timer_handle_t s_blink_timer = NULL;
static bool s_blink_on = false;
static rgb_state_t s_current = RGB_STATE_DEFAULT;

static void set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

/* 急停闪烁回调：红/灭交替 */
static void blink_cb(void *arg)
{
    (void)arg;
    s_blink_on = !s_blink_on;
    if (s_blink_on) {
        set_rgb(255, 0, 0);          /* 红 */
    } else {
        set_rgb(0, 0, 0);            /* 灭 */
    }
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
    if (!s_strip || st == s_current) {
        return;
    }

    /* 退出急停闪烁态时停用定时器 */
    if (s_blink_timer && esp_timer_is_active(s_blink_timer)) {
        esp_timer_stop(s_blink_timer);
    }

    if (st == RGB_STATE_ESTOP) {
        s_current = st;
        s_blink_on = false;
        esp_timer_start_periodic(s_blink_timer, ESTOP_BLINK_MS * 1000);
        ESP_LOGI(TAG, "LED state -> ESTOP (blink red)");
        return;
    }

    s_current = st;
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
