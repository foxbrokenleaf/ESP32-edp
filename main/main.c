/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:19:03
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-01 00:00:00
 * @FilePath: \epd\main\main.c
 * @Description: 应用程序入口
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi.h"

#include "EPD.h"
#include "GUI.h"
#include "bsp_rtc.h"
#include "bsp_http.h"
#include "GT30L32S4W.h"


static const char *TAG = "MAIN";

/**
 * @brief 字库芯片验证：SPI 自检 + 打印字模 + 显示演示页
 * @note  文字使用 GBK 转义序列，避免源文件编码影响；
 *        验证完成后可删除 app_main() 里对本函数的调用。
 */
static void GT30L32S4W_Demo(void)
{
    uint8_t  glyph[GT30L32S4W_GLYPH_MAX_BYTES] = {0};
    uint16_t advance = 0;

    esp_err_t err = GT30L32S4W_Init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT30L32S4W init failed: %s", esp_err_to_name(err));
        return;
    }

    err = GT30L32S4W_SelfTest();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT30L32S4W self test failed: %s", esp_err_to_name(err));
        return;
    }

    /* ASCII 'A'（8x16） */
    if (GT30L32S4W_ReadAscii(GT30L32S4W_FONT_ASCII_8X16, 'A',
                             glyph, sizeof(glyph), &advance) == ESP_OK) {
        ESP_LOGI(TAG, "ASCII 'A' 8x16 (advance=%u): %02X %02X %02X %02X %02X %02X %02X %02X ...",
                 advance, glyph[0], glyph[1], glyph[2], glyph[3],
                 glyph[4], glyph[5], glyph[6], glyph[7]);
    }

    /* GB2312 "啊" = 0xB0A1（16x16） */
    if (GT30L32S4W_ReadGb2312(GT30L32S4W_FONT_GB2312_16X16, 0xB0A1,
                              glyph, sizeof(glyph)) == ESP_OK) {
        ESP_LOGI(TAG, "GB2312 0xB0A1 16x16: %02X %02X %02X %02X %02X %02X %02X %02X ...",
                 glyph[0], glyph[1], glyph[2], glyph[3],
                 glyph[4], glyph[5], glyph[6], glyph[7]);
    }

    /* 演示页：验证 取字模 -> EPD 显示 整条链路 */
    EPD_Clear();
    /* 24x24：ASCII + 汉字混排（"你好 GT30L32S4W"） */
    GT30L32S4W_ShowString(4, 4, "\xC4\xE3\xBA\xC3 GT30L32S4W",
                          GT30L32S4W_FONT_ASCII_12X24,
                          GT30L32S4W_FONT_GB2312_24X24, 0);
    /* 16px Arial */
    GT30L32S4W_ShowString(4, 36, "Arial 16: Hello 0123",
                          GT30L32S4W_FONT_ASCII_16_ARIAL,
                          GT30L32S4W_FONT_GB2312_16X16, 0);
    /* 16px Times New Roman */
    GT30L32S4W_ShowString(4, 60, "Times 16: Hello 0123",
                          GT30L32S4W_FONT_ASCII_16_TIMES,
                          GT30L32S4W_FONT_GB2312_16X16, 0);
    /* 16x16 汉字："世界 温度" */
    GT30L32S4W_ShowString(4, 84, "\xCA\xC0\xBD\xE7 \xCE\xC2\xB6\xC8 16x16",
                          GT30L32S4W_FONT_ASCII_8X16,
                          GT30L32S4W_FONT_GB2312_16X16, 0);
    /* 32x32 汉字："课程表"（反白显示） */
    GT30L32S4W_ShowString(4, 108, "\xBF\xCE\xB3\xCC\xB1\xED 32x32",
                          GT30L32S4W_FONT_ASCII_16X32,
                          GT30L32S4W_FONT_GB2312_32X32, 1);
    EPD_Update();

    vTaskDelay(pdMS_TO_TICKS(2000));
}

void app_main(void)
{

    nvs_flash_init();

    bsp_rtc_init();
    rtc_print_task_start();

    WiFi_Init();
    Https_Init();

    EPD_Init();                          /* 初始化显示屏 */

    GT30L32S4W_Demo();                   /* 字库芯片验证页，验证后可删除 */

    CourseGUI();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
