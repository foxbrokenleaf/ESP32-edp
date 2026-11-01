/*
 * @Description: GT30L32S4W 字库芯片 BSP 层实现（ESP-IDF SPI Master）
 *
 * 时序（规格书）：
 *   03h READ      : CMD(03h) + ADDR[23:0]              + DATA...
 *   0Bh FAST_READ : CMD(0Bh) + ADDR[23:0] + DUMMY(1B) + DATA...
 *   数据在 SCLK 上升沿移入芯片、下降沿从 SO 移出 -> SPI Mode 0
 */

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "bsp_GT30L32S4W.h"

static const char *TAG = "GT30L32S4W";

/* 每次 SPI 事务最多搬多少字节（内部缓冲大小，DRAM 可用于 DMA） */
#define GT30L32S4W_XFER_CHUNK      256
/* 指令 1B + 地址 3B + dummy 1B 的余量 */
#define GT30L32S4W_XFER_OVERHEAD   8

static spi_device_handle_t s_spi_dev  = NULL;
static SemaphoreHandle_t   s_lock     = NULL;
static uint32_t            s_clock_hz = BSP_GT30L32S4W_DEFAULT_CLOCK_HZ;
static bool                s_ready    = false;

/* 收发缓冲必须是内部 RAM（DMA 能力），用 static 保证 */
static uint8_t s_tx_buf[GT30L32S4W_XFER_CHUNK + GT30L32S4W_XFER_OVERHEAD];
static uint8_t s_rx_buf[GT30L32S4W_XFER_CHUNK + GT30L32S4W_XFER_OVERHEAD];

/* ========================= 内部实现 ========================= */

/**
 * @brief  单次（可分包的）读操作
 * @param  cmd BSP_GT30L32S4W_CMD_READ 或 BSP_GT30L32S4W_CMD_FAST_READ
 */
static esp_err_t gt30l32s4w_read_internal(uint8_t cmd, uint32_t addr, uint8_t *dst, size_t len)
{
    if (dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready || s_spi_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (addr > 0xFFFFFFu || (addr + len) > 0x1000000u) {
        ESP_LOGE(TAG, "address out of range: addr=0x%06" PRIx32 ", len=%u", addr, (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }

    /* 头部长度：cmd + 3 字节地址（FAST_READ 多 1 字节 dummy） */
    const size_t hdr_len = (cmd == BSP_GT30L32S4W_CMD_FAST_READ) ? 5 : 4;
    esp_err_t ret = ESP_OK;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    while (len > 0) {
        size_t chunk = (len > GT30L32S4W_XFER_CHUNK) ? GT30L32S4W_XFER_CHUNK : len;
        size_t total = hdr_len + chunk;

        memset(s_tx_buf, 0x00, total);          /* dummy / 数据段填 0（MOSI 内容无关） */
        s_tx_buf[0] = cmd;
        s_tx_buf[1] = (uint8_t)((addr >> 16) & 0xFF);
        s_tx_buf[2] = (uint8_t)((addr >> 8) & 0xFF);
        s_tx_buf[3] = (uint8_t)(addr & 0xFF);

        spi_transaction_t trans;
        memset(&trans, 0, sizeof(trans));
        trans.length    = total * 8;
        trans.tx_buffer = s_tx_buf;
        trans.rx_buffer = s_rx_buf;

        ret = spi_device_polling_transmit(s_spi_dev, &trans);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi transmit failed: %s", esp_err_to_name(ret));
            break;
        }

        memcpy(dst, &s_rx_buf[hdr_len], chunk);

        dst  += chunk;
        addr += (uint32_t)chunk;
        len  -= chunk;
    }

    xSemaphoreGive(s_lock);
    return ret;
}

/* ========================= 对外接口 ========================= */

esp_err_t bsp_gt30l32s4w_init(void)
{
    return bsp_gt30l32s4w_init_with_clock(BSP_GT30L32S4W_DEFAULT_CLOCK_HZ);
}

esp_err_t bsp_gt30l32s4w_init_with_clock(uint32_t clock_hz)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (clock_hz == 0) {
        clock_hz = BSP_GT30L32S4W_DEFAULT_CLOCK_HZ;
    }
    if (clock_hz > BSP_GT30L32S4W_MAX_CLOCK_HZ) {
        ESP_LOGW(TAG, "clock %" PRIu32 "Hz too high, clamp to %dHz",
                 clock_hz, BSP_GT30L32S4W_MAX_CLOCK_HZ);
        clock_hz = BSP_GT30L32S4W_MAX_CLOCK_HZ;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "create mutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    /* HOLD# 若由 GPIO 控制，初始化时拉高（结束总线挂起） */
#if (BSP_GT30L32S4W_HOLD_PIN >= 0)
    gpio_config_t hold_cfg = {
        .pin_bit_mask = (1ULL << BSP_GT30L32S4W_HOLD_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t hold_err = gpio_config(&hold_cfg);
    if (hold_err != ESP_OK) {
        ESP_LOGE(TAG, "config HOLD pin failed: %s", esp_err_to_name(hold_err));
        return hold_err;
    }
    gpio_set_level(BSP_GT30L32S4W_HOLD_PIN, 1);
#endif

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = BSP_GT30L32S4W_MOSI_PIN,
        .miso_io_num     = BSP_GT30L32S4W_MISO_PIN,
        .sclk_io_num     = BSP_GT30L32S4W_SCLK_PIN,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = sizeof(s_tx_buf),
    };

    bool bus_owned = false;
    esp_err_t err = spi_bus_initialize(BSP_GT30L32S4W_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        bus_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        /* INVALID_STATE 表示总线已被其它驱动初始化，可继续在其上挂设备 */
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz   = (int)clock_hz,
        .mode             = 0,                        /* CPOL=0, CPHA=0 */
        .spics_io_num     = BSP_GT30L32S4W_CS_PIN,
        .queue_size       = 1,
        .cs_ena_pretrans  = 1,                        /* CS 建立时间 */
        .cs_ena_posttrans = 1,                        /* CS 保持时间 */
    };

    err = spi_bus_add_device(BSP_GT30L32S4W_SPI_HOST, &dev_cfg, &s_spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        if (bus_owned) {
            spi_bus_free(BSP_GT30L32S4W_SPI_HOST);
        }
        s_spi_dev = NULL;
        return err;
    }

    s_clock_hz = clock_hz;
    s_ready    = true;

    ESP_LOGI(TAG, "ready: SCLK=%d CS=%d MOSI=%d MISO=%d clock=%" PRIu32 "Hz",
             BSP_GT30L32S4W_SCLK_PIN, BSP_GT30L32S4W_CS_PIN,
             BSP_GT30L32S4W_MOSI_PIN, BSP_GT30L32S4W_MISO_PIN, clock_hz);
    return ESP_OK;
}

esp_err_t bsp_gt30l32s4w_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    esp_err_t err = spi_bus_remove_device(s_spi_dev);
    s_spi_dev = NULL;
    s_ready   = false;

    esp_err_t bus_err = spi_bus_free(BSP_GT30L32S4W_SPI_HOST);
    if (err == ESP_OK) {
        err = bus_err;
    }

    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    return err;
}

bool bsp_gt30l32s4w_is_ready(void)
{
    return s_ready;
}

uint32_t bsp_gt30l32s4w_get_clock_hz(void)
{
    return s_clock_hz;
}

esp_err_t bsp_gt30l32s4w_read(uint32_t addr, void *dst, size_t len)
{
    return gt30l32s4w_read_internal(BSP_GT30L32S4W_CMD_READ, addr, (uint8_t *)dst, len);
}

esp_err_t bsp_gt30l32s4w_fast_read(uint32_t addr, void *dst, size_t len)
{
    return gt30l32s4w_read_internal(BSP_GT30L32S4W_CMD_FAST_READ, addr, (uint8_t *)dst, len);
}

