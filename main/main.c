/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:19:03
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-10-20 23:57:22
 * @FilePath: \epd\main\main.c
 * @Description: 主程序
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


static const char *TAG = "MAIN";

void app_main(void)
{

    nvs_flash_init();

    bsp_rtc_init();
    rtc_print_task_start();

    WiFi_Init();

    EPD_Init();                          /* 初始化并清屏 */

    CourseGUI();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}