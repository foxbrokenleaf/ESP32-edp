/*
 * @Description: GT30L32S4W 字库芯片的"内置字模回退"实现（二分查找 + 拷贝点阵）
 *
 * 数据本体在 font_fallback_data.c，由 tools/gen_font_fallback.py 生成。
 */

#include <string.h>

#include "esp_log.h"

#include "font_fallback.h"

static const char *TAG = "FONT_FALLBACK";

/* (font, code) 组合键，用于二分查找比较 */
static inline uint32_t ff_key(uint16_t font, uint16_t code)
{
    return ((uint32_t)font << 16) | code;
}

/**
 * @brief  在升序索引表里查找指定字形
 * @return 命中的索引项指针；没有则返回 NULL
 */
static const font_fallback_entry_t *ff_find(gt30l32s4w_font_t font, uint16_t code)
{
    const uint32_t key = ff_key((uint16_t)font, code);
    size_t lo = 0;
    size_t hi = g_font_fallback_index_count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const font_fallback_entry_t *e = &g_font_fallback_index[mid];
        uint32_t k = ff_key(e->font, e->code);
        if (k == key) {
            return e;
        }
        if (k < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

bool FontFallback_HasFont(gt30l32s4w_font_t font)
{
    /* 找第一个字体编号 >= font 的项，命中即为该字体有数据 */
    for (size_t i = 0; i < g_font_fallback_index_count; i++) {
        if (g_font_fallback_index[i].font >= (uint16_t)font) {
            return (g_font_fallback_index[i].font == (uint16_t)font);
        }
    }
    return false;
}

size_t FontFallback_GlyphCount(void)
{
    return (size_t)g_font_fallback_index_count;
}

/**
 * @brief  把索引项指向的点阵拷贝到调用者缓冲
 */
static esp_err_t ff_copy(const font_fallback_entry_t *e, uint8_t *buf, size_t buf_size)
{
    if (e == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buf_size < e->length) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (e->offset + e->length > g_font_fallback_data_size) {
        ESP_LOGE(TAG, "index out of range: offset=%u len=%u size=%u",
                 (unsigned)e->offset, (unsigned)e->length,
                 (unsigned)g_font_fallback_data_size);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(buf, &g_font_fallback_data[e->offset], e->length);
    return ESP_OK;
}

esp_err_t FontFallback_ReadAscii(gt30l32s4w_font_t font, uint8_t ch,
                                 uint8_t *buf, size_t buf_size)
{
    return ff_copy(ff_find(font, ch), buf, buf_size);
}

esp_err_t FontFallback_ReadGbk(gt30l32s4w_font_t font, uint16_t gb_code,
                               uint8_t *buf, size_t buf_size)
{
    return ff_copy(ff_find(font, gb_code), buf, buf_size);
}
