#include "edp.h"
#include "edp_font.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ==================== ESP-IDF 引脚操作宏 ==================== */
#define PIN_MODE_OUTPUT(p)         do { \
        gpio_config_t io = { \
            .pin_bit_mask = (1ULL << (p)), \
            .mode = GPIO_MODE_OUTPUT, \
            .pull_up_en = GPIO_PULLUP_DISABLE, \
            .pull_down_en = GPIO_PULLDOWN_DISABLE, \
            .intr_type = GPIO_INTR_DISABLE, \
        }; \
        gpio_config(&io); \
    } while (0)

#define PIN_WRITE(p, v)            gpio_set_level((p), (v))
#define DELAY_MS(ms)               vTaskDelay(pdMS_TO_TICKS(ms))

/* ==================== 显存 ==================== */
uint8_t edp_buff[EDP_BUF_H][EDP_BUF_W];

/* ==================== 底层引脚操作 ==================== */
static inline void RST_Write(uint8_t s) { PIN_WRITE(EDP_PIN_RST, s ? 1 : 0); }
static inline void DC_Write (uint8_t s) { PIN_WRITE(EDP_PIN_DC,  s ? 1 : 0); }
static inline void CS_Write (uint8_t s) { PIN_WRITE(EDP_PIN_CS,  s ? 1 : 0); }
static inline void SCL_Write(uint8_t s) { PIN_WRITE(EDP_PIN_SCL, s ? 1 : 0); }
static inline void SDA_Write(uint8_t s) { PIN_WRITE(EDP_PIN_SDA, s ? 1 : 0); }

/* ==================== BUSY 等待（BUSY 未接 MCU，用固定延时） ==================== */
void edp_busy_wait(void)
{
    DELAY_MS(EDP_REFRESH_WAIT_MS);
}

/* ==================== 写数据 / 写命令 ==================== */
void edp_write_data(uint8_t dat)
{
    int i;
    CS_Write(0);
    DC_Write(1);
    for (i = 0; i < 8; i++) {
        SCL_Write(0);
        SDA_Write((dat & (0x80 >> i)) ? 1 : 0);
        SCL_Write(1);
        SCL_Write(0);
    }
    CS_Write(1);
}

void edp_write_command(uint8_t cmd)
{
    int i;
    CS_Write(0);
    DC_Write(0);
    for (i = 0; i < 8; i++) {
        SCL_Write(0);
        SDA_Write((cmd & (0x80 >> i)) ? 1 : 0);
        SCL_Write(1);
        SCL_Write(0);
    }
    CS_Write(1);
}

/* ==================== 复位 / SRAM 模式 ==================== */
void edp_cog_reset(void)
{
    DELAY_MS(5);  RST_Write(1);
    DELAY_MS(5);  RST_Write(0);
    DELAY_MS(10); RST_Write(1);
    DELAY_MS(5);  CS_Write(1);
    DELAY_MS(5);
}

void edp_sram_mode(void)
{
    DELAY_MS(50);
    edp_write_command(EDP_CMD_POWER_ON);        edp_write_command(EDP_CMD_POWER_ON);
    DELAY_MS(5);  edp_busy_wait();

    edp_write_command(EDP_CMD_DISPLAY_REFRESH); edp_write_command(EDP_CMD_DISPLAY_REFRESH);
    DELAY_MS(5);  edp_busy_wait();

    edp_write_command(EDP_CMD_TURN_OFF_DC);     edp_write_command(EDP_CMD_TURN_OFF_DC);
    edp_busy_wait();

    /* 不再拉低 RST / DC / CS，保持面板显示 */
    DC_Write(0);
    CS_Write(1);
    RST_Write(1);      // ★ 保持高电平，不复位
}


/* ==================== 初始化 ==================== */
void edp_display_init(void)
{
    edp_cog_reset();

    edp_write_command(EDP_CMD_SOFT_RESET); edp_write_data(0xE0);
    DELAY_MS(5);

    edp_write_command(EDP_CMD_WRITE_TEMPERATURE);  edp_write_data(0x19);
    edp_write_command(EDP_CMD_APPLAY_TEMPERATURE); edp_write_data(0x02);
    edp_write_command(EDP_CMD_PSR);
    edp_write_data(0xCF);
    edp_write_data(0x8D);
}

void edp_init(void)
{
    /* BUSY 未接 MCU，不配置 */
    PIN_MODE_OUTPUT(EDP_PIN_RST);
    PIN_MODE_OUTPUT(EDP_PIN_DC);
    PIN_MODE_OUTPUT(EDP_PIN_CS);
    PIN_MODE_OUTPUT(EDP_PIN_SCL);
    PIN_MODE_OUTPUT(EDP_PIN_SDA);

    RST_Write(1);
    DC_Write(1);
    CS_Write(1);
    SCL_Write(1);
    SDA_Write(1);

    memset(edp_buff, 0, sizeof(edp_buff));
    edp_display_init();
}

/* ==================== 清屏 ==================== */
void edp_clear(void)
{
    uint16_t i, j;

    memset(edp_buff, 0, sizeof(edp_buff));

    edp_display_init();

    edp_write_command(EDP_CMD_DATA_START_TRANS);
    for (i = 0; i < EDP_BUF_H; i++)
        for (j = 0; j < EDP_BUF_W; j++)
            edp_write_data(0x00);

    edp_write_command(EDP_CMD_SECOND_COLOR);
    for (i = 0; i < EDP_BUF_H; i++)
        for (j = 0; j < EDP_BUF_W; j++)
            edp_write_data(0x00);

    edp_sram_mode();
}

/* ==================== 刷新显存到屏幕 ==================== */
void edp_refresh(void)
{
    uint16_t i, j;

    edp_display_init();

    edp_write_command(EDP_CMD_DATA_START_TRANS);
    for (i = 0; i < EDP_BUF_H; i++)
        for (j = 0; j < EDP_BUF_W; j++)
            edp_write_data(edp_buff[i][j]);

    edp_write_command(EDP_CMD_SECOND_COLOR);
    for (i = 0; i < EDP_BUF_H; i++)
        for (j = 0; j < EDP_BUF_W; j++)
            edp_write_data(0x00);

    edp_sram_mode();
}

/* ==================== 内部：写显存 ==================== */
static void set_pixel_buffer(int x, int y, bool color)
{
    uint8_t mask;
    if (x < 0 || x >= EDP_WIDTH || y < 0 || y >= EDP_HEIGHT) return;
    mask = 0x80 >> (x & 0x07);

    /* 面板极性：1 = 黑，0 = 白。API 约定 color=true 表示白，故这里取反 */
    if (color)
        edp_buff[y][x >> 3] &= (uint8_t)~mask;   /* 白 → 写 0 */
    else
        edp_buff[y][x >> 3] |= mask;              /* 黑 → 写 1 */
}

/* ==================== 画点 ==================== */
void edp_draw_pixel(int x, int y, bool color)
{
    set_pixel_buffer(x, y, color);
}

/* ==================== 画线（Bresenham） ==================== */
void edp_draw_line(int x0, int y0, int x1, int y1, bool color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (1) {
        int e2;
        set_pixel_buffer(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ==================== 矩形 ==================== */
void edp_draw_rect(int x, int y, int w, int h, bool color)
{
    if (w <= 0 || h <= 0) return;
    edp_draw_line(x,         y,         x + w - 1, y,         color);
    edp_draw_line(x,         y + h - 1, x + w - 1, y + h - 1, color);
    edp_draw_line(x,         y,         x,         y + h - 1, color);
    edp_draw_line(x + w - 1, y,         x + w - 1, y + h - 1, color);
}

void edp_fill_rect(int x, int y, int w, int h, bool color)
{
    int i, j;
    if (w <= 0 || h <= 0) return;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            set_pixel_buffer(x + i, y + j, color);
}

/* ==================== 字模索引 ==================== */
static uint8_t font_index(char c)
{
    if (c == 32)                   return 0;
    else if (c >= '0' && c <= '9') return (uint8_t)(c - 47);
    else if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 28);
    else if (c >= 'a' && c <= 'z') return (uint8_t)(c - 86);
    return 0;
}

/* ==================== 绘制字符串 ==================== */
void edp_draw_string(int x, int y, const char *str, uint8_t size, bool invert)
{
    const int COLS = EDP_FONT_COLS;
    const int ROWS = EDP_FONT_ROWS;
    const int SP   = 1;
    int cursorX = x;
    int col, row, sx, sy;
    uint8_t idx, bits;
    bool on, color;

    if (size < 1) size = 1;

    while (*str) {
        idx = font_index(*str);

        if (invert) {
            for (col = 0; col < COLS * size; col++)
                for (row = 0; row < ROWS * size; row++)
                    set_pixel_buffer(cursorX + col, y + row, true);
        }

        for (col = 0; col < COLS; col++) {
            bits = edp_font[idx][col];
            for (row = 0; row < ROWS; row++) {
                on = (bits >> row) & 0x01;
                color = invert ? (on ? false : true) : on;
                for (sx = 0; sx < size; sx++)
                    for (sy = 0; sy < size; sy++)
                        set_pixel_buffer(cursorX + col * size + sx,
                                         y + row * size + sy,
                                         color);
            }
        }
        cursorX += (COLS + SP) * size;
        str++;
    }
}