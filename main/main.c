/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:19:03
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-02 00:27:12
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

    esp_err_t err = GT30L32S4W_Init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT30L32S4W init failed: %s", esp_err_to_name(err));
        return;
    } 



    ESP_LOGI(TAG, "wifi init");
    WiFi_Init();
    Https_Init();
    ESP_LOGI(TAG, "net init done");

    /* 顺序说明：目前"先刷屏、后联网"是唯一验证过稳定的组合。
     * 放到 WiFi_Init() 之后时曾出现"某一整条竖带不显示、看起来像画面整体左移"，
     * 但后续实验（联网后画整宽黑条可以完整刷新）说明这不是单纯的供电跌落，
     * 根因仍在定位中：GUI.c 的 GUI_DEBUG_DUMP 会把逻辑显存抽样打印到串口，
     * 用于区分"内容没画进显存"和"画进去了但没刷到屏上"。定位清楚前不要改顺序。 */
    CourseGUI();    

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
