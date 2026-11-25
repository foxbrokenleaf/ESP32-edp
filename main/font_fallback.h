/*
 * @Description: GT30L32S4W 字库芯片的"内置字模回退"接口
 *
 * 背景
 *   板上 GT30L32S4W 若不响应（HOLD# 未拉高 / 未贴片 / 走线断开 / 供电异常），
 *   自检必然失败，中文就全部显示不出来。本模块把工程实际用到的字模从
 *   datasheet/GT30L32S4W.bin（2MB 芯片 ROM 镜像）里抠出来编译进固件，
 *   芯片不在线时由 GT30L32S4W.c 自动改读这里的数据，显示功能照常可用。
 *
 * 数据文件
 *   font_fallback_data.c 由 tools/gen_font_fallback.py 自动生成，不要手改；
 *   GUI 里新增了文字后重新执行  python tools/gen_font_fallback.py  即可。
 */

#ifndef __FONT_FALLBACK_H__
#define __FONT_FALLBACK_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#include "GT30L32S4W.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 内置字库索引项：按 (font, code) 升序排列，支持二分查找 */
typedef struct {
    uint16_t font;      /* gt30l32s4w_font_t 枚举值 */
    uint16_t code;      /* ASCII 字体=字符值(0x20~0x7E)；汉字字体=GBK 双字节码 */
    uint32_t offset;    /* 该字形在 g_font_fallback_data 中的偏移 */
    uint16_t length;    /* 该字形字节数（等于芯片字库的记录长度） */
} font_fallback_entry_t;

/* 由 font_fallback_data.c 提供 */
extern const font_fallback_entry_t g_font_fallback_index[];
extern const unsigned g_font_fallback_index_count;
extern const uint8_t g_font_fallback_data[];
extern const unsigned g_font_fallback_data_size;

/**
 * @brief  内置子集里是否包含某个字体（用于判断能否脱离芯片工作）
 */
bool FontFallback_HasFont(gt30l32s4w_font_t font);

/**
 * @brief  内置子集总字形数（0 表示没有生成数据）
 */
size_t FontFallback_GlyphCount(void);

/**
 * @brief  从内置子集取一个 ASCII 字形
 * @param  font      ASCII 字体编号
 * @param  ch        字符值（0x20~0x7E）
 * @param  buf       输出缓冲，长度至少为该字体的记录长度
 * @param  buf_size  缓冲大小
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 未收录该字形
 * @note   不等宽字体的记录头 2 字节宽度也会原样拷出，与芯片路径格式一致
 */
esp_err_t FontFallback_ReadAscii(gt30l32s4w_font_t font, uint8_t ch,
                                 uint8_t *buf, size_t buf_size);

/**
 * @brief  从内置子集取一个 GB2312/特殊字符字形
 * @param  font     GB2312 字体编号（或 GT30L32S4W_FONT_GB_SPEC_8X16）
 * @param  gb_code  GBK 双字节码，如 0xB0A1("啊")
 * @param  buf      输出缓冲，长度至少为该字体的记录长度
 * @param  buf_size 缓冲大小
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 未收录该字形
 */
esp_err_t FontFallback_ReadGbk(gt30l32s4w_font_t font, uint16_t gb_code,
                               uint8_t *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* __FONT_FALLBACK_H__ */
