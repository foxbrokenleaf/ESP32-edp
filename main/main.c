/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:19:03
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-26 22:02:44
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
#include "DataStruct.h"
#include "Checklist.h"
#include "Timetable.h"
#include "GT30L32S4W.h"
#include "bsp_GT30L32S4W.h"
#include "WT588F.h"
#include "bsp_key.h"
#include "app_selftest.h"

static const char *TAG = "MAIN";

/*
 * 上电整机自检（NVS / 字库 / EPD / 语音 / 按键 / RTC）：
 *   1 = 开机跑一次，串口打表格 + 墨水屏出结果页（语音项会真的播一段）；
 *   0 = 关闭，开机直接进业务。
 * 单项开关在 app_selftest.h 里（APP_SELFTEST_TEST_xxx）。
 */
#define APP_SELFTEST_BOOT       1

/* 语音工程里要播的段地址，按实际工程改（单字节 0~223 / 双字节 0~999） */
#define APP_VOICE_TEST_ADDR     1

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

/* 语音的上电验证已并入整机自检（app_selftest.c 的 VOICE 项），此处不再单独实现 */

void app_main(void)
{
    App_NvsInit();

    /* Load course settings saved by the web page (defaults when NVS is empty). */
    Course_Init();

    /* 清单（待办列表）同样保存在 NVS 里，NVS 为空时用演示默认值 */
    Checklist_Init();

    /* Load the course table from NVS (empty table when NVS is blank). */
    Timetable_Init();

    EPD_Init();

    bsp_key_start(36, SW3_Key, BSP_KEY_EVENT_PRESS);
    bsp_key_start(39, SW2_Key, BSP_KEY_EVENT_PRESS);
    bsp_key_start(34, SW1_Key, BSP_KEY_EVENT_PRESS);

    esp_err_t err = GT30L32S4W_Init();
    if (err == ESP_OK) {
        err = GT30L32S4W_SelfTest();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT30L32S4W self test failed: %s", esp_err_to_name(err));

        /* 自检失败：扫一遍候选引脚，看字库到底接在哪几个脚上 */
        (void)bsp_gt30l32s4w_bus_probe();

        /*
         * 再用 100Hz 的节奏在 CS#/SCLK/SI 上打一段方波：
         * 示波器挂在 GPIO23(CS#) 上、下降沿触发、1ms/div 就能确认
         * MCU 是否真的在片选（有波形 = MCU 在驱动，无波形 = 引脚没通/被别处占用）。
         * 时长由 BSP_GT30L32S4W_FAIL_TOGGLE_MS 控制，改成 0 可关闭。
         *
         * 这里不再单独刷屏/等待：字库失败会由下面的整机自检结果页
         * （FONT FAIL）统一显示，避免一上电连刷两次屏。
         */
#if (BSP_GT30L32S4W_FAIL_TOGGLE_MS > 0)
        (void)bsp_gt30l32s4w_pin_toggle_test(BSP_GT30L32S4W_FAIL_TOGGLE_MS);
#endif
    } else {
        ESP_LOGI(TAG, "GT30L32S4W self test OK");
    }

    bsp_rtc_init();

    /*
     * 语音芯片：上电默认一线串口 + 单字节发码（必须与语音工程烧写时的设定一致，
     * 不一致时用 WT588F_SetWireMode()/WT588F_SetAddressMode() 改）。
     * 没接芯片时只打告警，不影响其它功能。
     * 用法：WT588F_Play(段地址);            // 立即返回
     *       WT588F_SetVolume(0~15);
     *       WT588F_PlayAndWait(段地址, 5000); // 等播完，需要 BUSY 接线
     */
    err = WT588F_Init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WT588F init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "WT588F init ok");
    }

#if APP_SELFTEST_BOOT
    /*
     * 整机自检：串口打表格 + 墨水屏出结果页。
     * 语音项会真的播一段（APP_SELFTEST_VOICE_ADDR），并采样 BUSY 判断芯片是否响应：
     *   PASS 但没声音   -> 查功放/喇叭；
     *   FAIL(no busy)  -> 查一线/两线设定、门控电平（AU_CLK_O ≈2.6V）、CS 脚。
     * 自检内部的字库/语音/EPD 调用是顺序执行的，不会在共用总线（GPIO26/25）上打架。
     */
    (void)app_selftest_run_and_show(NULL);
#endif

    ESP_LOGI(TAG, "wifi init");
    WiFi_Init();
    Https_Init();
    ESP_LOGI(TAG, "net init done");

    // 网络初始化完成后启动 SNTP，首次同步成功后时间会写入 NVS
    bsp_sntp_init();

    // TimetableGUI();

    while (1) {
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


void SW3_Key(bsp_key_event_t event, void *user_ctx){
    /* 上一页：环形回绕，避免 layout_no-- 在 0 时下溢成 255 */
    layout_no = (uint8_t)((layout_no + GUI_LAYOUT_COUNT - 1) % GUI_LAYOUT_COUNT);
    GuiUpdate();
    ESP_LOGI(TAG, "SW3_Key layout=%d", (int)layout_no);
}

void SW2_Key(bsp_key_event_t event, void *user_ctx){
    ESP_LOGI(TAG, "SW2_Key");

    /*
     * 按键播一段：顺带验证“在别的任务里发码”也安全
     * （语音 BSP 内部有互斥锁，一帧约 13ms 忙等，占住扫描任务的时间可接受）。
     */
    esp_err_t err = WT588F_Play(APP_VOICE_TEST_ADDR);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "voice: play %d failed: %s", APP_VOICE_TEST_ADDR, esp_err_to_name(err));
    }
    GuiUpdate();

}

void SW1_Key(bsp_key_event_t event, void *user_ctx){
    /* 下一页：环形切换 */
    layout_no = (uint8_t)((layout_no + 1) % GUI_LAYOUT_COUNT);
    GuiUpdate();
    ESP_LOGI(TAG, "SW1_Key layout=%d", (int)layout_no);
}
