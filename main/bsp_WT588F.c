/*
 * @Description: WT588F 语音芯片 BSP 层实现（纯 GPIO 位时序，不占用硬件外设）
 *
 * 时序全部按规格书 V1.03 第 6 章实现，参数见 bsp_WT588F.h 顶部说明。
 * 之所以不用 ESP32 的硬件 SPI/UART 发码：
 *   1. 一线/两线串口的“位”是 200us~600us 的高/低电平组合，不是标准 SPI 波形；
 *   2. 本工程 tick=100Hz，vTaskDelay 粒度 10ms，只能用 esp_rom_delay_us 忙等。
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "bsp_WT588F.h"

static const char *TAG = "WT588F_BSP";

/* ========================= 引脚合法性检查（编译期） ========================= */

/* 通信/片选脚都要能输出：0~33（GPIO34~39 只能输入） */
#if (BSP_WT588F_DATA1_CLK2_PIN < 0) || (BSP_WT588F_DATA1_CLK2_PIN > 33)
#error "BSP_WT588F_DATA1_CLK2_PIN: 请填 0~33（可输出的 GPIO 编号）"
#endif
#if (BSP_WT588F_DATA1_CLK2_PIN >= 6) && (BSP_WT588F_DATA1_CLK2_PIN <= 11)
#error "BSP_WT588F_DATA1_CLK2_PIN: GPIO6~GPIO11 连接 SPI Flash，不能使用"
#endif
#if (BSP_WT588F_CS_PIN >= 0) && ((BSP_WT588F_CS_PIN > 33) || \
    ((BSP_WT588F_CS_PIN >= 6) && (BSP_WT588F_CS_PIN <= 11)))
#error "BSP_WT588F_CS_PIN: 请填 0~33 且避开 GPIO6~GPIO11（-1 表示没有门控电路）"
#endif
#if (BSP_WT588F_DATA2_PIN >= 0) && ((BSP_WT588F_DATA2_PIN > 33) || \
    ((BSP_WT588F_DATA2_PIN >= 6) && (BSP_WT588F_DATA2_PIN <= 11)))
#error "BSP_WT588F_DATA2_PIN: 请填 0~33 且避开 GPIO6~GPIO11（-1 表示不接）"
#endif
#if (BSP_WT588F_BUSY_PIN >= 0) && ((BSP_WT588F_BUSY_PIN > 39) || \
    ((BSP_WT588F_BUSY_PIN >= 6) && (BSP_WT588F_BUSY_PIN <= 11)))
#error "BSP_WT588F_BUSY_PIN: 请填 0~39 且避开 GPIO6~GPIO11（-1 表示不接）"
#endif

/* ========================= 内部状态 ========================= */

/* 一帧约 13ms，多任务同时发码会互相打断，必须串行化 */
static SemaphoreHandle_t s_lock = NULL;
static bool              s_ready = false;
static bsp_wt588f_wire_t s_wire  = BSP_WT588F_DEFAULT_WIRE_MODE;

/* 信息回读状态：UART 是否已接管 BUSY 脚、驱动是否由本层安装 */
static bool              s_info_uart_on    = false;
static bool              s_info_uart_owned = false;

/* ========================= 延时 / 引脚辅助 ========================= */

static inline void wt_delay_us(uint32_t us)
{
    if (us) {
        esp_rom_delay_us(us);
    }
}

static inline void wt_delay_ms(uint32_t ms)
{
    if (ms) {
        esp_rom_delay_us(ms * 1000U);
    }
}

/* DATA1/CLK2：经 Q9 门控到芯片 PIN2 —— 一线模式的 DATA，两线模式的 CLK */
#define WT_DATA1(x)     gpio_set_level((gpio_num_t)BSP_WT588F_DATA1_CLK2_PIN, (x) ? 1 : 0)

/* DATA2：两线模式的 DATA（一线模式会释放成输入，交还给字库/EPD） */
#if (BSP_WT588F_DATA2_PIN >= 0)
#define WT_DATA2(x)     gpio_set_level((gpio_num_t)BSP_WT588F_DATA2_PIN, (x) ? 1 : 0)
#else
#define WT_DATA2(x)     ((void)(x))
#endif

/**
 * @brief  开/关 CS 门控（Q9 集电极）
 *
 * 开 = 选中语音芯片：AU_CLK_O 跟随 GPIO26，芯片能收到位时序；
 * 关 = AU_CLK_O 被 R21 10K 拉到地，语音芯片与共用的 GPIO26/25 完全隔离。
 */
static inline void wt_cs_write(bool on)
{
#if (BSP_WT588F_CS_PIN >= 0)
    int level = on ? 1 : 0;
#if (BSP_WT588F_CS_ACTIVE_LEVEL == 0)
    level = !level;
#endif
    gpio_set_level((gpio_num_t)BSP_WT588F_CS_PIN, level);
#else
    (void)on;   /* 板上没有门控电路：CLK/DATA 直连芯片 */
#endif
}

/**
 * @brief  占用 / 释放 DATA2（GPIO25，与字库 SI、EPD SDA 是同一根线）
 *
 * 一线模式用不到 DATA2，必须释放成高阻输入：否则把 GPIO25 驱动成低会破坏
 * 字库/EPD 的空闲电平。只有切到两线模式才把它配置成推挽输出。
 */
static void wt_data2_use(bool used)
{
#if (BSP_WT588F_DATA2_PIN >= 0)
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BSP_WT588F_DATA2_PIN),
        .mode         = used ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&cfg);
#else
    (void)used;
#endif
}

static esp_err_t wt_lock(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void wt_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

/* ========================= 引脚初始化 ========================= */

/**
 * @brief  配置通信引脚：DATA1/CLK2 + CS 推挽输出，BUSY 输入，DATA2 由调用方决定
 *
 * 规格书要求 IO2（DATA1/CLK2）不能加上拉，所以这里把内部上/下拉全部关掉。
 * 芯片 IO 自带 220K 下拉，MCU 侧再上拉会干扰上电时的模式判断。
 * 注意：DATA2（GPIO25）与字库 SI / EPD SDA 共用，不在这里配置，见 wt_data2_use()。
 */
static esp_err_t wt_pins_config(void)
{
    uint64_t out_mask = (1ULL << BSP_WT588F_DATA1_CLK2_PIN);
#if (BSP_WT588F_CS_PIN >= 0)
    out_mask |= (1ULL << BSP_WT588F_CS_PIN);
#endif

    gpio_config_t io_cfg = {
        .pin_bit_mask = out_mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio config data pins failed: %s", esp_err_to_name(err));
        return err;
    }

#if (BSP_WT588F_BUSY_PIN >= 0)
    gpio_config_t busy_cfg = {
        .pin_bit_mask = (1ULL << BSP_WT588F_BUSY_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&busy_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio config busy pin failed: %s", esp_err_to_name(err));
        return err;
    }
#endif

    return ESP_OK;
}

/**
 * @brief  总线置于空闲电平并关闭门控（调用方需已持锁或处于初始化阶段）
 *
 * 一线：DATA1(CLK 脚) 高，门控关 —— 芯片只看到低电平，规格书要求“发码前先高 5ms”
 *       正好由每帧开头的拉高满足；
 * 两线：门控关 + CLK 高 + DATA 高。
 * DATA2 在一线模式下已释放为输入，这里绝不驱动它（与字库 SI / EPD SDA 共用）。
 */
static void wt_bus_idle(void)
{
    wt_cs_write(false);

    if (s_wire == BSP_WT588F_WIRE_TWO) {
        WT_DATA2(1);
    }
    WT_DATA1(1);
}

/* ========================= 一线串口位时序 ========================= */

/**
 * @brief  一线串口发送一个字节（调用方需已持锁且已打开 CS 门控）
 *
 * 时序：DATA 高 5ms -> 低 5ms -> 8 位（低位先发）-> 低 + 2ms 间隔
 *   位 1：高 600us / 低 200us
 *   位 0：高 200us / 低 600us
 * 注意：DATA1 经 Q9 后高电平只有 3.3V - Vbe ≈ 2.6V，量程/逻辑判断按此计算。
 */
static void wt_send_byte_onewire(uint8_t byte)
{
    /* 1) 发码前先拉高 >= 5ms（规格书要求，否则某些条件下不响应） */
    WT_DATA1(1);
    wt_delay_ms(BSP_WT588F_IDLE_HIGH_MS);

    /* 2) 起帧：拉低 5ms（允许 5~20ms） */
    WT_DATA1(0);
    wt_delay_ms(BSP_WT588F_START_LOW_MS);

    /* 3) 8 位数据，低位先发，“1”对应长高电平 */
    for (int i = 0; i < 8; i++) {
        if (byte & 0x01U) {
            WT_DATA1(1);
            wt_delay_us(BSP_WT588F_BIT_ONE_HIGH_US);
            WT_DATA1(0);
            wt_delay_us(BSP_WT588F_BIT_ONE_LOW_US);
        } else {
            WT_DATA1(1);
            wt_delay_us(BSP_WT588F_BIT_ZERO_HIGH_US);
            WT_DATA1(0);
            wt_delay_us(BSP_WT588F_BIT_ZERO_LOW_US);
        }
        byte >>= 1;
    }

    /* 4) 帧尾保持低电平（芯片内部 220K 下拉，低电平不灌电流），并留出指令间隔 */
    WT_DATA1(0);
    wt_delay_ms(BSP_WT588F_CMD_GAP_MS);
}

/* ========================= 两线串口位时序 ========================= */

/**
 * @brief  两线串口发送一个字节（调用方需已持锁且已打开 CS 门控）
 *
 * 时序：CLK 高 5ms（DATA 置高）-> CLK 低 5ms -> 8 位
 *       每位：DATA 在 CLK 低电平期间更新，CLK 高/低各 350us，上升沿采样
 * 注意：CLK 经 Q9 后高电平 ≈2.6V；DATA（GPIO25）是直连，不受门控影响。
 */
static void wt_send_byte_twowire(uint8_t byte)
{
#if (BSP_WT588F_DATA2_PIN < 0)
    /* 没有 DATA2 引脚：两线模式不可用 */
    ESP_LOGE(TAG, "two-wire mode needs BSP_WT588F_DATA2_PIN");
    (void)byte;
#else
    /* 1) CLK 拉高 >= 5ms，DATA 同时置高（空闲电平） */
    WT_DATA2(1);
    WT_DATA1(1);
    wt_delay_ms(BSP_WT588F_IDLE_HIGH_MS);

    /* 2) 起帧：CLK 拉低 5ms，数据在 CLK 低电平期间准备 */
    WT_DATA1(0);
    wt_delay_ms(BSP_WT588F_START_LOW_MS);

    /* 3) 8 位数据，低位先发，上升沿采样 */
    for (int i = 0; i < 8; i++) {
        WT_DATA2((byte >> i) & 0x01U);
        wt_delay_us(BSP_WT588F_CLK_HALF_US);    /* CLK 低电平 */
        WT_DATA1(1);                            /* 上升沿：芯片采样 */
        wt_delay_us(BSP_WT588F_CLK_HALF_US);    /* CLK 高电平 */
        WT_DATA1(0);
    }

    /* 4) 空闲：CLK 低、DATA 低，并留出指令间隔 */
    WT_DATA2(0);
    wt_delay_ms(BSP_WT588F_CMD_GAP_MS);
#endif
}

/* ========================= 帧 / 指令发送（调用方已持锁） ========================= */

static void wt_send_byte_locked(uint8_t byte)
{
    if (s_wire == BSP_WT588F_WIRE_TWO) {
        wt_send_byte_twowire(byte);
    } else {
        wt_send_byte_onewire(byte);
    }
}

static void wt_send_word_locked(uint16_t word)
{
    /* 双字节：先高字节后低字节，每个字节各自成帧（规格书 20.4ms = 10.2ms × 2） */
    wt_send_byte_locked((uint8_t)(word >> 8));
    wt_send_byte_locked((uint8_t)(word & 0xFFU));
}

/* ========================= 对外接口 ========================= */

esp_err_t bsp_wt588f_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = wt_pins_config();
    if (err != ESP_OK) {
        return err;
    }

    s_wire = BSP_WT588F_DEFAULT_WIRE_MODE;
    /* 一线模式先把 DATA2(GPIO25) 释放成输入，交还给字库/EPD；两线模式才接管 */
    wt_data2_use(s_wire == BSP_WT588F_WIRE_TWO);
    wt_bus_idle();

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            ESP_LOGE(TAG, "create mutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    s_ready = true;

    /* 规格书：芯片上电初始化约 40ms，期间不响应指令 */
    TickType_t wait = pdMS_TO_TICKS(BSP_WT588F_POWER_ON_MS);
    vTaskDelay(wait ? wait : 1);

    ESP_LOGI(TAG, "init ok: %s, CLK/DATA1=GPIO%d DATA2=GPIO%d CS=GPIO%d BUSY=GPIO%d",
             (s_wire == BSP_WT588F_WIRE_TWO) ? "two-wire" : "one-wire",
             BSP_WT588F_DATA1_CLK2_PIN, BSP_WT588F_DATA2_PIN,
             BSP_WT588F_CS_PIN, BSP_WT588F_BUSY_PIN);
    return ESP_OK;
}

esp_err_t bsp_wt588f_deinit(void)
{
    if (s_info_uart_on) {
        (void)bsp_wt588f_info_readback_end();
    }
    if (!s_ready) {
        return ESP_OK;
    }

    /*
     * 先关 CS 门控并保持为输出低：GPIO26/25 随后会被字库/EPD 拉来拉去，
     * 门控一旦悬空被拉高，语音芯片就会把那些波形当成指令。
     */
    wt_cs_write(false);

    /* 释放总线：通信脚改成输入，避免芯片休眠时被 MCU 灌电流 */
    gpio_set_direction((gpio_num_t)BSP_WT588F_DATA1_CLK2_PIN, GPIO_MODE_INPUT);
#if (BSP_WT588F_DATA2_PIN >= 0)
    gpio_set_direction((gpio_num_t)BSP_WT588F_DATA2_PIN, GPIO_MODE_INPUT);
#endif
#if (BSP_WT588F_BUSY_PIN >= 0)
    gpio_set_direction((gpio_num_t)BSP_WT588F_BUSY_PIN, GPIO_MODE_INPUT);
#endif

    s_ready = false;
    if (s_lock) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }

    ESP_LOGI(TAG, "deinit");
    return ESP_OK;
}

bool bsp_wt588f_is_ready(void)
{
    return s_ready;
}

esp_err_t bsp_wt588f_set_wire_mode(bsp_wt588f_wire_t mode)
{
    if (mode != BSP_WT588F_WIRE_ONE && mode != BSP_WT588F_WIRE_TWO) {
        return ESP_ERR_INVALID_ARG;
    }
#if (BSP_WT588F_DATA2_PIN < 0)
    if (mode == BSP_WT588F_WIRE_TWO) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_info_uart_on) {
        /* 回读期间总线正被 UART 用作 RX，先结束回读再切换 */
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = wt_lock();
    if (err != ESP_OK) {
        return err;
    }
    s_wire = mode;
    /* 两线模式接管 DATA2，一线模式把它还给字库/EPD */
    wt_data2_use(mode == BSP_WT588F_WIRE_TWO);
    wt_bus_idle();
    wt_unlock();

    ESP_LOGI(TAG, "wire mode -> %s", (mode == BSP_WT588F_WIRE_TWO) ? "two-wire" : "one-wire");
    return ESP_OK;
}

bsp_wt588f_wire_t bsp_wt588f_get_wire_mode(void)
{
    return s_wire;
}

esp_err_t bsp_wt588f_send_byte(uint8_t byte)
{
    esp_err_t err = wt_lock();
    if (err != ESP_OK) {
        return err;
    }

    /* 开门控 -> 发帧 -> 关门控：GPIO26/25 的占用窗口尽量短 */
    wt_cs_write(true);
    wt_send_byte_locked(byte);
    wt_cs_write(false);

    wt_unlock();

    ESP_LOGD(TAG, "tx 0x%02X", byte);
    return ESP_OK;
}

esp_err_t bsp_wt588f_send_word(uint16_t word)
{
    esp_err_t err = wt_lock();
    if (err != ESP_OK) {
        return err;
    }

    /* 双字节指令整条都在门控打开的窗口内发完，中间不释放总线 */
    wt_cs_write(true);
    wt_send_word_locked(word);
    wt_cs_write(false);

    wt_unlock();

    ESP_LOGD(TAG, "tx 0x%04X", word);
    return ESP_OK;
}

/* ========================= BUSY 检测 ========================= */

uint8_t bsp_wt588f_busy_level(void)
{
#if (BSP_WT588F_BUSY_PIN >= 0)
    return (uint8_t)gpio_get_level((gpio_num_t)BSP_WT588F_BUSY_PIN);
#else
    return 1;   /* 未接 BUSY：视为空闲 */
#endif
}

bool bsp_wt588f_is_busy(void)
{
    return bsp_wt588f_busy_level() == BSP_WT588F_BUSY_ACTIVE_LEVEL;
}

esp_err_t bsp_wt588f_wait_idle(uint32_t timeout_ms)
{
#if (BSP_WT588F_BUSY_PIN < 0)
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_wt588f_is_busy()) {
        return ESP_OK;
    }
    if (timeout_ms == 0) {
        return ESP_ERR_TIMEOUT;
    }

    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    if (timeout == 0) {
        timeout = 1;
    }
    TickType_t start = xTaskGetTickCount();

    while (bsp_wt588f_is_busy()) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);  /* tick=100Hz 时约 10ms 一次，语音段通常上百 ms，够用 */
    }
    return ESP_OK;
#endif
}

/* ========================= 门控 / 硬件自检 ========================= */

esp_err_t bsp_wt588f_cs_set(bool selected)
{
#if (BSP_WT588F_CS_PIN < 0)
    (void)selected;
    return ESP_ERR_NOT_SUPPORTED;   /* 板上没有门控电路 */
#else
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = wt_lock();
    if (err != ESP_OK) {
        return err;
    }
    wt_cs_write(selected);
    wt_unlock();

    ESP_LOGD(TAG, "cs -> %d", selected ? 1 : 0);
    return ESP_OK;
#endif
}

esp_err_t bsp_wt588f_bus_test(uint32_t duration_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms == 0) {
        duration_ms = 1000;
    }

    esp_err_t err = wt_lock();
    if (err != ESP_OK) {
        return err;
    }

    TickType_t total = pdMS_TO_TICKS(duration_ms);
    if (total == 0) {
        total = 1;
    }
    TickType_t start = xTaskGetTickCount();

    ESP_LOGI(TAG, "bus test %u ms: 探头挂 AU_CLK_O，应与 CS/CLK 同步出现脉冲",
             (unsigned)duration_ms);

    while ((xTaskGetTickCount() - start) < total) {
        /* 1) 门控关 3ms：AU_CLK_O 应 ≈0V（此时 CLK 高也传不过去） */
        wt_cs_write(false);
        WT_DATA1(1);
        wt_delay_ms(3);

        /* 2) 门控开，CLK 打 8 个 400us 方波：AU_CLK_O 跟随，高电平 ≈3.3V-Vbe≈2.6V */
        wt_cs_write(true);
        for (int i = 0; i < 8; i++) {
            WT_DATA1(1);
            wt_delay_us(200);
            WT_DATA1(0);
            wt_delay_us(200);
        }

        /* 3) 关门控，空闲一个 tick（100Hz 时约 10ms），波形约 60Hz 便于示波器触发 */
        wt_cs_write(false);
        vTaskDelay(1);
    }

    wt_bus_idle();
    wt_unlock();

    ESP_LOGI(TAG, "bus test done");
    return ESP_OK;
}

/* ========================= 工程信息回读（9600 8N1，占用 BUSY 脚） ========================= */

esp_err_t bsp_wt588f_info_readback_begin(void)
{
#if (BSP_WT588F_BUSY_PIN < 0)
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_info_uart_on) {
        return ESP_OK;
    }

    uart_port_t port = (uart_port_t)BSP_WT588F_INFO_UART_PORT;
    if (uart_is_driver_installed(port)) {
        ESP_LOGE(TAG, "uart%d is already installed, choose another port", (int)port);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = uart_driver_install(port, BSP_WT588F_INFO_RX_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    s_info_uart_owned = true;

    uart_config_t cfg = {
        .baud_rate  = BSP_WT588F_INFO_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_param_config(port, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(port, UART_PIN_NO_CHANGE, BSP_WT588F_BUSY_PIN,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        err = uart_flush_input(port);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart config failed: %s", esp_err_to_name(err));
        uart_driver_delete(port);
        s_info_uart_owned = false;
        return err;
    }

    s_info_uart_on = true;
    ESP_LOGD(TAG, "BUSY(GPIO%d) -> uart%d RX @%d", BSP_WT588F_BUSY_PIN,
             (int)port, BSP_WT588F_INFO_BAUD_RATE);
    return ESP_OK;
#endif
}

esp_err_t bsp_wt588f_info_readback_read(uint8_t *buf, size_t buf_size,
                                        size_t *out_len, uint32_t gap_ms)
{
    if (buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_info_uart_on) {
        return ESP_ERR_INVALID_STATE;
    }
    if (gap_ms == 0) {
        gap_ms = BSP_WT588F_INFO_GAP_MS;
    }

    uart_port_t port = (uart_port_t)BSP_WT588F_INFO_UART_PORT;
    TickType_t  gap  = pdMS_TO_TICKS(gap_ms);
    if (gap == 0) {
        gap = 1;
    }

    /*
     * 回传是变长文本：只要 gap_ms 内还有新字节就继续收，
     * 收满缓冲区或连续 gap_ms 没有数据就结束。
     */
    size_t total = 0;
    while (total < buf_size) {
        int n = uart_read_bytes(port, buf + total, (uint32_t)(buf_size - total), gap);
        if (n <= 0) {
            break;
        }
        total += (size_t)n;
    }

    if (out_len) {
        *out_len = total;
    }
    ESP_LOGI(TAG, "info readback: %u byte(s)", (unsigned)total);
    return (total > 0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t bsp_wt588f_info_readback_end(void)
{
    if (!s_info_uart_on) {
        return ESP_OK;
    }

    uart_port_t port = (uart_port_t)BSP_WT588F_INFO_UART_PORT;
    if (s_info_uart_owned) {
        uart_driver_delete(port);
        s_info_uart_owned = false;
    }
    s_info_uart_on = false;

#if (BSP_WT588F_BUSY_PIN >= 0)
    /* UART 释放后把 BUSY 脚还原成普通输入（保持无上下拉） */
    gpio_set_direction((gpio_num_t)BSP_WT588F_BUSY_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)BSP_WT588F_BUSY_PIN, GPIO_FLOATING);
#endif

    ESP_LOGD(TAG, "uart released, BUSY back to GPIO input");
    return ESP_OK;
}

