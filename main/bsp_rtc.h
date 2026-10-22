/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-09-21 23:42:45
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-10-15 11:17:49
 * @FilePath: \RMS5000-BlackBoxByESP32\main\bsp_rtc.h
 * @Description: RTC模块
 * 
 * Copyright (c) 2026 by ${git_name} ${git_email} ${git_name_email}, All Rights Reserved. 
 */

#ifndef __BSP_RTC_H__
#define __BSP_RTC_H__

#include <time.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= 基础 RTC 接口 ========================= */

esp_err_t bsp_rtc_init(void);
esp_err_t rtc_set_timezone(const char *tz);

esp_err_t rtc_get_time(struct tm *timeinfo);
time_t    rtc_get_time_seconds(void);
esp_err_t rtc_get_time_us(struct timeval *tv_now);

esp_err_t rtc_set_time_seconds(time_t time_sec);
esp_err_t rtc_set_time_tm(const struct tm *timeinfo);

esp_err_t rtc_format_time(char *buf, size_t buf_size, const char *format);
void      rtc_print_time(void);

esp_err_t rtc_print_task_start(void);
void      rtc_print_task_stop(void);

/* ========================= NVS 存储接口 ========================= */

/* time_t 版本 */
esp_err_t rtc_save_time_to_nvs(void);
esp_err_t rtc_restore_time_from_nvs(bool *restored);

/* struct tm 版本 */
esp_err_t rtc_save_tm_to_nvs(const struct tm *timeinfo);
esp_err_t rtc_load_tm_from_nvs(struct tm *timeinfo);
esp_err_t rtc_restore_tm_from_nvs(bool *restored);
esp_err_t rtc_save_current_tm_to_nvs(void);

/* 时区版本 */
esp_err_t rtc_save_timezone_to_nvs(const char *tz);
esp_err_t rtc_restore_timezone_from_nvs(void);

/* ========================= SNTP 接口 ========================= */
void bsp_sntp_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_RTC_H__ */