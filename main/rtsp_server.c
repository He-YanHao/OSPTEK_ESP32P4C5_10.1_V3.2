/*
 * rtsp_server —— S4：极简 RTSP / RTP Server（H.264 over RTP）。
 *
 * 设计取舍：官方 espressif/esp_rtsp_service 依赖 esp_media_service /
 * esp_service / gmf_core / media_lib_sal / esp_media_protocols 一整套
 * esp-adf 生态，且示例锁 esp_wifi_remote ~0.14.3（本工程为 1.6.5）。
 * 本工程只需要「单客户端 + H.264 + 局域网」，直接用 lwIP socket 实现
 * RTSP 信令与 RTP 打包，零额外组件、零协议栈黑盒，便于排障。
 *
 * 支持：
 *   - OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN / GET_PARAMETER
 *   - RTP over UDP (RTP/AVP/UDP;unicast;client_port=)
 *   - RTP over TCP interleaved (RTP/AVP/TCP;interleaved=0-1)
 *   - Annex-B 起始码切 NAL、single-NAL / FU-A 分包、SPS/PPS 提取进 SDP
 *   - 独立 tx 任务 + slot 队列：网络拥塞不阻塞 camera_preview 的帧提交
 */

#include "sdkconfig.h"
#include "rtsp_server.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

#include "lwip/sockets.h"

#define TAG              "rtsp_srv"
#define RTP_PT           96
#define RTP_SSRC         0x50414335u      /* "PAC5" */
#define RTP_TS_STEP      (90000 / 30)    /* 30 fps @ 90 kHz */
#define RTP_MTU          1400
#define FRAME_SLOTS      4
#define FRAME_CAP        (128 * 1024)
#define MAX_NALS         16
#define RTSP_RX_CAP      2048

typedef struct {
    uint8_t  slot;
    uint32_t len;
    bool     is_idr;
} frame_msg_t;

static uint8_t *s_slots[FRAME_SLOTS];
static QueueHandle_t s_free_q;
static QueueHandle_t s_tx_q;

static int s_listen_fd = -1;
static volatile int s_rtsp_fd = -1;
static int s_rtp_fd = -1;
static volatile bool s_streaming;
static volatile bool s_tcp_interleaved;
static volatile uint8_t s_interleaved_chan;
static struct sockaddr_in s_rtp_peer;
static volatile uint16_t s_seq;
static volatile uint32_t s_ts;
static char s_session_id[16];

static uint8_t  s_sps[64];
static uint8_t  s_pps[64];
static uint32_t s_sps_len;
static uint32_t s_pps_len;

static volatile uint32_t s_sessions;
static volatile uint32_t s_packets;
static volatile uint32_t s_bytes;
static volatile uint32_t s_dropped;

/* ------------------------------------------------------------------ */
/* base64（供 SDP sprop-parameter-sets）                                */
/* ------------------------------------------------------------------ */
static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *in, uint32_t len, char *out, int cap)
{
    int o = 0;
    for (uint32_t i = 0; i < len; i += 3) {
        uint32_t v = ((uint32_t)in[i]) << 16;
        if (i + 1 < len) v |= ((uint32_t)in[i + 1]) << 8;
        if (i + 2 < len) v |= in[i + 2];
        uint32_t n = (len - i) >= 3 ? 3 : (len - i);
        if (o + 5 > cap) return -1;
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = (n > 1) ? b64tab[(v >> 6) & 63] : '=';
        out[o++] = (n > 2) ? b64tab[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------ */
/* RTP                                                                  */
/* ------------------------------------------------------------------ */
static void rtp_header(uint8_t *p, bool marker, uint16_t seq, uint32_t ts)
{
    p[0] = 0x80;                                   /* V=2 P=0 X=0 CC=0 */
    p[1] = (uint8_t)((marker ? 0x80 : 0x00) | RTP_PT);
    p[2] = (uint8_t)(seq >> 8);
    p[3] = (uint8_t)(seq & 0xff);
    p[4] = (uint8_t)(ts >> 24);
    p[5] = (uint8_t)(ts >> 16);
    p[6] = (uint8_t)(ts >> 8);
    p[7] = (uint8_t)(ts & 0xff);
    p[8]  = (uint8_t)(RTP_SSRC >> 24);
    p[9]  = (uint8_t)(RTP_SSRC >> 16);
    p[10] = (uint8_t)(RTP_SSRC >> 8);
    p[11] = (uint8_t)(RTP_SSRC & 0xff);
}

static void rtp_send(const uint8_t *pkt, uint32_t len)
{
    int n = -1;
    if (s_tcp_interleaved) {
        int fd = s_rtsp_fd;
        if (fd >= 0) {
            uint8_t hdr[4] = { '$', s_interleaved_chan,
                               (uint8_t)(len >> 8), (uint8_t)(len & 0xff) };
            if (send(fd, hdr, 4, 0) == 4) {
                n = send(fd, pkt, len, 0);
            }
        }
    } else if (s_rtp_fd >= 0) {
        n = sendto(s_rtp_fd, pkt, len, 0,
                   (const struct sockaddr *)&s_rtp_peer, sizeof(s_rtp_peer));
    }
    if (n > 0) {
        s_packets++;
        s_bytes += len;
    }
}

/* 单个 NAL（不含起始码），按需 FU-A 分片 */
static void packetize_nal(const uint8_t *nal, uint32_t len, bool last_nal)
{
    uint8_t pkt[12 + 2 + RTP_MTU];
    const uint8_t nal_hdr = nal[0];
    const uint8_t ntype = nal_hdr & 0x1f;
    const uint8_t *body = nal + 1;
    uint32_t body_len = len - 1;

    if (body_len <= RTP_MTU) {
        uint16_t seq = ++s_seq;
        rtp_header(pkt, last_nal, seq, s_ts);
        pkt[12] = nal_hdr;
        memcpy(pkt + 13, body, body_len);
        rtp_send(pkt, 13 + body_len);
        return;
    }

    const uint8_t fu_ind = (uint8_t)((nal_hdr & 0xe0) | 28);
    uint32_t off = 0;
    while (off < body_len) {
        uint32_t chunk = body_len - off;
        if (chunk > RTP_MTU - 2) chunk = RTP_MTU - 2;
        const bool first = (off == 0);
        const bool end = (off + chunk >= body_len);
        uint16_t seq = ++s_seq;
        rtp_header(pkt, last_nal && end, seq, s_ts);
        pkt[12] = fu_ind;
        pkt[13] = (uint8_t)((first ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | ntype);
        memcpy(pkt + 14, body + off, chunk);
        rtp_send(pkt, 14 + chunk);
        off += chunk;
    }
}

/* 按 Annex-B 起始码切 NAL；顺带缓存 SPS/PPS */
static void process_frame(const uint8_t *buf, uint32_t len)
{
    uint32_t sc_pos[MAX_NALS];
    uint32_t nal_pos[MAX_NALS];
    int n = 0;
    uint32_t i = 0;

    while (i + 3 < len && n < MAX_NALS) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
            sc_pos[n] = i;
            nal_pos[n] = i + 4;
            n++;
            i += 4;
        } else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            sc_pos[n] = i;
            nal_pos[n] = i + 3;
            n++;
            i += 3;
        } else {
            i++;
        }
    }
    if (n == 0) return;

    s_ts += RTP_TS_STEP;
    for (int k = 0; k < n; k++) {
        uint32_t end = (k + 1 < n) ? sc_pos[k + 1] : len;
        uint32_t nal_len = end - nal_pos[k];
        /* 去掉起始码前可能存在的 padding zero（最多 3 个） */
        for (int t = 0; t < 3 && nal_len > 0 && buf[nal_pos[k] + nal_len - 1] == 0; t++) {
            nal_len--;
        }
        if (nal_len < 2) continue;

        const uint8_t *nal = buf + nal_pos[k];
        uint8_t type = nal[0] & 0x1f;
        if (type == 7 && s_sps_len == 0 && nal_len <= sizeof(s_sps)) {
            memcpy(s_sps, nal, nal_len);
            s_sps_len = nal_len;
        } else if (type == 8 && s_pps_len == 0 && nal_len <= sizeof(s_pps)) {
            memcpy(s_pps, nal, nal_len);
            s_pps_len = nal_len;
        }

        if (s_streaming) {
            packetize_nal(nal, nal_len, k == (n - 1));
        }
    }
}

static void rtsp_tx_task(void *arg)
{
    (void)arg;
    frame_msg_t msg;
    while (1) {
        if (xQueueReceive(s_tx_q, &msg, portMAX_DELAY) != pdTRUE) continue;
        if (msg.slot < FRAME_SLOTS && s_slots[msg.slot]) {
            process_frame(s_slots[msg.slot], msg.len);
        }
        xQueueSend(s_free_q, &msg.slot, 0);
    }
}

void rtsp_server_push_frame(const uint8_t *annexb, size_t len, bool is_idr)
{
    if (!s_tx_q || !annexb || len == 0) return;
    if (len > FRAME_CAP) len = FRAME_CAP;

    uint8_t slot;
    if (xQueueReceive(s_free_q, &slot, 0) != pdTRUE) {
        s_dropped++;
        return;
    }
    frame_msg_t msg = { .slot = slot, .len = (uint32_t)len, .is_idr = is_idr };
    memcpy(s_slots[slot], annexb, len);
    if (xQueueSend(s_tx_q, &msg, 0) != pdTRUE) {
        xQueueSend(s_free_q, &slot, 0);
        s_dropped++;
    }
}

/* ------------------------------------------------------------------ */
/* SDP                                                                  */
/* ------------------------------------------------------------------ */
static void build_sdp(char *out, size_t cap)
{
    static char sps64[128];
    static char pps64[64];
    if (s_sps_len) b64_encode(s_sps, s_sps_len, sps64, sizeof(sps64));
    if (s_pps_len) b64_encode(s_pps, s_pps_len, pps64, sizeof(pps64));

    static char pli[8];
    snprintf(pli, sizeof(pli), "42C01F");
    if (s_sps_len >= 4) {
        snprintf(pli, sizeof(pli), "%02x%02x%02x",
                 s_sps[1], s_sps[2], s_sps[3]);
    }

    static char fmtp[320];
    if (s_sps_len && s_pps_len) {
        snprintf(fmtp, sizeof(fmtp),
                 "a=fmtp:96 packetization-mode=1;profile-level-id=%s;"
                 "sprop-parameter-sets=%s,%s\r\n", pli, sps64, pps64);
    } else {
        snprintf(fmtp, sizeof(fmtp),
                 "a=fmtp:96 packetization-mode=1;profile-level-id=%s\r\n", pli);
    }

    snprintf(out, cap,
             "v=0\r\n"
             "o=- 0 0 IN IP4 0.0.0.0\r\n"
             "s=ESP32-P4 H264 Live\r\n"
             "c=IN IP4 0.0.0.0\r\n"
             "t=0 0\r\n"
             "a=control:*\r\n"
             "m=video 0 RTP/AVP %d\r\n"
             "a=rtpmap:%d H264/90000\r\n"
             "%s"
             "a=control:trackID=0\r\n",
             RTP_PT, RTP_PT, fmtp);
}

/* ------------------------------------------------------------------ */
/* RTSP 信令                                                            */
/* ------------------------------------------------------------------ */
static void rtsp_reply(int fd, int cseq, const char *extra,
                       const char *body, const char *ctype)
{
    static char hdr[768];
    int n = snprintf(hdr, sizeof(hdr),
                     "RTSP/1.0 200 OK\r\n"
                     "CSeq: %d\r\n"
                     "Server: ESP32-P4/1.0\r\n"
                     "%s", cseq, extra ? extra : "");
    if (body) {
        n += snprintf(hdr + n, sizeof(hdr) - n,
                      "Content-Type: %s\r\n"
                      "Content-Length: %u\r\n",
                      ctype ? ctype : "application/sdp",
                      (unsigned)strlen(body));
    }
    n += snprintf(hdr + n, sizeof(hdr) - n, "\r\n");
    send(fd, hdr, n, 0);
    if (body) send(fd, body, strlen(body), 0);
}

static int parse_cseq(const char *req)
{
    const char *p = strstr(req, "CSeq");
    if (!p) return 0;
    p = strchr(p, ':');
    return p ? atoi(p + 1) : 0;
}

static void rtsp_rtp_error(int fd, int cseq, const char *extra)
{
    static char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "RTSP/1.0 461 Unsupported Transport\r\n"
                     "CSeq: %d\r\n"
                     "%s\r\n", cseq, extra ? extra : "");
    send(fd, buf, n, 0);
}

static void rtsp_session(int fd)
{
    static char rx[RTSP_RX_CAP];
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) != 0) {
        memset(&peer, 0, sizeof(peer));
    }

    snprintf(s_session_id, sizeof(s_session_id), "%08lx",
             (unsigned long)esp_random());
    s_sessions++;
    s_streaming = false;
    s_tcp_interleaved = false;
    s_rtsp_fd = fd;

    ESP_LOGI(TAG, "client connected, session=%s", s_session_id);

    while (1) {
        int n = recv(fd, rx, sizeof(rx) - 1, 0);
        if (n <= 0) break;
        rx[n] = '\0';
        if (!strstr(rx, "\r\n\r\n")) continue;

        char method[16] = { 0 }, uri[256] = { 0 };
        if (sscanf(rx, "%15s %255s", method, uri) < 1) continue;
        int cseq = parse_cseq(rx);
        char session_hdr[64];
        snprintf(session_hdr, sizeof(session_hdr), "Session: %s;timeout=60\r\n",
                 s_session_id);

        if (strcmp(method, "OPTIONS") == 0) {
            rtsp_reply(fd, cseq,
                       "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n",
                       NULL, NULL);

        } else if (strcmp(method, "DESCRIBE") == 0) {
            /* 等首帧到达，让 SDP 能带上 SPS/PPS（最多 2 s） */
            for (int w = 0; w < 20 && s_sps_len == 0; w++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            static char sdp[1024];
            build_sdp(sdp, sizeof(sdp));
            static char base[512];
            snprintf(base, sizeof(base), "Content-Base: %s/\r\n", uri);
            rtsp_reply(fd, cseq, base, sdp, "application/sdp");
            ESP_LOGI(TAG, "DESCRIBE: sps=%u pps=%u",
                     (unsigned)s_sps_len, (unsigned)s_pps_len);

        } else if (strcmp(method, "SETUP") == 0) {
            uint16_t cp1 = 0, cp2 = 0;
            const char *p = strstr(rx, "client_port=");
            const char *q = strstr(rx, "interleaved=");
            static char trans[256];

            if (p && sscanf(p + 12, "%hu-%hu", &cp1, &cp2) >= 1) {
                memset(&s_rtp_peer, 0, sizeof(s_rtp_peer));
                s_rtp_peer.sin_family = AF_INET;
                s_rtp_peer.sin_addr.s_addr = peer.sin_addr.s_addr;
                s_rtp_peer.sin_port = htons(cp1);
                s_tcp_interleaved = false;
                snprintf(trans, sizeof(trans),
                         "Transport: RTP/AVP/UDP;unicast;client_port=%u-%u;"
                         "server_port=5004-5005;ssrc=%08x\r\n",
                         (unsigned)cp1, (unsigned)cp2, (unsigned)RTP_SSRC);
                ESP_LOGI(TAG, "SETUP UDP client_port=%u-%u",
                         (unsigned)cp1, (unsigned)cp2);
            } else if (q) {
                unsigned ch1 = 0, ch2 = 0;
                sscanf(q + 12, "%u-%u", &ch1, &ch2);
                s_interleaved_chan = (uint8_t)ch1;
                s_tcp_interleaved = true;
                snprintf(trans, sizeof(trans),
                         "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u;"
                         "ssrc=%08x\r\n", ch1, ch2, (unsigned)RTP_SSRC);
                ESP_LOGI(TAG, "SETUP TCP interleaved=%u-%u", ch1, ch2);
            } else {
                rtsp_rtp_error(fd, cseq, NULL);
                continue;
            }
            static char extra[384];
            snprintf(extra, sizeof(extra), "%s%s", session_hdr, trans);
            rtsp_reply(fd, cseq, extra, NULL, NULL);

        } else if (strcmp(method, "PLAY") == 0) {
            static char extra[512];
            snprintf(extra, sizeof(extra),
                     "%s"
                     "Range: npt=0.000-\r\n"
                     "RTP-Info: url=%s/trackID=0;seq=%u;rtptime=%u\r\n",
                     session_hdr, uri, (unsigned)s_seq, (unsigned)s_ts);
            s_streaming = true;
            rtsp_reply(fd, cseq, extra, NULL, NULL);
            ESP_LOGI(TAG, "PLAY -> streaming started (UDP=%d)",
                     (int)!s_tcp_interleaved);

        } else if (strcmp(method, "TEARDOWN") == 0) {
            s_streaming = false;
            rtsp_reply(fd, cseq, session_hdr, NULL, NULL);
            break;
        } else {
            rtsp_reply(fd, cseq, session_hdr, NULL, NULL);
        }
    }

    s_streaming = false;
    s_rtsp_fd = -1;
    close(fd);
    ESP_LOGI(TAG, "session closed");
}

static void rtsp_accept_task(void *arg)
{
    uint16_t port = (uint16_t)(uintptr_t)arg;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(port),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 1) < 0) {
        ESP_LOGE(TAG, "bind/listen on %u failed", (unsigned)port);
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    s_listen_fd = fd;
    ESP_LOGI(TAG, "RTSP server listening on rtsp://0.0.0.0:%u/live",
             (unsigned)port);

    while (1) {
        int c = accept(fd, NULL, NULL);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        rtsp_session(c);   /* 单客户端，串行处理 */
    }
}

esp_err_t rtsp_server_start(uint16_t port)
{
    if (s_tx_q) return ESP_OK;   /* 已启动 */

    s_free_q = xQueueCreate(FRAME_SLOTS, sizeof(uint8_t));
    s_tx_q = xQueueCreate(FRAME_SLOTS, sizeof(frame_msg_t));
    if (!s_free_q || !s_tx_q) return ESP_ERR_NO_MEM;

    for (int i = 0; i < FRAME_SLOTS; i++) {
        s_slots[i] = heap_caps_malloc(FRAME_CAP, MALLOC_CAP_SPIRAM);
        if (!s_slots[i]) {
            ESP_LOGE(TAG, "frame slot %d alloc failed", i);
            return ESP_ERR_NO_MEM;
        }
        uint8_t idx = (uint8_t)i;
        xQueueSend(s_free_q, &idx, 0);
    }

    s_rtp_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_rtp_fd < 0) {
        ESP_LOGE(TAG, "RTP UDP socket failed");
        return ESP_FAIL;
    }

    if (xTaskCreate(rtsp_tx_task, "rtsp_tx", 6144, NULL, 6, NULL) != pdPASS ||
        xTaskCreate(rtsp_accept_task, "rtsp_srv", 8192,
                    (void *)(uintptr_t)port, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void rtsp_server_get_stats(uint32_t *sessions, uint32_t *packets,
                            uint32_t *bytes, uint32_t *dropped)
{
    if (sessions) *sessions = s_sessions;
    if (packets)  *packets  = s_packets;
    if (bytes)    *bytes    = s_bytes;
    if (dropped)  *dropped  = s_dropped;
}
