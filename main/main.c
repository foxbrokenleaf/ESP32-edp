/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:19:03
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-25 17:07:49
 * @FilePath: \epd\main\main.c
 * @Description: 应用程序入口
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "wifi.h"

#include "EPD.h"
#include "GUI.h"
#include "bsp_rtc.h"
#include "bsp_http.h"
#include "GT30L32S4W.h"
#include "font_fallback.h"
#include "bsp_GT30L32S4W.h"
#include "bsp_key.h"

static const char *TAG = "MAIN";

void SW3_Key(bsp_key_event_t event, void *user_ctx);
void SW2_Key(bsp_key_event_t event, void *user_ctx);
void SW1_Key(bsp_key_event_t event, void *user_ctx);

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

    EPD_Init();

    bsp_key_start(36, SW3_Key, BSP_KEY_EVENT_PRESS);
    bsp_key_start(39, SW2_Key, BSP_KEY_EVENT_PRESS);
    bsp_key_start(34, SW1_Key, BSP_KEY_EVENT_PRESS);

    esp_err_t err = GT30L32S4W_Init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT30L32S4W init failed: %s", esp_err_to_name(err));
        return;
    }

    /*
     * 上电自检：读回已知字模与参考数据比较。
     * 失败说明字库芯片未接好/引脚定义不对（供电、HOLD#、走线…），
     * 此时自动改用内置字模子集继续运行，屏幕上给出提示，便于脱离串口定位。
     */
    err = GT30L32S4W_SelfTest();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT30L32S4W self test failed: %s", esp_err_to_name(err));

        /* 自检失败：扫一遍候选引脚，看字库到底接在哪几个脚上 */
        (void)bsp_gt30l32s4w_bus_probe();

        /*
         * 再用 100Hz 的节奏在 CS#/SCLK/SI 上打一段方波：
         * 示波器挂在 GPIO23(CS#) 上、下降沿触发、1ms/div 就能确认
         * MCU 是否真的在片选（有波形 = MCU 在驱动，无波形 = 引脚没通/被别处占用）。
         * 时长由 BSP_GT30L32S4W_FAIL_TOGGLE_MS 控制，改成 0 可关闭。
         */
#if (BSP_GT30L32S4W_FAIL_TOGGLE_MS > 0)
        (void)bsp_gt30l32s4w_pin_toggle_test(BSP_GT30L32S4W_FAIL_TOGGLE_MS);
#endif

        if (FontFallback_HasFont(GT30L32S4W_FONT_ASCII_8X16)) {
            ESP_LOGW(TAG, "改用内置字模子集运行（%u 个字形）",
                     (unsigned)FontFallback_GlyphCount());

            char line1[] = "FONT CHIP OFF";
            char line2[] = "USE EMBEDDED FONT";
            EPD_Clear();
            EPD_ShowString(0, 0, line1, EPD_8X16, 0);
            EPD_ShowString(0, 24, line2, EPD_8X16, 0);
            EPD_Update();
            vTaskDelay(pdMS_TO_TICKS(3000));
        } else {
            char line1[] = "FONT ERROR";
            char line2[] = "CLK26 SI25 CS23 SO35";
            EPD_Clear();
            EPD_ShowString(0, 0, line1, EPD_8X16, 0);
            EPD_ShowString(0, 24, line2, EPD_8X16, 0);
            EPD_Update();
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    } else {
        ESP_LOGI(TAG, "GT30L32S4W self test OK");
    }

    bsp_rtc_init();



    

    ESP_LOGI(TAG, "wifi init");
    WiFi_Init();
    Https_Init();
    ESP_LOGI(TAG, "net init done");

    // 网络初始化完成后启动 SNTP，首次同步成功后时间会写入 NVS
    bsp_sntp_init();

    // CourseGUI();    

    while (1) {
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


void SW3_Key(bsp_key_event_t event, void *user_ctx){
    layout_no--;
    ESP_LOGI(TAG, "SW3_Key");
}

void SW2_Key(bsp_key_event_t event, void *user_ctx){
    ESP_LOGI(TAG, "SW2_Key");    
}

void SW1_Key(bsp_key_event_t event, void *user_ctx){
    ESP_LOGI(TAG, "SW1_Key");
    layout_no++;
}