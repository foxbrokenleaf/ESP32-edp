/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-19 20:12:00
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-02 19:14:47
 * @FilePath: \epd\main\GUI.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "GUI.h"
#include "EPD.h"
#include "GT30L32S4W.h"
#include <time.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_rtc_time.h"
#include "bsp_rtc.h"

uint8_t layout_no = 1;

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

void CourseGUI(void){
    EPD_Clear();
    
    //上课状态
    GT30L32S4W_ShowString(0, 0, "正", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_32X32, 0);
    GT30L32S4W_ShowString(0, 32, "在", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_32X32, 0);
    GT30L32S4W_ShowString(0, 64, "上", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_32X32, 0);
    GT30L32S4W_ShowString(0, 96, "课", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_32X32, 0); 

    //课程名称
    GT30L32S4W_ShowString(48, 32, "Python高级应用", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_32X32, 0);
    GT30L32S4W_ShowString(112, 64, "理论课", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_32X32, 0);

    //上课地点
    GT30L32S4W_ShowString(276, 0, "教", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_16X16, 0);
    EPD_ShowChar(280, 16, 'D', EPD_8X16, 0);
    EPD_ShowChar(280, 32, '-', EPD_8X16, 0);
    EPD_ShowChar(280, 48, '4', EPD_8X16, 0);
    EPD_ShowChar(280, 64, '0', EPD_8X16, 0);
    EPD_ShowChar(280, 80, '8', EPD_8X16, 0);

    //上课时间
    //固定
    EPD_ShowChar(155, 131, 'S', EPD_8X16, 0);
    EPD_ShowChar(251, 131, 'E', EPD_8X16, 0);
    //变动
    EPD_ShowString(163, 131, "15:35~17:10", EPD_8X16, 0);

    //第n周
    GT30L32S4W_ShowString(276, 100, "第", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_16X16, 0);
    EPD_ShowNum(276, 116, 2, 2, EPD_8X16, 0);   //n
    GT30L32S4W_ShowString(276, 132, "周", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_16X16, 0);

    //时间
    struct tm *tmp_tm = malloc(sizeof(struct tm));
    rtc_get_time(tmp_tm);
    EPD_ShowNum(1, 132, 1900 + tmp_tm->tm_year, 4, EPD_8X16, 0);  //年
    EPD_ShowNum(33, 132, 1 + tmp_tm->tm_mon, 2, EPD_8X16, 0);   //月
    EPD_ShowNum(49, 132, tmp_tm->tm_mday, 2, EPD_8X16, 0);   //日

    EPD_ShowNum(73, 132, tmp_tm->tm_hour, 2, EPD_8X16, 0);   //时
    EPD_ShowChar(89, 132, ':', EPD_8X16, 0);
    EPD_ShowNum(97, 132, tmp_tm->tm_min, 2, EPD_8X16, 0);   //分
    free(tmp_tm);

    //框线
    EPD_DrawLine(143, 128, 143, 151);
    EPD_DrawLine(128, 128, 128, 151);
    EPD_DrawLine(47, 0, 47, 128);
    EPD_DrawLine(32, 0, 32, 128);
    EPD_DrawLine(272, 0, 272, 151);
    EPD_DrawLine(0, 128, 272, 128);

    // EPD_DrawRectangle(0, 0, 296, 152, EPD_FILLED);

    //美化

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

void ClockGUI(void){
    EPD_Clear();

    {    
    char *buf = malloc(5);
    if (rtc_format_time(buf, 5, "%Y") == ESP_OK)
        GT30L32S4W_ShowString(4, 88, buf, GT30L32S4W_FONT_ASCII_32_ARIAL, GT30L32S4W_FONT_GB2312_16X16, 0);
    free(buf);
    }
    {    
    char *buf = malloc(6);
    if (rtc_format_time(buf, 6, "%m/%d") == ESP_OK)
        GT30L32S4W_ShowString(0, 120, buf, GT30L32S4W_FONT_ASCII_32_ARIAL, GT30L32S4W_FONT_GB2312_16X16, 0);
    free(buf);
    }    
    {    
    char *buf = malloc(10);
    if (rtc_format_time(buf, 10, "%A") == ESP_OK)
        GT30L32S4W_ShowString(120, 120, buf, GT30L32S4W_FONT_ASCII_32_ARIAL, GT30L32S4W_FONT_GB2312_16X16, 0);
    free(buf);
    }
    {
    char *buf = malloc(6);
    if (rtc_format_time(buf, 6, "%H:%M") == ESP_OK)
        GT30L32S4W_ShowString(146, 51, buf, GT30L32S4W_FONT_ASCII_32_ARIAL, GT30L32S4W_FONT_GB2312_16X16, 0);
    free(buf);
    }    
    GT30L32S4W_ShowString(256, 0, "\xAC\xC0\xAC\xC4 \xAC\xC6\xAC\xC9", GT30L32S4W_FONT_ASCII_8X16, GT30L32S4W_FONT_GB_SPEC_8X16, 0);


    //框线
    EPD_DrawLine(0, 15, 295, 15);
    EPD_DrawLine(79, 120, 295, 120);
    EPD_DrawLine(80, 15, 80, 151);  

    EPD_Update();
}