/*
 * @Description: GT30L32S4W 字库�?片应用层实现
 *
 * 地址表来源：�?�? ROM 逐字节分�? + 显示渲染验证（�?? GT30L32S4W.h 顶部说明�?
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_log.h"

#include "bsp_GT30L32S4W.h"
#include "GT30L32S4W.h"
#include "EPD.h"

static const char *TAG = "GT30L32S4W_APP";

/* ========================= 字库�? ========================= */

typedef struct {
    uint32_t base_addr;     /* 记录起�?�地址（不等�?�字体含 2 字节宽度头） */
    uint16_t record_size;   /* 每条记录字节�? */
    uint8_t  width;         /* 点阵框�?�度（像素） */
    uint8_t  height;        /* 点阵框高度（像素�? */
    uint8_t  bytes_per_row; /* 点阵每�?�字节数 */
    uint8_t  header_size;   /* 点阵前字节数（不等�?�字体的宽度头） */
} gt_font_record_t;

static const gt_font_record_t s_font_table[GT30L32S4W_FONT_MAX] = {
    /* 定�?? ASCII */
    [GT30L32S4W_FONT_ASCII_5X7]     = { 0x1DDF80, 8,   5,  8,  1, 0 },
    [GT30L32S4W_FONT_ASCII_7X8]     = { 0x1DE280, 8,   7,  8,  1, 0 },
    [GT30L32S4W_FONT_ASCII_6X12]    = { 0x1DBE00, 12,  6,  12, 1, 0 },
    [GT30L32S4W_FONT_ASCII_8X16]    = { 0x1DD780, 16,  8,  16, 1, 0 },
    [GT30L32S4W_FONT_ASCII_12X24]   = { 0x1DFF00, 48,  12, 24, 2, 0 },
    [GT30L32S4W_FONT_ASCII_16X32]   = { 0x1E5A50, 64,  16, 32, 2, 0 },

    /* 不等�? ASCII�?2 字节字�?? + 点阵 */
    [GT30L32S4W_FONT_ASCII_12_ARIAL] = { 0x1DC400, 26,  12, 12, 2, 2 },
    [GT30L32S4W_FONT_ASCII_12_TIMES] = { 0x1DCDC0, 26,  12, 12, 2, 2 },
    [GT30L32S4W_FONT_ASCII_16_ARIAL] = { 0x1DE580, 34,  16, 16, 2, 2 },
    [GT30L32S4W_FONT_ASCII_16_TIMES] = { 0x1DF240, 34,  16, 16, 2, 2 },
    [GT30L32S4W_FONT_ASCII_24_ARIAL] = { 0x1E22D0, 74,  24, 24, 3, 2 },
    [GT30L32S4W_FONT_ASCII_24_TIMES] = { 0x1E3E90, 74,  24, 24, 3, 2 },
    [GT30L32S4W_FONT_ASCII_32_ARIAL] = { 0x1E99D0, 130, 32, 32, 4, 2 },
    [GT30L32S4W_FONT_ASCII_32_TIMES] = { 0x1ECA90, 130, 32, 32, 4, 2 },

    /* GB2312 �?�? */
    [GT30L32S4W_FONT_GB2312_12X12]  = { 0x000000, 24,  12, 12, 2, 0 },
    [GT30L32S4W_FONT_GB2312_16X16]  = { 0x02C9D0, 32,  16, 16, 2, 0 },
    [GT30L32S4W_FONT_GB2312_24X24]  = { 0x068190, 72,  24, 24, 3, 0 },
    [GT30L32S4W_FONT_GB2312_32X32]  = { 0x0EDF00, 128, 32, 32, 4, 0 },

    /* 特殊字�?? 8x16�?0xACA1~0xACDF */
    [GT30L32S4W_FONT_GB_SPEC_8X16]  = { 0x1F2880, 16,  8,  16, 1, 0 },
};

/* ASCII �?见字符个数（0x20~0x7E�? */
#define GT_ASCII_FIRST_CHAR     0x20
#define GT_ASCII_CHAR_COUNT     95

/* GB2312 符号区字符个数（区码 0xA1~0xA9，共 846 �?�? */
#define GT_GB_SYMBOL_COUNT      846

/* ========================= 内部工具 ========================= */

static bool gt_font_is_ascii(gt30l32s4w_font_t font)
{
    return (font >= GT30L32S4W_FONT_ASCII_5X7) && (font <= GT30L32S4W_FONT_ASCII_32_TIMES);
}

static bool gt_font_is_gb(gt30l32s4w_font_t font)
{
    return (font >= GT30L32S4W_FONT_GB2312_12X12) && (font <= GT30L32S4W_FONT_GB_SPEC_8X16);
}

/**
 * @brief  GB2312 编码 -> 字库索引
 * @note   区码 0xA1~0xA9：�?�号区，index = (�?-0xA1)*94 + (�?-0xA1)
 *         区码 0xB0~0xF7：汉字区，index = 846 + (�?-0xB0)*94 + (�?-0xA1)
 *         区码 0xAA~0xAF �? GB2312 �?定义区，�?片字库不�?
 */
static esp_err_t gt_gb2312_to_index(uint16_t gb_code, uint16_t *index)
{
    uint8_t qu  = (uint8_t)(gb_code >> 8);
    uint8_t wei = (uint8_t)(gb_code & 0xFF);

    if (wei < 0xA1 || wei > 0xFE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (qu >= 0xA1 && qu <= 0xA9) {
        *index = (uint16_t)((qu - 0xA1) * 94 + (wei - 0xA1));
        return ESP_OK;
    }
    if (qu >= 0xB0 && qu <= 0xF7) {
        *index = (uint16_t)(GT_GB_SYMBOL_COUNT + (qu - 0xB0) * 94 + (wei - 0xA1));
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

/* GB2312 符号�? + 汉字区总字数：846 + 6763 */
#define GT_GB_TOTAL_COUNT       7609

/* �?检用参考字模：ASCII 8x16 �? 'A' */
static const uint8_t s_ref_ascii_A_8x16[16] = {
    0x00, 0x10, 0x28, 0x28, 0x28, 0x44, 0x44, 0x7C,
    0x82, 0x82, 0x82, 0x82, 0x00, 0x00, 0x00, 0x00,
};

/* ========================= 初�?�化 / �?检 ========================= */

esp_err_t GT30L32S4W_Init(void)
{
    return bsp_gt30l32s4w_init();
}

esp_err_t GT30L32S4W_InitWithClock(uint32_t clock_hz)
{
    return bsp_gt30l32s4w_init_with_clock(clock_hz);
}

static void gt_log_dump(const char *title, const uint8_t *data, size_t len)
{
    char line[3 * 16 + 48];
    size_t pos = 0;
    int n = snprintf(line, sizeof(line), "%s:", title);
    pos = (n > 0) ? (size_t)n : 0;
    for (size_t i = 0; i < len && i < 16 && pos + 4 < sizeof(line); i++) {
        n = snprintf(line + pos, sizeof(line) - pos, " %02X", data[i]);
        if (n <= 0) {
            break;
        }
        pos += (size_t)n;
    }
    ESP_LOGE(TAG, "%s", line);
}

esp_err_t GT30L32S4W_SelfTest(void)
{
    uint8_t buf[GT30L32S4W_GLYPH_MAX_BYTES];
    uint16_t advance = 0;

    esp_err_t err = GT30L32S4W_ReadAscii(GT30L32S4W_FONT_ASCII_8X16, 'A',
                                        buf, sizeof(buf), &advance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "self test: read ascii failed: %s", esp_err_to_name(err));
        return err;
    }
    if (memcmp(buf, s_ref_ascii_A_8x16, sizeof(s_ref_ascii_A_8x16)) != 0) {
        gt_log_dump("ascii 8x16 'A' expect", s_ref_ascii_A_8x16, sizeof(s_ref_ascii_A_8x16));
        gt_log_dump("ascii 8x16 'A' actual", buf, sizeof(s_ref_ascii_A_8x16));
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* GB2312 16x16 �? "�?" (0xB0A1) �? 4 字节 */
    err = GT30L32S4W_ReadGb2312(GT30L32S4W_FONT_GB2312_16X16, 0xB0A1,
                               buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "self test: read gb2312 failed: %s", esp_err_to_name(err));
        return err;
    }
    if (buf[0] != 0x00 || buf[1] != 0x00 || buf[2] != 0x0E || buf[3] != 0xFC) {
        gt_log_dump("gb2312 16x16 0xB0A1 actual", buf, 16);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "self test passed, spi clock=%" PRIu32 "Hz", bsp_gt30l32s4w_get_clock_hz());
    return ESP_OK;
}

/* ========================= 字库信息 / 取字�? ========================= */

esp_err_t GT30L32S4W_GetFontInfo(gt30l32s4w_font_t font, gt30l32s4w_font_info_t *info)
{
    if (info == NULL || font >= GT30L32S4W_FONT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const gt_font_record_t *f = &s_font_table[font];
    info->base_addr    = f->base_addr;
    info->width        = f->width;
    info->height       = f->height;
    info->record_size  = f->record_size;
    info->bytes_per_row = f->bytes_per_row;
    info->header_size  = f->header_size;
    info->proportional = (f->header_size > 0);
    return ESP_OK;
}

esp_err_t GT30L32S4W_ReadAscii(gt30l32s4w_font_t font, char c,
                               uint8_t *buf, size_t buf_size, uint16_t *advance)
{
    if (buf == NULL || !gt_font_is_ascii(font)) {
        return ESP_ERR_INVALID_ARG;
    }

    const gt_font_record_t *f = &s_font_table[font];
    if (buf_size < f->record_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t ch = (uint8_t)c;
    if (ch < GT_ASCII_FIRST_CHAR || ch > (GT_ASCII_FIRST_CHAR + GT_ASCII_CHAR_COUNT - 1)) {
        ch = '?';   /* 不可显示字�?�统一�? '?'，避免取到界外数�? */
    }

    uint32_t addr = f->base_addr + (uint32_t)(ch - GT_ASCII_FIRST_CHAR) * f->record_size;
    esp_err_t err = bsp_gt30l32s4w_read(addr, buf, f->record_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read ascii 0x%02X @0x%06" PRIx32 " failed: %s",
                 ch, addr, esp_err_to_name(err));
        return err;
    }

    if (advance != NULL) {
        uint16_t adv = f->width;
        if (f->header_size >= 2) {
            /* 不等宽字体：记录�? 2 字节大�??字�?? */
            adv = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
            if (adv == 0 || adv > f->width) {
                adv = f->width;
            }
        }
        *advance = adv;
    }
    return ESP_OK;
}

esp_err_t GT30L32S4W_ReadGb2312(gt30l32s4w_font_t font, uint16_t gb_code,
                                uint8_t *buf, size_t buf_size)
{
    if (buf == NULL || !gt_font_is_gb(font)) {
        return ESP_ERR_INVALID_ARG;
    }

    const gt_font_record_t *f = &s_font_table[font];
    if (buf_size < f->record_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t index = 0;
    if (font == GT30L32S4W_FONT_GB_SPEC_8X16) {
        /* 特殊字�?�：0xACA1~0xACDF 顺序排列 */
        if (gb_code < 0xACA1 || gb_code > 0xACDF) {
            return ESP_ERR_INVALID_ARG;
        }
        index = (uint16_t)(gb_code - 0xACA1);
    } else {
        esp_err_t err = gt_gb2312_to_index(gb_code, &index);
        if (err != ESP_OK) {
            return err;
        }
        if (index >= GT_GB_TOTAL_COUNT) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    uint32_t addr = f->base_addr + (uint32_t)index * f->record_size;
    esp_err_t err = bsp_gt30l32s4w_read(addr, buf, f->record_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read gb2312 0x%04X (idx=%u) @0x%06" PRIx32 " failed: %s",
                 gb_code, index, addr, esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 直接读取�?片原始数�?（扩展字�? / 图片字模等）
 */
esp_err_t GT30L32S4W_ReadRaw(uint32_t addr, void *buf, size_t len)
{
    return bsp_gt30l32s4w_read(addr, buf, len);
}

/* ========================= 显示 ========================= */

/**
 * @brief �? 1bpp 点阵画到 EPD 逻辑缓冲区（bit=1 画黑点，bit=0 擦白点）
 */
static void gt_draw_bitmap(int16_t x, int16_t y, const uint8_t *bitmap,
                           uint8_t width, uint8_t height, uint8_t bytes_per_row,
                           uint8_t inverted)
{
    for (uint8_t row = 0; row < height; row++) {
        const uint8_t *line = bitmap + (size_t)row * bytes_per_row;
        for (uint8_t col = 0; col < width; col++) {
            bool on = (line[col >> 3] & (0x80u >> (col & 0x07))) != 0;
            if (inverted) {
                on = !on;
            }
            if (on) {
                EPD_DrawPoint((int16_t)(x + col), (int16_t)(y + row));
            } else {
                EPD_ClearPoint((int16_t)(x + col), (int16_t)(y + row));
            }
        }
    }
}

esp_err_t GT30L32S4W_ShowChar(int16_t x, int16_t y, char c,
                              gt30l32s4w_font_t font, uint8_t inverted,
                              uint16_t *advance)
{
    if (!gt_font_is_ascii(font)) {
        return ESP_ERR_INVALID_ARG;
    }

    const gt_font_record_t *f = &s_font_table[font];
    uint8_t buf[GT30L32S4W_GLYPH_MAX_BYTES];
    uint16_t adv = f->width;

    esp_err_t err = GT30L32S4W_ReadAscii(font, c, buf, sizeof(buf), &adv);
    if (err != ESP_OK) {
        return err;
    }

    gt_draw_bitmap(x, y, buf + f->header_size, f->width, f->height,
                   f->bytes_per_row, inverted);

    if (advance != NULL) {
        *advance = adv;
    }
    return ESP_OK;
}

esp_err_t GT30L32S4W_ShowGbChar(int16_t x, int16_t y, uint16_t gb_code,
                                gt30l32s4w_font_t font, uint8_t inverted)
{
    if (!gt_font_is_gb(font)) {
        return ESP_ERR_INVALID_ARG;
    }

    const gt_font_record_t *f = &s_font_table[font];
    uint8_t buf[GT30L32S4W_GLYPH_MAX_BYTES];

    esp_err_t err = GT30L32S4W_ReadGb2312(font, gb_code, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    gt_draw_bitmap(x, y, buf + f->header_size, f->width, f->height,
                   f->bytes_per_row, inverted);
    return ESP_OK;
}

esp_err_t GT30L32S4W_ShowString(int16_t x, int16_t y, const char *gbk_text, gt30l32s4w_font_t ascii_font, gt30l32s4w_font_t gb_font, uint8_t inverted)
{
    if (gbk_text == NULL || !gt_font_is_ascii(ascii_font) || !gt_font_is_gb(gb_font)) {
        return ESP_ERR_INVALID_ARG;
    }

    const gt_font_record_t *af = &s_font_table[ascii_font];
    const gt_font_record_t *gf = &s_font_table[gb_font];

    /* 行高取两者较大值；ASCII 与汉字按底部（基线）对齐 */
    const int16_t line_height = (gf->height > af->height) ? (int16_t)gf->height
                                                          : (int16_t)af->height;
    const int16_t ascii_dy    = (int16_t)(line_height - (int16_t)af->height);

    const uint8_t *p = (const uint8_t *)gbk_text;
    int16_t cx = x;
    int16_t cy = y;

    while (*p != '\0') {
        if (*p == '\n') {
            cx  = x;
            cy += line_height;
            p++;
            continue;
        }
        if (*p == '\r') {
            p++;
            continue;
        }

        if (*p < 0x80) {
            /* ASCII 单字�? */
            uint16_t adv = af->width;
            if (GT30L32S4W_ShowChar(cx, (int16_t)(cy + ascii_dy), (char)(*p),
                                    ascii_font, inverted, &adv) == ESP_OK) {
                cx += (int16_t)adv;
            } else {
                cx += (int16_t)af->width;
            }
            p++;
        } else {
            /* GB2312/GBK 双字�? */
            if (p[1] == '\0') {
                break;
            }
            uint16_t code = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
            (void)GT30L32S4W_ShowGbChar(cx, cy, code, gb_font, inverted);
            cx += (int16_t)gf->width;   /* 字库外的字�?�也占位，避免文字错�? */
            p += 2;
        }
    }
    return ESP_OK;
}


