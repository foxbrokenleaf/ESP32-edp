/*
 * @Description: WT588F 语音芯片应用层实现
 *
 * 指令换算规则（规格书 6.2.2 / 6.2.6）：
 *   功能           单字节发码        双字节发码
 *   播放地址       0x00~0xDF         0x0000~0x03E7
 *   音量           0xE0~0xEF         0xFFE0~0xFFEF
 *   循环当前       F2                FFF2
 *   连码前缀       F3                FFF3
 *   连码循环开     F4 01             FFF4 0001
 *   连码循环关     F4 00             FFF4 0000
 *   停止           FE                FFFE
 *   回读信息       F9 00/01/02       FFF9 0000/0001/0002
 *
 * 本层不额外加延时：BSP 层每帧结束已保证 2ms 的指令间隔，
 * 这里的每条指令由一帧或两帧组成，直接连着发即可。
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "bsp_WT588F.h"
#include "WT588F.h"

static const char *TAG = "WT588F";

static bool               s_ready     = false;
static wt588f_addr_mode_t s_addr_mode = WT588F_DEFAULT_ADDR_MODE;
static uint8_t            s_volume    = WT588F_VOLUME_MAX;

/* ========================= 内部工具 ========================= */

static uint16_t wt_addr_max(void)
{
    return (s_addr_mode == WT588F_ADDR_DOUBLE_BYTE) ? WT588F_ADDR_MAX_DOUBLE_BYTE
                                                    : WT588F_ADDR_MAX_SINGLE_BYTE;
}

/**
 * @brief  按当前发码方式发送一条“单/双字节形式不同”的指令
 * @param  single 单字节模式的命令（如 0xF2）
 * @param  dbl    双字节模式的命令（如 0xFFF2）
 */
static esp_err_t wt_send_cmd(uint8_t single, uint16_t dbl)
{
    if (s_addr_mode == WT588F_ADDR_DOUBLE_BYTE) {
        return bsp_wt588f_send_word(dbl);
    }
    return bsp_wt588f_send_byte(single);
}

/**
 * @brief  发送语音地址（带范围检查）
 */
static esp_err_t wt_send_addr(uint16_t addr)
{
    uint16_t max = wt_addr_max();
    if (addr > max) {
        ESP_LOGE(TAG, "addr %u out of range (max %u, %s-byte mode)",
                 (unsigned)addr, (unsigned)max,
                 (s_addr_mode == WT588F_ADDR_DOUBLE_BYTE) ? "double" : "single");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_addr_mode == WT588F_ADDR_DOUBLE_BYTE) {
        return bsp_wt588f_send_word(addr);
    }
    return bsp_wt588f_send_byte((uint8_t)addr);
}

/* ========================= 初始化 / 配置 ========================= */

esp_err_t WT588F_Init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = bsp_wt588f_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = bsp_wt588f_set_wire_mode((WT588F_DEFAULT_WIRE_MODE == WT588F_WIRE_TWO)
                                   ? BSP_WT588F_WIRE_TWO : BSP_WT588F_WIRE_ONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set wire mode failed: %s", esp_err_to_name(err));
        (void)bsp_wt588f_deinit();
        return err;
    }

    s_addr_mode = WT588F_DEFAULT_ADDR_MODE;
    s_volume    = WT588F_VOLUME_MAX;
    s_ready     = true;

    ESP_LOGI(TAG, "init ok: %s, %s-byte addr mode",
             (WT588F_DEFAULT_WIRE_MODE == WT588F_WIRE_TWO) ? "two-wire" : "one-wire",
             (s_addr_mode == WT588F_ADDR_DOUBLE_BYTE) ? "double" : "single");
    return ESP_OK;
}

esp_err_t WT588F_DeInit(void)
{
    s_ready = false;
    return bsp_wt588f_deinit();
}

bool WT588F_IsReady(void)
{
    return s_ready && bsp_wt588f_is_ready();
}

esp_err_t WT588F_SetWireMode(wt588f_wire_t mode)
{
    if (mode != WT588F_WIRE_ONE && mode != WT588F_WIRE_TWO) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_wt588f_set_wire_mode((mode == WT588F_WIRE_TWO) ? BSP_WT588F_WIRE_TWO
                                                              : BSP_WT588F_WIRE_ONE);
}

esp_err_t WT588F_SetAddressMode(wt588f_addr_mode_t mode)
{
    if (mode != WT588F_ADDR_SINGLE_BYTE && mode != WT588F_ADDR_DOUBLE_BYTE) {
        return ESP_ERR_INVALID_ARG;
    }
    s_addr_mode = mode;
    ESP_LOGI(TAG, "addr mode -> %s-byte",
             (mode == WT588F_ADDR_DOUBLE_BYTE) ? "double" : "single");
    return ESP_OK;
}

wt588f_addr_mode_t WT588F_GetAddressMode(void)
{
    return s_addr_mode;
}

uint8_t WT588F_GetVolume(void)
{
    return s_volume;
}

/* ========================= 播放控制 ========================= */

esp_err_t WT588F_Play(uint16_t addr)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return wt_send_addr(addr);
}

esp_err_t WT588F_PlayChain(const uint16_t *addr, uint8_t count)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (addr == NULL || count == 0 || count > WT588F_CHAIN_MAX) {
        ESP_LOGE(TAG, "invalid chain: ptr=%p count=%u (max %u)",
                 (const void *)addr, (unsigned)count, WT588F_CHAIN_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0; i < count; i++) {
        /* 每段都带 F3 前缀：播放中收到不会打断前面的连码 */
        esp_err_t err = wt_send_cmd(BSP_WT588F_CMD_CHAIN, 0xFFF3);
        if (err != ESP_OK) {
            return err;
        }
        err = wt_send_addr(addr[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t WT588F_PlayChainLoop(const uint16_t *addr, uint8_t count)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (addr == NULL || count == 0 || count > WT588F_CHAIN_MAX) {
        ESP_LOGE(TAG, "invalid chain loop: ptr=%p count=%u (max %u)",
                 (const void *)addr, (unsigned)count, WT588F_CHAIN_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    /* F4 01（FFF4 0001）= 连码循环播放，后面直接跟地址，不再加 F3 */
    esp_err_t err = wt_send_cmd(BSP_WT588F_CMD_CHAIN_LOOP, 0xFFF4);
    if (err == ESP_OK) {
        err = wt_send_cmd(BSP_WT588F_CHAIN_LOOP_ON, 0x0001);
    }
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t i = 0; i < count; i++) {
        err = wt_send_addr(addr[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t WT588F_StopChainLoop(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = wt_send_cmd(BSP_WT588F_CMD_CHAIN_LOOP, 0xFFF4);
    if (err != ESP_OK) {
        return err;
    }
    return wt_send_cmd(BSP_WT588F_CHAIN_LOOP_OFF, 0x0000);
}

esp_err_t WT588F_Stop(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return wt_send_cmd(BSP_WT588F_CMD_STOP, 0xFFFE);
}

esp_err_t WT588F_Loop(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 规格书：先发播放指令再发 F2；再次发 F2 停掉当前循环 */
    return wt_send_cmd(BSP_WT588F_CMD_LOOP, 0xFFF2);
}

esp_err_t WT588F_SetVolume(uint8_t volume)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume > WT588F_VOLUME_MAX) {
        ESP_LOGE(TAG, "volume %u out of range (0~%u)", (unsigned)volume, WT588F_VOLUME_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    /* 单字节 E0~EF；双字节 FFE0~FFEF（0xFF00 | 0xE0 | volume 即 0xFFE0+volume） */
    esp_err_t err = wt_send_cmd((uint8_t)(BSP_WT588F_CMD_VOLUME_BASE | volume),
                                (uint16_t)(0xFF00U | BSP_WT588F_CMD_VOLUME_BASE | volume));
    if (err == ESP_OK) {
        s_volume = volume;
        ESP_LOGD(TAG, "volume -> %u", (unsigned)volume);
    }
    return err;
}

/* ========================= 状态查询 / 等待 ========================= */

bool WT588F_IsBusy(void)
{
    return bsp_wt588f_is_busy();
}

esp_err_t WT588F_WaitIdle(uint32_t timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return bsp_wt588f_wait_idle(timeout_ms);
}

esp_err_t WT588F_PlayAndWait(uint16_t addr, uint32_t timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = WT588F_Play(addr);
    if (err != ESP_OK || timeout_ms == 0) {
        return err;
    }

#if (BSP_WT588F_BUSY_PIN < 0)
    /* 没接 BUSY 脚，只能发码，无法判断播放结束 */
    return ESP_ERR_NOT_SUPPORTED;
#else
    /*
     * 先等 BUSY 进入“播放中”：PWM 模式 500us~35ms、DAC 模式 30~65ms 才变化，
     * 这里给 WT588F_BUSY_START_TIMEOUT_MS（120ms）余量。
     */
    TickType_t start_timeout = pdMS_TO_TICKS(WT588F_BUSY_START_TIMEOUT_MS);
    if (start_timeout == 0) {
        start_timeout = 1;
    }
    TickType_t start   = xTaskGetTickCount();
    bool       started = false;

    while ((xTaskGetTickCount() - start) < start_timeout) {
        if (bsp_wt588f_is_busy()) {
            started = true;
            break;
        }
        vTaskDelay(1);
    }

    if (!started) {
        /* 极短的提示音可能已经播完，也可能是 BUSY 没接对，交给调用方判断 */
        ESP_LOGW(TAG, "busy not detected within %d ms", WT588F_BUSY_START_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    return bsp_wt588f_wait_idle(timeout_ms);
#endif
}

/* ========================= 工程信息回读 ========================= */

esp_err_t WT588F_ReadInfo(wt588f_info_t type, uint8_t *buf, size_t buf_size,
                          size_t *out_len, uint32_t gap_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (type != WT588F_INFO_MAIN_PROGRAM && type != WT588F_INFO_PROJECT_NO &&
        type != WT588F_INFO_VERSION_NO) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 规格书：F9 指令只有在空闲状态下发送有效 */
    if (WT588F_IsBusy()) {
        ESP_LOGW(TAG, "read info requires idle chip");
        return ESP_ERR_INVALID_STATE;
    }

    /* 必须先让 UART 接管 BUSY 脚，否则芯片回传的开头几个字节会丢 */
    esp_err_t err = bsp_wt588f_info_readback_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "info readback begin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* F9 前缀 + 子命令：单字节 F9 00；双字节 FFF9 0000 */
    err = wt_send_cmd(BSP_WT588F_CMD_INFO, 0xFFF9);
    if (err == ESP_OK) {
        err = wt_send_cmd((uint8_t)type, (uint16_t)type);
    }
    if (err == ESP_OK) {
        err = bsp_wt588f_info_readback_read(buf, buf_size, out_len, gap_ms);
    }

    esp_err_t end_err = bsp_wt588f_info_readback_end();
    if (err == ESP_OK && end_err != ESP_OK) {
        err = end_err;
    }
    return err;
}

/* ========================= 自检（F9 工程信息回读） ========================= */

void WT588F_InfoToText(const uint8_t *buf, size_t len, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    if (buf == NULL) {
        out[0] = '\0';
        return;
    }

    size_t n = (len < out_size - 1) ? len : (out_size - 1);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = buf[i];
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out[n] = '\0';
}

esp_err_t WT588F_SelfTest(wt588f_selftest_t *result)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));

    /* F9 只有在空闲状态才有效：先把上一段语音的尾巴等干净 */
    esp_err_t wait = WT588F_WaitIdle(WT588F_SELFTEST_IDLE_TIMEOUT_MS);
    if (wait != ESP_OK && wait != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "selftest: chip still busy (%s), F9 may be ignored",
                 esp_err_to_name(wait));
    }

    struct info_req {
        wt588f_info_t type;
        const char   *name;
        uint8_t      *buf;
        size_t       *len;
    } req[3] = {
        { WT588F_INFO_MAIN_PROGRAM, "main program", result->main_program, &result->main_program_len },
        { WT588F_INFO_PROJECT_NO,   "project no",   result->project_no,   &result->project_no_len   },
        { WT588F_INFO_VERSION_NO,   "version no",   result->version_no,   &result->version_no_len   },
    };

    esp_err_t first_err = ESP_OK;

    for (int i = 0; i < 3; i++) {
        esp_err_t err = WT588F_ReadInfo(req[i].type, req[i].buf, WT588F_INFO_TEXT_MAX,
                                        req[i].len, WT588F_SELFTEST_INFO_GAP_MS);
        if (err == ESP_OK) {
            /* 同时给文本和原始字节：编号在多数工程里是 ASCII（如 "9F2C"），
             * 少数工程直接回二进制，那就只能看 hex */
            char   text[WT588F_INFO_TEXT_MAX];
            char   hex[3 * 8 + 1];
            size_t n   = (*req[i].len < 8) ? *req[i].len : 8;
            size_t pos = 0;

            WT588F_InfoToText(req[i].buf, *req[i].len, text, sizeof(text));
            hex[0] = '\0';
            for (size_t k = 0; k < n && (sizeof(hex) - pos) > 3; k++) {
                pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02X ", req[i].buf[k]);
            }
            ESP_LOGI(TAG, "selftest: %s = \"%s\" [%s](%u byte)", req[i].name, text, hex,
                     (unsigned)*req[i].len);
            continue;
        }

        ESP_LOGW(TAG, "selftest: %s read failed: %s", req[i].name, esp_err_to_name(err));
        if (i == 0 && first_err == ESP_OK) {
            first_err = err;        /* 主控程序编号读不到 = 芯片不回话 */
            if (err == ESP_ERR_NOT_SUPPORTED) {
                break;              /* BUSY 没接，后面两项也不可能成功 */
            }
        }
    }

    if (first_err == ESP_OK) {
        ESP_LOGI(TAG, "selftest: info readback ok (chip online)");
    }
    return first_err;
}

/* ========================= 原始指令 ========================= */

esp_err_t WT588F_SendRaw(uint16_t code)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_addr_mode == WT588F_ADDR_SINGLE_BYTE) {
        if (code > 0xFF) {
            ESP_LOGE(TAG, "raw 0x%04X needs double-byte mode", code);
            return ESP_ERR_INVALID_ARG;
        }
        return bsp_wt588f_send_byte((uint8_t)code);
    }
    return bsp_wt588f_send_word(code);
}

