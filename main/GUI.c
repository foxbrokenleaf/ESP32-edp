/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-19 20:12:00
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-02 00:10:52
 * @FilePath: \epd\main\GUI.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "GUI.h"
#include "EPD.h"
#include "GT30L32S4W.h"

/* ===== 调试开关：1=把逻辑显存抽样成字符画打印到串口 =====
 * 用途：区分"内容根本没画进显存" 和 "画进去了但没刷到屏上"
 * 定位完成后改成 0 即可关闭（不影响功能） */
#define GUI_DEBUG_DUMP      1

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
    EPD_ShowChineseChar(276, 0, Font_16x16_E7A791, EPD_16X16, 0);
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
    EPD_ShowChineseChar(276, 100, Font_16x16_E783AB, EPD_16X16, 0);
    EPD_ShowNum(276, 116, 2, 2, EPD_8X16, 0);   //n
    EPD_ShowChineseChar(276, 132, Font_16x16_E783AB, EPD_16X16, 0);

    //时间
    EPD_ShowNum(1, 132, 2026, 4, EPD_8X16, 0);  //年
    EPD_ShowNum(33, 132, 10, 2, EPD_8X16, 0);   //月
    EPD_ShowNum(49, 132, 20, 2, EPD_8X16, 0);   //日

    EPD_ShowNum(73, 132, 0, 2, EPD_8X16, 0);   //时
    EPD_ShowChar(89, 132, ':', EPD_8X16, 0);
    EPD_ShowNum(97, 132, 2, 2, EPD_8X16, 0);   //分

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