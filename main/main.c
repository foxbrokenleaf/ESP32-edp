/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:19:03
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-10-18 23:29:11
 * @FilePath: \epd\main\main.c
 * @Description: 主程序
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "EPD.h"
#include "GUI.h"

static const char *TAG = "MAIN";

void app_main(void)
{

    EPD_Init();                          /* 初始化并清屏 */

    CourseGUI();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}