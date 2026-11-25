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
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/spi_periph.h"

#include "bsp_GT30L32S4W.h"

static const char *TAG = "GT30L32S4W";

/* 每次 SPI 事务最多搬多少字节（内部缓冲大小，DRAM 可用于 DMA） */
#define GT30L32S4W_XFER_CHUNK      256
/* 指令 1B + 地址 3B + dummy 1B 的余量 */
#define GT30L32S4W_XFER_OVERHEAD   8

#if !BSP_GT30L32S4W_USE_BITBANG
static spi_device_handle_t s_spi_dev  = NULL;
#endif
static SemaphoreHandle_t   s_lock     = NULL;
static uint32_t            s_clock_hz = BSP_GT30L32S4W_DEFAULT_CLOCK_HZ;
static bool                s_ready    = false;

/*
 * CS# 网络被外部硬短路到 GND 时置 true：
 *   此时把 CS# 配成高阻（不再驱动），避免长期对地灌 40mA 把 pad 烧掉，
 *   也避免拉低整板电源。检测在 bsp_gt30l32s4w_pin_toggle_test() 里做。
 */
static bool                s_cs_shorted = false;

#if BSP_GT30L32S4W_USE_BITBANG
/* 定义在下面的“引脚翻转测试”小节，初始化时要提前用 */
static bool gt_line_check(gpio_num_t pin, const char *name);
#endif

#if !BSP_GT30L32S4W_USE_BITBANG
/* 收发缓冲必须是内部 RAM（DMA 能力），用 static 保证 */
static uint8_t s_tx_buf[GT30L32S4W_XFER_CHUNK + GT30L32S4W_XFER_OVERHEAD];
static uint8_t s_rx_buf[GT30L32S4W_XFER_CHUNK + GT30L32S4W_XFER_OVERHEAD];
#endif

/* ========================= 引脚配置 ========================= */

/*
 * 引脚说明（实测确认，见 bsp_GT30L32S4W.h 说明 4）：
 *   本芯片使用 GPIO26(SCLK) / GPIO23(CS#) / GPIO25(SI) / GPIO35(SO)。
 *   其中 26/25 与墨水屏的软件 SPI（SCL=GPIO26 / SDA=GPIO25）是同一根网络，
 *   两片共用时钟/数据线、靠各自片选区分（字库 CS#=23，EPD CS=27）。
 *   每次读前 pins_init()、读后 pins_deinit() 会把 26/25 重新配置成普通 GPIO，
 *   与 EPD 交替使用不会互相打断。
 *
 *   注意：早期版本误把 SCLK/SI 写成 22/21，字库收不到时钟，读回的字模全是 FF，
 *   表现就是“字库字符不显示”；bus probe 会给出真实接线
 *   `PROBE HIT: SCLK=26 SI=25 CS=23 SO=35`。
 */

#if !BSP_GT30L32S4W_USE_BITBANG
static void gt30l32s4w_pins_to_gpio(void)
{
    /* SCLK / MOSI：普通 GPIO 输出，空闲高电平（与 EPD 软件 SPI 一致） */
    gpio_set_direction((gpio_num_t)BSP_GT30L32S4W_SCLK_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BSP_GT30L32S4W_SCLK_PIN, 1);

    gpio_set_direction((gpio_num_t)BSP_GT30L32S4W_MOSI_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BSP_GT30L32S4W_MOSI_PIN, 1);

    /* CS# 拉高，避免下次接管时误选中 */
    gpio_set_direction((gpio_num_t)BSP_GT30L32S4W_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BSP_GT30L32S4W_CS_PIN, 1);

    /* MISO 保持输入：gpio_get_level() 始终反映引脚电平，无需断开矩阵输入 */
    gpio_set_direction((gpio_num_t)BSP_GT30L32S4W_MISO_PIN, GPIO_MODE_INPUT);

#if (BSP_GT30L32S4W_HOLD_PIN >= 0)
    gpio_set_direction((gpio_num_t)BSP_GT30L32S4W_HOLD_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BSP_GT30L32S4W_HOLD_PIN, 1);
#endif
}
#endif /* !BSP_GT30L32S4W_USE_BITBANG */

/* ========================= bit-bang（纯 GPIO 时序） ========================= */

#if BSP_GT30L32S4W_USE_BITBANG

#define GT_SCLK_HIGH()  gpio_set_level((gpio_num_t)BSP_GT30L32S4W_SCLK_PIN, 1)
#define GT_SCLK_LOW()   gpio_set_level((gpio_num_t)BSP_GT30L32S4W_SCLK_PIN, 0)
#define GT_MOSI_HIGH()  gpio_set_level((gpio_num_t)BSP_GT30L32S4W_MOSI_PIN, 1)
#define GT_MOSI_LOW()   gpio_set_level((gpio_num_t)BSP_GT30L32S4W_MOSI_PIN, 0)
#define GT_CS_HIGH()    gpio_set_level((gpio_num_t)BSP_GT30L32S4W_CS_PIN, 1)
#define GT_CS_LOW()     gpio_set_level((gpio_num_t)BSP_GT30L32S4W_CS_PIN, 0)
#define GT_MISO_READ()  gpio_get_level((gpio_num_t)BSP_GT30L32S4W_MISO_PIN)

static inline void gt_bb_delay(void)
{
#if (BSP_GT30L32S4W_BITBANG_DELAY_US > 0)
    esp_rom_delay_us(BSP_GT30L32S4W_BITBANG_DELAY_US);
#endif
}

/**
 * @brief  配置字库引脚的 GPIO 方向（bit-bang 用，不涉及 SPI 外设/GPIO 矩阵）
 * @param  idle_sclk_high true : SCLK 空闲高电平（通用空闲态）
 *                         false: SCLK 空闲低电平（字库 SPI Mode 0）
 */
static esp_err_t gt_gpio_config(bool idle_sclk_high)
{
    /*
     * CS# 若已被判定为对地短路，就把它排除在输出之外（保持高阻），
     * 防止长期对地灌电流。SCLK/SI 不受影响。
     */
    uint64_t out_mask = (1ULL << BSP_GT30L32S4W_SCLK_PIN) |
                        (1ULL << BSP_GT30L32S4W_MOSI_PIN);
    if (!s_cs_shorted) {
        out_mask |= (1ULL << BSP_GT30L32S4W_CS_PIN);
    }

    const gpio_config_t out_cfg = {
        .pin_bit_mask = out_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    if (s_cs_shorted) {
        const gpio_config_t cs_in_cfg = {
            .pin_bit_mask = (1ULL << BSP_GT30L32S4W_CS_PIN),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        (void)gpio_config(&cs_in_cfg);
    }

    /*
     * 先把输出寄存器的电平置成空闲值，再切换成输出，避免切换瞬间
     * 在 SCLK 上产生一个毛刺时钟（会把字库的状态机打乱）。
     * gpio_set_level() 在引脚还是输入时同样会写入输出寄存器。
     */
    GT_CS_HIGH();
    GT_MOSI_HIGH();
    if (idle_sclk_high) {
        GT_SCLK_HIGH();
    } else {
        GT_SCLK_LOW();
    }

    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config SCLK/MOSI/CS failed: %s", esp_err_to_name(err));
        return err;
    }

    /* GPIO34~39 只能输入，且没有内部上下拉 */
    const gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << BSP_GT30L32S4W_MISO_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&in_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config MISO failed: %s", esp_err_to_name(err));
        return err;
    }

    /* CS 高、MOSI 高（空闲），SCLK 按使用方要求 */
    GT_CS_HIGH();
    GT_MOSI_HIGH();
    if (idle_sclk_high) {
        GT_SCLK_HIGH();
    } else {
        GT_SCLK_LOW();
    }
    return ESP_OK;
}

/* SPI Mode 0：SCLK 空闲低，字库在上升沿采样 MOSI */
static void gt_bb_write_byte(uint8_t value)
{
    for (int i = 7; i >= 0; i--) {
        GT_SCLK_LOW();
        if (value & (1u << i)) {
            GT_MOSI_HIGH();
        } else {
            GT_MOSI_LOW();
        }
        gt_bb_delay();
        GT_SCLK_HIGH();
        gt_bb_delay();
    }
    GT_SCLK_LOW();
}

/* SPI Mode 0：字库在 SCLK 下降沿输出下一位，主机在低电平期间读取 */
static uint8_t gt_bb_read_byte(void)
{
    uint8_t value = 0;
    for (int i = 0; i < 8; i++) {
        GT_SCLK_LOW();                 /* 下降沿：字库输出下一位 */
        gt_bb_delay();
        value = (uint8_t)((value << 1) | (GT_MISO_READ() ? 1u : 0u));
        GT_SCLK_HIGH();                /* 上升沿：完成本 bit */
        gt_bb_delay();
    }
    GT_SCLK_LOW();
    return value;
}

/**
 * @brief  bit-bang 读取（03h READ / 0Bh FAST_READ）
 */
static void gt30l32s4w_bitbang_read(uint8_t cmd, uint32_t addr, uint8_t *dst, size_t len)
{
    /* 空闲电平：CS 高、SCLK 低（Mode 0）、MOSI 高 */
    GT_CS_HIGH();
    GT_SCLK_LOW();
    GT_MOSI_HIGH();
    gt_bb_delay();

    GT_CS_LOW();
    gt_bb_delay();

    gt_bb_write_byte(cmd);
    gt_bb_write_byte((uint8_t)((addr >> 16) & 0xFF));
    gt_bb_write_byte((uint8_t)((addr >> 8) & 0xFF));
    gt_bb_write_byte((uint8_t)(addr & 0xFF));
    if (cmd == BSP_GT30L32S4W_CMD_FAST_READ) {
        gt_bb_write_byte(0x00);        /* 1 字节 dummy */
    }

    for (size_t i = 0; i < len; i++) {
        dst[i] = gt_bb_read_byte();
    }

    gt_bb_delay();
    GT_CS_HIGH();
    gt_bb_delay();
}

#endif /* BSP_GT30L32S4W_USE_BITBANG */

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
#if !BSP_GT30L32S4W_USE_BITBANG
    if (!s_ready || s_spi_dev == NULL) {
#else
    if (!s_ready) {
#endif
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
    esp_err_t ret = ESP_OK;

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* 引脚与 EPD 等外设复用：每次传输前重新配置/接回引脚 */
    ret = bsp_gt30l32s4w_pins_init();
    if (ret != ESP_OK) {
        xSemaphoreGive(s_lock);
        return ret;
    }

#if BSP_GT30L32S4W_USE_BITBANG
    /* 软件时序：整段一次读完，无 DMA 缓冲长度限制 */
    gt30l32s4w_bitbang_read(cmd, addr, dst, len);
#else
    const size_t hdr_len = (cmd == BSP_GT30L32S4W_CMD_FAST_READ) ? 5 : 4;

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
#endif

    /* 传输结束，把引脚交还普通 GPIO，供 EPD 的软件 SPI 继续使用 */
    (void)bsp_gt30l32s4w_pins_deinit();

    xSemaphoreGive(s_lock);
    return ret;
}

/* ========================= 对外接口 ========================= */

esp_err_t bsp_gt30l32s4w_pins_init(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

#if BSP_GT30L32S4W_USE_BITBANG
    /* bit-bang：引脚就是普通 GPIO，重新配置一遍即可（SCLK 空闲低） */
    return gt_gpio_config(false);
#else
    const spi_signal_conn_t *sig = &spi_periph_signal[BSP_GT30L32S4W_SPI_HOST];

    /*
     * gpio_set_direction() 内部会调用 gpio_hal_matrix_out_default()，先把引脚
     * 在 GPIO 矩阵里的输出信号复位成 SIG_GPIO_OUT_IDX；所以必须先设置方向，
     * 再用 esp_rom_gpio_connect_out_signal() 把 SPI 信号接回去
     * （与 IDF spi_common.c 配置 GPIO 矩阵引脚时的顺序一致）。
     */
    gpio_set_direction((gpio_num_t)BSP_GT30L32S4W_SCLK_PIN, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(BSP_GT30L32S4W_SCLK_PIN, sig->spiclk_out, false, false);
    esp_rom_gpio_connect_in_signal(BSP_GT30L32S4W_SCLK_PIN, sig->spiclk_in, false);

    gpio_set_direction((gpio_num_t)BSP_GT30L32S4W_MOSI_PIN, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(BSP_GT30L32S4W_MOSI_PIN, sig->spid_out, false, false);
    esp_rom_gpio_connect_in_signal(BSP_GT30L32S4W_MOSI_PIN, sig->spid_in, false);

    /* MISO：GPIO35 是纯输入引脚，只能配置为输入 */
    gpio_set_direction((gpio_num_t)BSP_GT30L32S4W_MISO_PIN, GPIO_MODE_INPUT);
    esp_rom_gpio_connect_in_signal(BSP_GT30L32S4W_MISO_PIN, sig->spiq_in, false);

    /* CS#：使用 SPI 外设的硬件片选信号 CS0 */
    gpio_set_direction((gpio_num_t)BSP_GT30L32S4W_CS_PIN, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level((gpio_num_t)BSP_GT30L32S4W_CS_PIN, 1);
    esp_rom_gpio_connect_out_signal(BSP_GT30L32S4W_CS_PIN, sig->spics_out[0], false, false);
    esp_rom_gpio_connect_in_signal(BSP_GT30L32S4W_CS_PIN, sig->spics_in, false);

#if (BSP_GT30L32S4W_HOLD_PIN >= 0)
    gpio_set_level((gpio_num_t)BSP_GT30L32S4W_HOLD_PIN, 1);
#endif

    return ESP_OK;
#endif /* BSP_GT30L32S4W_USE_BITBANG */
}

esp_err_t bsp_gt30l32s4w_pins_deinit(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

#if BSP_GT30L32S4W_USE_BITBANG
    /* bit-bang：结束时 SCLK 回到空闲高电平 */
    return gt_gpio_config(true);
#else
    gt30l32s4w_pins_to_gpio();
    return ESP_OK;
#endif
}

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

#if BSP_GT30L32S4W_USE_BITBANG
    (void)clock_hz;   /* bit-bang 速率由 BSP_GT30L32S4W_BITBANG_DELAY_US 决定 */

    esp_err_t gpio_err = gt_gpio_config(true);
    if (gpio_err != ESP_OK) {
        return gpio_err;
    }

    s_clock_hz = (BSP_GT30L32S4W_BITBANG_DELAY_US > 0)
                     ? (uint32_t)(1000000u / (2u * BSP_GT30L32S4W_BITBANG_DELAY_US))
                     : 5000000u;
    s_ready = true;

    ESP_LOGI(TAG, "ready (bit-bang): SCLK=%d CS=%d MOSI=%d MISO=%d ~%" PRIu32 "Hz",
             BSP_GT30L32S4W_SCLK_PIN, BSP_GT30L32S4W_CS_PIN,
             BSP_GT30L32S4W_MOSI_PIN, BSP_GT30L32S4W_MISO_PIN, s_clock_hz);

#if (BSP_GT30L32S4W_SCOPE_TOGGLE_MS > 0)
    /* 测线模式：上电先打一段稳定方波，示波器挂在 CS#/SCLK 上即可确认驱动与通断 */
    (void)bsp_gt30l32s4w_pin_toggle_test(BSP_GT30L32S4W_SCOPE_TOGGLE_MS);
#endif

    /*
     * 上电先确认 CS# 有没有被板子短路：CS# 若被拉死在 0，芯片永远收不到
     * “片选拉高”这个沿，后面的读操作全部白跑，而且长期驱动高电平会灌电流。
     * 判定为短路后本层会把 CS# 置高阻，bus probe 也会跳过（扫描无意义）。
     */
    if (!gt_line_check((gpio_num_t)BSP_GT30L32S4W_CS_PIN, "CS#")) {
        s_cs_shorted = true;
        ESP_LOGE(TAG, "CS# (GPIO%d) 被外部拉死在低电平：CS# 引脚改为高阻，"
                 "bus probe 将被跳过。请检查 CS# 走线 / 字库 pin1 对地是否短路。",
                 BSP_GT30L32S4W_CS_PIN);
        (void)gt_gpio_config(true);
    }

    return ESP_OK;
#else
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

    /*
     * 初始化完成后把引脚置于空闲态（CS 高、SCLK 高、MOSI 高），
     * 避免总线上出现误触发的时钟沿。
     */
    (void)bsp_gt30l32s4w_pins_deinit();

    ESP_LOGI(TAG, "ready: SCLK=%d CS=%d MOSI=%d MISO=%d clock=%" PRIu32 "Hz",
             BSP_GT30L32S4W_SCLK_PIN, BSP_GT30L32S4W_CS_PIN,
             BSP_GT30L32S4W_MOSI_PIN, BSP_GT30L32S4W_MISO_PIN, clock_hz);
    return ESP_OK;
#endif /* BSP_GT30L32S4W_USE_BITBANG */
}

esp_err_t bsp_gt30l32s4w_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

#if BSP_GT30L32S4W_USE_BITBANG
    s_ready = false;

    /* 交还 EPD 软件 SPI：SCLK 空闲高 */
    esp_err_t err = gt_gpio_config(true);

    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    return err;
#else
    esp_err_t err = spi_bus_remove_device(s_spi_dev);
    s_spi_dev = NULL;

    esp_err_t bus_err = spi_bus_free(BSP_GT30L32S4W_SPI_HOST);
    if (err == ESP_OK) {
        err = bus_err;
    }

    s_ready = false;

    /*
     * spi_bus_free() 内部会把这些引脚 gpio_reset_pin() 成输入状态，
     * 重新配置成空闲高电平的普通 GPIO，保证复用引脚的 EPD 软件 SPI 仍可用。
     */
    gt30l32s4w_pins_to_gpio();

    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    return err;
#endif
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

/* ========================= 引脚翻转测试（示波器 / 万用表测线用） ========================= */

#if BSP_GT30L32S4W_USE_BITBANG

/**
 * @brief  驱动引脚后回读 pad 真实电平（判断是否被外部短路/强驱动）
 * @note   必须用 GPIO_MODE_INPUT_OUTPUT，否则输入通路关闭，读不到外部电平
 * @return true pad 电平跟随驱动（引脚正常）；false 引脚被外部拉死
 */
static bool gt_line_check(gpio_num_t pin, const char *name)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "line %s (GPIO%d): config failed", name, (int)pin);
        return false;
    }

    gpio_set_level(pin, 1);
    esp_rom_delay_us(200);
    const int hi = gpio_get_level(pin);

    gpio_set_level(pin, 0);
    esp_rom_delay_us(200);
    const int lo = gpio_get_level(pin);

    gpio_set_level(pin, 1);

    /*
     * 再用“纯输入 + 内部上拉”判一次：
     *   上拉（约 45kΩ）能读到 1  → 引脚/键合正常，只是被外部强拉低或短路
     *   上拉也读到 0             → 该网络被硬短路到 GND（或对地 <几十 Ω）
     */
    const gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    int pu = -1;
    if (gpio_config(&in_cfg) == ESP_OK) {
        esp_rom_delay_us(500);
        pu = gpio_get_level(pin);
    }

    if (hi == 1 && lo == 0) {
        ESP_LOGI(TAG, "line %s (GPIO%d): drive 1/0 -> read 1/0, OK", name, (int)pin);
    } else if (pu == 0) {
        ESP_LOGE(TAG, "line %s (GPIO%d): 驱动高仍读到 0，内部上拉也读到 0 → "
                 "该网络被硬短路到 GND（或对地只有几欧）", name, (int)pin);
    } else {
        ESP_LOGE(TAG, "line %s (GPIO%d): drive 1/0 -> read %d/%d，内部上拉读到 %d → "
                 "外部有强下拉/别的器件在拉低（不是硬短路，也不是 MCU 没驱动）",
                 name, (int)pin, hi, lo, pu);
    }

    /* 恢复成纯输出（调用方之后会重新 gt_gpio_config 复位空闲电平） */
    const gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&out_cfg);
    gpio_set_level(pin, 1);

    return (hi == 1 && lo == 0);
}

esp_err_t bsp_gt30l32s4w_pin_toggle_test(uint32_t duration_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms == 0) {
        duration_ms = 1000;
    }

    esp_err_t err = gt_gpio_config(false);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * 第 1 步：驱动后立刻回读 pad 的真实电平。
     *   纯 GPIO_MODE_OUTPUT 会关掉输入通路，gpio_get_level() 读到的是输出寄存器
     *   而不是引脚实际电平；要判断“脚有没有被外部拉死”，必须用 INPUT_OUTPUT。
     *   结果含义：
     *     drive 1/0 -> read 1/0  : MCU 把引脚驱动起来了（问题在走线/芯片侧）
     *     drive 1/0 -> read 0/0  : 该网络被外部短路到 GND（或这一脚根本没接到 MCU）
     *     drive 1/0 -> read 1/1  : 被外部上拉到 3V3 / 有别的器件强驱动高
     */
    if (!s_cs_shorted &&
        !gt_line_check((gpio_num_t)BSP_GT30L32S4W_CS_PIN, "CS#")) {
        s_cs_shorted = true;
        ESP_LOGE(TAG, "CS# 网络不可用：GPIO%d 已被外部拉死，后续该脚保持高阻不再驱动"
                 "（避免对地灌电流）。请查 CS# 走线 / 字库芯片 pin1 是否对地短路。",
                 BSP_GT30L32S4W_CS_PIN);
    }
    (void)gt_line_check((gpio_num_t)BSP_GT30L32S4W_SCLK_PIN, "SCLK");
    (void)gt_line_check((gpio_num_t)BSP_GT30L32S4W_MOSI_PIN, "SI");

    /* 回读阶段用的是 INPUT_OUTPUT，这里恢复成纯输出再打波形 */
    err = gt_gpio_config(false);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * 第 2 步：两段波形
     *   前段 100Hz（周期 10ms，CS# 低 0.32ms / 高 9.68ms）：
     *     示波器下降沿触发、1ms/div 直接抓到一串脉冲。
     *   后段 2Hz（CS# 低 250ms / 高 250ms，占空比 50%）：
     *     任何一个档位的示波器、甚至万用表都看得见——
     *     万用表直流档应读到约 1.6V（50% x 3.3V），读到恒定 0V 就是被短路到地。
     */
    const uint32_t fast_ms    = duration_ms / 3u;
    const uint32_t slow_ms    = duration_ms - fast_ms;
    const uint32_t fast_cycles = fast_ms / 10u;
    const uint32_t slow_cycles = slow_ms / 500u;

    ESP_LOGW(TAG, "pin toggle test: CS=GPIO%d SCLK=GPIO%d SI=GPIO%d, %" PRIu32 "ms "
             "(前段 100Hz x %" PRIu32 " 脉冲, 后段 2Hz x %" PRIu32 " 周期)",
             BSP_GT30L32S4W_CS_PIN, BSP_GT30L32S4W_SCLK_PIN, BSP_GT30L32S4W_MOSI_PIN,
             duration_ms, fast_cycles, slow_cycles);

    /* 前段：100Hz，方便示波器触发 */
    for (uint32_t n = 0; n < fast_cycles; n++) {
        GT_CS_LOW();
        for (int bit = 7; bit >= 0; bit--) {
            GT_SCLK_LOW();
            if (0xA5u & (1u << bit)) {
                GT_MOSI_HIGH();
            } else {
                GT_MOSI_LOW();
            }
            esp_rom_delay_us(20);
            GT_SCLK_HIGH();
            esp_rom_delay_us(20);
        }
        GT_SCLK_LOW();
        GT_CS_HIGH();
        GT_MOSI_HIGH();
        /* 8 bit x 40us = 320us 已在上面花掉，补 9680us 凑成 10ms 周期（100Hz） */
        esp_rom_delay_us(9680);
    }

    /* 后段：2Hz 慢速方波，示波器/万用表都能明确读到高低两种电平 */
    for (uint32_t n = 0; n < slow_cycles; n++) {
        GT_CS_LOW();
        GT_SCLK_LOW();
        GT_MOSI_LOW();
        esp_rom_delay_us(250000);

        GT_CS_HIGH();
        GT_SCLK_HIGH();
        GT_MOSI_HIGH();
        esp_rom_delay_us(250000);
    }

    ESP_LOGW(TAG, "pin toggle test done");

    /* 回到空闲态：CS 高、SCLK 高、MOSI 高（与 pins_deinit 一致） */
    return gt_gpio_config(true);
}

#else /* !BSP_GT30L32S4W_USE_BITBANG */

esp_err_t bsp_gt30l32s4w_pin_toggle_test(uint32_t duration_ms)
{
    (void)duration_ms;
    ESP_LOGW(TAG, "pin toggle test is only available in bit-bang mode");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* BSP_GT30L32S4W_USE_BITBANG */

/* ========================= 引脚自动探测（诊断用） ========================= */

/*
 * 字库读不回数据时不再靠猜：把“可能当 CS / MISO 用”的引脚全扫一遍，
 * 每次读 8x16 'A' 的前 4 个数据字节（ROM 中为 00 10 28 28 -> 0x00102828），
 * 看哪一组 SCLK/SI/CS/MISO 能拿回这个值。
 *
 * 判据（同一组连读两次必须一致，避免浮空脚的随机噪声）：
 *   0x00102828             -> 命中，这就是实际接线
 *   全 0x00000000 / 全 1   -> 该引脚上没有有效数据
 */
#if BSP_GT30L32S4W_USE_BITBANG

#define GT_PROBE_EXPECT   0x00102828u   /* 8x16 'A' 的第 1~4 个数据字节 */
/*
 * 采样位数：cmd 1B + addr 3B = 32 个时钟之后才是数据，
 * 再打 4 个字节（32 个时钟）即可采到数据字节 0~3。
 */
#define GT_PROBE_CLOCKS   64

/* 可能作为 MISO 的引脚（避开 flash 6~11、UART 1/3、EPD 的 4/13/14/25/26/27、strapping 12） */
static const uint8_t s_probe_miso_pins[] = {
    35, 34, 39, 36, 37, 38, 32, 33, 19, 18, 5, 2, 0, 15, 16, 17,
};

/* 可能作为 CS 的引脚（避开 flash/UART/EPD 引脚、字库 SCLK=26、strapping 12） */
static const uint8_t s_probe_cs_pins[] = {
    23, 32, 33, 19, 18, 5, 2, 0, 15, 16, 17,
};

/* 可能的 SCLK/SI 组合：字库实际使用的 26/25，以及早期误判的 22/21 */
static const uint8_t s_probe_clk_pins[][2] = {
    { BSP_GT30L32S4W_SCLK_PIN, BSP_GT30L32S4W_MOSI_PIN },
    { 22, 21 },
};

#define GT_PROBE_MISO_NUM  (sizeof(s_probe_miso_pins) / sizeof(s_probe_miso_pins[0]))
#define GT_PROBE_CS_NUM    (sizeof(s_probe_cs_pins) / sizeof(s_probe_cs_pins[0]))
#define GT_PROBE_CLK_NUM   (sizeof(s_probe_clk_pins) / sizeof(s_probe_clk_pins[0]))

#if BSP_GT30L32S4W_PROBE_FULL_SCAN
/*
 * 全组合扫描用的引脚集合（未被本工程占用的普通 GPIO）。
 * 特意避开 16/17（WROVER 模块上是 PSRAM 的 CS/CLK）。
 * SCLK/SI/CS 任取其中 3 个，遍历所有排列，只在前面都失败时才跑。
 */
static const uint8_t s_probe_bus_pins[] = {
    0, 2, 5, 15, 18, 19, 21, 22, 23, 32, 33,
};
#define GT_PROBE_BUS_NUM   (sizeof(s_probe_bus_pins) / sizeof(s_probe_bus_pins[0]))
#endif
static uint32_t s_probe_cap_a[GT_PROBE_MISO_NUM];
static uint32_t s_probe_cap_b[GT_PROBE_MISO_NUM];

static void gt_probe_pin_input(uint8_t pin)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&cfg);
}

static void gt_probe_pin_output(uint8_t pin, int level)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_set_level((gpio_num_t)pin, level);
    (void)gpio_config(&cfg);
    gpio_set_level((gpio_num_t)pin, level);
}

/**
 * @brief  用给定引脚组合读 4 个数据字节，同时采样所有候选 MISO 引脚
 * @param  capture 长度 GT_PROBE_MISO_NUM，采样结果（最后 32 个采样位 = 数据字节 0~3）
 */
static void gt_probe_read_once(uint8_t sclk, uint8_t mosi, uint8_t cs, uint32_t *capture)
{
    static const uint8_t hdr[8] = { 0x03, 0x1D, 0xD9, 0x90, 0x00, 0x00, 0x00, 0x00 };

    for (size_t i = 0; i < GT_PROBE_MISO_NUM; i++) {
        capture[i] = 0;
    }

    gpio_set_level((gpio_num_t)cs, 0);
    esp_rom_delay_us(2);

    for (int b = 0; b < GT_PROBE_CLOCKS; b++) {
        gpio_set_level((gpio_num_t)sclk, 0);
        gpio_set_level((gpio_num_t)mosi, (hdr[b >> 3] >> (7 - (b & 7))) & 1);
        esp_rom_delay_us(1);
        gpio_set_level((gpio_num_t)sclk, 1);
        for (size_t i = 0; i < GT_PROBE_MISO_NUM; i++) {
            uint8_t pin = s_probe_miso_pins[i];
            /* 自己驱动着的引脚采样没有意义 */
            if (pin == sclk || pin == mosi || pin == cs) {
                continue;
            }
            capture[i] = (capture[i] << 1) | (uint32_t)gpio_get_level((gpio_num_t)pin);
        }
        esp_rom_delay_us(1);
    }

    gpio_set_level((gpio_num_t)sclk, 0);
    gpio_set_level((gpio_num_t)cs, 1);
    esp_rom_delay_us(2);
}

esp_err_t bsp_gt30l32s4w_bus_probe(void)
{
    if (s_cs_shorted) {
        ESP_LOGE(TAG, "bus probe skipped: CS# (GPIO%d) 已被判定对地短路，换脚扫描没有意义；"
                 "请先修复 CS# 走线或更换字库芯片",
                 BSP_GT30L32S4W_CS_PIN);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "bus probe: read 8x16 'A' @0x1DD990, expect 0x%08" PRIX32,
             (uint32_t)GT_PROBE_EXPECT);

    bool found = false;

    /* 当前 MISO 在候选表里的下标（表首通常是 35） */
    int hit_idx = -1;
    for (size_t i = 0; i < GT_PROBE_MISO_NUM; i++) {
        if (s_probe_miso_pins[i] == BSP_GT30L32S4W_MISO_PIN) {
            hit_idx = (int)i;
        }
    }

    /*
     * 第 1 步：先按当前引脚定义直接读一次（排除偶发失败）。
     * 第 2 步：逐个把“空闲候选脚”拉高再读 —— HOLD#（pin7）若接到了某个 GPIO
     *         且悬空/被拉低，芯片会进入挂起状态、SO 变高阻，表现就是读回全 0。
     *         哪根线拉高后能读回正确数据，它就是 HOLD#。
     */
    for (size_t i = 0; i < GT_PROBE_MISO_NUM; i++) {
        gt_probe_pin_input(s_probe_miso_pins[i]);
    }
    for (size_t i = 0; i < GT_PROBE_CS_NUM; i++) {
        gt_probe_pin_input(s_probe_cs_pins[i]);
    }
    gt_probe_pin_output(BSP_GT30L32S4W_SCLK_PIN, 0);
    gt_probe_pin_output(BSP_GT30L32S4W_MOSI_PIN, 1);
    gt_probe_pin_output(BSP_GT30L32S4W_CS_PIN, 1);

    if (hit_idx >= 0) {
        gt_probe_read_once(BSP_GT30L32S4W_SCLK_PIN, BSP_GT30L32S4W_MOSI_PIN,
                           BSP_GT30L32S4W_CS_PIN, s_probe_cap_a);
        gt_probe_read_once(BSP_GT30L32S4W_SCLK_PIN, BSP_GT30L32S4W_MOSI_PIN,
                           BSP_GT30L32S4W_CS_PIN, s_probe_cap_b);

        if (s_probe_cap_a[hit_idx] == s_probe_cap_b[hit_idx] &&
            s_probe_cap_a[hit_idx] == GT_PROBE_EXPECT) {
            ESP_LOGI(TAG, "PROBE HIT: 当前引脚就能读到正确数据（自检失败属偶发）");
            found = true;
        } else {
            for (size_t i = 0; i < GT_PROBE_CS_NUM && !found; i++) {
                const uint8_t spare = s_probe_cs_pins[i];
                if (spare == BSP_GT30L32S4W_SCLK_PIN || spare == BSP_GT30L32S4W_MOSI_PIN ||
                    spare == BSP_GT30L32S4W_CS_PIN || spare == BSP_GT30L32S4W_MISO_PIN) {
                    continue;
                }

                gt_probe_pin_output(spare, 1);

                gt_probe_read_once(BSP_GT30L32S4W_SCLK_PIN, BSP_GT30L32S4W_MOSI_PIN,
                                   BSP_GT30L32S4W_CS_PIN, s_probe_cap_a);
                gt_probe_read_once(BSP_GT30L32S4W_SCLK_PIN, BSP_GT30L32S4W_MOSI_PIN,
                                   BSP_GT30L32S4W_CS_PIN, s_probe_cap_b);

                if (s_probe_cap_a[hit_idx] == s_probe_cap_b[hit_idx] &&
                    s_probe_cap_a[hit_idx] == GT_PROBE_EXPECT) {
                    ESP_LOGI(TAG, "PROBE HIT: 拉高 GPIO%u 后字库恢复响应 -> 它就是 HOLD#，"
                             "请设置 BSP_GT30L32S4W_HOLD_PIN = %u", spare, spare);
                    found = true;
                }

                gt_probe_pin_input(spare);
            }
        }
    }

    for (size_t c = 0; c < GT_PROBE_CLK_NUM && !found; c++) {
        const uint8_t sclk = s_probe_clk_pins[c][0];
        const uint8_t mosi = s_probe_clk_pins[c][1];

        /* 候选脚先全部复位成输入，再逐个当 CS 试 */
        for (size_t i = 0; i < GT_PROBE_MISO_NUM; i++) {
            gt_probe_pin_input(s_probe_miso_pins[i]);
        }
        for (size_t i = 0; i < GT_PROBE_CS_NUM; i++) {
            gt_probe_pin_input(s_probe_cs_pins[i]);
        }

        gt_probe_pin_output(sclk, 0);
        gt_probe_pin_output(mosi, 1);

        for (size_t cs_idx = 0; cs_idx < GT_PROBE_CS_NUM; cs_idx++) {
            const uint8_t cs = s_probe_cs_pins[cs_idx];
            if (cs == sclk || cs == mosi) {
                continue;
            }

            gt_probe_pin_output(cs, 1);

            gt_probe_read_once(sclk, mosi, cs, s_probe_cap_a);
            gt_probe_read_once(sclk, mosi, cs, s_probe_cap_b);

            for (size_t i = 0; i < GT_PROBE_MISO_NUM; i++) {
                const uint32_t v = s_probe_cap_a[i];
                if (v != s_probe_cap_b[i]) {
                    continue;                   /* 两次不一致：浮空/噪声，忽略 */
                }
                if (v == GT_PROBE_EXPECT) {
                    ESP_LOGI(TAG, "PROBE HIT: SCLK=%u SI=%u CS=%u SO=%u -> 0x%08" PRIX32,
                             sclk, mosi, cs, s_probe_miso_pins[i], v);
                    found = true;
                    break;
                }
                if (v != 0x00000000u && v != 0xFFFFFFFFu) {
                    ESP_LOGW(TAG, "probe SCLK=%u SI=%u CS=%u: SO%u=0x%08" PRIX32
                             " (有数据但不对)",
                             sclk, mosi, cs, s_probe_miso_pins[i], v);
                }
            }

            gt_probe_pin_input(cs);
        }

        gt_probe_pin_input(sclk);
        gt_probe_pin_input(mosi);
    }

    /*
     * 第 2b 步：把所有空闲候选脚一起拉高再读。
     * 有些板子用“电源使能 + HOLD#”两个脚控制字库，单脚拉高看不出效果。
     */
    if (!found) {
        for (size_t i = 0; i < GT_PROBE_CS_NUM; i++) {
            const uint8_t pin = s_probe_cs_pins[i];
            if (pin == BSP_GT30L32S4W_SCLK_PIN || pin == BSP_GT30L32S4W_MOSI_PIN ||
                pin == BSP_GT30L32S4W_CS_PIN || pin == BSP_GT30L32S4W_MISO_PIN) {
                continue;
            }
            gt_probe_pin_output(pin, 1);
        }
        gt_probe_pin_output(BSP_GT30L32S4W_CS_PIN, 1);

        for (size_t c = 0; c < GT_PROBE_CLK_NUM && !found; c++) {
            const uint8_t sclk = s_probe_clk_pins[c][0];
            const uint8_t mosi = s_probe_clk_pins[c][1];

            gt_probe_pin_output(sclk, 0);
            gt_probe_pin_output(mosi, 1);
            gt_probe_read_once(sclk, mosi, BSP_GT30L32S4W_CS_PIN, s_probe_cap_a);
            gt_probe_read_once(sclk, mosi, BSP_GT30L32S4W_CS_PIN, s_probe_cap_b);
            gt_probe_pin_input(sclk);
            gt_probe_pin_input(mosi);

            if (hit_idx >= 0 &&
                s_probe_cap_a[hit_idx] == GT_PROBE_EXPECT &&
                s_probe_cap_b[hit_idx] == GT_PROBE_EXPECT) {
                ESP_LOGI(TAG, "PROBE HIT: 把所有空闲脚一起拉高后字库响应"
                         "（存在电源使能/HOLD# 之类控制脚，需逐个排查）");
                found = true;
            }
        }

        for (size_t i = 0; i < GT_PROBE_CS_NUM; i++) {
            gt_probe_pin_input(s_probe_cs_pins[i]);
        }
    }

    /*
     * 第 3 步：全组合扫描（只在前面都失败时跑，约 1 秒）。
     * 把上面那组 GPIO 任意 3 个当作 SCLK/SI/CS，看是否有一种组合能读回
     * 0x00102828 —— 命中即说明字库实际接在别的引脚上。
     */
#if BSP_GT30L32S4W_PROBE_FULL_SCAN
    if (!found && hit_idx >= 0) {
        int noise_logged = 0;

        ESP_LOGW(TAG, "bus probe: full pin scan (SCLK/SI/CS combinations), ~1s ...");

        for (size_t a = 0; a < GT_PROBE_BUS_NUM && !found; a++) {
            for (size_t b = 0; b < GT_PROBE_BUS_NUM && !found; b++) {
                if (a == b) {
                    continue;
                }
                for (size_t c = 0; c < GT_PROBE_BUS_NUM && !found; c++) {
                    if (c == a || c == b) {
                        continue;
                    }

                    const uint8_t sclk = s_probe_bus_pins[a];
                    const uint8_t mosi = s_probe_bus_pins[b];
                    const uint8_t cs   = s_probe_bus_pins[c];

                    for (size_t i = 0; i < GT_PROBE_MISO_NUM; i++) {
                        gt_probe_pin_input(s_probe_miso_pins[i]);
                    }
                    for (size_t i = 0; i < GT_PROBE_BUS_NUM; i++) {
                        gt_probe_pin_input(s_probe_bus_pins[i]);
                    }
                    gt_probe_pin_output(sclk, 0);
                    gt_probe_pin_output(mosi, 1);
                    gt_probe_pin_output(cs, 1);

                    gt_probe_read_once(sclk, mosi, cs, s_probe_cap_a);

                    for (size_t i = 0; i < GT_PROBE_MISO_NUM && !found; i++) {
                        const uint32_t v = s_probe_cap_a[i];
                        if (v == 0x00000000u || v == 0xFFFFFFFFu) {
                            continue;
                        }
                        /* 有反应就再读一次确认不是噪声 */
                        gt_probe_read_once(sclk, mosi, cs, s_probe_cap_b);
                        if (s_probe_cap_b[i] != v) {
                            continue;
                        }
                        if (v == GT_PROBE_EXPECT) {
                            ESP_LOGI(TAG, "PROBE HIT: SCLK=%u SI=%u CS=%u SO=%u -> 0x%08" PRIX32,
                                     sclk, mosi, cs, s_probe_miso_pins[i], v);
                            found = true;
                            break;
                        }
                        if (noise_logged < 10) {
                            noise_logged++;
                            ESP_LOGW(TAG, "probe SCLK=%u SI=%u CS=%u: SO%u=0x%08" PRIX32
                                     " (稳定但非预期)", sclk, mosi, cs,
                                     s_probe_miso_pins[i], v);
                        }
                    }
                }
            }
        }
    }
#endif /* BSP_GT30L32S4W_PROBE_FULL_SCAN */

    /* 恢复：墨水屏软件 SPI 引脚回到空闲高，字库引脚按常规配置 */
    gt_probe_pin_output(26, 1);
    gt_probe_pin_output(25, 1);
    for (size_t i = 0; i < GT_PROBE_MISO_NUM; i++) {
        gt_probe_pin_input(s_probe_miso_pins[i]);
    }
    (void)gt_gpio_config(true);

    if (!found) {
        ESP_LOGE(TAG, "probe: no font chip response on any pin combo");
        ESP_LOGE(TAG, "probe: check 3V3/GND, HOLD#(pin7) 2K pull-up to 3V3,");
        ESP_LOGE(TAG, "probe: and continuity of CS#/SCLK/SI/SO to the MCU");
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

#else /* !BSP_GT30L32S4W_USE_BITBANG */

esp_err_t bsp_gt30l32s4w_bus_probe(void)
{
    ESP_LOGW(TAG, "bus probe is only available in bit-bang mode");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* BSP_GT30L32S4W_USE_BITBANG */


