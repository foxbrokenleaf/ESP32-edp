/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:19:03
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-01 20:49:31
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

/**
 * @brief 初始化 NVS：兼容"分区写满/版本升级"导致的启动死循环
 */
static void App_NvsInit(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs: %s, erase and re-init", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "nvs ok");
    }
}

void app_main(void)
{
    App_NvsInit();

    ESP_LOGI(TAG, "rtc init");
    bsp_rtc_init();
    rtc_print_task_start();

    /* 先刷屏、再联网络：墨水屏刷新电流较大，避免和 WiFi 发射抢电源 */
    ESP_LOGI(TAG, "epd init");
    EPD_Init();

    GT30L32S4W_Demo();                   /* 字库芯片验证页（约 6 s 一次全屏刷新） */

    ESP_LOGI(TAG, "draw course gui");
    // CourseGUI();                         /* 画课程表并刷新（全屏约 6 s） */
    ESP_LOGI(TAG, "course gui refreshed");

    ESP_LOGI(TAG, "wifi init");
    WiFi_Init();
    Https_Init();
    ESP_LOGI(TAG, "net init done");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
