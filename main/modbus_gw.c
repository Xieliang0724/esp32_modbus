/*
 * modbus_gw.c - Modbus TCP 从站（Server）实现
 *
 * ESP32 本体作为 Modbus TCP 从站，寄存器直接映射到本机外设
 * （见 mb_device.h 的寄存器映射表），不再通过 UART 转发 RTU。
 *
 *   - 监听本地端口（默认 502 明文 + 可选 802 TLS）
 *   - MBAP 解析/组帧、事务 ID 回填
 *   - 客户端 IP 白名单（空 = 无限制）
 *   - 端口、TLS、白名单均可通过配网页面配置
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"

#include "modbus_gw.h"
#include "mb_device.h"

static const char *TAG = "mb_gw";

#define MAX_FRAME_SIZE     260          /* 最大帧：7 MBAP + 253 PDU */
#define MAX_TCP_CLIENTS    4
#define MAX_LISTENERS      2            /* 明文 + TLS 两个监听 */

static gw_config_t s_cfg;
static bool s_running = false;

static SemaphoreHandle_t s_slot_mutex = NULL;  /* 客户端槽位分配互斥 */

static TaskHandle_t s_listener_tasks[MAX_LISTENERS] = {0};
static TaskHandle_t s_client_tasks[MAX_TCP_CLIENTS] = {0};
static int s_client_fds[MAX_TCP_CLIENTS] = {-1, -1, -1, -1};
static int s_listen_fds[MAX_LISTENERS] = {-1, -1};

/* ------------------------------------------------------------------ */
/* TLS：固件内置服务器证书（单向 TLS，验证设备身份）                     */
/* ------------------------------------------------------------------ */

extern const uint8_t server_cert_pem_start[] asm("_binary_server_cert_pem_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_server_cert_pem_end");
extern const uint8_t server_key_pem_start[]  asm("_binary_server_key_pem_start");
extern const uint8_t server_key_pem_end[]    asm("_binary_server_key_pem_end");

static mbedtls_x509_crt s_srv_cert;
static mbedtls_pk_context s_srv_key;
static bool s_tls_ready = false;

static void tls_init(void)
{
    mbedtls_x509_crt_init(&s_srv_cert);
    mbedtls_pk_init(&s_srv_key);

    /* EMBED_FILES 字节数组无 null 结尾，而 mbedTLS 的 PEM 解析要求
     * 缓冲区以 '\0' 结尾，故先拷贝到带结尾符的缓冲区再解析。 */
    size_t cert_len = server_cert_pem_end - server_cert_pem_start;
    size_t key_len = server_key_pem_end - server_key_pem_start;
    uint8_t *buf = malloc((cert_len > key_len ? cert_len : key_len) + 1);
    if (!buf) {
        ESP_LOGE(TAG, "tls buf malloc failed");
        return;
    }

    int ret = 0;
    memcpy(buf, server_cert_pem_start, cert_len);
    buf[cert_len] = '\0';
    ret = mbedtls_x509_crt_parse(&s_srv_cert, buf, cert_len + 1);
    if (ret != 0) {
        ESP_LOGE(TAG, "cert parse failed: -0x%04X", -ret);
        free(buf);
        return;
    }
    memcpy(buf, server_key_pem_start, key_len);
    buf[key_len] = '\0';
    ret = mbedtls_pk_parse_key(&s_srv_key, buf, key_len + 1, NULL, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "key parse failed: -0x%04X", -ret);
        free(buf);
        return;
    }
    free(buf);
    s_tls_ready = true;
    ESP_LOGI(TAG, "TLS server cert loaded");
}

/* TLS BIO 回调：包装 socket fd（阻塞式） */
static int tls_send_cb(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = (int)(intptr_t)ctx;
    return send(fd, buf, len, 0);
}

static int tls_recv_cb(void *ctx, unsigned char *buf, size_t len)
{
    int fd = (int)(intptr_t)ctx;
    return recv(fd, buf, len, 0);
}

/* 统一收发：plain 用 recv/send，TLS 用 mbedtls_ssl_read/write */
static int mb_net_recv(int fd, mbedtls_ssl_context *ssl, uint8_t *buf, size_t len)
{
    if (ssl) {
        return mbedtls_ssl_read(ssl, buf, len);
    }
    return recv(fd, buf, len, 0);
}

static int mb_net_send(int fd, mbedtls_ssl_context *ssl, const uint8_t *buf, size_t len)
{
    if (ssl) {
        return mbedtls_ssl_write(ssl, buf, len);
    }
    return send(fd, buf, len, 0);
}

/* ------------------------------------------------------------------ */
/* TCP 客户端任务：读 MBAP 请求 -> 本机寄存器 -> 回 MBAP 响应            */
/* ------------------------------------------------------------------ */

static void close_client(int idx)
{
    if (idx < 0 || idx >= MAX_TCP_CLIENTS) {
        return;
    }
    /* 用 mutex 做原子 CAS：先摘 fd 再置 -1，再释放 close，
     * 避免与 gw_stop / 其他路径出现 double close。 */
    int fd_to_close = -1;
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    if (s_client_fds[idx] >= 0) {
        fd_to_close = s_client_fds[idx];
        s_client_fds[idx] = -1;
        s_client_tasks[idx] = NULL;
    }
    xSemaphoreGive(s_slot_mutex);
    if (fd_to_close >= 0) {
        close(fd_to_close);
    }
}

static int find_free_client_slot(void)
{
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_client_fds[i] < 0) {
            return i;
        }
    }
    return -1;
}

/* 客户端任务参数 */
typedef struct {
    int  fd;
    bool tls;
} client_arg_t;

static void log_pdu_hex(const char *dir, uint8_t fc, const uint8_t *data, size_t len)
{
    /* 只打印首帧摘要，避免刷屏。hex 缓冲按 show_max*3 + 1 分配，
     * 防止 snprintf 在 i*3 接近 sizeof(hex) 时 size_t 下溢
     * 绕开长度检查导致栈益处（len≥28 即触发）。 */
    const size_t show_max = 28;
    char hex[show_max * 3 + 1];
    memset(hex, 0, sizeof(hex));
    size_t show = len < show_max ? len : show_max;
    for (size_t i = 0; i < show; i++) {
        snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", data[i]);
    }
    ESP_LOGD(TAG, "%s fc=0x%02X %uB: %s%s", dir, fc, len, hex, len > show ? "..." : "");
}

static void tcp_client_task(void *arg)
{
    client_arg_t *ca = (client_arg_t *)arg;
    int fd = ca->fd;
    bool tls = ca->tls;
    free(ca);

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config ssl_conf;
    mbedtls_ssl_context *ssl_p = NULL;

    if (tls) {
        if (!s_tls_ready) {
            ESP_LOGE(TAG, "TLS not ready, close client");
            close(fd);
            vTaskDelete(NULL);
            return;
        }
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&ssl_conf);
        int ret = mbedtls_ssl_config_defaults(&ssl_conf,
                                              MBEDTLS_SSL_IS_SERVER,
                                              MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret == 0) {
            mbedtls_ssl_conf_authmode(&ssl_conf, MBEDTLS_SSL_VERIFY_NONE);  /* 单向：不验证客户端 */
            mbedtls_ssl_conf_own_cert(&ssl_conf, &s_srv_cert, &s_srv_key);
            ret = mbedtls_ssl_setup(&ssl, &ssl_conf);
        }
        if (ret == 0) {
            mbedtls_ssl_set_bio(&ssl, (void *)(intptr_t)fd, tls_send_cb, tls_recv_cb, NULL);
            ret = mbedtls_ssl_handshake(&ssl);
        }
        if (ret != 0) {
            ESP_LOGW(TAG, "TLS handshake failed: -0x%04X", -ret);
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&ssl_conf);
            close(fd);
            vTaskDelete(NULL);
            return;
        }
        ssl_p = &ssl;
        ESP_LOGI(TAG, "TLS client handshake OK");
    }

    uint8_t mbap[7];
    uint8_t buf[MAX_FRAME_SIZE];

    while (s_running) {
        /* 读 MBAP 头（7 字节） */
        size_t got = 0;
        while (got < sizeof(mbap) && s_running) {
            int n = mb_net_recv(fd, ssl_p, mbap + got, sizeof(mbap) - got);
            if (n <= 0) {
                goto client_done;   /* EOF / 连接关闭 / stop */
            }
            got += n;
        }
        uint16_t tid = (mbap[0] << 8) | mbap[1];
        uint16_t len = (mbap[4] << 8) | mbap[5];
        if (len < 1 || len > MAX_FRAME_SIZE - 7) {
            ESP_LOGW(TAG, "bad MBAP len=%u", len);
            goto client_done;
        }
        /* MBAP: uid 已在 7 字节头中 (mbap[6])，len = uid(1) + PDU，
         * 因此这里只需再读 len-1 字节的 PDU */
        uint8_t uid = mbap[6];
        size_t pdu_len = len - 1;
        got = 0;
        while (got < pdu_len && s_running) {
            int n = mb_net_recv(fd, ssl_p, buf + got, pdu_len - got);
            if (n <= 0) {
                goto client_done;
            }
            got += n;
        }
        const uint8_t *pdu = buf;
        if (pdu_len > 0) {
            log_pdu_hex("REQ", pdu[0], pdu, pdu_len);
        }

        /* 本机寄存器响应（始终生成响应，含异常码） */
        uint8_t resp_pdu[MAX_FRAME_SIZE];
        size_t resp_pdu_len = 0;
        mb_device_handle_pdu(uid, pdu, pdu_len, resp_pdu, &resp_pdu_len);
        if (resp_pdu_len == 0 || !s_running) {
            continue;   /* 空 PDU：忽略，等待下一条 */
        }
        log_pdu_hex("RES", resp_pdu[0], resp_pdu, resp_pdu_len);

        /* 组 MBAP 响应：[tid][0x0000][len'=1+pdu_len][uid][pdu] */
        uint8_t rsp[7 + MAX_FRAME_SIZE];
        rsp[0] = (tid >> 8) & 0xFF;
        rsp[1] = tid & 0xFF;
        rsp[2] = 0;
        rsp[3] = 0;
        uint16_t rlen = 1 + resp_pdu_len;
        rsp[4] = (rlen >> 8) & 0xFF;
        rsp[5] = rlen & 0xFF;
        rsp[6] = uid;
        memcpy(rsp + 7, resp_pdu, resp_pdu_len);
        size_t total = 7 + resp_pdu_len;
        size_t sent = 0;
        while (sent < total && s_running) {
            int n = mb_net_send(fd, ssl_p, rsp + sent, total - sent);
            if (n <= 0) {
                goto client_done;
            }
            sent += n;
        }
    }

client_done:
    ESP_LOGI(TAG, "client disconnected");
    if (ssl_p) {
        mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&ssl_conf);
    }
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_client_fds[i] == fd) {
            close_client(i);
            break;
        }
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* TCP 监听任务                                                        */
/* ------------------------------------------------------------------ */

/* 监听任务参数 */
typedef struct {
    uint16_t port;
    bool     tls;
    int      idx;   /* 对应 s_listen_fds 槽位 */
} listener_arg_t;

static void tcp_listener_task(void *arg)
{
    listener_arg_t *la = (listener_arg_t *)arg;
    uint16_t port = la->port;
    bool tls = la->tls;
    int idx = la->idx;
    free(la);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        s_listen_fds[idx] = -1;
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind port %u failed", port);
        close(fd);
        s_listen_fds[idx] = -1;
        vTaskDelete(NULL);
        return;
    }
    listen(fd, 4);
    s_listen_fds[idx] = fd;
    ESP_LOGI(TAG, "Modbus TCP%s slave listening on port %u", tls ? " (TLS)" : "", port);

    while (s_running) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client = accept(fd, (struct sockaddr *)&peer, &peer_len);
        if (client < 0) {
            if (!s_running) {
                break;
            }
            continue;
        }

        /* 客户端 IP 白名单 */
        if (s_cfg.client_ip[0] != '\0') {
            char peer_ip[16];
            strlcpy(peer_ip, inet_ntoa(peer.sin_addr), sizeof(peer_ip));
            if (strcmp(peer_ip, s_cfg.client_ip) != 0) {
                ESP_LOGW(TAG, "reject client %s (allowlist %s)", peer_ip, s_cfg.client_ip);
                close(client);
                continue;
            }
            ESP_LOGI(TAG, "client %s allowed", peer_ip);
        } else {
            ESP_LOGI(TAG, "client connected (no allowlist)");
        }

        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        int slot = find_free_client_slot();
        if (slot >= 0) {
            s_client_fds[slot] = client;
        }
        xSemaphoreGive(s_slot_mutex);
        if (slot < 0) {
            ESP_LOGW(TAG, "too many clients, reject");
            close(client);
            continue;
        }
        client_arg_t *ca = malloc(sizeof(client_arg_t));
        if (!ca) {
            /* 归还槽位，避免泄漏 */
            xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
            s_client_fds[slot] = -1;
            s_client_tasks[slot] = NULL;
            xSemaphoreGive(s_slot_mutex);
            ESP_LOGE(TAG, "client arg malloc failed");
            close(client);
            continue;
        }
        ca->fd = client;
        ca->tls = tls;
        if (xTaskCreate(tcp_client_task, "mb_tcp_client", 8192, ca, 6,
                        &s_client_tasks[slot]) != pdPASS) {
            /* 任务创建失败，归还槽位 */
            xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
            s_client_fds[slot] = -1;
            s_client_tasks[slot] = NULL;
            xSemaphoreGive(s_slot_mutex);
            ESP_LOGE(TAG, "client task create failed");
            free(ca);
            close(client);
            continue;
        }
    }

    s_listen_fds[idx] = -1;
    close(fd);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

static void gw_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listen_fds[i] >= 0) {
            close(s_listen_fds[i]);
            s_listen_fds[i] = -1;
        }
    }
    /* 客户端 socket 用 shutdown 触发 recv 立即返回，close 由 client task
     * 自己在 client_done 里 close_client(i) 完成，避免与主任务 double close。
     * s_slot_mutex 保证读 s_client_fds[i] 与 client task 修改互斥。 */
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_client_fds[i] >= 0) {
            shutdown(s_client_fds[i], SHUT_RDWR);
        }
    }
    xSemaphoreGive(s_slot_mutex);
    /* 任务退出后会自己 vTaskDelete(NULL)，这里只需等待其自然退出，
     * 绝不能再用句柄 vTaskDelete —— 双重删除会破坏 FreeRTOS 任务链表。 */
    vTaskDelay(pdMS_TO_TICKS(1000));

    for (int i = 0; i < MAX_LISTENERS; i++) {
        s_listener_tasks[i] = NULL;
    }
    /* 兜底：若 client task 未在 1s 内退出（TLS 阻塞等），强制 close 释放 fd。
     * close_client 内部有 mutex + CAS，与 task 内的 close_client 天然互斥。 */
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (s_client_fds[i] >= 0) {
            ESP_LOGW(TAG, "client %d did not exit within 1s, force close", i);
            close_client(i);
        }
    }
    ESP_LOGI(TAG, "server stopped");
}

static esp_err_t gw_start(const gw_config_t *cfg)
{
    memcpy(&s_cfg, cfg, sizeof(s_cfg));

    if (s_cfg.port == 0) {
        s_cfg.port = 502;
    }
    if (s_cfg.tls_enabled && s_cfg.tls_port == 0) {
        s_cfg.tls_port = 802;
    }
    if (s_cfg.tls_enabled && s_cfg.tls_port == s_cfg.port) {
        ESP_LOGE(TAG, "tls_port must differ from port");
        return ESP_ERR_INVALID_ARG;
    }

    /* 槽位互斥锁首次创建后复用，避免重复创建泄漏 */
    if (!s_slot_mutex) {
        s_slot_mutex = xSemaphoreCreateMutex();
    }
    if (!s_slot_mutex) {
        ESP_LOGE(TAG, "slot mutex create failed");
        return ESP_ERR_NO_MEM;
    }

    /* TLS 初始化（解析内置证书/私钥，仅一次） */
    if (s_cfg.tls_enabled && !s_tls_ready) {
        tls_init();
    }

    /* 分配监听任务参数：明文失败=硬失败（返回错误），TLS 失败=降级（仅告警） */
    listener_arg_t *la = malloc(sizeof(listener_arg_t));
    if (!la) {
        ESP_LOGE(TAG, "listener arg malloc failed");
        return ESP_ERR_NO_MEM;
    }
    la->port = s_cfg.port;
    la->tls = false;
    la->idx = 0;

    listener_arg_t *la2 = NULL;
    if (s_cfg.tls_enabled && s_tls_ready) {
        la2 = malloc(sizeof(listener_arg_t));
        if (la2) {
            la2->port = s_cfg.tls_port;
            la2->tls = true;
            la2->idx = 1;
        } else {
            ESP_LOGW(TAG, "TLS listener arg malloc failed, TLS disabled");
        }
    }

    s_running = true;

    if (xTaskCreate(tcp_listener_task, "mb_tcp_listen", 4096, la, 5,
                    &s_listener_tasks[0]) != pdPASS) {
        free(la);            /* 任务未创建，la 不会被 free，需手动释放 */
        s_running = false;
        ESP_LOGE(TAG, "listen task create failed");
        return ESP_ERR_NO_MEM;
    }
    if (la2) {
        if (xTaskCreate(tcp_listener_task, "mb_tls_listen", 4096, la2, 5,
                        &s_listener_tasks[1]) != pdPASS) {
            free(la2);       /* TLS 监听创建失败不致命，明文照常工作 */
            ESP_LOGW(TAG, "TLS listen task create failed, TLS disabled");
        }
    }
    ESP_LOGI(TAG, "server started (port %u%s, allowlist=%s)",
             s_cfg.port, s_cfg.tls_enabled ? " + TLS" : "",
             s_cfg.client_ip[0] ? s_cfg.client_ip : "none");
    if (s_cfg.tls_enabled) {
        ESP_LOGI(TAG, "TLS port %u", s_cfg.tls_port);
    }
    return ESP_OK;
}

void modbus_gw_init(void)
{
    gw_config_t cfg;
    gw_config_load(&cfg);
    if (cfg.enabled) {
        if (gw_start(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "server start failed, disabled");
            gw_stop();
        }
    } else {
        ESP_LOGI(TAG, "server disabled");
    }
}

esp_err_t modbus_gw_reconfigure(const gw_config_t *cfg)
{
    gw_stop();
    if (!cfg->enabled) {
        ESP_LOGI(TAG, "server disabled by config");
        return ESP_OK;
    }
    return gw_start(cfg);
}

void modbus_gw_get_config(gw_config_t *cfg)
{
    memcpy(cfg, &s_cfg, sizeof(s_cfg));
}

bool modbus_gw_is_running(void)
{
    return s_running;
}
