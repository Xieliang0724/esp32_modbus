/*
 * mb_device.c - Modbus TCP 从站设备模型实现
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"

#include "wifi_mgr.h"
#include "mb_device.h"

static const char *TAG = "mb_dev";

/* ------------------------------------------------------------------ */
/* GPIO 引脚表（menuconfig 配置，-1 = 未使用）                          */
/* ------------------------------------------------------------------ */

static const int8_t s_di_gpios[MB_MAX_DI] = {
    CONFIG_MB_DI_GPIO_0,
    CONFIG_MB_DI_GPIO_1,
    CONFIG_MB_DI_GPIO_2,
    CONFIG_MB_DI_GPIO_3,
};

static const int8_t s_do_gpios[MB_MAX_DO] = {
    CONFIG_MB_DO_GPIO_0,
    CONFIG_MB_DO_GPIO_1,
    CONFIG_MB_DO_GPIO_2,
    CONFIG_MB_DO_GPIO_3,
};

/* 急停（2 路常闭 NC）与复位按钮（常开 NO）——建议触点一端接 GND，
 * 内部上拉 + 软件取反：急停正常闭合=1 / 按下=0，复位按下=1。 */
#define MB_ESTOP_COUNT 2
static const int8_t s_estop_gpios[MB_ESTOP_COUNT] = {
    CONFIG_MB_ESTOP1_GPIO,
    CONFIG_MB_ESTOP2_GPIO,
};
static const int8_t s_reset_gpio = CONFIG_MB_RESET_BTN_GPIO;

/* ------------------------------------------------------------------ */
/* 寄存器数据模型                                                       */
/* ------------------------------------------------------------------ */

static bool s_coils[MB_MAX_DO];        /* DO 影子状态（同时驱动 GPIO） */
static uint16_t s_hold[MB_MAX_HOLD];   /* 保持寄存器（RAM，掉电清零）   */
static SemaphoreHandle_t s_reg_mutex;  /* 线圈/保持寄存器访问互斥       */

/* 急停锁存状态：true = 曾触发急停（寄存器 0），需复位按钮解除 */
static bool s_estop_latched[MB_ESTOP_COUNT];
static bool s_reset_prev = false;      /* 复位按钮上次电平（上升沿检测） */

/* 输入寄存器（只读）：0x3000 起 */
#define INREG_ESTOP_STATE 0   /* 设备急停综合状态：1=正常，0=触发（锁存） */
#define INREG_ESTOP1_RAW  1   /* 急停1 实时触点：1=闭合正常，0=按下/断开 */
#define INREG_ESTOP2_RAW  2   /* 急停2 实时触点 */
#define INREG_RESET_RAW   3   /* 复位按钮实时：1=按下 */
#define INREG_FW_VERSION  4   /* 固件版本 major<<8|minor */
#define INREG_WIFI_STATE  5   /* STA 状态 0=未连接 1=已连接 */
#define INREG_UPTIME_LO   6   /* 运行秒数低 16 位 */
#define INREG_UPTIME_HI   7   /* 运行秒数高 16 位 */

/* ------------------------------------------------------------------ */
/* 工具函数                                                             */
/* ------------------------------------------------------------------ */

static bool gpio_valid(int8_t gpio)
{
    return gpio >= 0 && gpio <= 28;
}

static void di_read_bit(uint8_t idx, bool *out)
{
    if (idx >= MB_MAX_DI) {
        *out = false;
        return;
    }
    if (!gpio_valid(s_di_gpios[idx])) {
        *out = false;
        return;
    }
    *out = (gpio_get_level(s_di_gpios[idx]) != 0);
}

/* 读取急停触点原始逻辑电平：触点闭合(正常)=true，按下断开=false。
 * 接线约定：NC 触点一端接 GND，GPIO 内部上拉 → 电平=0 为闭合，
 * 按下断开后上拉为 1，故取反即为触点状态。 */
static bool estop_contact_ok(uint8_t idx)
{
    if (idx >= MB_ESTOP_COUNT || !gpio_valid(s_estop_gpios[idx])) {
        return true;   /* 未配置/禁用：视为正常 */
    }
    return (gpio_get_level(s_estop_gpios[idx]) == 0);
}

/* 复位按钮按下状态（按下=true）。接线约定：NO 触点一端接 GND，
 * 内部上拉 → 电平=0 为按下。 */
static bool reset_btn_pressed(void)
{
    if (!gpio_valid(s_reset_gpio)) {
        return false;
    }
    return (gpio_get_level(s_reset_gpio) == 0);
}

/* 更新急停锁存（在读取 DI4/DI5、输入寄存器 0x3000 时调用）：
 *   - 急停按下（触点断开）→ 立即锁存（寄存器 0）
 *   - 急停松开后，检测到复位按钮上升沿 → 解除锁存（寄存器 1）
 * 复位沿为全局事件：一次按下同时解除所有已松开急停的锁存。
 * 返回综合状态：true = 任一路急停锁存中（触发）。 */
static bool estop_latch_update(void)
{
    bool active = false;
    xSemaphoreTake(s_reg_mutex, portMAX_DELAY);
    bool reset = reset_btn_pressed();
    bool reset_edge = reset && !s_reset_prev;
    s_reset_prev = reset;

    for (int i = 0; i < MB_ESTOP_COUNT; i++) {
        if (!estop_contact_ok((uint8_t)i)) {
            s_estop_latched[i] = true;             /* 急停触发，锁存 */
        } else if (reset_edge) {
            s_estop_latched[i] = false;            /* 松开 + 复位按下，解除 */
        }
        active |= s_estop_latched[i];
    }
    xSemaphoreGive(s_reg_mutex);
    return active;
}

/* 对外：设备急停综合状态（含锁存更新），供 LED 等模块轮询 */
bool mb_device_estop_active(void)
{
    return estop_latch_update();
}

/* 离散输入读取：DI0-3 通用 / DI4-5 急停（实时触点，无锁存） / DI6 复位按钮 / DI7 恒 0 */
static void di_read_full(uint8_t idx, bool *out)
{
    if (idx < 4) {
        di_read_bit(idx, out);
        return;
    }
    switch (idx) {
    case MB_DI_ESTOP1:
    case MB_DI_ESTOP2:
        /* 实时 IO 状态：1=触点闭合（正常），0=按下/断开，随按键实时变化 */
        *out = estop_contact_ok(idx - MB_DI_ESTOP1);
        break;
    case MB_DI_RESET_BTN:
        *out = reset_btn_pressed();
        break;
    default:
        *out = false;
        break;
    }
}

static void do_write_bit(uint8_t idx, bool on)
{
    if (!gpio_valid(s_do_gpios[idx])) {
        return;
    }
    gpio_set_level(s_do_gpios[idx], on ? 1 : 0);
}

/* 读取固件版本号（从 PROJECT_VER 解析 "vX.Y.Z..." → (X<<8)|Y） */
static uint16_t fw_version_reg(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    const char *v = desc ? desc->version : "";
    if (*v == 'v') {
        v++;
    }
    int major = 0, minor = 0;
    if (sscanf(v, "%d.%d", &major, &minor) == 2) {
        return (uint16_t)((major << 8) | (minor & 0xFF));
    }
    return 0;
}

static void inreg_read(uint8_t idx, uint16_t *out)
{
    switch (idx) {
    case INREG_ESTOP_STATE:
        /* 设备急停综合状态：1=正常，0=触发（锁存，需复位解除） */
        *out = estop_latch_update() ? 0 : 1;
        break;
    case INREG_ESTOP1_RAW:
        *out = estop_contact_ok(0) ? 1 : 0;
        break;
    case INREG_ESTOP2_RAW:
        *out = estop_contact_ok(1) ? 1 : 0;
        break;
    case INREG_RESET_RAW:
        *out = reset_btn_pressed() ? 1 : 0;
        break;
    case INREG_FW_VERSION:
        *out = fw_version_reg();
        break;
    case INREG_WIFI_STATE:
        *out = (wifi_mgr_get_state() == WIFI_MGR_STATE_CONNECTED) ? 1 : 0;
        break;
    case INREG_UPTIME_LO:
    case INREG_UPTIME_HI: {
        uint32_t secs = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        *out = (idx == INREG_UPTIME_LO) ? (secs & 0xFFFF) : ((secs >> 16) & 0xFFFF);
        break;
    }
    default:
        *out = 0;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 异常响应                                                             */
/* ------------------------------------------------------------------ */

#define MB_EX_ILLEGAL_FUNCTION 0x01
#define MB_EX_ILLEGAL_ADDRESS  0x02
#define MB_EX_ILLEGAL_VALUE    0x03

static void exception(uint8_t fc, uint8_t code, uint8_t *resp, size_t *resp_len)
{
    resp[0] = fc | 0x80;
    resp[1] = code;
    *resp_len = 2;
}

/* ------------------------------------------------------------------ */
/* 功能码处理                                                           */
/* ------------------------------------------------------------------ */

/* FC01/02：读线圈 / 读离散输入（连续 bit 打包，region_size 为该区域数量） */
static void read_bits(uint8_t fc, uint16_t base_idx, uint16_t region_size,
                      const uint8_t *pdu, size_t pdu_len,
                      uint8_t *resp, size_t *resp_len)
{
    if (pdu_len < 5) {
        exception(fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_len);
        return;
    }
    uint16_t addr = (uint16_t)((pdu[1] << 8) | pdu[2]);
    uint16_t qty  = (uint16_t)((pdu[3] << 8) | pdu[4]);
    if (qty < 1 || qty > 2000 || addr < base_idx || addr + qty > base_idx + region_size) {
        exception(fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_len);
        return;
    }
    uint16_t off = addr - base_idx;
    uint8_t byte_cnt = (uint8_t)((qty + 7) / 8);
    resp[0] = fc;
    resp[1] = byte_cnt;
    memset(resp + 2, 0, byte_cnt);
    for (uint16_t i = 0; i < qty; i++) {
        bool val = false;
        if (fc == 0x01) {
            xSemaphoreTake(s_reg_mutex, portMAX_DELAY);
            val = s_coils[off + i];
            xSemaphoreGive(s_reg_mutex);
        } else {
            di_read_full((uint8_t)(off + i), &val);
        }
        if (val) {
            resp[2 + i / 8] |= (uint8_t)(1 << (i % 8));
        }
    }
    *resp_len = (size_t)2 + byte_cnt;
}

/* FC03/04：读保持寄存器 / 读输入寄存器 */
static void read_words(uint8_t fc, uint16_t base_idx, const uint8_t *pdu,
                       size_t pdu_len, uint8_t *resp, size_t *resp_len)
{
    if (pdu_len < 5) {
        exception(fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_len);
        return;
    }
    uint16_t addr = (uint16_t)((pdu[1] << 8) | pdu[2]);
    uint16_t qty  = (uint16_t)((pdu[3] << 8) | pdu[4]);
    uint16_t total = (fc == 0x03) ? MB_MAX_HOLD : MB_MAX_INREG;
    if (qty < 1 || qty > 125 || addr < base_idx || addr + qty > base_idx + total) {
        exception(fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_len);
        return;
    }
    uint16_t off = addr - base_idx;
    resp[0] = fc;
    resp[1] = (uint8_t)(qty * 2);
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t val = 0;
        if (fc == 0x03) {
            xSemaphoreTake(s_reg_mutex, portMAX_DELAY);
            val = s_hold[off + i];
            xSemaphoreGive(s_reg_mutex);
        } else {
            inreg_read((uint8_t)(off + i), &val);
        }
        resp[2 + i * 2] = (uint8_t)(val >> 8);
        resp[3 + i * 2] = (uint8_t)(val & 0xFF);
    }
    *resp_len = (size_t)2 + qty * 2;
}

/* FC05：写单个线圈（0xFF00=ON，0x0000=OFF） */
static void write_single_coil(uint8_t fc, const uint8_t *pdu, size_t pdu_len,
                              uint8_t *resp, size_t *resp_len)
{
    if (pdu_len < 5) {
        exception(fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_len);
        return;
    }
    uint16_t addr = (uint16_t)((pdu[1] << 8) | pdu[2]);
    uint16_t val  = (uint16_t)((pdu[3] << 8) | pdu[4]);
    if ((uint16_t)(addr - MB_COIL_BASE) >= MB_MAX_DO) {
        exception(fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_len);
        return;
    }
    if (val != 0x0000 && val != 0xFF00) {
        exception(fc, MB_EX_ILLEGAL_VALUE, resp, resp_len);
        return;
    }
    uint8_t idx = (uint8_t)(addr - MB_COIL_BASE);
    bool on = (val == 0xFF00);
    xSemaphoreTake(s_reg_mutex, portMAX_DELAY);
    s_coils[idx] = on;
    xSemaphoreGive(s_reg_mutex);
    do_write_bit(idx, on);
    /* 回显请求 */
    memcpy(resp, pdu, 5);
    *resp_len = 5;
}

/* FC06：写单个保持寄存器 */
static void write_single_reg(uint8_t fc, const uint8_t *pdu, size_t pdu_len,
                             uint8_t *resp, size_t *resp_len)
{
    if (pdu_len < 5) {
        exception(fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_len);
        return;
    }
    uint16_t addr = (uint16_t)((pdu[1] << 8) | pdu[2]);
    uint16_t val  = (uint16_t)((pdu[3] << 8) | pdu[4]);
    if (addr < MB_HOLDREG_BASE || addr >= MB_HOLDREG_BASE + MB_MAX_HOLD) {
        exception(fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_len);
        return;
    }
    uint8_t idx = (uint8_t)(addr - MB_HOLDREG_BASE);
    xSemaphoreTake(s_reg_mutex, portMAX_DELAY);
    s_hold[idx] = val;
    xSemaphoreGive(s_reg_mutex);
    /* 回显请求 */
    memcpy(resp, pdu, 5);
    *resp_len = 5;
}

/* FC0F：写多个线圈 */
static void write_multi_coils(uint8_t fc, const uint8_t *pdu, size_t pdu_len,
                              uint8_t *resp, size_t *resp_len)
{
    if (pdu_len < 6) {
        exception(fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_len);
        return;
    }
    uint16_t addr = (uint16_t)((pdu[1] << 8) | pdu[2]);
    uint16_t qty  = (uint16_t)((pdu[3] << 8) | pdu[4]);
    uint8_t byte_cnt = pdu[5];
    if (qty < 1 || qty > 2000 || addr + qty > MB_COIL_BASE + MB_MAX_DO) {
        exception(fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_len);
        return;
    }
    if (byte_cnt != (qty + 7) / 8 || (size_t)6 + byte_cnt > pdu_len) {
        exception(fc, MB_EX_ILLEGAL_VALUE, resp, resp_len);
        return;
    }
    uint16_t off = addr - MB_COIL_BASE;
    xSemaphoreTake(s_reg_mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < qty; i++) {
        bool on = (pdu[6 + i / 8] >> (i % 8)) & 1;
        s_coils[off + i] = on;
        do_write_bit((uint8_t)(off + i), on);
    }
    xSemaphoreGive(s_reg_mutex);
    /* 响应：[0F][addr][qty] */
    resp[0] = fc;
    resp[1] = pdu[1];
    resp[2] = pdu[2];
    resp[3] = pdu[3];
    resp[4] = pdu[4];
    *resp_len = 5;
}

/* FC10：写多个保持寄存器 */
static void write_multi_regs(uint8_t fc, const uint8_t *pdu, size_t pdu_len,
                             uint8_t *resp, size_t *resp_len)
{
    if (pdu_len < 6) {
        exception(fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_len);
        return;
    }
    uint16_t addr = (uint16_t)((pdu[1] << 8) | pdu[2]);
    uint16_t qty  = (uint16_t)((pdu[3] << 8) | pdu[4]);
    uint8_t byte_cnt = pdu[5];
    if (qty < 1 || qty > 125 || addr < MB_HOLDREG_BASE ||
        addr + qty > MB_HOLDREG_BASE + MB_MAX_HOLD) {
        exception(fc, MB_EX_ILLEGAL_ADDRESS, resp, resp_len);
        return;
    }
    if (byte_cnt != qty * 2 || (size_t)6 + byte_cnt > pdu_len) {
        exception(fc, MB_EX_ILLEGAL_VALUE, resp, resp_len);
        return;
    }
    uint16_t off = addr - MB_HOLDREG_BASE;
    xSemaphoreTake(s_reg_mutex, portMAX_DELAY);
    for (uint16_t i = 0; i < qty; i++) {
        s_hold[off + i] = (uint16_t)((pdu[6 + i * 2] << 8) | pdu[7 + i * 2]);
    }
    xSemaphoreGive(s_reg_mutex);
    /* 响应：[10][addr][qty] */
    resp[0] = fc;
    resp[1] = pdu[1];
    resp[2] = pdu[2];
    resp[3] = pdu[3];
    resp[4] = pdu[4];
    *resp_len = 5;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                             */
/* ------------------------------------------------------------------ */

esp_err_t mb_device_init(void)
{
    if (!s_reg_mutex) {
        s_reg_mutex = xSemaphoreCreateMutex();
        if (!s_reg_mutex) {
            ESP_LOGE(TAG, "reg mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    for (int i = 0; i < MB_MAX_DI; i++) {
        if (!gpio_valid(s_di_gpios[i])) {
            continue;
        }
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_di_gpios[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,   /* 干接点默认接 GND，内部上拉 */
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
    }
    for (int i = 0; i < MB_ESTOP_COUNT; i++) {
        if (!gpio_valid(s_estop_gpios[i])) {
            continue;
        }
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_estop_gpios[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,   /* NC 触点接 GND：闭合=低，断开=高 */
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
    }
    if (gpio_valid(s_reset_gpio)) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_reset_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,   /* NO 触点接 GND：松开=高，按下=低 */
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
    }
    for (int i = 0; i < MB_MAX_DO; i++) {
        s_coils[i] = false;
        if (!gpio_valid(s_do_gpios[i])) {
            continue;
        }
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_do_gpios[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        gpio_set_level(s_do_gpios[i], 0);   /* 上电默认全部断开 */
    }
    memset(s_hold, 0, sizeof(s_hold));
    memset(s_estop_latched, 0, sizeof(s_estop_latched));
    s_reset_prev = false;

    ESP_LOGI(TAG, "slave model init: DI[%d,%d,%d,%d] DO[%d,%d,%d,%d] ESTOP[%d,%d] RESET=%d",
             s_di_gpios[0], s_di_gpios[1], s_di_gpios[2], s_di_gpios[3],
             s_do_gpios[0], s_do_gpios[1], s_do_gpios[2], s_do_gpios[3],
             s_estop_gpios[0], s_estop_gpios[1], s_reset_gpio);
    return ESP_OK;
}

void mb_device_handle_pdu(uint8_t uid, const uint8_t *pdu, size_t pdu_len,
                          uint8_t *resp, size_t *resp_len)
{
    (void)uid;
    *resp_len = 0;
    if (pdu_len < 1) {
        return;
    }
    uint8_t fc = pdu[0];
    switch (fc) {
    case 0x01: read_bits(fc, MB_COIL_BASE, MB_MAX_DO, pdu, pdu_len, resp, resp_len); break;
    case 0x02: read_bits(fc, MB_DI_BASE, MB_MAX_DI, pdu, pdu_len, resp, resp_len); break;
    case 0x03: read_words(fc, MB_HOLDREG_BASE, pdu, pdu_len, resp, resp_len); break;
    case 0x04: read_words(fc, MB_INREG_BASE,   pdu, pdu_len, resp, resp_len); break;
    case 0x05: write_single_coil(fc, pdu, pdu_len, resp, resp_len); break;
    case 0x06: write_single_reg(fc, pdu, pdu_len, resp, resp_len); break;
    case 0x0F: write_multi_coils(fc, pdu, pdu_len, resp, resp_len); break;
    case 0x10: write_multi_regs(fc, pdu, pdu_len, resp, resp_len); break;
    default:
        exception(fc, MB_EX_ILLEGAL_FUNCTION, resp, resp_len);
        break;
    }
}
