/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-09-21 23:42:40
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-10-21 23:35:15
 * @FilePath: \RMS5000-BlackBoxByESP32\main\bsp_rtc.c
 * @Description: RTC模块
 * 
 * Copyright (c) 2026 by ${git_name} ${git_email} ${git_name_email}, All Rights Reserved. 
 */

#include "bsp_rtc.h"
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "RTC";

// 默认时区：中国标准时间 UTC+8
#define DEFAULT_TIMEZONE "CST-8"

// 定时打印任务参数
#define RTC_PRINT_TASK_NAME      "rtc_print_task"
#define RTC_PRINT_TASK_STACK     4096
#define RTC_PRINT_TASK_PRIO      5
#define RTC_PRINT_TASK_PERIOD_MS 1000

// NVS 存储相关定义
#define RTC_NVS_NAMESPACE       "rtc_storage"   // NVS 命名空间
#define RTC_NVS_KEY_TIME        "last_time"     // 保存时间（time_t）的键名
#define RTC_NVS_KEY_TM          "last_tm"       // 保存 struct tm 的键名
#define RTC_NVS_KEY_TIMEZONE    "timezone"      // 保存时区的键名

// 定时打印任务句柄
static TaskHandle_t s_rtc_print_task_handle = NULL;

/* ========================= 内部函数声明 ========================= */

static void rtc_print_task(void *arg);

static esp_err_t rtc_nvs_save_time(time_t time_sec);
static esp_err_t rtc_nvs_load_time(time_t *time_sec);

static esp_err_t rtc_nvs_save_tm(const struct tm *timeinfo);
static esp_err_t rtc_nvs_load_tm(struct tm *timeinfo);

static esp_err_t rtc_nvs_save_timezone(const char *tz);
static esp_err_t rtc_nvs_load_timezone(char *tz, size_t len);

/* ========================= 对外接口实现 ========================= */

/***********************************************************************
 *@author
 *@deprecated           RTC初始化
 *@see                  无
 *@param                无
 *@return               ESP32错误码
 *@exception            初始化RTC模块失败、任务启动失败
 *@date                 2026/10/04 20:48
 *@version              V0.0
 *************************************************************************/
esp_err_t bsp_rtc_init(void)
{
    ESP_LOGI(TAG, "RTC module init start");

    // 先尝试从 NVS 恢复时区
    if (rtc_restore_timezone_from_nvs() != ESP_OK) {
        ESP_LOGI(TAG, "Use default timezone");
        esp_err_t ret = rtc_set_timezone(DEFAULT_TIMEZONE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "RTC module can't initialized");
            return ret;
        }
    }

    // 尝试从 NVS 恢复上次时间（struct tm 版本，与 SETTIME 保存的键一致）
    bool restored = false;
    rtc_restore_tm_from_nvs(&restored);
    if (!restored) {
        ESP_LOGW(TAG, "No saved time in NVS, system time is default (1970)");
    }

    // 打印初始状态
    rtc_print_time();

    // 启动定时打印任务
    esp_err_t ret = rtc_print_task_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start rtc_print_task");
        return ret;
    }

    ESP_LOGI(TAG, "RTC module initialized");
    return ESP_OK;
}

esp_err_t rtc_set_timezone(const char *tz)
{
    if (tz == NULL) {
        ESP_LOGE(TAG, "Timezone string is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // 设置 TZ 环境变量
    if (setenv("TZ", tz, 1) != 0) {
        ESP_LOGE(TAG, "Failed to set TZ environment variable");
        return ESP_FAIL;
    }

    // 更新 C 库时区运行时数据
    tzset();

    ESP_LOGI(TAG, "Timezone set to: %s", tz);

    return ESP_OK;
}

esp_err_t rtc_get_time(struct tm *timeinfo)
{
    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now;
    time(&now);

    // 使用线程安全的 localtime_r
    if (localtime_r(&now, timeinfo) == NULL) {
        ESP_LOGE(TAG, "Failed to convert time to local time");
        return ESP_FAIL;
    }

    return ESP_OK;
}

time_t rtc_get_time_seconds(void)
{
    return time(NULL);
}

esp_err_t rtc_get_time_us(struct timeval *tv_now)
{
    if (tv_now == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (gettimeofday(tv_now, NULL) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t rtc_set_time_seconds(time_t time_sec)
{
    struct timeval tv = {
        .tv_sec = time_sec,
        .tv_usec = 0,
    };

    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to set system time");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "System time set to %lld seconds", (long long)time_sec);
    return ESP_OK;
}

esp_err_t rtc_set_time_tm(const struct tm *timeinfo)
{
    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 复制一份，避免修改调用者的数据
    struct tm tm_copy = *timeinfo;

    // mktime 会自动规范化 tm_wday / tm_yday，并根据 tm_isdst 处理夏令时
    // 为保证转换正确，将 tm_isdst 置为 -1 让系统自动判断
    tm_copy.tm_isdst = -1;

    time_t time_sec = mktime(&tm_copy);
    if (time_sec == (time_t)-1) {
        ESP_LOGE(TAG, "Failed to convert struct tm to time_t");
        return ESP_FAIL;
    }

    return rtc_set_time_seconds(time_sec);
}

/* ========================= NVS 存储功能 ========================= */

/**
 * @brief 将当前系统时间（time_t）保存到 NVS
 *
 * @return ESP_OK 成功，其他失败
 */
esp_err_t rtc_save_time_to_nvs(void)
{
    time_t now = time(NULL);
    return rtc_nvs_save_time(now);
}

/**
 * @brief 从 NVS 读取保存的时间（time_t）并写入系统时间
 *
 * @param[out] restored 是否成功恢复（可选，可为 NULL）
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 表示 NVS 中无数据
 */
esp_err_t rtc_restore_time_from_nvs(bool *restored)
{
    if (restored) {
        *restored = false;
    }

    time_t saved = 0;
    esp_err_t ret = rtc_nvs_load_time(&saved);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No saved time in NVS (key=%s)", RTC_NVS_KEY_TIME);
        return ret;
    }

    ret = rtc_set_time_seconds(saved);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore time from NVS");
        return ret;
    }

    if (restored) {
        *restored = true;
    }

    ESP_LOGI(TAG, "Restored time from NVS: %lld", (long long)saved);
    return ESP_OK;
}

/**
 * @brief 将 struct tm 结构体保存到 NVS
 *
 * @param[in] timeinfo 指向待保存的时间结构体
 * @return ESP_OK 成功，其他失败
 */
esp_err_t rtc_save_tm_to_nvs(const struct tm *timeinfo)
{
    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return rtc_nvs_save_tm(timeinfo);
}

/**
 * @brief 从 NVS 读取 struct tm 结构体
 *
 * @param[out] timeinfo 输出的时间结构体
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 表示 NVS 中无数据
 */
esp_err_t rtc_load_tm_from_nvs(struct tm *timeinfo)
{
    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return rtc_nvs_load_tm(timeinfo);
}

/**
 * @brief 从 NVS 读取 struct tm 并设置为系统时间
 *
 * @param[out] restored 是否成功恢复（可选，可为 NULL）
 * @return ESP_OK 成功
 */
esp_err_t rtc_restore_tm_from_nvs(bool *restored)
{
    if (restored) {
        *restored = false;
    }

    struct tm timeinfo = {0};
    esp_err_t ret = rtc_nvs_load_tm(&timeinfo);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No saved struct tm in NVS (key=%s)", RTC_NVS_KEY_TM);
        return ret;
    }

    ret = rtc_set_time_tm(&timeinfo);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restore system time from NVS");
        return ret;
    }

    if (restored) {
        *restored = true;
    }

    ESP_LOGI(TAG, "Restored struct tm from NVS: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return ESP_OK;
}

/**
 * @brief 将当前系统时间（struct tm）保存到 NVS
 *
 * @return ESP_OK 成功
 */
esp_err_t rtc_save_current_tm_to_nvs(void)
{
    struct tm timeinfo = {0};
    esp_err_t ret = rtc_get_time(&timeinfo);
    if (ret != ESP_OK) {
        return ret;
    }
    return rtc_nvs_save_tm(&timeinfo);
}

/**
 * @brief 保存时区字符串到 NVS
 *
 * @param[in] tz 时区字符串，如 "CST-8"
 * @return ESP_OK 成功
 */
esp_err_t rtc_save_timezone_to_nvs(const char *tz)
{
    if (tz == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return rtc_nvs_save_timezone(tz);
}

/**
 * @brief 从 NVS 读取时区并应用
 *
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 表示无数据
 */
esp_err_t rtc_restore_timezone_from_nvs(void)
{
    char tz[32] = {0};
    esp_err_t ret = rtc_nvs_load_timezone(tz, sizeof(tz));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No saved timezone in NVS");
        return ret;
    }

    return rtc_set_timezone(tz);
}

esp_err_t rtc_format_time(char *buf, size_t buf_size, const char *format)
{
    if (buf == NULL || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now;
    struct tm timeinfo;

    time(&now);
    if (localtime_r(&now, &timeinfo) == NULL) {
        return ESP_FAIL;
    }

    size_t len = strftime(buf, buf_size, format, &timeinfo);
    if (len == 0 && buf_size > 0) {
        // 缓冲区不足或格式化失败
        ESP_LOGE(TAG, "Failed to format time, buffer size may be insufficient");
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

void rtc_print_time(void)
{
    char buf[64];
    if (rtc_format_time(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %A") == ESP_OK) {
        ESP_LOGI(TAG, "Current time: %s", buf);
    }
}

esp_err_t rtc_print_task_start(void)
{
    if (s_rtc_print_task_handle != NULL) {
        ESP_LOGW(TAG, "rtc_print_task already started");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(
        rtc_print_task,
        RTC_PRINT_TASK_NAME,
        RTC_PRINT_TASK_STACK,
        NULL,
        RTC_PRINT_TASK_PRIO,
        &s_rtc_print_task_handle
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create rtc_print_task");
        s_rtc_print_task_handle = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "rtc_print_task started");
    return ESP_OK;
}

void rtc_print_task_stop(void)
{
    if (s_rtc_print_task_handle != NULL) {
        vTaskDelete(s_rtc_print_task_handle);
        s_rtc_print_task_handle = NULL;
        ESP_LOGI(TAG, "rtc_print_task stopped");
    }
}

/* ========================= 内部函数实现 ========================= */

/**
 * @brief RTC 时间打印任务
 *        每 1 秒打印一次当前时间
 */
static void rtc_print_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(RTC_PRINT_TASK_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "rtc_print_task running");

    while (1) {
        rtc_print_time();
        // 使用 vTaskDelayUntil 保证周期稳定，不随任务执行时间漂移
        vTaskDelayUntil(&last_wake, period);
    }
}

/* ========================= NVS 内部函数实现 ========================= */

/**
 * @brief 将时间（秒）保存到 NVS
 */
static esp_err_t rtc_nvs_save_time(time_t time_sec)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(RTC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // time_t 可能为 64 位，使用 blob 存储更安全
    ret = nvs_set_blob(handle, RTC_NVS_KEY_TIME, &time_sec, sizeof(time_sec));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Time saved to NVS: %lld", (long long)time_sec);
    return ESP_OK;
}

/**
 * @brief 从 NVS 读取时间（秒）
 */
static esp_err_t rtc_nvs_load_time(time_t *time_sec)
{
    if (time_sec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(RTC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t required_size = sizeof(time_t);
    ret = nvs_get_blob(handle, RTC_NVS_KEY_TIME, time_sec, &required_size);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (required_size != sizeof(time_t)) {
        ESP_LOGE(TAG, "NVS time size mismatch: %d != %d",
                 (int)required_size, (int)sizeof(time_t));
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 将 struct tm 保存到 NVS
 *
 * 使用 blob 存储，保证结构体内存布局完整保存
 */
static esp_err_t rtc_nvs_save_tm(const struct tm *timeinfo)
{
    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(RTC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, RTC_NVS_KEY_TM, timeinfo, sizeof(struct tm));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob(struct tm) failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "struct tm saved to NVS: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    return ESP_OK;
}

/**
 * @brief 从 NVS 读取 struct tm
 */
static esp_err_t rtc_nvs_load_tm(struct tm *timeinfo)
{
    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(RTC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t required_size = sizeof(struct tm);
    ret = nvs_get_blob(handle, RTC_NVS_KEY_TM, timeinfo, &required_size);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(struct tm) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 校验大小是否匹配（防止结构体版本变化导致读取错乱）
    if (required_size != sizeof(struct tm)) {
        ESP_LOGE(TAG, "NVS struct tm size mismatch: got %d, expect %d",
                 (int)required_size, (int)sizeof(struct tm));
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 保存时区字符串到 NVS
 */
static esp_err_t rtc_nvs_save_timezone(const char *tz)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(RTC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(handle, RTC_NVS_KEY_TIMEZONE, tz);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str failed: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return ret;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Timezone saved to NVS: %s", tz);
    }
    return ret;
}

/**
 * @brief 从 NVS 读取时区字符串
 */
static esp_err_t rtc_nvs_load_timezone(char *tz, size_t len)
{
    if (tz == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(RTC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t required_size = len;
    ret = nvs_get_str(handle, RTC_NVS_KEY_TIMEZONE, tz, &required_size);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }

    return ret;
}