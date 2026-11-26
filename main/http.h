/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-21 00:03:47
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-26 22:30:00
 * @FilePath: \epd\main\http.h
 * @Description: 网页管理模块接口：关于设备 / 设置课程表 / 清单。
 *
 * 由 bsp_http.c 在 HTTP 服务启动后调用 http_web_register() 注册以下路由：
 *   GET  /                管理页面（关于设备 + 设置课程表 + 清单）
 *   GET  /api/about       设备信息 JSON
 *   GET  /api/course      当前课表 JSON
 *   POST /api/course      更新课表 JSON（保存 NVS 并重绘墨水屏）
 *   GET  /api/checklist   当前清单 JSON
 *   POST /api/checklist   更新清单 JSON（保存 NVS 并切到清单页重绘）
 *   GET  /favicon.ico     空响应，避免浏览器反复 404
 */
#ifndef __HTTP_H__
#define __HTTP_H__

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 注册网页与 API 路由。
 * server：httpd_start() 返回的服务句柄。
 * 返回 ESP_OK 表示全部注册成功。
 */
esp_err_t http_web_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif /* __HTTP_H__ */
