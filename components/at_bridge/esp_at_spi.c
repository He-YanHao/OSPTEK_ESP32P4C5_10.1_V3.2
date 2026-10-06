#include "esp_at_spi.h"
#include "hal_spi.h"
#include "freertos_port.h"
#include <string.h>

/* ============================================================
 *  调试日志开关
 *   0 = 关闭（生产环境）
 *   1 = 打开（调试协议问题）
 * ============================================================ */
#ifndef ESP_AT_SPI_DEBUG
#define ESP_AT_SPI_DEBUG  0
#endif

#if ESP_AT_SPI_DEBUG
#include <stdio.h>
#define AT_LOG(fmt, ...)  printf("[AT_SPI] " fmt "\r\n", ##__VA_ARGS__)
#else
#define AT_LOG(fmt, ...)  do {} while (0)
#endif

/* 单次等 IRQ 的最长时间：每次最多等 200ms，然后回顶层检查剩余预算 */
#define AT_WAIT_IRQ_MAX_MS   200

/* 重试退避：短退避 1ms，让 C5 喘口气 */
#define AT_RETRY_BACKOFF_MS  1

/* ---------- 全局状态 ---------- */
static SemaphoreHandle_t s_bus_mutex = NULL;    /* 保护 SPI 总线 */
static uint8_t           s_send_seq  = 0;       /* 写请求序号 */
static uint8_t           s_last_consumed_seq = 0; /* 上次已消费的读序号 */
static bool              s_seq_valid = false;   /* seq 去重是否生效 */
static int               s_inited = 0;

/* 大包分段读的状态机 */
typedef struct {
    uint8_t  seq;         /* 当前包的序号 */
    uint16_t total_len;   /* 整包总长度 */
    uint16_t read_len;    /* 已读长度 */
    bool     in_progress; /* 是否处于分段读中间 */
} pkt_state_t;

static pkt_state_t s_pkt = { .in_progress = false };

/* ---------- 底层 SPI 事务封装 ---------- */
static int at_spi_xfer(uint8_t cmd, uint8_t addr,
                       const uint8_t *tx, uint8_t *rx, size_t len)
{
    return hal_spi_transfer(cmd, addr, tx, rx, len);
}

/* 查询从机状态（RDBUF） */
static int at_query_slave(spi_recv_opt_t *out)
{
    return at_spi_xfer(CMD_HD_RDBUF_REG, RDBUF_START_ADDR,
                       NULL, (uint8_t *)out, sizeof(*out));
}

/* 发送写请求（WRBUF） */
static int at_request_write(uint8_t seq, uint16_t len)
{
    spi_send_opt_t opt = {
        .magic    = SPI_SEND_MAGIC,
        .send_seq = seq,
        .send_len = len,
    };
    return at_spi_xfer(CMD_HD_WRBUF_REG, WRBUF_START_ADDR,
                       (const uint8_t *)&opt, NULL, sizeof(opt));
}

/*
 * 协议层初始化。
 * 内部调 hal_spi_init，创建互斥量，重置所有状态。
 */
int esp_at_spi_init(void)
{
    if (s_inited) return 0;
    if (hal_spi_init() != 0) return -1;

    s_bus_mutex = xSemaphoreCreateMutex();
    if (!s_bus_mutex) return -1;

    s_send_seq = 0;
    s_last_consumed_seq = 0;
    s_seq_valid = false;
    s_pkt.in_progress = false;
    s_inited = 1;
    return 0;
}

/*
 * 复位 C5 并等 ready 脉冲。
 * 内部会清空所有协议状态。
 */
int esp_at_spi_reset_and_wait_ready(uint32_t timeout_ms)
{
    hal_c5_reset();
    s_pkt.in_progress = false;
    s_send_seq = 0;
    s_last_consumed_seq = 0;
    s_seq_valid = false;
    return hal_spi_wait_irq(timeout_ms);
}

/* 转发等 IRQ（避免上层直接依赖 hal_spi） */
int esp_at_spi_wait_irq(uint32_t timeout_ms)
{
    return hal_spi_wait_irq(timeout_ms);
}

/* ============================================================
 * 写数据到 C5
 * ============================================================
 *
 * 【流程】
 *   1. 发 WRBUF 声明 "我要写 N 字节"
 *   2. 等 C5 的 IRQ（表示 C5 状态已更新）
 *   3. 发 RDBUF 查询 C5 允许写多少（transmit_len）
 *   4. 发 WRDMA 写实际数据（send_len = min(chunk, transmit_len)）
 *   5. 发 WR_END 通知完成
 *
 * 【关键约束】
 *   - 必须等 IRQ 再查 RDBUF（C5 的 RDBUF 更新滞后于 IRQ）
 *   - 单次 WRDMA 不能超过 C5 声明的 transmit_len（超出被静默丢弃）
 *   - WR_END 失败 = C5 侧未提交，done 不累加
 *
 * 【重试策略】
 *   - IRQ 超时：不放弃，回顶层用剩余预算重试（新 seq）
 *   - WRDMA 失败：重试同一段
 *   - WR_END 双失败：break，done 未累加，上层重试
 */
int esp_at_spi_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!data || !len) return -1;
    if (xSemaphoreTake(s_bus_mutex, portMAX_DELAY) != pdTRUE) return -1;

    size_t done = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t to    = pdMS_TO_TICKS(timeout_ms);

    while (done < len) {
        /* 检查总预算是否耗尽 */
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= to) break;
        TickType_t remain = to - elapsed;

        /* 单次 chunk 上限 */
        size_t chunk = len - done;
        if (chunk > SPI_AT_MAX_PAYLOAD) chunk = SPI_AT_MAX_PAYLOAD;

        /* -------- 步骤 1：发 WRBUF -------- */
        uint8_t next_seq = (s_send_seq + 1) & 0xFF;
        if (at_request_write(next_seq, (uint16_t)chunk) != 0) {
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            continue;
        }
        s_send_seq = next_seq;   /* 只有发送成功才推进 seq */

        /* -------- 步骤 2：等 IRQ（超时上限 200ms） -------- */
        uint32_t remain_ms = (uint32_t)(remain * portTICK_PERIOD_MS);
        if (remain_ms > AT_WAIT_IRQ_MAX_MS) remain_ms = AT_WAIT_IRQ_MAX_MS;

        if (hal_spi_wait_irq(remain_ms) != 0) {
            /* IRQ 超时不放弃：回顶层检查剩余预算再试 */
            AT_LOG("wait_irq timeout, retry");
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            continue;
        }

        /* -------- 步骤 3：RDBUF 确认方向 -------- */
        spi_recv_opt_t opt;
        if (at_query_slave(&opt) != 0) {
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            continue;
        }
        if (opt.direct != SPI_DIR_WRITE) {
            /* C5 有 READ 数据要发，让路给读路径 */
            AT_LOG("slave not writable: direct=%u", opt.direct);
            break;
        }

        /* -------- 步骤 4：WRDMA 写数据（受 transmit_len 约束） -------- */
        size_t send_len = (chunk < opt.transmit_len) ? chunk : opt.transmit_len;
        if (send_len == 0 || send_len > SPI_AT_MAX_PAYLOAD) {
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            continue;
        }

        if (at_spi_xfer(CMD_HD_WRDMA_REG, 0x00, data + done, NULL, send_len) != 0) {
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            continue;   /* WRDMA 失败，重试同一段 */
        }

        /* -------- 步骤 5：WR_END 通知完成 -------- */
        if (at_spi_xfer(CMD_HD_WR_END_REG, 0x00, NULL, NULL, 0) != 0) {
            vTaskDelay(pdMS_TO_TICKS(AT_RETRY_BACKOFF_MS));
            if (at_spi_xfer(CMD_HD_WR_END_REG, 0x00, NULL, NULL, 0) != 0) {
                /* 双失败：这段未提交，done 不累加 */
                AT_LOG("WR_END failed twice, rollback %u bytes",
                       (unsigned)send_len);
                break;
            }
        }

        done += send_len;   /* 只有全部成功才累加 */
    }

    xSemaphoreGive(s_bus_mutex);
    return (int)done;
}

/* ============================================================
 * 从 C5 读一包数据
 * ============================================================
 *
 * 【流程】
 *   1. RDBUF 查询 C5 是否有数据要发
 *   2. seq 去重：如果 seq 与上次已消费的相同，跳过
 *   3. RDDMA 读数据（大包自动分段）
 *   4. INT0 通知 C5 "这一包已消费"
 *
 * 【关键约束】
 *   - 整包读完才发 INT0（中途发会丢剩余数据）
 *   - seq 去重防止 C5 状态未推进时重复读同一包
 *   - INT0 成功后才更新 s_last_consumed_seq
 *
 * 【返回值】
 *   > 0  本次读入字节数（可能小于整包，下次调用继续）
 *   = 0  无数据（可能是空闲，或 C5 状态未推进）
 *   < 0  协议错误
 */
int esp_at_spi_read(uint8_t *buf, size_t buf_size)
{
    if (!buf || !buf_size) return 0;
    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;

    int ret = 0;
    bool error = false;

    /* -------- 阶段 1：如果没有进行中的包，查询新包 -------- */
    if (!s_pkt.in_progress) {
        spi_recv_opt_t opt;
        if (at_query_slave(&opt) != 0) goto done;
        if (opt.direct != SPI_DIR_READ || opt.transmit_len == 0) goto done;

        /* seq 去重：已消费过的包不重复读 */
        if (s_seq_valid && opt.seq_num == s_last_consumed_seq) goto done;

        /* 记录新包状态，进入分段读模式 */
        s_pkt.seq         = opt.seq_num;
        s_pkt.total_len   = opt.transmit_len;
        s_pkt.read_len    = 0;
        s_pkt.in_progress = true;
    }

    /* -------- 阶段 2：读一段（受 buf_size 限制） -------- */
    size_t remain  = s_pkt.total_len - s_pkt.read_len;
    size_t to_read = (remain < buf_size) ? remain : buf_size;

    size_t done = 0;
    while (done < to_read) {
        size_t c = to_read - done;
        if (c > SPI_AT_MAX_PAYLOAD) c = SPI_AT_MAX_PAYLOAD;

        if (at_spi_xfer(CMD_HD_RDDMA_REG, 0x00, NULL, buf + done, c) != 0) {
            /* RDDMA 失败：发 INT0 让 C5 推进（避免卡死），清状态 */
            at_spi_xfer(CMD_HD_INT0_REG, 0x00, NULL, NULL, 0);
            s_pkt.in_progress = false;
            error = true;
            goto done;
        }
        done += c;
    }
    s_pkt.read_len += to_read;
    ret = (int)to_read;

    /* -------- 阶段 3：整包读完才发 INT0 -------- */
    if (s_pkt.read_len >= s_pkt.total_len) {
        bool int0_ok = false;

        /* INT0 首次尝试 */
        if (at_spi_xfer(CMD_HD_INT0_REG, 0x00, NULL, NULL, 0) == 0) {
            int0_ok = true;
        } else {
            /* 短延时后重试一次 */
            vTaskDelay(pdMS_TO_TICKS(1));
            if (at_spi_xfer(CMD_HD_INT0_REG, 0x00, NULL, NULL, 0) == 0) {
                int0_ok = true;
            }
        }

        if (int0_ok) {
            /* 成功消费：记录 seq 以便下次去重 */
            s_last_consumed_seq = s_pkt.seq;
            s_seq_valid = true;
        } else {
            /* INT0 失败：C5 侧未推进，可能会重发同一 seq。
             * 不清 s_last_consumed_seq，但暂时禁用去重，
             * 让下次读到同一 seq 时能重试。 */
            AT_LOG("INT0 failed, will resync on next read");
            s_seq_valid = false;
            error = true;
        }
        s_pkt.in_progress = false;
    }

done:
    xSemaphoreGive(s_bus_mutex);
    return error ? -1 : ret;
}