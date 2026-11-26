/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-21 00:03:47
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-26 22:30:00
 * @FilePath: \epd\main\http.c
 * @Description: 网页管理模块（关于设备 / 设置课程表）。
 *
 * 页面使用内嵌 HTML（不依赖文件系统），接口：
 *   GET  /api/about   设备硬件、固件、网络、运行时间等信息
 *   GET  /api/course  当前课表
 *   POST /api/course  更新课表 -> 写入 NVS -> 重绘墨水屏
 */
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "DataStruct.h"
#include "GUI.h"
#include "bsp_rtc.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HTTP_WEB";

/* POST 请求体上限（课表 JSON 很短） */
#define WEB_COURSE_BODY_MAX         768
/* 保存课表后重绘墨水屏的任务栈（与按键任务一致） */
#define WEB_GUI_TASK_STACK          4096

/* ========================= 内嵌网页 ========================= */
/*
 * 每行一个 C 字符串，发送时逐行拼接（额外补换行）。
 * 页面内部一律使用单引号，避免 C 字符串转义。
 */
static const char *const INDEX_HTML[] = {
"<!DOCTYPE html>",
"<html lang='zh-CN'>",
"<head>",
"<meta charset='utf-8'>",
"<meta name='viewport' content='width=device-width, initial-scale=1'>",
"<title>墨水屏课表 · 设备管理</title>",
"<style>",
":root { color-scheme: dark; }",
"* { box-sizing: border-box; }",
"body {",
"  margin: 0;",
"  min-height: 100vh;",
"  font-family: 'Segoe UI', 'Microsoft YaHei', sans-serif;",
"  background: radial-gradient(circle at top, #2a3040 0%, #1a1b1e 55%);",
"  color: #e6e6e6;",
"}",
"header {",
"  padding: 22px 20px 10px;",
"  max-width: 760px;",
"  margin: 0 auto;",
"}",
"header h1 { margin: 0; font-size: 1.45rem; letter-spacing: 0.5px; }",
"header p { margin: 6px 0 0; color: #9aa4b2; font-size: 0.86rem; }",
"nav {",
"  display: flex;",
"  gap: 10px;",
"  max-width: 760px;",
"  margin: 8px auto 0;",
"  padding: 0 20px;",
"}",
".tab {",
"  flex: 1;",
"  padding: 11px 16px;",
"  border-radius: 12px;",
"  border: 1px solid #3e4147;",
"  background: #2b2d31;",
"  color: #b0b8c4;",
"  font-size: 0.95rem;",
"  cursor: pointer;",
"  transition: all 0.15s;",
"}",
".tab.active {",
"  background: #5b8cff;",
"  border-color: #5b8cff;",
"  color: #fff;",
"  box-shadow: 0 8px 18px rgba(91, 140, 255, 0.25);",
"}",
"main { max-width: 760px; margin: 0 auto; padding: 16px 20px 40px; }",
".card {",
"  background: #24262a;",
"  border: 1px solid #3e4147;",
"  border-radius: 16px;",
"  padding: 18px 20px;",
"  margin-bottom: 14px;",
"}",
".card h2 {",
"  margin: 0 0 12px;",
"  font-size: 1rem;",
"  letter-spacing: 0.3px;",
"  display: flex;",
"  align-items: center;",
"  gap: 8px;",
"}",
".card h2::before {",
"  content: '';",
"  width: 4px;",
"  height: 16px;",
"  border-radius: 2px;",
"  background: #5b8cff;",
"}",
".row {",
"  display: flex;",
"  justify-content: space-between;",
"  gap: 14px;",
"  padding: 8px 0;",
"  border-bottom: 1px dashed #3a3d42;",
"  font-size: 0.9rem;",
"}",
".row:last-child { border-bottom: none; padding-bottom: 0; }",
".row .k { color: #9aa4b2; flex: none; }",
".row .v { text-align: right; word-break: break-all; }",
".field { display: flex; flex-direction: column; gap: 6px; margin-bottom: 14px; }",
".field label { font-size: 0.82rem; color: #9aa4b2; letter-spacing: 0.4px; }",
".field input, .field select {",
"  width: 100%;",
"  padding: 11px 13px;",
"  background: #1d1f22;",
"  border: 1px solid #3e4147;",
"  border-radius: 11px;",
"  color: #e6e6e6;",
"  font-size: 0.95rem;",
"  outline: none;",
"}",
".field input:focus, .field select:focus { border-color: #5b8cff; }",
".time-range { display: flex; align-items: center; gap: 10px; }",
".time-range span { color: #9aa4b2; font-size: 0.85rem; }",
".hint { color: #8a93a0; font-size: 0.78rem; margin: 0; }",
".actions { display: flex; gap: 10px; justify-content: flex-end; margin-top: 6px; }",
".btn {",
"  padding: 11px 22px;",
"  border: none;",
"  border-radius: 30px;",
"  font-size: 0.92rem;",
"  font-weight: 600;",
"  cursor: pointer;",
"  transition: background 0.15s, transform 0.1s;",
"}",
".btn:active { transform: scale(0.97); }",
".btn-primary { background: #5b8cff; color: #fff; }",
".btn-primary:hover { background: #6f9cff; }",
".btn-ghost { background: #34373c; color: #c9d1dc; }",
".btn-ghost:hover { background: #3f4349; }",
"#toast {",
"  position: fixed;",
"  left: 50%;",
"  bottom: 28px;",
"  transform: translateX(-50%) translateY(20px);",
"  padding: 11px 22px;",
"  border-radius: 40px;",
"  background: #2b2d31;",
"  border: 1px solid #4a4e54;",
"  color: #e6e6e6;",
"  font-size: 0.88rem;",
"  opacity: 0;",
"  pointer-events: none;",
"  transition: all 0.2s;",
"  max-width: 90vw;",
"  text-align: center;",
"}",
"#toast.show { opacity: 1; transform: translateX(-50%) translateY(0); }",
"#toast.ok { border-color: #3f9c5c; }",
"#toast.err { border-color: #d05a5a; }",
"@media (max-width: 480px) {",
"  .actions { flex-direction: column-reverse; }",
"  .btn { width: 100%; }",
"}",
"</style>",
"</head>",
"<body>",
"<header>",
"  <h1>墨水屏课表 · 设备管理</h1>",
"  <p id='hostLine'>正在读取设备信息…</p>",
"</header>",
"<nav>",
"  <button class='tab active' id='tabAbout' type='button'>关于设备</button>",
"  <button class='tab' id='tabCourse' type='button'>设置课程表</button>",
"</nav>",
"<main>",
"  <section id='pageAbout'>",
"    <div class='card'>",
"      <h2>设备信息</h2>",
"      <div id='deviceList'></div>",
"    </div>",
"    <div class='card'>",
"      <h2>运行状态</h2>",
"      <div id='runtimeList'></div>",
"    </div>",
"    <div class='card'>",
"      <h2>当前课表</h2>",
"      <div id='courseSummary'></div>",
"    </div>",
"  </section>",
"  <section id='pageCourse' hidden>",
"    <div class='card'>",
"      <h2>课程设置</h2>",
"      <div class='field'>",
"        <label for='status'>本节状态</label>",
"        <select id='status'>",
"          <option value='0'>今日无课</option>",
"          <option value='1'>准备上课</option>",
"          <option value='2'>正在上课</option>",
"        </select>",
"      </div>",
"      <div class='field'>",
"        <label for='type'>课程类型</label>",
"        <select id='type'>",
"          <option value='0'>理论课</option>",
"          <option value='1'>实践课</option>",
"        </select>",
"      </div>",
"      <div class='field'>",
"        <label for='postion'>上课地点</label>",
"        <select id='postion'>",
"          <option value='0'>教学楼</option>",
"          <option value='1'>科技楼</option>",
"        </select>",
"      </div>",
"      <div class='field'>",
"        <label for='room'>教室编号</label>",
"        <input id='room' type='text' maxlength='4' placeholder='如 A102' autocomplete='off'>",
"        <p class='hint'>4 位字符：第 1 位为区域，后 3 位为房间号。</p>",
"      </div>",
"      <div class='field'>",
"        <label>上课时间</label>",
"        <div class='time-range'>",
"          <input id='startTime' type='time' value='08:00'>",
"          <span>至</span>",
"          <input id='endTime' type='time' value='09:40'>",
"        </div>",
"        <p class='hint'>结束时间必须晚于开始时间。</p>",
"      </div>",
"      <div class='field'>",
"        <label for='week'>学期周次</label>",
"        <input id='week' type='number' min='1' max='30' value='12'>",
"      </div>",
"      <div class='actions'>",
"        <button class='btn btn-ghost' id='reloadBtn' type='button'>读取当前值</button>",
"        <button class='btn btn-primary' id='saveBtn' type='button'>保存并刷新屏幕</button>",
"      </div>",
"    </div>",
"  </section>",
"</main>",
"<div id='toast'></div>",
"<script>",
"const STATUS_TEXT = ['今日无课', '准备上课', '正在上课'];",
"const TYPE_TEXT = ['理论课', '实践课'];",
"const POSITION_TEXT = ['教学楼', '科技楼'];",
"function el(id) { return document.getElementById(id); }",
"function esc(value) {",
"  return String(value === null || value === undefined ? '' : value)",
"    .replace(/[&<>]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]; });",
"}",
"function rows(target, pairs) {",
"  el(target).innerHTML = pairs.map(function (p) {",
"    return `<div class='row'><span class='k'>${esc(p[0])}</span><span class='v'>${esc(p[1])}</span></div>`;",
"  }).join('');",
"}",
"let toastTimer = null;",
"function toast(message, ok) {",
"  const box = el('toast');",
"  box.textContent = message;",
"  box.className = 'show ' + (ok ? 'ok' : 'err');",
"  clearTimeout(toastTimer);",
"  toastTimer = setTimeout(function () { box.className = ''; }, 3200);",
"}",
"function showTab(name) {",
"  const about = name === 'about';",
"  el('tabAbout').classList.toggle('active', about);",
"  el('tabCourse').classList.toggle('active', !about);",
"  el('pageAbout').hidden = !about;",
"  el('pageCourse').hidden = about;",
"  if (about) { loadAbout(); } else { loadCourse(); }",
"}",
"async function loadAbout() {",
"  try {",
"    const data = await Promise.all([",
"      fetch('/api/about').then(function (r) { return r.json(); }),",
"      fetch('/api/course').then(function (r) { return r.json(); })",
"    ]);",
"    const about = data[0];",
"    const course = data[1];",
"    el('hostLine').textContent = about.ip + ' · ' + about.chip_model + ' · 固件 ' + about.firmware;",
"    rows('deviceList', [",
"      ['芯片型号', about.chip_model],",
"      ['核心数', about.chip_cores + ' 核'],",
"      ['芯片版本', 'v' + about.chip_revision],",
"      ['MAC 地址', about.mac],",
"      ['Flash 容量', about.flash_size],",
"      ['固件版本', about.firmware],",
"      ['IDF 版本', about.idf_version],",
"      ['编译时间', about.build_time]",
"    ]);",
"    rows('runtimeList', [",
"      ['运行时间', about.uptime],",
"      ['复位原因', about.reset_reason],",
"      ['空闲堆', about.heap_free],",
"      ['最小空闲堆', about.heap_min],",
"      ['Wi-Fi', about.ssid + '（' + about.rssi + ' dBm）'],",
"      ['IP 地址', about.ip],",
"      ['设备时间', about.time]",
"    ]);",
"    rows('courseSummary', [",
"      ['本节状态', STATUS_TEXT[course.Course_Lesson_Status] || '未知'],",
"      ['课程类型', TYPE_TEXT[course.Course_Lesson_Type] || '未知'],",
"      ['上课地点', (POSITION_TEXT[course.Course_Lesson_Postion] || '未知') + '-' + String(course.Course_Lesson_Postion_Str || '').slice(1)],",
"      ['上课时间', course.Course_Lesson_Time],",
"      ['学期周次', '第 ' + course.Semester_Week_Now + ' 周']",
"    ]);",
"  } catch (e) {",
"    toast('设备信息读取失败', false);",
"  }",
"}",
"function fillCourse(course) {",
"  el('status').value = String(course.Course_Lesson_Status);",
"  el('type').value = String(course.Course_Lesson_Type);",
"  el('postion').value = String(course.Course_Lesson_Postion);",
"  el('room').value = course.Course_Lesson_Postion_Str || '';",
"  const parts = String(course.Course_Lesson_Time || '').split('~');",
"  if (parts.length === 2) {",
"    el('startTime').value = parts[0];",
"    el('endTime').value = parts[1];",
"  }",
"  el('week').value = course.Semester_Week_Now;",
"}",
"async function loadCourse() {",
"  try {",
"    const course = await fetch('/api/course').then(function (r) { return r.json(); });",
"    fillCourse(course);",
"  } catch (e) {",
"    toast('课表读取失败', false);",
"  }",
"}",
"async function saveCourse() {",
"  const room = (el('room').value || '').trim().toUpperCase();",
"  if (room.length !== 4) {",
"    toast('教室编号必须是 4 位字符，如 A102', false);",
"    return;",
"  }",
"  const start = el('startTime').value;",
"  const end = el('endTime').value;",
"  if (start >= end) {",
"    toast('结束时间必须晚于开始时间', false);",
"    return;",
"  }",
"  const body = {",
"    Course_Lesson_Status: Number(el('status').value),",
"    Course_Lesson_Type: Number(el('type').value),",
"    Course_Lesson_Postion: Number(el('postion').value),",
"    Course_Lesson_Postion_Str: room,",
"    Course_Lesson_Time: start + '~' + end,",
"    Semester_Week_Now: Number(el('week').value)",
"  };",
"  el('saveBtn').disabled = true;",
"  try {",
"    const res = await fetch('/api/course', {",
"      method: 'POST',",
"      headers: { 'Content-Type': 'application/json' },",
"      body: JSON.stringify(body)",
"    });",
"    const data = await res.json();",
"    toast(data.message || (res.ok ? '保存成功' : '保存失败'), res.ok);",
"    if (res.ok) { await loadCourse(); }",
"  } catch (e) {",
"    toast('保存请求失败', false);",
"  } finally {",
"    el('saveBtn').disabled = false;",
"  }",
"}",
"el('tabAbout').addEventListener('click', function () { showTab('about'); });",
"el('tabCourse').addEventListener('click', function () { showTab('course'); });",
"el('reloadBtn').addEventListener('click', loadCourse);",
"el('saveBtn').addEventListener('click', saveCourse);",
"showTab('about');",
"</script>",
"</body>",
"</html>",
};

/* ========================= 通用响应 ========================= */

static esp_err_t web_send_json(httpd_req_t *req, cJSON *root)
{
    char *out;
    esp_err_t ret;

    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);

    return ret;
}

static esp_err_t web_send_result(httpd_req_t *req, bool ok, const char *message, const char *status)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "message", (message != NULL) ? message : "");
    httpd_resp_set_status(req, status);

    return web_send_json(req, root);
}

/* ========================= 格式化小工具 ========================= */

static void web_format_bytes(char *buf, size_t len, uint32_t bytes)
{
    if (bytes >= (1024U * 1024U)) {
        snprintf(buf, len, "%u.%u MB", (unsigned)(bytes / (1024U * 1024U)),
                 (unsigned)((bytes % (1024U * 1024U)) * 10U / (1024U * 1024U)));
    } else if (bytes >= 1024U) {
        snprintf(buf, len, "%u.%u KB", (unsigned)(bytes / 1024U),
                 (unsigned)((bytes % 1024U) * 10U / 1024U));
    } else {
        snprintf(buf, len, "%u B", (unsigned)bytes);
    }
}

static void web_format_uptime(char *buf, size_t len)
{
    uint64_t total = (uint64_t)(esp_timer_get_time() / 1000000LL);
    uint64_t days = total / 86400ULL;
    uint64_t hour = (total % 86400ULL) / 3600ULL;
    uint64_t min = (total % 3600ULL) / 60ULL;
    uint64_t sec = total % 60ULL;

    snprintf(buf, len, "%llu 天 %02llu:%02llu:%02llu",
             (unsigned long long)days, (unsigned long long)hour,
             (unsigned long long)min, (unsigned long long)sec);
}

static const char *web_reset_reason_text(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "上电复位";
    case ESP_RST_EXT:       return "外部复位";
    case ESP_RST_SW:        return "软件复位";
    case ESP_RST_PANIC:     return "异常复位";
    case ESP_RST_INT_WDT:   return "中断看门狗";
    case ESP_RST_TASK_WDT:  return "任务看门狗";
    case ESP_RST_WDT:       return "看门狗";
    case ESP_RST_DEEPSLEEP: return "深睡眠唤醒";
    case ESP_RST_BROWNOUT:  return "电压跌落";
    case ESP_RST_SDIO:      return "SDIO 复位";
    default:                return "未知";
    }
}

/* ========================= 保存后重绘墨水屏 ========================= */

static volatile bool s_gui_refresh_busy = false;

static void web_gui_refresh_task(void *arg)
{
    (void)arg;

    /* 先让 HTTP 响应发回浏览器，再刷屏（刷一帧比较慢） */
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "按网页设置重绘墨水屏");
    layout_no = 0;      /* 保存后切到课程表页，保证改动能立刻看到 */
    GuiUpdate();

    s_gui_refresh_busy = false;
    vTaskDelete(NULL);
}

static void web_request_gui_refresh(void)
{
    if (s_gui_refresh_busy) {
        return;
    }
    s_gui_refresh_busy = true;

    if (xTaskCreate(web_gui_refresh_task, "web_gui", WEB_GUI_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        s_gui_refresh_busy = false;
        ESP_LOGW(TAG, "创建刷屏任务失败");
    }
}

/* ========================= 关于设备 ========================= */

static esp_err_t web_about_get(httpd_req_t *req)
{
    cJSON *root;
    esp_chip_info_t chip;
    const esp_app_desc_t *app = esp_app_get_description();
    esp_netif_t *netif;
    esp_netif_ip_info_t ip_info;
    wifi_ap_record_t ap;
    struct tm tm_now;
    uint32_t flash_size = 0;
    uint8_t mac[6] = { 0 };
    char model[16] = CONFIG_IDF_TARGET;
    char mac_str[18] = "--";
    char flash_str[16] = "--";
    char ssid[33] = "--";
    char ip[16] = "--";
    char uptime[32] = "--";
    char heap_free[16] = "--";
    char heap_min[16] = "--";
    char time_str[32] = "--";
    char build_str[32] = "--";
    int rssi = 0;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    esp_chip_info(&chip);

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(mac_str, sizeof(mac_str), MACSTR, MAC2STR(mac));
    }

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        web_format_bytes(flash_str, sizeof(flash_str), flash_size);
    }

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(ssid, ap.ssid, sizeof(ap.ssid));
        ssid[sizeof(ssid) - 1] = '\0';
        rssi = ap.rssi;
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
    }

    web_format_uptime(uptime, sizeof(uptime));
    web_format_bytes(heap_free, sizeof(heap_free), (uint32_t)esp_get_free_heap_size());
    web_format_bytes(heap_min, sizeof(heap_min), (uint32_t)esp_get_minimum_free_heap_size());

    if (app != NULL) {
        snprintf(build_str, sizeof(build_str), "%s %s", app->date, app->time);
    }

    if (rtc_get_time(&tm_now) == ESP_OK) {
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_now);
    }

    cJSON_AddStringToObject(root, "chip_model", model);
    cJSON_AddNumberToObject(root, "chip_cores", chip.cores);
    cJSON_AddNumberToObject(root, "chip_revision", chip.revision);
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "flash_size", flash_str);
    cJSON_AddStringToObject(root, "firmware", (app != NULL) ? app->version : "--");
    cJSON_AddStringToObject(root, "project", (app != NULL) ? app->project_name : "--");
    cJSON_AddStringToObject(root, "idf_version", (app != NULL) ? app->idf_ver : "--");
    cJSON_AddStringToObject(root, "build_time", build_str);
    cJSON_AddStringToObject(root, "uptime", uptime);
    cJSON_AddStringToObject(root, "reset_reason", web_reset_reason_text(esp_reset_reason()));
    cJSON_AddStringToObject(root, "heap_free", heap_free);
    cJSON_AddStringToObject(root, "heap_min", heap_min);
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddNumberToObject(root, "rssi", rssi);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "time", time_str);

    return web_send_json(req, root);
}

/* ========================= 设置课程表 ========================= */

static esp_err_t web_course_get(httpd_req_t *req)
{
    char *json = Course_ToJson();
    esp_err_t ret;

    if (json == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);

    return ret;
}

static esp_err_t web_course_post(httpd_req_t *req)
{
    char *body;
    char err_msg[96] = { 0 };
    size_t received = 0;
    int retry = 0;
    esp_err_t ret;

    if (req->content_len == 0 || req->content_len >= WEB_COURSE_BODY_MAX) {
        return web_send_result(req, false, "请求内容长度非法", "400 Bad Request");
    }

    body = malloc(req->content_len + 1);
    if (body == NULL) {
        return httpd_resp_send_500(req);
    }

    while (received < req->content_len) {
        int got = httpd_req_recv(req, body + received, req->content_len - received);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++retry > 3) {
                free(body);
                return web_send_result(req, false, "读取请求内容超时", "408 Request Timeout");
            }
            continue;
        }
        if (got <= 0) {
            free(body);
            return web_send_result(req, false, "读取请求内容失败", "400 Bad Request");
        }
        received += (size_t)got;
        retry = 0;
    }
    body[received] = '\0';

    ret = Course_ApplyJson(body, err_msg, sizeof(err_msg));
    free(body);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "更新课表失败：%s（%s）", err_msg, esp_err_to_name(ret));
        return web_send_result(req, false, (err_msg[0] != '\0') ? err_msg : "保存失败",
                               "400 Bad Request");
    }

    ESP_LOGI(TAG, "网页已更新课表：%s", err_msg);
    web_request_gui_refresh();

    return web_send_result(req, true, "课表已保存，墨水屏正在刷新", "200 OK");
}

/* ========================= 页面与 favicon ========================= */

static esp_err_t web_index_get(httpd_req_t *req)
{
    const size_t count = sizeof(INDEX_HTML) / sizeof(INDEX_HTML[0]);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    for (size_t i = 0; i < count; i++) {
        esp_err_t ret = httpd_resp_sendstr_chunk(req, INDEX_HTML[i]);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = httpd_resp_sendstr_chunk(req, "\n");
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t web_favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ========================= 路由注册 ========================= */

esp_err_t http_web_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = web_index_get,   .user_ctx = NULL },
        { .uri = "/api/about",   .method = HTTP_GET,  .handler = web_about_get,   .user_ctx = NULL },
        { .uri = "/api/course",  .method = HTTP_GET,  .handler = web_course_get,  .user_ctx = NULL },
        { .uri = "/api/course",  .method = HTTP_POST, .handler = web_course_post, .user_ctx = NULL },
        { .uri = "/favicon.ico", .method = HTTP_GET,  .handler = web_favicon_get, .user_ctx = NULL },
    };
    esp_err_t ret;

    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ret = httpd_register_uri_handler(server, &routes[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败：%s", routes[i].uri, esp_err_to_name(ret));
            return ret;
        }
    }

    ESP_LOGI(TAG, "网页路由注册完成：关于设备 / 设置课程表");
    return ESP_OK;
}
