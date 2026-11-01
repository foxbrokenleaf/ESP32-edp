/*
 * @Description: GT30L32S4W 字库芯片 BSP 层（SPI 总线 + 读操作）
 *
 * 硬件连接（可在此处修改）：
 *   SCLK  -> GPIO22
 *   CS#   -> GPIO23
 *   SI    -> GPIO21
 *   SO    -> GPIO35
 *   HOLD# -> 硬件上拉（规格书建议 2K 上拉到 3.3V）
 *
 * 说明：
 *   1. GT30L32S4W 只有 2 条指令（03h READ / 0Bh FAST_READ），芯片为只读，
 *      因此本层只实现读取，没有写接口。
 *   2. 一次操作 = CS 拉低 + 指令 + 3 字节地址 (+1 字节 dummy) + 连续数据。
 *   3. SPI 模式 0（CPOL=0, CPHA=0），SCLK 上限 120MHz（规格书），
 *      ESP32 主机侧上限 80MHz。
 */

#ifndef __BSP_GT30L32S4W_H__
#define __BSP_GT30L32S4W_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= 引脚定义 ========================= */

#define BSP_GT30L32S4W_SCLK_PIN          22
#define BSP_GT30L32S4W_CS_PIN            23
#define BSP_GT30L32S4W_MOSI_PIN          21
#define BSP_GT30L32S4W_MISO_PIN          35

/*
 * HOLD# 引脚：
 *   若硬件已把 HOLD# 上拉到 3.3V，则保持 -1（软件不控制）；
 *   若 HOLD# 接到了 GPIO，则改成对应 GPIO 编号，初始化时会被拉高。
 */
#define BSP_GT30L32S4W_HOLD_PIN          (-1)

/* ========================= 总线参数 ========================= */

#define BSP_GT30L32S4W_SPI_HOST          SPI2_HOST
#define BSP_GT30L32S4W_DEFAULT_CLOCK_HZ  (10 * 1000 * 1000)   /* 10MHz，保守值 */
#define BSP_GT30L32S4W_MAX_CLOCK_HZ      (80 * 1000 * 1000)   /* ESP32 SPI 主机上限 */

/* ========================= 指令定义 ========================= */

#define BSP_GT30L32S4W_CMD_READ          0x03   /* Read Data Bytes */
#define BSP_GT30L32S4W_CMD_FAST_READ     0x0B   /* Read Data Bytes at Higher Speed */

/* ========================= 接口 ========================= */

/**
 * @brief  初始化字库芯片（使用默认时钟）
 * @return ESP_OK 成功
 */
esp_err_t bsp_gt30l32s4w_init(void);

/**
 * @brief  初始化字库芯片并指定 SPI 时钟
 * @param  clock_hz SPI 时钟频率，0 表示使用默认值；超过 80MHz 会被钳位
 * @return ESP_OK 成功
 */
esp_err_t bsp_gt30l32s4w_init_with_clock(uint32_t clock_hz);

/**
 * @brief  释放 SPI 设备与总线
 * @return ESP_OK 成功
 */
esp_err_t bsp_gt30l32s4w_deinit(void);

/**
 * @brief  查询是否已初始化
 */
bool bsp_gt30l32s4w_is_ready(void);

/**
 * @brief  当前 SPI 时钟频率
 */
uint32_t bsp_gt30l32s4w_get_clock_hz(void);

/**
 * @brief  使用 03h 指令读取任意长度数据（低速，兼容性最好）
 * @param  addr 24 位字库地址
 * @param  dst  目标缓冲区
 * @param  len  读取字节数（可超过内部 DMA 缓冲长度，自动分包）
 * @return ESP_OK 成功
 */
esp_err_t bsp_gt30l32s4w_read(uint32_t addr, void *dst, size_t len);

/**
 * @brief  使用 0Bh 指令快速读取任意长度数据（需先发 1 字节 dummy）
 * @param  addr 24 位字库地址
 * @param  dst  目标缓冲区
 * @param  len  读取字节数
 * @return ESP_OK 成功
 */
esp_err_t bsp_gt30l32s4w_fast_read(uint32_t addr, void *dst, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_GT30L32S4W_H__ */

