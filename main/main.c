/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:19:03
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-02 22:01:32
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

    bsp_key_start(17, SW3_Key, BSP_KEY_EVENT_PRESS);
    bsp_key_start(18, SW2_Key, BSP_KEY_EVENT_PRESS);
    bsp_key_start(19, SW1_Key, BSP_KEY_EVENT_PRESS);

    esp_err_t err = GT30L32S4W_Init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT30L32S4W init failed: %s", esp_err_to_name(err));
        return;
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