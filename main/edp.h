#ifndef __EDP_H__
#define __EDP_H__

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ==================== 硬件引脚（按实际接线修改） ==================== */
#define EDP_PIN_BUSY   4
#define EDP_PIN_RST    13
#define EDP_PIN_DC     14
#define EDP_PIN_CS     27
#define EDP_PIN_SCL    26
#define EDP_PIN_SDA    25

/* ==================== 屏幕参数 ==================== */
#define EDP_WIDTH      152
#define EDP_HEIGHT     296
#define EDP_BUF_W      (EDP_WIDTH / 8)   /* 每行字节数 = 19 */
#define EDP_BUF_H      EDP_HEIGHT        /* 296 */

#define EDP_REFRESH_WAIT_MS   2000

/* ==================== 命令定义 ==================== */
#define EDP_CMD_POWER_ON            0x04
#define EDP_CMD_POWER_OFF           0x02
#define EDP_CMD_DISPLAY_REFRESH     0x12
#define EDP_CMD_TURN_OFF_DC         0x02
#define EDP_CMD_SOFT_RESET          0x00
#define EDP_CMD_WRITE_TEMPERATURE   0xE5
#define EDP_CMD_APPLAY_TEMPERATURE  0xE0
#define EDP_CMD_PSR                 0x00
#define EDP_CMD_DATA_START_TRANS    0x10
#define EDP_CMD_SECOND_COLOR        0x13

/* ==================== 显存（外部可见，供绘图使用） ==================== */
extern uint8_t edp_buff[EDP_BUF_H][EDP_BUF_W];

/* ==================== 驱动 API ==================== */
void edp_init(void);
void edp_busy_wait(void);
void edp_write_data(uint8_t dat);
void edp_write_command(uint8_t cmd);

void edp_cog_reset(void);
void edp_sram_mode(void);
void edp_display_init(void);
void edp_clear(void);
void edp_refresh(void);          /* 将 edp_buff 刷到屏幕 */

/* ==================== 基本绘图 API ==================== */
void edp_draw_pixel(int x, int y, bool color);
void edp_draw_line(int x0, int y0, int x1, int y1, bool color);
void edp_draw_rect(int x, int y, int w, int h, bool color);
void edp_fill_rect(int x, int y, int w, int h, bool color);

/*
 * 绘制字体
 *   x, y   : 左上角坐标
 *   str    : 字符串
 *   size   : 字号倍数（>=1，1=原始高度）
 *   invert : 反色
 *            false -> 背景黑、字白（字写 1）
 *            true  -> 背景白、字黑（先铺白底，字写 0）
 */
void edp_draw_string(int x, int y, const char *str, uint8_t size, bool invert);

#endif /* __EDP_H__ */