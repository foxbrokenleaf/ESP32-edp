/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:19:03
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-10-18 22:13:33
 * @FilePath: \epd\main\main.c
 * @Description: 主程序
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "edp.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "EDP demo start");

    edp_init();
    edp_clear();                                  /* 全白 */

    /* 四角标记 */
    edp_fill_rect(0,   0,   10, 10, false);       /* 左上 */
    edp_fill_rect(142, 0,   10, 10, false);       /* 右上 */
    edp_fill_rect(0,   286, 10, 10, false);       /* 左下 */
    edp_fill_rect(142, 286, 10, 10, false);       /* 右下 */

    /* 中线 */
    edp_draw_line(76, 0, 76, 295, false);         /* 竖直 */
    edp_draw_line(0, 148, 151, 148, false);       /* 水平 */

    /* 字符串：白底黑字 */
    edp_draw_string(10, 20, "Hello EDP 123", 1, true);
    edp_draw_string(10, 50, "TEST", 2, true);

    edp_refresh();

    ESP_LOGI(TAG, "EDP demo done");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}