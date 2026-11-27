/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-19 20:12:00
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-26 21:59:45
 * @FilePath: \epd\main\GUI.c
 * @Description: 墨水屏界面（当前课程 / 时钟 / 日历 / 清单）
 *
 * 统一视觉风格：
 *   1. 顶部：黑色圆角标题栏，白字左标题 + 右信息；
 *   2. 中部：圆角卡片分区（课表页）、整屏大字号（时钟页）、月历网格（日历页）、
 *      待办条目列表（清单页）；
 *   3. 底部：虚线上边线 + 左侧状态文字 + 右侧页码点（实心 = 当前页）。
 *
 * 四个界面由 GuiUpdate() 按 layout_no 切换，绘制完成后统一走 Gui_Flush() 刷屏。
 */
#include "GUI.h"
#include "EPD.h"
#include "GT30L32S4W.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_rtc_time.h"
#include "bsp_rtc.h"
#include "DataStruct.h"
#include "Checklist.h"
#include "Timetable.h"

uint8_t layout_no = 1;

/* ==================== Course data ==================== */
/* Course_* variables are defined in DataStruct.c, updated by the web page and saved in NVS. */
/* ===== 调试开关：1=把逻辑显存抽样成字符画打印到串口 =====
 * 用途：区分"内容根本没画进显存" 和 "画进去了但没刷到屏上"
 * 定位完成后改成 0 即可关闭（不影响功能） */
#define GUI_DEBUG_DUMP      0

#if GUI_DEBUG_DUMP
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 每 4x4 像素抽 1 个字符：'#'=黑  '.'=白
 * 同时打印复位原因 / 剩余堆 / 当前任务(app_main)栈水位（单位：字） */
static const char *TAG = "GUI_DBG";

static void GUI_DebugDump(void)
{
    char line[EPD_WIDTH / 4 + 2];
    int  n;
    int  x, y;

    ESP_LOGI(TAG, "reset_reason=%d heap_free=%u stack_hwm_words=%u",
             (int)esp_reset_reason(), (unsigned)esp_get_free_heap_size(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    for (y = 0; y < EPD_HEIGHT; y += 4) {
        n = 0;
        for (x = 0; x < EPD_WIDTH; x += 4) {
            line[n++] = EPD_GetPoint((int16_t)x, (int16_t)y) ? '.' : '#';
        }
        line[n] = '\0';
        ESP_LOGI(TAG, "%3d|%s", y, line);
    }
}
#endif

/* ==================== 函数声明 ==================== */
void ClockGUI(void);
void CalendarGUI(void);
void ListGUI(void);


void GuiUpdate(void){
    if (layout_no >= GUI_LAYOUT_COUNT) {
        layout_no = 0;              /* 防越界：layout_no 被别处乱改时归位到第一页 */
    }

    switch (layout_no)
    {
    case 0:
        TimetableGUI();
        break;
    case 1:
        ClockGUI();
        break;
    case 2:
        CalendarGUI();
        break;
    case 3:
        ListGUI();
        break;
    default:
        layout_no = 0;
        break;
    }
}

/* ========================= 公共布局常量 ========================= */

/* 整屏逻辑尺寸：296 x 152（横屏） */
#define GUI_W               EPD_WIDTH
#define GUI_MARGIN          2
#define GUI_HEADER_X        GUI_MARGIN
#define GUI_HEADER_Y        2
#define GUI_HEADER_W        (GUI_W - GUI_MARGIN * 2)
#define GUI_HEADER_H        20
#define GUI_CARD_Y          25
#define GUI_CARD_H          102
#define GUI_FOOTER_LINE_Y   130
#define GUI_FOOTER_TEXT_Y   133

/* ========================= 公共绘制工具 ========================= */

/* 整数平方根（只用于圆角计算，入参很小） */
static int16_t Gui_SqrtI(int32_t v)
{
    int16_t r = 0;

    if (v <= 0) {
        return 0;
    }
    while ((int32_t)(r + 1) * (r + 1) <= v) {
        r++;
    }
    return r;
}

/* 圆角矩形第 row 行左右各需要内缩的像素数 */
static int16_t Gui_RoundInset(int16_t row, int16_t h, int16_t r)
{
    int16_t dy;

    if (r <= 0) {
        return 0;
    }
    if (row >= r && row <= h - 1 - r) {
        return 0;
    }
    if (h - 1 - row < row) {
        row = (int16_t)(h - 1 - row);       /* 下半部分镜像到上半部分 */
    }
    dy = (int16_t)(r - row);
    return (int16_t)(r - Gui_SqrtI((int32_t)r * r - (int32_t)dy * dy));
}

/* 实心圆角矩形（黑底） */
static void Gui_FillRoundRect(int16_t x, int16_t y, uint16_t w, uint16_t h, int16_t r)
{
    int16_t row, ins;

    if (w == 0 || h == 0) {
        return;
    }
    if (r * 2 > (int16_t)h) r = (int16_t)(h / 2);
    if (r * 2 > (int16_t)w) r = (int16_t)(w / 2);

    for (row = 0; row < (int16_t)h; row++) {
        ins = Gui_RoundInset(row, (int16_t)h, r);
        EPD_DrawLine((int16_t)(x + ins), (int16_t)(y + row),
                     (int16_t)(x + (int16_t)w - 1 - ins), (int16_t)(y + row));
    }
}

/* 空心圆角矩形（1px 线宽） */
static void Gui_DrawRoundRect(int16_t x, int16_t y, uint16_t w, uint16_t h, int16_t r)
{
    int16_t row, ins;

    if (w < 2 || h < 2) {
        return;
    }
    if (r * 2 > (int16_t)h) r = (int16_t)(h / 2);
    if (r * 2 > (int16_t)w) r = (int16_t)(w / 2);

    /* 上、下两条直边 */
    EPD_DrawLine((int16_t)(x + r), y, (int16_t)(x + (int16_t)w - 1 - r), y);
    EPD_DrawLine((int16_t)(x + r), (int16_t)(y + (int16_t)h - 1),
                 (int16_t)(x + (int16_t)w - 1 - r), (int16_t)(y + (int16_t)h - 1));

    /* 左右两条直边 + 四个圆角 */
    for (row = 0; row < (int16_t)h; row++) {
        ins = Gui_RoundInset(row, (int16_t)h, r);
        EPD_DrawPoint((int16_t)(x + ins), (int16_t)(y + row));
        EPD_DrawPoint((int16_t)(x + (int16_t)w - 1 - ins), (int16_t)(y + row));
    }
}

/* 进度条：圆角外框 + 圆头填充，percent = 0~100 */
static void Gui_DrawProgressBar(int16_t x, int16_t y, uint16_t w, uint16_t h, uint8_t percent)
{
    uint16_t inner_h;
    uint16_t fill_w;

    if (w < 8 || h < 6) {
        return;
    }
    if (percent > 100) {
        percent = 100;
    }

    inner_h = (uint16_t)(h - 4);
    Gui_DrawRoundRect(x, y, w, h, (int16_t)(h / 2));
    if (percent == 0) {
        return;
    }

    fill_w = (uint16_t)(((uint32_t)(w - 4) * percent) / 100U);
    if (fill_w < inner_h) {
        fill_w = inner_h;               /* 太短时至少画一个圆头，看起来才像进度 */
    }
    Gui_FillRoundRect((int16_t)(x + 2), (int16_t)(y + 2), fill_w, inner_h,
                      (int16_t)(inner_h / 2));
}

/* 量出 GB2312/ASCII 混排字符串的像素宽度（只读字库，不画图） */
static int16_t Gui_MeasureText(const char *text,
                               gt30l32s4w_font_t ascii_font,
                               gt30l32s4w_font_t gb_font)
{
    gt30l32s4w_font_info_t gb_info;
    uint8_t buf[GT30L32S4W_GLYPH_MAX_BYTES];
    const uint8_t *p = (const uint8_t *)text;
    int16_t w = 0;

    if (text == NULL) {
        return 0;
    }
    if (GT30L32S4W_GetFontInfo(gb_font, &gb_info) != ESP_OK) {
        gb_info.width = 16;
    }

    while (*p != '\0') {
        if (*p < 0x80) {
            uint16_t adv = 0;
            if (GT30L32S4W_ReadAscii(ascii_font, (char)(*p), buf, sizeof(buf), &adv) == ESP_OK) {
                w += (int16_t)adv;
            } else {
                w += 8;
            }
            p++;
        } else {
            if (p[1] == '\0') {
                break;
            }
            w += (int16_t)gb_info.width;
            p += 2;
        }
    }
    return w;
}

/* ========================= 公共小工具 ========================= */

/* tm_wday：0=周日 ... 6=周六 */
static const char *Gui_WeekdayLong(int wday)
{
    static const char *const name[7] = {
        "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
    };
    if (wday < 0 || wday > 6) {
        return "星期?";
    }
    return name[wday];
}

static const char *Gui_WeekdayShort(int wday)
{
    static const char *const name[7] = {"日", "一", "二", "三", "四", "五", "六"};
    if (wday < 0 || wday > 6) {
        return "?";
    }
    return name[wday];
}

/* 当月天数：tm_mon 为 0~11 */
static int Gui_DaysInMonth(int tm_year, int tm_mon)
{
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int year, d;

    if (tm_mon < 0 || tm_mon > 11) {
        return 31;
    }
    d = days[tm_mon];
    if (tm_mon == 1) {
        year = 1900 + tm_year;
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
            d = 29;
        }
    }
    return d;
}

/* 一年中的第几周（周一为一周起点） */
static int Gui_WeekOfYear(const struct tm *t)
{
    int monday_wday = (t->tm_wday + 6) % 7;         /* 0=周一 */
    int week = (t->tm_yday + 7 - monday_wday) / 7;

    return (week < 1) ? 1 : week;
}

/* ========================= 公共版面块 ========================= */

/* 顶部黑色圆角标题栏：左边标题，右边补充信息（都按白字黑底画） */
static void Gui_DrawHeader(const char *title, const char *right)
{
    int16_t w;

    Gui_FillRoundRect(GUI_HEADER_X, GUI_HEADER_Y, GUI_HEADER_W, GUI_HEADER_H, 7);

    if (title != NULL) {
        GT30L32S4W_ShowString((int16_t)(GUI_HEADER_X + 7), (int16_t)(GUI_HEADER_Y + 3), title,
                              GT30L32S4W_FONT_ASCII_8X16, GT30L32S4W_FONT_GB2312_16X16, 1);
    }
    if (right != NULL) {
        w = Gui_MeasureText(right, GT30L32S4W_FONT_ASCII_8X16, GT30L32S4W_FONT_GB2312_16X16);
        GT30L32S4W_ShowString((int16_t)(GUI_HEADER_X + GUI_HEADER_W - 7 - w),
                              (int16_t)(GUI_HEADER_Y + 3), right,
                              GT30L32S4W_FONT_ASCII_8X16, GT30L32S4W_FONT_GB2312_16X16, 1);
    }
}

/* 底部状态栏：虚线上边线 + 左侧文字 + 右侧页码点 */
static void Gui_DrawFooter(const char *text)
{
    int16_t i, cx;

    EPD_DrawDashedLine((int16_t)(GUI_MARGIN + 4), GUI_FOOTER_LINE_Y,
                       (int16_t)(GUI_W - GUI_MARGIN - 5), GUI_FOOTER_LINE_Y);

    if (text != NULL) {
        GT30L32S4W_ShowString((int16_t)(GUI_MARGIN + 5), GUI_FOOTER_TEXT_Y, text,
                              GT30L32S4W_FONT_ASCII_8X16, GT30L32S4W_FONT_GB2312_16X16, 0);
    }

    for (i = 0; i < GUI_LAYOUT_COUNT; i++) {
        cx = (int16_t)(GUI_W - GUI_MARGIN - 4 - (GUI_LAYOUT_COUNT - 1 - i) * 10);
        EPD_DrawCircle(cx, (int16_t)(GUI_FOOTER_LINE_Y + 12), 2,
                       (i == (int16_t)layout_no) ? EPD_FILLED : EPD_UNFILLED);
    }
}

/* 统一刷屏入口（保留原来的显存抽样调试开关） */
static void Gui_Flush(void)
{
#if GUI_DEBUG_DUMP
    GUI_DebugDump();

    int64_t t_refresh_us = esp_timer_get_time();
#endif

    EPD_Update();

#if GUI_DEBUG_DUMP
    /* 若这里耗时远小于 EPD_BUSY_FIXED_DELAY_MS，说明刷新等待没生效 */
    ESP_LOGI(TAG, "EPD_Update took %d ms", (int)((esp_timer_get_time() - t_refresh_us) / 1000));
#endif
}

/* ========================= 时钟界面 ========================= */
void ClockGUI(void)
{
    struct tm tm_now;
    char buf[64];
    uint8_t percent = 0;
    int16_t tw;

    if (rtc_get_time(&tm_now) != ESP_OK) {
        memset(&tm_now, 0, sizeof(tm_now));
    }

    EPD_Clear();

    /* ---------- 顶部标题栏：右侧星期 ---------- */
    Gui_DrawHeader("时钟", Gui_WeekdayLong(tm_now.tm_wday));

    /* ---------- 大字号 HH:MM（32x64 点阵，整组居中） ---------- */
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    EPD_ShowString(45, 24, buf, EPD_32X64, 0);

    /* 秒：右侧，与大字底部对齐，中间加一条竖线做分隔 */
    snprintf(buf, sizeof(buf), "%02d", tm_now.tm_sec);
    EPD_DrawLine(211, 56, 211, 87);
    GT30L32S4W_ShowString(217, 56, buf,
                          GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_16X16, 0);

    /* ---------- 日期（居中，Arial 16） ---------- */
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             1900 + tm_now.tm_year, tm_now.tm_mon + 1, tm_now.tm_mday);
    tw = Gui_MeasureText(buf, GT30L32S4W_FONT_ASCII_16_ARIAL, GT30L32S4W_FONT_GB2312_16X16);
    GT30L32S4W_ShowString((int16_t)((GUI_W - tw) / 2), 92, buf,
                          GT30L32S4W_FONT_ASCII_16_ARIAL, GT30L32S4W_FONT_GB2312_16X16, 0);

    /* ---------- 今日进度条 + 百分比 ---------- */
    {
        uint32_t passed = (uint32_t)tm_now.tm_hour * 3600U
                        + (uint32_t)tm_now.tm_min * 60U
                        + (uint32_t)tm_now.tm_sec;

        percent = (uint8_t)(passed * 100U / 86400U);
    }
    Gui_DrawProgressBar(8, 112, 258, 8, percent);
    snprintf(buf, sizeof(buf), "%d%%", percent);
    GT30L32S4W_ShowString(269, 113, buf,
                          GT30L32S4W_FONT_ASCII_6X12, GT30L32S4W_FONT_GB2312_12X12, 0);

    /* ---------- 底部状态栏 ---------- */
    {
        char foot[64];

        snprintf(foot, sizeof(foot), "今天第 %d 天  第 %d 周",
                 tm_now.tm_yday + 1, Gui_WeekOfYear(&tm_now));
        Gui_DrawFooter(foot);
    }

    Gui_Flush();
}

/* ========================= 日历界面 ========================= */

/* 某年某月某日是星期几：0=周日 ... 6=周六（Zeller 公式，纯整数运算） */
static int Gui_WeekdayOfDate(int year, int month, int day)
{
    int y = year, m = month, k, j, h;

    if (m < 3) {
        m += 12;
        y -= 1;
    }
    k = y % 100;
    j = y / 100;
    h = (day + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;   /* 0=周六 */
    return (h + 6) % 7;
}

void CalendarGUI(void)
{
    struct tm tm_now;
    char buf[32];
    int year, month, first_wday, days, day, rows;
    int row, col, idx;
    int16_t gx = 1, gy;
    int16_t cell_w = 42, cell_h = 15;
    int16_t cx, cy, tw;

    if (rtc_get_time(&tm_now) != ESP_OK) {
        memset(&tm_now, 0, sizeof(tm_now));
    }

    year  = 1900 + tm_now.tm_year;
    month = tm_now.tm_mon + 1;

    EPD_Clear();

    /* ---------- 顶部标题栏：右侧年月 ---------- */
    snprintf(buf, sizeof(buf), "%04d-%02d", year, month);
    Gui_DrawHeader("日历", buf);

    /* ---------- 星期表头 ---------- */
    for (col = 0; col < 7; col++) {
        const char *w = Gui_WeekdayShort(col);
        tw = Gui_MeasureText(w, GT30L32S4W_FONT_ASCII_6X12, GT30L32S4W_FONT_GB2312_12X12);

        cx = (int16_t)(gx + col * cell_w + (cell_w - tw) / 2);
        GT30L32S4W_ShowString(cx, 25, w,
                              GT30L32S4W_FONT_ASCII_6X12, GT30L32S4W_FONT_GB2312_12X12, 0);
    }

    /* 表头下沿虚线分隔 */
    EPD_DrawDashedLine(gx, 37, (int16_t)(gx + 7 * cell_w - 1), 37);

    /* ---------- 日期格子 ---------- */
    first_wday = Gui_WeekdayOfDate(year, month, 1);
    days       = Gui_DaysInMonth(tm_now.tm_year, tm_now.tm_mon);

    /* 本月占几行（4~6 行）：行数少的月份把网格整体居中，避免下半屏空一片 */
    rows = (first_wday + days + 6) / 7;
    gy   = (int16_t)(38 + (90 - rows * cell_h) / 2);

    idx = first_wday;                       /* 第一个日期落在第几列 */
    for (day = 1; day <= days; day++) {
        row = idx / 7;
        col = idx % 7;

        cx = (int16_t)(gx + col * cell_w);
        cy = (int16_t)(gy + row * cell_h);

        snprintf(buf, sizeof(buf), "%d", day);
        tw = Gui_MeasureText(buf, GT30L32S4W_FONT_ASCII_6X12, GT30L32S4W_FONT_GB2312_12X12);

        if (day == tm_now.tm_mday) {
            /* 今天：整格黑底白字 */
            Gui_FillRoundRect((int16_t)(cx + (cell_w - 24) / 2), cy, 24, cell_h, 6);
            GT30L32S4W_ShowString((int16_t)(cx + (cell_w - tw) / 2), (int16_t)(cy + 1), buf,
                                  GT30L32S4W_FONT_ASCII_6X12, GT30L32S4W_FONT_GB2312_12X12, 1);
        } else if (col == 0 || col == 6) {
            GT30L32S4W_ShowString((int16_t)(cx + (cell_w - tw) / 2), (int16_t)(cy + 1), buf,
                                  GT30L32S4W_FONT_ASCII_6X12, GT30L32S4W_FONT_GB2312_12X12, 0);
        } else {
            GT30L32S4W_ShowString((int16_t)(cx + (cell_w - tw) / 2), (int16_t)(cy + 1), buf,
                                  GT30L32S4W_FONT_ASCII_6X12, GT30L32S4W_FONT_GB2312_12X12, 0);
        }

        idx++;
    }

    /* ---------- 底部状态栏 ---------- */
    snprintf(buf, sizeof(buf), "第 %d 周  %s", Gui_WeekOfYear(&tm_now),
             Gui_WeekdayLong(tm_now.tm_wday));
    Gui_DrawFooter(buf);

    Gui_Flush();
}

/* ========================= 通用文本绘制工具 ========================= */

/*
 * GB2312 文本的像素宽度：ASCII 6px、汉字 12px
 * （与 GT30L32S4W_FONT_ASCII_6X12 / GT30L32S4W_FONT_GB2312_12X12 的点阵一致）
 */
static int16_t Gui_Gb2312TextWidth(const char *gbk)
{
    const uint8_t *p = (const uint8_t *)gbk;
    int16_t w = 0;

    if (gbk == NULL) {
        return 0;
    }
    while (*p != '\0') {
        if (*p < 0x80) {
            w = (int16_t)(w + 6);
            p++;
        } else {
            w = (int16_t)(w + 12);
            p += 2;
        }
    }
    return w;
}

/*
 * 画网页里填的 UTF-8 文本：
 * 字库芯片只认 GB2312，先用 Checklist_Utf8ToGb2312() 转码再取模显示。
 * 返回实际占用的像素宽度（0 表示没有画）。
 */
static int16_t Gui_ShowUtf8String(int16_t x, int16_t y, const char *utf8)
{
    char gbk[CHECKLIST_ITEM_TEXT_LEN * 2 + 1];

    if (utf8 == NULL || utf8[0] == '\0') {
        return 0;
    }
    if (Checklist_Utf8ToGb2312(utf8, gbk, sizeof(gbk)) == 0) {
        return 0;
    }

    GT30L32S4W_ShowString(x, y, gbk,
                          GT30L32S4W_FONT_ASCII_6X12, GT30L32S4W_FONT_GB2312_12X12, 0);
    return Gui_Gb2312TextWidth(gbk);
}

/* ========================= 课程界面（当前 / 下一节） ========================= */
/*
 * 课程页只显示“现在正在上”或“今天下一节”的课程：
 * - 按 RTC 的星期、当前学期周次和 HH:MM 时间筛选；
 * - 现在没有课就找今天最近的一节；今天没有后续课程则显示“今天已无课程”；
 * - day=0 的旧版课程没有星期信息，不参与筛选。
 */

#define GUI_COURSE_NAME_MAX_W   (GUI_W - 20)

/* 把 "HH:MM" 转成分钟；Timetable 已校验格式 */
static uint16_t Gui_TimeToMinutes(const char *text)
{
    return (uint16_t)(((text[0] - '0') * 10 + (text[1] - '0')) * 60 +
                      (text[3] - '0') * 10 + (text[4] - '0'));
}

/* 按像素宽度截断 GB2312 文本（ascii_w / han_w 为对应字体单字宽度） */
static void Gui_TruncateGb2312(char *gbk, int16_t max_width,
                               int16_t ascii_w, int16_t han_w)
{
    const uint8_t *p = (const uint8_t *)gbk;
    int16_t w = 0;
    size_t used = 0;

    while (p[used] != '\0') {
        if (p[used] < 0x80) {
            if (w + ascii_w > max_width) {
                break;
            }
            w += ascii_w;
            used++;
        } else {
            if (p[used + 1] == '\0' || w + han_w > max_width) {
                break;
            }
            w += han_w;
            used += 2;
        }
    }
    gbk[used] = '\0';
}

/* UTF-8 -> GB2312 后按 16x16 字体显示，返回实际像素宽度 */
static int16_t Gui_ShowUtf8String16(int16_t x, int16_t y,
                                    const char *utf8, int16_t max_width)
{
    char gbk[TIMETABLE_NAME_LEN * 2 + 1];

    if (utf8 == NULL || utf8[0] == '\0') {
        return 0;
    }
    if (Checklist_Utf8ToGb2312(utf8, gbk, sizeof(gbk)) == 0) {
        return 0;
    }
    Gui_TruncateGb2312(gbk, max_width, 8, 16);
    if (gbk[0] == '\0') {
        return 0;
    }
    GT30L32S4W_ShowString(x, y, gbk,
                          GT30L32S4W_FONT_ASCII_8X16,
                          GT30L32S4W_FONT_GB2312_16X16, 0);
    return Gui_MeasureText(gbk, GT30L32S4W_FONT_ASCII_8X16,
                           GT30L32S4W_FONT_GB2312_16X16);
}

void TimetableGUI(void)
{
    struct tm tm_now;
    const timetable_course_t *cur = NULL;
    const timetable_course_t *next = NULL;
    uint16_t cur_start = 0, cur_end = 0, next_start = 0;
    uint8_t cur_pstart = 0, cur_pend = 0, next_pstart = 0, next_pend = 0;
    uint16_t now_min;
    uint8_t week, day, i, j;
    char buf[64];
    char info[TIMETABLE_CLASSROOM_LEN + TIMETABLE_TEACHER_LEN + 8];

    if (rtc_get_time(&tm_now) != ESP_OK) {
        memset(&tm_now, 0, sizeof(tm_now));
    }

    /* RTC: 0=周日 ... 6=周六；课程表: 1=周一 ... 7=周日 */
    day = (tm_now.tm_wday == 0) ? 7 : (uint8_t)tm_now.tm_wday;
    now_min = (uint16_t)(tm_now.tm_hour * 60 + tm_now.tm_min);
    week = (Semester_Week_Now >= TIMETABLE_WEEK_MIN &&
            Semester_Week_Now <= TIMETABLE_WEEK_MAX)
               ? Semester_Week_Now : TIMETABLE_WEEK_MIN;

    for (i = 0; i < Timetable_Count; i++) {
        const timetable_course_t *course = &Timetable_Courses[i];
        uint16_t start_min, end_min;

        if (course->day != day || course->start_time[0] == '\0' ||
            course->end_time[0] == '\0') {
            continue;
        }
        start_min = Gui_TimeToMinutes(course->start_time);
        end_min = Gui_TimeToMinutes(course->end_time);

        for (j = 0; j < course->session_count; j++) {
            if (course->sessions[j].week != week) {
                continue;
            }
            if (now_min >= start_min && now_min < end_min) {
                if (cur == NULL || start_min > cur_start) {
                    cur = course;
                    cur_start = start_min;
                    cur_end = end_min;
                    cur_pstart = course->sessions[j].start;
                    cur_pend = course->sessions[j].end;
                }
            } else if (start_min > now_min) {
                if (next == NULL || start_min < next_start) {
                    next = course;
                    next_start = start_min;
                    next_pstart = course->sessions[j].start;
                    next_pend = course->sessions[j].end;
                }
            }
        }
    }

    EPD_Clear();

    snprintf(buf, sizeof(buf), "%s %02d:%02d", Gui_WeekdayLong(tm_now.tm_wday),
             tm_now.tm_hour, tm_now.tm_min);
    Gui_DrawHeader("课程", buf);
    Gui_DrawRoundRect(GUI_MARGIN, GUI_CARD_Y, GUI_W - GUI_MARGIN * 2,
                      GUI_CARD_H, 8);

    if (cur != NULL || next != NULL) {
        const timetable_course_t *course = (cur != NULL) ? cur : next;
        uint8_t pstart = (cur != NULL) ? cur_pstart : next_pstart;
        uint8_t pend = (cur != NULL) ? cur_pend : next_pend;
        const char *status = (cur != NULL) ? "正在上课" : "下一节课";
        int16_t status_w = Gui_MeasureText(status,
                                           GT30L32S4W_FONT_ASCII_8X16,
                                           GT30L32S4W_FONT_GB2312_16X16);

        /* 状态徽标 */
        Gui_FillRoundRect(8, 30, 78, 20, 10);
        GT30L32S4W_ShowString((int16_t)(8 + (78 - status_w) / 2), 32, status,
                              GT30L32S4W_FONT_ASCII_8X16,
                              GT30L32S4W_FONT_GB2312_16X16, 1);

        /* 节次 */
        snprintf(buf, sizeof(buf), "第 %u-%u 节", (unsigned)pstart, (unsigned)pend);
        GT30L32S4W_ShowString(92, 34, buf,
                              GT30L32S4W_FONT_ASCII_6X12,
                              GT30L32S4W_FONT_GB2312_12X12, 0);

        /* 课程名 */
        (void)Gui_ShowUtf8String16(10, 54, course->name,
                                   GUI_COURSE_NAME_MAX_W);

        /* 上课时间 */
        snprintf(buf, sizeof(buf), "%s-%s", course->start_time, course->end_time);
        GT30L32S4W_ShowString(10, 74, buf,
                              GT30L32S4W_FONT_ASCII_12X24,
                              GT30L32S4W_FONT_GB2312_12X12, 0);

        /* 教室 / 教师 */
        info[0] = '\0';
        if (course->classroom[0] != '\0') {
            strlcpy(info, course->classroom, sizeof(info));
        }
        if (course->teacher[0] != '\0') {
            if (info[0] != '\0') {
                strlcat(info, " / ", sizeof(info));
            }
            strlcat(info, course->teacher, sizeof(info));
        }
        if (info[0] != '\0') {
            (void)Gui_ShowUtf8String(10, 102, info);
        }

        /* 当前课程进度 */
        if (cur != NULL) {
            uint8_t percent = (uint8_t)((now_min - cur_start) * 100U /
                                        (cur_end - cur_start));
            Gui_DrawProgressBar(10, 116, GUI_W - 20, 6, percent);
        }
    } else {
        const char *msg = (Timetable_Count == 0) ? "课程表为空" : "今天已无课程";
        int16_t w = Gui_MeasureText(msg, GT30L32S4W_FONT_ASCII_8X16,
                                    GT30L32S4W_FONT_GB2312_16X16);

        GT30L32S4W_ShowString((int16_t)((GUI_W - w) / 2), 62, msg,
                              GT30L32S4W_FONT_ASCII_8X16,
                              GT30L32S4W_FONT_GB2312_16X16, 0);

        if (Timetable_Count == 0) {
            const char *hint = "打开网页添加课程表";
            w = Gui_MeasureText(hint, GT30L32S4W_FONT_ASCII_6X12,
                                GT30L32S4W_FONT_GB2312_12X12);
            GT30L32S4W_ShowString((int16_t)((GUI_W - w) / 2), 88, hint,
                                  GT30L32S4W_FONT_ASCII_6X12,
                                  GT30L32S4W_FONT_GB2312_12X12, 0);
        }
    }

    snprintf(buf, sizeof(buf), "第 %u 周  %s", (unsigned)week,
             Gui_WeekdayLong(tm_now.tm_wday));
    Gui_DrawFooter(buf);
    Gui_Flush();
}

/* ========================= 清单界面 ========================= */

void ListGUI(void)
{
    struct tm tm_now;
    char buf[64];
    int16_t row_y = GUI_CARD_Y + 2;
    uint8_t total = Checklist_TotalCount();
    uint8_t done  = Checklist_DoneCount();
    uint8_t drawn = 0;
    int i;

    if (rtc_get_time(&tm_now) != ESP_OK) {
        memset(&tm_now, 0, sizeof(tm_now));
    }

    EPD_Clear();

    /* ---------- 顶部标题栏：右侧完成进度 ---------- */
    snprintf(buf, sizeof(buf), "%d/%d", (int)done, (int)total);
    Gui_DrawHeader("清单", buf);

    /* ---------- 待办条目：序号 + 复选框 + 文字 ---------- */
    for (i = 0; i < CHECKLIST_ITEM_COUNT; i++) {
        const checklist_item_t *item = &Checklist_Items[i];
        int16_t text_w;

        if (item->text[0] == '\0') {
            continue;                       /* 空项不占一行（网页里清空文字即可删除） */
        }

        snprintf(buf, sizeof(buf), "%d", (int)drawn + 1);
        GT30L32S4W_ShowString(6, (int16_t)(row_y + 1), buf,
                              GT30L32S4W_FONT_ASCII_6X12, GT30L32S4W_FONT_GB2312_12X12, 0);

        /* 复选框：空心方框，已完成再画一个对勾 */
        EPD_DrawRectangle(17, (int16_t)(row_y + 1), 10, 10, EPD_UNFILLED);
        if (item->done) {
            EPD_DrawLine(19, (int16_t)(row_y + 6), 21, (int16_t)(row_y + 9));
            EPD_DrawLine(21, (int16_t)(row_y + 9), 25, (int16_t)(row_y + 4));
        }

        text_w = Gui_ShowUtf8String(32, row_y, item->text);
        if (item->done && text_w > 2) {
            /* 已完成：文字中间划一条删除线 */
            EPD_DrawLine(32, (int16_t)(row_y + 6), (int16_t)(32 + text_w - 1),
                         (int16_t)(row_y + 6));
        }

        row_y = (int16_t)(row_y + 12);
        drawn++;
    }

    /* ---------- 空清单提示 ---------- */
    if (drawn == 0) {
        const char *hint1 = "清单为空";
        const char *hint2 = "打开网页添加待办事项";
        int16_t w1 = Gui_MeasureText(hint1, GT30L32S4W_FONT_ASCII_8X16,
                                     GT30L32S4W_FONT_GB2312_16X16);
        int16_t w2 = Gui_MeasureText(hint2, GT30L32S4W_FONT_ASCII_6X12,
                                     GT30L32S4W_FONT_GB2312_12X12);

        GT30L32S4W_ShowString((int16_t)((GUI_W - w1) / 2), 62, hint1,
                              GT30L32S4W_FONT_ASCII_8X16, GT30L32S4W_FONT_GB2312_16X16, 0);
        GT30L32S4W_ShowString((int16_t)((GUI_W - w2) / 2), 84, hint2,
                              GT30L32S4W_FONT_ASCII_6X12, GT30L32S4W_FONT_GB2312_12X12, 0);
    }

    /* ---------- 底部状态栏：日期（RTC 未校时则显示完成进度） ---------- */
    if (tm_now.tm_year > 0) {
        snprintf(buf, sizeof(buf), "%02d-%02d %s",
                 tm_now.tm_mon + 1, tm_now.tm_mday, Gui_WeekdayLong(tm_now.tm_wday));
    } else {
        snprintf(buf, sizeof(buf), "已完成 %d/%d 项", (int)done, (int)total);
    }
    Gui_DrawFooter(buf);

    Gui_Flush();
}

/* __GUI_EOF__ */
