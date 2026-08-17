/*
 * rgb_led.h - 板载 RGB 状态灯（WS2812 可寻址灯珠, GPIO27）
 *
 * 状态颜色：
 *   RGB_STATE_DEFAULT   橙色 - AP/STA 均未连接（默认/配网待连接）
 *   RGB_STATE_AP_CLIENT 蓝色 - 有设备连上了 SoftAP（正在配网）
 *   RGB_STATE_CONNECTED 绿色 - STA 联网成功（已连上路由器）
 *   RGB_STATE_ESTOP     红色闪烁 - 急停触发（覆盖其他状态，解除后恢复）
 *
 * 优先级：急停（RGB_STATE_ESTOP）> 网络状态色。
 * wifi_mgr 等模块照常调用 rgb_led_set_state()，急停激活时这些调用
 * 被内部忽略（不覆盖红闪）；急停解除后自动恢复最后一次请求的网络色。
 */
#pragma once

typedef enum {
    RGB_STATE_DEFAULT = 0,   /* 橙色 */
    RGB_STATE_AP_CLIENT,     /* 蓝色 */
    RGB_STATE_CONNECTED,     /* 绿色 */
    RGB_STATE_ESTOP,         /* 红色闪烁（急停） */
} rgb_state_t;

/* 初始化 LED 驱动（WS2812 @ CONFIG_PROV_LED_GPIO） */
void rgb_led_init(void);

/* 设置网络状态色（wifi_mgr 等调用）；急停激活时被忽略 */
void rgb_led_set_state(rgb_state_t st);

/* 设置急停状态：true=红灯闪烁（覆盖网络色），false=恢复网络色。
 * 幂等，可周期性调用。 */
void rgb_led_set_estop(bool active);
