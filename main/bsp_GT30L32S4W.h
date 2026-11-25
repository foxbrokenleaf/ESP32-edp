/*
 * @Description: GT30L32S4W 字库芯片 BSP 层（SPI 总线 + 读操作）
 *
 * 硬件连接（已用逻辑分析仪 + bus probe 实测确认，见下方说明 4）：
 *   SCLK  -> GPIO26（与 EPD 的 SCL 共用）
 *   CS#   -> GPIO23（字库独占；EPD 的片选是 GPIO27）
 *   SI    -> GPIO25（与 EPD 的 SDA 共用）
 *   SO    -> GPIO35（34~39 号引脚只能输入，无内部上下拉）
 *   HOLD# -> 硬件上拉（规格书建议 2K 上拉到 3.3V）
 *
 * 说明：
 *   1. GT30L32S4W 只有 2 条指令（03h READ / 0Bh FAST_READ），芯片为只读，
 *      因此本层只实现读取，没有写接口。
 *   2. 一次操作 = CS 拉低 + 指令 + 3 字节地址 (+1 字节 dummy) + 连续数据。
 *   3. SPI 模式 0（CPOL=0, CPHA=0），SCLK 上限 120MHz（规格书），
 *      ESP32 主机侧上限 80MHz。
 *   4. 字库与 EPD 的软件 SPI **共用同一组时钟/数据线**：
 *      SCLK/SI = GPIO26/25 与 EPD 的 SCL/SDA 是同一根网络，
 *      靠各自的片选区分（字库 CS#=GPIO23，EPD CS=GPIO27）。
 *      所以两边任何时候都只能有一个 CS 有效；本层每次读之前调用
 *      pins_init()、读完调用 pins_deinit() 把 26/25 交还成普通 GPIO，
 *      与 EPD 的软件 SPI 交替使用不会互相打断。
 *      注意：早期版本把这里写成 GPIO22/21，实测字库收不到时钟，
 *      自检始终失败（读回全 FF），bus probe 会打印
 *      `PROBE HIT: SCLK=26 SI=25 CS=23 SO=35`。
 *   5. 若把 SCLK/MOSI 改到与其它外设复用的引脚上，请在使用硬件 SPI
 *      （BSP_GT30L32S4W_USE_BITBANG = 0）时保留 pins_init/pins_deinit 的
 *      重新路由逻辑：gpio_config()/gpio_set_direction() 会把 GPIO 矩阵里的
 *      SPI 输出信号复位掉。
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

#define BSP_GT30L32S4W_SCLK_PIN          26
#define BSP_GT30L32S4W_CS_PIN            23
#define BSP_GT30L32S4W_MOSI_PIN          25
#define BSP_GT30L32S4W_MISO_PIN          35

/*
 * HOLD# 引脚：
 *   若硬件已把 HOLD# 上拉到 3.3V，则保持 -1（软件不控制）；
 *   若 HOLD# 接到了 GPIO，则改成对应 GPIO 编号，初始化时会被拉高。
 */
#define BSP_GT30L32S4W_HOLD_PIN          (-1)

/* ========================= 读取方式 ========================= */

/*
 * 1 = 纯 GPIO 软件时序（bit-bang），默认
 *     字库的 SCLK/MOSI/CS/MISO 全部用 gpio_set_level()/gpio_get_level()
 *     直接翻转，完全不经过 SPI 外设和 GPIO 矩阵，任何 gpio_config() 都
 *     不会把通信打断。
 * 0 = ESP-IDF SPI Master（硬件 SPI，走 GPIO 矩阵，速度更快）
 */
#define BSP_GT30L32S4W_USE_BITBANG        1

/*
 * bit-bang 半周期延时（us）：1 ≈ 500kHz，0 = 不额外延时（只受 CPU/GPIO 速度限制）。
 * 字库规格书 SCLK 上限 120MHz，500kHz 已足够，且读一个 32x32 汉字仅约 2ms。
 */
#define BSP_GT30L32S4W_BITBANG_DELAY_US   1

/*
 * 自检失败时是否做“全组合引脚扫描”（把一组空闲 GPIO 任意 3 个当作
 * SCLK/SI/CS 遍历，约 1.3 秒）。只在自检失败时执行，用于定位实际接线；
 * 接线确认后可以改成 0 以加快失败时的启动。
 */
#define BSP_GT30L32S4W_PROBE_FULL_SCAN    1

/*
 * 示波器/万用表测线用的“引脚翻转测试”：
 *   一次测试 = 连续 duration_ms 毫秒、约 100Hz 的周期波形
 *   （CS 拉低 -> 打 8 个 SCLK 脉冲（MOSI=0xA5）-> CS 拉高 -> 空闲 7ms）。
 *
 * BSP_GT30L32S4W_SCOPE_TOGGLE_MS
 *   初始化完成后自动执行一次翻转测试的时长（0 = 关闭）。
 *   平时保持 0；需要量 CS#/SCLK/SI 到底有没有被 MCU 驱动、通断对不对时，
 *   临时改成 2000~10000，烧录后复位就能在示波器上一眼看到稳定的方波。
 *
 * BSP_GT30L32S4W_FAIL_TOGGLE_MS
 *   自检失败（字库芯片无响应）后自动执行的翻转测试时长，默认 3000ms。
 *   这样不必改宏、不必重新编译，复位时把示波器挂在 CS# 上即可确认
 *   “MCU 到底有没有发片选”；确认接线问题后可改成 0 省掉这 3 秒。
 *
 *   测试内容（日志会逐步打印，不依赖示波器也能判断）：
 *     1) 回读自检：把 CS#/SCLK/SI 逐个驱动 1 再驱动 0，并回读 pad 真实电平
 *        —— 打印 “drive 1/0 -> read 1/0, OK” 说明 MCU 确实驱动得动这个脚；
 *           打印 “read 0/0” 说明该网络被短路到 GND 或根本不在这个脚上。
 *     2) 前段 1/3 时间：100Hz 脉冲（CS# 低 0.32ms / 高 9.68ms），示波器易触发；
 *     3) 后段 2/3 时间：2Hz 方波（低 250ms / 高 250ms），占空比 50%，
 *        万用表直流档应读到约 1.6V；读到恒定 0V 就是被短到地。
 *
 * 注意：稳态运行时不会再有 CS 波形——自检一旦判定芯片离线，
 *       后续取字模全部走内置子集，不再访问 SPI 总线。
 */
#define BSP_GT30L32S4W_SCOPE_TOGGLE_MS    0
#define BSP_GT30L32S4W_FAIL_TOGGLE_MS     3000

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
 * @brief  准备字库引脚（每次读写前调用）
 *
 * bit-bang 模式：把 SCLK/MOSI/CS 重新配置成输出、MISO 配置成输入，
 *                并置成 SPI Mode 0 的空闲电平（CS 高、SCLK 低）。
 *
 * 硬件 SPI 模式：把 SCLK/MOSI/MISO/CS 重新接回 SPI2 的 GPIO 矩阵。
 *                若引脚与 EPD 等软件 SPI 外设复用，该外设的
 *                gpio_config()/gpio_set_direction() 会把矩阵输出信号复位为
 *                SIG_GPIO_OUT_IDX，SPI 信号因此断开，必须重新路由。
 *
 *       bsp_gt30l32s4w_read() / bsp_gt30l32s4w_fast_read() 内部已自动调用，
 *       一般无需手动调用。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 字库尚未初始化
 */
esp_err_t bsp_gt30l32s4w_pins_init(void);

/**
 * @brief  读写结束后恢复引脚空闲状态
 *
 * 本函数把 SCLK/MOSI/CS 保持空闲高电平、MISO 保持输入。
 * bsp_gt30l32s4w_read() / bsp_gt30l32s4w_fast_read() 在每次传输结束后自动调用。
 *
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 字库尚未初始化
 */
esp_err_t bsp_gt30l32s4w_pins_deinit(void);

/**
 * @brief  引脚自动探测（诊断用）
 *
 * 字库读不回数据时调用：把可能作为 CS / SO 使用的引脚全部扫一遍，
 * 用一次已知读操作（8x16 'A' 的前 4 个数据字节 = 00 10 28 28）判断实际接线。
 * 命中时串口打印 `PROBE HIT: SCLK=x SI=y CS=z SO=w`，可直接据此改引脚宏。
 *
 * @return ESP_OK             找到能读回正确数据的引脚组合
 *         ESP_ERR_NOT_FOUND  所有组合都读不到数据（供电 / HOLD# / 连线问题）
 *         ESP_ERR_NOT_SUPPORTED 非 bit-bang 模式不支持
 */
esp_err_t bsp_gt30l32s4w_bus_probe(void);

/**
 * @brief  引脚翻转测试（示波器 / 万用表测线用）
 *
 * 连续 duration_ms 毫秒输出约 100Hz 的周期波形：
 *   CS# 拉低 -> 8 个 SCLK 脉冲（MOSI=0xA5）-> CS# 拉高 -> 空闲 ~7ms。
 *
 * 用途：确认 MCU 是否真的在驱动 CS#/SCLK/SI（示波器可直接触发稳定波形），
 *       或确认 CS#/SCLK/SI 到芯片的通断（万用表看平均电压，约 4% 占空比）。
 *       测试结束会恢复“CS 高、SCLK 高、MOSI 高”的空闲电平。
 *
 * @param  duration_ms 测试时长（ms），0 按 1000ms 处理
 * @return ESP_OK 完成；ESP_ERR_INVALID_STATE 尚未初始化；
 *         ESP_ERR_NOT_SUPPORTED 非 bit-bang 模式
 */
esp_err_t bsp_gt30l32s4w_pin_toggle_test(uint32_t duration_ms);

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

