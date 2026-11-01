/*
 * @Description: GT30L32S4W 字库芯片应用层（字模地址计算 + 取字模 + 显示）
 *
 * 一、芯片点阵布局（已用芯片 ROM 数据逐字节校验）
 *   GB2312 点阵：索引 index = (区码 - 0xA1) * 94 + (位码 - 0xA1)
 *               区码 0xA1~0xA9 为符号区（846 个），0xB0~0xF7 为汉字区
 *               汉字区索引在符号区之后，故：index = 846 + (区码 - 0xB0) * 94 + (位码 - 0xA1)
 *               例：“啊”(0xB0A1) index = 846
 *       12x12 基址 0x000000，每字 24 字节
 *       16x16 基址 0x02C9D0，每字 32 字节
 *       24x24 基址 0x068190，每字 72 字节
 *       32x32 基址 0x0EDF00，每字 128 字节
 *
 *   ASCII 定宽点阵：索引 index = 字符 - 0x20（0x20~0x7E，96 个）
 *        5x7  基址 0x1DDF80，每字 8  字节（1 字节/行，8 行）
 *        7x8  基址 0x1DE280，每字 8  字节（1 字节/行，8 行）
 *        6x12 基址 0x1DBE00，每字 12 字节（1 字节/行，12 行）
 *        8x16 基址 0x1DD780，每字 16 字节（1 字节/行，16 行）
 *       12x24 基址 0x1DFF00，每字 48 字节（2 字节/行，24 行）
 *       16x32 基址 0x1E5A50，每字 64 字节（2 字节/行，32 行）
 *
 *   ASCII 不等宽（Arial / Times New Roman）：每条记录 = 2 字节宽度(BE) + 点阵
 *       12px Arial 记录基址 0x1DC400，记录长度 26（2 + 24，2 字节/行，12 行）
 *       12px Times 记录基址 0x1DCDC0，记录长度 26
 *       16px Arial 记录基址 0x1DE580，记录长度 34（2 + 32，2 字节/行，16 行）
 *       16px Times 记录基址 0x1DF240，记录长度 34
 *       24px Arial 记录基址 0x1E22D0，记录长度 74（2 + 72，3 字节/行，24 行）
 *       24px Times 记录基址 0x1E3E90，记录长度 74
 *       32px Arial 记录基址 0x1E99D0，记录长度 130（2 + 128，4 字节/行，32 行）
 *       32px Times 记录基址 0x1ECA90，记录长度 130
 *
 *   特殊字符 8x16（0xACA1~0xACDF，63 个）：基址 0x1F2880，每字 16 字节
 *
 *   点阵格式：1bpp，高位(MSB)在左，逐行存储，bit=1 表示字亮点。
 *
 * 二、注意事项
 *   1. 显示接口的文字必须是 GB2312/GBK 编码（工程源文件本身为 GBK 编码，
 *      直接写 "温度" 即可）；纯 UTF-8 字符串会显示异常。
 *   2. 内部扩展字库（拼音 AAA1~ABC0 / 扩展汉字等）厂商未公开地址表，
 *      本驱动不内置；可自行用 GT30L32S4W_ReadRaw() 按需读取。
 *   3. 显示函数依赖同目录 EPD.c 的 EPD_DrawPoint / EPD_ClearPoint。
 */

#ifndef __GT30L32S4W_H__
#define __GT30L32S4W_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= 字库编号 ========================= */

typedef enum {
    GT30L32S4W_FONT_ASCII_5X7 = 0,      /* 定宽 5x7  */
    GT30L32S4W_FONT_ASCII_7X8,          /* 定宽 7x8  */
    GT30L32S4W_FONT_ASCII_6X12,         /* 定宽 6x12 */
    GT30L32S4W_FONT_ASCII_8X16,         /* 定宽 8x16 */
    GT30L32S4W_FONT_ASCII_12X24,        /* 定宽 12x24 */
    GT30L32S4W_FONT_ASCII_16X32,        /* 定宽 16x32 */

    GT30L32S4W_FONT_ASCII_12_ARIAL,     /* Arial 12px，不等宽 */
    GT30L32S4W_FONT_ASCII_12_TIMES,     /* Times New Roman 12px，不等宽 */
    GT30L32S4W_FONT_ASCII_16_ARIAL,     /* Arial 16px，不等宽 */
    GT30L32S4W_FONT_ASCII_16_TIMES,     /* Times New Roman 16px，不等宽 */
    GT30L32S4W_FONT_ASCII_24_ARIAL,     /* Arial 24px，不等宽 */
    GT30L32S4W_FONT_ASCII_24_TIMES,     /* Times New Roman 24px，不等宽 */
    GT30L32S4W_FONT_ASCII_32_ARIAL,     /* Arial 32px，不等宽 */
    GT30L32S4W_FONT_ASCII_32_TIMES,     /* Times New Roman 32px，不等宽 */

    GT30L32S4W_FONT_GB2312_12X12,       /* 中文字库 12x12 */
    GT30L32S4W_FONT_GB2312_16X16,       /* 中文字库 16x16 */
    GT30L32S4W_FONT_GB2312_24X24,       /* 中文字库 24x24 */
    GT30L32S4W_FONT_GB2312_32X32,       /* 中文字库 32x32 */

    GT30L32S4W_FONT_GB_SPEC_8X16,       /* 特殊字符 8x16（0xACA1~0xACDF） */

    GT30L32S4W_FONT_MAX
} gt30l32s4w_font_t;

/* 单字最大字节数：32x32 不等宽 = 2 字节宽度 + 128 字节点阵 */
#define GT30L32S4W_GLYPH_MAX_BYTES      130
/* 单字最大宽度/高度（像素） */
#define GT30L32S4W_GLYPH_MAX_WIDTH      32
#define GT30L32S4W_GLYPH_MAX_HEIGHT     32

/* ========================= 字库信息 ========================= */

typedef struct {
    uint32_t base_addr;         /* 该字库第一条记录的起始地址 */
    uint16_t width;             /* 点阵框宽度（像素） */
    uint16_t height;            /* 点阵框高度（像素，即行数） */
    uint16_t record_size;       /* 每条记录字节数（含宽度头） */
    uint8_t  bytes_per_row;     /* 点阵每行字节数 */
    uint8_t  header_size;       /* 点阵前的宽度字节数：定宽字体为 0，不等宽为 2 */
    bool     proportional;      /* true=不等宽字体（记录含宽度头） */
} gt30l32s4w_font_info_t;

/* ========================= 初始化 / 自检 ========================= */

/**
 * @brief  初始化字库芯片（SPI 总线 + 设备）
 * @return ESP_OK 成功
 */
esp_err_t GT30L32S4W_Init(void);

/**
 * @brief  使用指定 SPI 时钟初始化
 * @param  clock_hz 时钟频率，0 表示默认值（10MHz），上限 80MHz
 */
esp_err_t GT30L32S4W_InitWithClock(uint32_t clock_hz);

/**
 * @brief  读回已知字模做校验，确认芯片在线且地址表正确
 * @return ESP_OK 校验通过；ESP_ERR_INVALID_RESPONSE 数据不符（芯片未接好）
 */
esp_err_t GT30L32S4W_SelfTest(void);

/* ========================= 取字模 ========================= */

/**
 * @brief  查询字库信息
 * @param  font 字库编号
 * @param  info 输出信息
 */
esp_err_t GT30L32S4W_GetFontInfo(gt30l32s4w_font_t font, gt30l32s4w_font_info_t *info);

/**
 * @brief  读取一个 ASCII 字符的点阵
 * @param  font     必须是 ASCII 字库
 * @param  c        字符，范围 0x20~0x7E，其余字符按 '?' 处理
 * @param  buf      输出缓冲（容量至少 record_size）
 * @param  buf_size 缓冲大小
 * @param  advance  输出该字符的步进宽度（像素）：定宽字体 = width，
 *                  不等宽字体 = 记录头中的字宽；可为 NULL
 * @return ESP_OK 成功
 */
esp_err_t GT30L32S4W_ReadAscii(gt30l32s4w_font_t font, char c,
                               uint8_t *buf, size_t buf_size, uint16_t *advance);

/**
 * @brief  读取一个 GB2312 汉字的点阵
 * @param  font     必须是 GB2312 字库（或 GT30L32S4W_FONT_GB_SPEC_8X16）
 * @param  gb_code  GB2312 编码，高字节为区码，低字节为位码，如 0xB0A1("啊")
 * @param  buf      输出缓冲（容量至少 record_size）
 * @param  buf_size 缓冲大小
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 编码不在字库范围内
 */
esp_err_t GT30L32S4W_ReadGb2312(gt30l32s4w_font_t font, uint16_t gb_code,
                                uint8_t *buf, size_t buf_size);

/**
 * @brief  直接读取原始数据（用于扩展字库、图片字模等）
 * @param  addr 字库 24 位地址
 * @param  buf  输出缓冲
 * @param  len  字节数
 */
esp_err_t GT30L32S4W_ReadRaw(uint32_t addr, void *buf, size_t len);

/* ========================= 显示（依赖 EPD.c） ========================= */

/**
 * @brief  显示单个 ASCII 字符
 * @param  x,y      左上角逻辑坐标
 * @param  c        字符
 * @param  font     ASCII 字库
 * @param  inverted 1=反白显示（白字黑底）
 * @param  advance  输出步进宽度（像素），可为 NULL
 * @return ESP_OK 成功
 */
esp_err_t GT30L32S4W_ShowChar(int16_t x, int16_t y, char c,
                              gt30l32s4w_font_t font, uint8_t inverted,
                              uint16_t *advance);

/**
 * @brief  显示单个 GB2312 汉字
 * @param  x,y      左上角逻辑坐标
 * @param  gb_code  GB2312 编码
 * @param  font     GB2312 字库
 * @param  inverted 1=反白显示
 * @return ESP_OK 成功
 */
esp_err_t GT30L32S4W_ShowGbChar(int16_t x, int16_t y, uint16_t gb_code,
                                gt30l32s4w_font_t font, uint8_t inverted);

/**
 * @brief  显示 GB2312/GBK 字符串（自动区分 ASCII 与汉字，支持 '\n' 换行）
 * @param  x,y        起始逻辑坐标
 * @param  gbk_text   GB2312/GBK 编码字符串（源文件为 GBK，可直接写中文）
 * @param  ascii_font ASCII 字库
 * @param  gb_font    汉字字库
 * @param  inverted   1=反白显示
 * @return ESP_OK 成功
 */
esp_err_t GT30L32S4W_ShowString(int16_t x, int16_t y, const char *gbk_text,
                                gt30l32s4w_font_t ascii_font,
                                gt30l32s4w_font_t gb_font, uint8_t inverted);

#ifdef __cplusplus
}
#endif

#endif /* __GT30L32S4W_H__ */

