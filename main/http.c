/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-21 00:03:47
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-26 22:30:00
 * @FilePath: \epd\main\http.c
 * @Description: 网页管理模块（关于设备 / 课表学期设置 / 清单 / 完整课程表）。
 *
 * 页面使用内嵌 HTML（不依赖文件系统），接口：
 *   GET  /api/about      设备硬件、固件、网络、运行时间等信息
 *   GET  /api/course     当前课表学期设置（学年/学期/当前周次）
 *   POST /api/course     更新学期设置 -> 写入 NVS -> 重绘墨水屏
 *   GET  /api/checklist  当前清单（待办事项）
 *   POST /api/checklist  更新清单 -> 写入 NVS -> 切到清单页重绘
 *   GET  /api/timetable  完整课程表（与导入格式一致）
 *   POST /api/timetable  导入/覆盖完整课程表 -> 校验 -> 写入 NVS
 *   DELETE /api/timetable 清空完整课程表 -> 写入 NVS
 */
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "Checklist.h"
#include "DataStruct.h"
#include "Timetable.h"
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
#include "nvs.h"

static const char *TAG = "HTTP_WEB";

/* POST 请求体上限（课表 JSON 很短） */
#define WEB_COURSE_BODY_MAX         768
/* 清单 POST 请求体上限：8 项 x (40 字节文字 + JSON 包装) 留足余量 */
#define WEB_CHECKLIST_BODY_MAX      1536
/* 课程表 POST 请求体上限：16 门课 x 64 个时段（紧凑 JSON 约 34 KB），留足余量 */
#define WEB_TIMETABLE_BODY_MAX      49152
/* 保存后重绘墨水屏的任务栈（与按键任务一致） */
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
"  flex-wrap: wrap;",
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
".field textarea {",
"  width: 100%;",
"  min-height: 240px;",
"  padding: 11px 13px;",
"  background: #1d1f22;",
"  border: 1px solid #3e4147;",
"  border-radius: 11px;",
"  color: #e6e6e6;",
"  font-family: Consolas, 'Courier New', monospace;",
"  font-size: 0.82rem;",
"  line-height: 1.5;",
"  resize: vertical;",
"  outline: none;",
"}",
".field textarea:focus { border-color: #5b8cff; }",
".field input[type=file] { padding: 9px 12px; font-size: 0.82rem; }",
".time-range { display: flex; align-items: center; gap: 10px; }",
".time-range span { color: #9aa4b2; font-size: 0.85rem; }",
".hint { color: #8a93a0; font-size: 0.78rem; margin: 0; }",
".list-row { display: flex; align-items: center; gap: 10px; margin-bottom: 8px; }",
".list-row .no { width: 20px; flex: none; text-align: right; color: #8a93a0; font-size: 0.82rem; }",
".list-row input[type=checkbox] {",
"  width: 20px;",
"  height: 20px;",
"  flex: none;",
"  accent-color: #5b8cff;",
"  cursor: pointer;",
"}",
".list-row input[type=text] {",
"  flex: 1;",
"  min-width: 0;",
"  padding: 10px 12px;",
"  background: #1d1f22;",
"  border: 1px solid #3e4147;",
"  border-radius: 10px;",
"  color: #e6e6e6;",
"  font-size: 0.92rem;",
"  outline: none;",
"}",
".list-row input[type=text]:focus { border-color: #5b8cff; }",
".list-row input.done { color: #7d8590; text-decoration: line-through; }",
"#listRows { margin: 10px 0; }",
".actions { display: flex; gap: 10px; justify-content: flex-end; margin-top: 6px; }",
".grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }",
".course-list { display: flex; flex-direction: column; gap: 8px; margin: 10px 0; }",
".course-item {",
"  display: flex;",
"  align-items: flex-start;",
"  gap: 10px;",
"  padding: 10px 12px;",
"  border-radius: 11px;",
"  background: #1d1f22;",
"  border: 1px solid #3e4147;",
"}",
".course-item .info { flex: 1; min-width: 0; }",
".course-item .name { font-size: 0.94rem; font-weight: 600; color: #e6e6e6; }",
".course-item .meta { margin-top: 4px; font-size: 0.78rem; color: #8a93a0; line-height: 1.45; }",
".course-item .code {",
"  flex: none;",
"  padding: 3px 8px;",
"  border-radius: 20px;",
"  background: #2d3748;",
"  color: #9db7ff;",
"  font-size: 0.72rem;",
"  font-weight: 600;",
"}",
".btn-small { padding: 7px 14px; font-size: 0.8rem; }",
".btn-danger { background: #a94b4b; color: #fff; }",
".btn-danger:hover { background: #bf5a5a; }",
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
"  .grid2 { grid-template-columns: 1fr; }",
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
"  <button class='tab' id='tabTimetable' type='button'>课程表</button>",
"  <button class='tab' id='tabChecklist' type='button'>清单</button>",
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
"  <section id='pageChecklist' hidden>",
"    <div class='card'>",
"      <h2>今日清单</h2>",
"      <p class='hint' id='listStat'>正在读取…</p>",
"      <div id='listRows'></div>",
"      <p class='hint'>最多 8 项；文字留空保存后即删除该项；每项最多 40 字节（约 13 个汉字），只支持 GB2312 汉字、英文和数字。</p>",
"      <div class='actions'>",
"        <button class='btn btn-ghost' id='listReloadBtn' type='button'>读取当前值</button>",
"        <button class='btn btn-ghost' id='listClearBtn' type='button'>清空填写</button>",
"        <button class='btn btn-primary' id='listSaveBtn' type='button'>保存并刷新屏幕</button>",
"      </div>",
"    </div>",
"  </section>",
"  <section id='pageTimetable' hidden>",
"    <div class='card'>",
"      <h2>学期设置</h2>",
"      <div class='grid2'>",
"        <div class='field'>",
"          <label for='academicYear'>学年</label>",
"          <input id='academicYear' type='text' maxlength='9' placeholder='2025-2026' autocomplete='off'>",
"        </div>",
"        <div class='field'>",
"          <label for='semester'>学期</label>",
"          <select id='semester'>",
"            <option value='0'>未设置</option>",
"            <option value='1'>第一学期</option>",
"            <option value='2'>第二学期</option>",
"          </select>",
"        </div>",
"      </div>",
"      <div class='field'>",
"        <label for='displayWeek'>学期周次（墨水屏当前显示）</label>",
"        <input id='displayWeek' type='number' min='1' max='53' value='12'>",
"      </div>",
"      <p class='hint'>完整课表页按这里的周次筛选课程；学年 / 学期也会一并保存到 NVS。</p>",
"      <div class='actions'>",
"        <button class='btn btn-ghost' id='settingsReloadBtn' type='button'>读取当前值</button>",
"        <button class='btn btn-primary' id='settingsSaveBtn' type='button'>保存学期设置</button>",
"      </div>",
"    </div>",
"    <div class='card'>",
"      <h2>手动添加课程</h2>",
"      <div class='field'>",
"        <label for='courseName'>课程名称</label>",
"        <input id='courseName' type='text' maxlength='40' placeholder='如 高等数学' autocomplete='off'>",
"      </div>",
"      <div class='field'>",
"        <label for='courseClassroom'>上课地点</label>",
"        <input id='courseClassroom' type='text' maxlength='24' placeholder='如 A102' autocomplete='off'>",
"      </div>",
"      <div class='field'>",
"        <label for='courseTeacher'>上课老师</label>",
"        <input id='courseTeacher' type='text' maxlength='24' placeholder='如 张老师' autocomplete='off'>",
"      </div>",
"      <div class='field'>",
"        <label>第 x-y 节</label>",
"        <div class='time-range'>",
"          <input id='periodStart' type='number' min='1' max='99' value='1'>",
"          <span>至</span>",
"          <input id='periodEnd' type='number' min='1' max='99' value='2'>",
"        </div>",
"      </div>",
"      <div class='field'>",
"        <label>上下课时间</label>",
"        <div class='time-range'>",
"          <input id='courseStartTime' type='time' value='08:00'>",
"          <span>至</span>",
"          <input id='courseEndTime' type='time' value='09:40'>",
"        </div>",
"      </div>",
"      <div class='field'>",
"        <label for='courseWeeks'>学期周次（如 1-16 或 1,3,5-8）</label>",
"        <input id='courseWeeks' type='text' maxlength='120' value='1-16' autocomplete='off'>",
"      </div>",
"      <p class='hint'>学年 / 学期请先在上方“学期设置”中保存；添加课程后立即写入 NVS 并刷新墨水屏。课程名 ≤40 字节，教室 / 老师 ≤24 字节，且只支持 GB2312 字符。</p>",
"      <div class='actions'>",
"        <button class='btn btn-primary' id='manualAddBtn' type='button'>添加到课程表</button>",
"      </div>",
"    </div>",
"    <div class='card'>",
"      <h2>当前课程</h2>",
"      <p class='hint' id='timetableStat'>正在读取…</p>",
"      <div id='courseList' class='course-list'></div>",
"      <div class='actions'>",
"        <button class='btn btn-danger' id='clearTimetableBtn' type='button'>清空课程表</button>",
"      </div>",
"    </div>",
"    <div class='card'>",
"      <h2>JSON 导入 / 导出</h2>",
"      <p class='hint'>导入会覆盖当前全部课程。根对象键为 4 位数字课程编号；每门课必须包含 CourseName、CourseDuration、CourseSessions、Classroom、Teacher，可选 StartTime / EndTime（HH:MM）。</p>",
"      <div class='field'>",
"        <label for='timetableFile'>选择 JSON 文件</label>",
"        <input id='timetableFile' type='file' accept='.json,application/json'>",
"      </div>",
"      <div class='field'>",
"        <label for='timetableJson'>课程表 JSON</label>",
"        <textarea id='timetableJson' spellcheck='false' placeholder='在此粘贴课程表 JSON'></textarea>",
"      </div>",
"      <p class='hint'>至少 1 门、最多 16 门课程；每门最多 8 个 CourseDuration、64 个 CourseSessions；节次 1~99，周次 1~53。</p>",
"      <div class='actions'>",
"        <button class='btn btn-ghost' id='timetableTemplateBtn' type='button'>填入模板</button>",
"        <button class='btn btn-ghost' id='timetableReloadBtn' type='button'>读取当前</button>",
"        <button class='btn btn-ghost' id='timetableDownloadBtn' type='button'>导出 JSON</button>",
"        <button class='btn btn-primary' id='timetableSaveBtn' type='button'>导入并保存</button>",
"      </div>",
"    </div>",
"  </section>",
"</main>",
"<div id='toast'></div>",
"<script>",
"const SEMESTER_TEXT = ['未设置', '第一学期', '第二学期'];",
"const TIMETABLE_TEMPLATE = {",
"  '0001': {",
"    CourseName: '高等数学',",
"    CourseDuration: [{ Start: 1, End: 2 }],",
"    CourseSessions: [",
"      { Week: 1, Start: 1, End: 2 },",
"      { Week: 2, Start: 1, End: 2 }",
"    ],",
"    Classroom: 'A102',",
"    Teacher: '张老师',",
"    StartTime: '08:00',",
"    EndTime: '09:40'",
"  }",
"};",
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
"  const names = ['about', 'timetable', 'checklist'];",
"  names.forEach(function (n) {",
"    const key = n.charAt(0).toUpperCase() + n.slice(1);",
"    el('tab' + key).classList.toggle('active', n === name);",
"    el('page' + key).hidden = (n !== name);",
"  });",
"  if (name === 'about') { loadAbout(); }",
"  else if (name === 'timetable') { loadSettings(); loadTimetable(); }",
"  else { loadChecklist(); }",
"}",
"async function loadAbout() {",
"  try {",
"    const data = await Promise.all([",
"      fetch('/api/about').then(function (r) { return r.json(); }),",
"      fetch('/api/course').then(function (r) { return r.json(); }),",
"      fetch('/api/timetable').then(function (r) { return r.json(); })",
"    ]);",
"    const about = data[0];",
"    const course = data[1];",
"    const timetable = data[2] || {};",
"    const keys = Object.keys(timetable);",
"    let sessions = 0;",
"    keys.forEach(function (key) {",
"      sessions += ((timetable[key] || {}).CourseSessions || []).length;",
"    });",
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
"      ['学年', course.AcademicYear || '未设置'],",
"      ['学期', SEMESTER_TEXT[course.Semester] || '未设置'],",
"      ['当前显示周次', '第 ' + course.Semester_Week_Now + ' 周'],",
"      ['课程门数', keys.length + ' 门'],",
"      ['上课时段', sessions + ' 个']",
"    ]);",
"  } catch (e) {",
"    toast('设备信息读取失败', false);",
"  }",
"}",
"function fillSettings(course) {",
"  el('academicYear').value = course.AcademicYear || '';",
"  el('semester').value = String(course.Semester || 0);",
"  el('displayWeek').value = course.Semester_Week_Now || 1;",
"}",
"async function loadSettings() {",
"  try {",
"    const course = await fetch('/api/course').then(function (r) { return r.json(); });",
"    fillSettings(course);",
"  } catch (e) {",
"    toast('学期设置读取失败', false);",
"  }",
"}",
"async function saveSettings() {",
"  const year = (el('academicYear').value || '').trim();",
"  if (year && !/^\\d{4}-\\d{4}$/.test(year)) {",
"    toast('学年格式应为 YYYY-YYYY，如 2025-2026', false);",
"    return;",
"  }",
"  const week = Number(el('displayWeek').value);",
"  if (!Number.isInteger(week) || week < 1 || week > 53) {",
"    toast('学期周次应为 1~53 的整数', false);",
"    return;",
"  }",
"  const body = {",
"    AcademicYear: year,",
"    Semester: Number(el('semester').value),",
"    Semester_Week_Now: week",
"  };",
"  el('settingsSaveBtn').disabled = true;",
"  try {",
"    const res = await fetch('/api/course', {",
"      method: 'POST',",
"      headers: { 'Content-Type': 'application/json' },",
"      body: JSON.stringify(body)",
"    });",
"    const data = await res.json();",
"    toast(data.message || (res.ok ? '学期设置已保存' : '保存失败'), res.ok);",
"    if (res.ok) { await loadSettings(); }",
"  } catch (e) {",
"    toast('保存请求失败', false);",
"  } finally {",
"    el('settingsSaveBtn').disabled = false;",
"  }",
"}",
"const LIST_MAX = 8;",
"const LIST_TEXT_MAX = 40;",
"function byteLength(text) {",
"  return new TextEncoder().encode(text).length;",
"}",
"function buildListRows() {",
"  let html = '';",
"  for (let i = 0; i < LIST_MAX; i++) {",
"    html += `<div class='list-row'>` +",
"      `<span class='no'>${i + 1}</span>` +",
"      `<input id='listDone${i}' type='checkbox' title='标记完成'>` +",
"      `<input id='listText${i}' type='text' maxlength='40' autocomplete='off' placeholder='第 ${i + 1} 项'>` +",
"      `</div>`;",
"  }",
"  el('listRows').innerHTML = html;",
"  for (let i = 0; i < LIST_MAX; i++) {",
"    el('listDone' + i).addEventListener('change', function () { syncDoneStyle(i); });",
"  }",
"}",
"function syncDoneStyle(i) {",
"  el('listText' + i).classList.toggle('done', el('listDone' + i).checked);",
"}",
"function fillList(data) {",
"  const items = data.items || [];",
"  for (let i = 0; i < LIST_MAX; i++) {",
"    const item = items[i] || { text: '', done: false };",
"    el('listText' + i).value = item.text || '';",
"    el('listDone' + i).checked = !!item.done;",
"    syncDoneStyle(i);",
"  }",
"  el('listStat').textContent = '已完成 ' + (data.done || 0) + ' / ' + (data.total || 0) + ' 项';",
"}",
"async function loadChecklist() {",
"  try {",
"    const data = await fetch('/api/checklist').then(function (r) { return r.json(); });",
"    fillList(data);",
"  } catch (e) {",
"    toast('清单读取失败', false);",
"  }",
"}",
"function clearList() {",
"  for (let i = 0; i < LIST_MAX; i++) {",
"    el('listText' + i).value = '';",
"    el('listDone' + i).checked = false;",
"    syncDoneStyle(i);",
"  }",
"  el('listStat').textContent = '填写已清空，保存后生效';",
"}",
"async function saveChecklist() {",
"  const items = [];",
"  for (let i = 0; i < LIST_MAX; i++) {",
"    const text = (el('listText' + i).value || '').trim();",
"    if (byteLength(text) > LIST_TEXT_MAX) {",
"      toast('第 ' + (i + 1) + ' 项太长：最多 ' + LIST_TEXT_MAX + ' 字节（约 13 个汉字）', false);",
"      return;",
"    }",
"    items.push({ text: text, done: el('listDone' + i).checked });",
"  }",
"  el('listSaveBtn').disabled = true;",
"  try {",
"    const res = await fetch('/api/checklist', {",
"      method: 'POST',",
"      headers: { 'Content-Type': 'application/json' },",
"      body: JSON.stringify({ items: items })",
"    });",
"    const data = await res.json();",
"    toast(data.message || (res.ok ? '保存成功' : '保存失败'), res.ok);",
"    if (res.ok) { await loadChecklist(); }",
"  } catch (e) {",
"    toast('保存请求失败', false);",
"  } finally {",
"    el('listSaveBtn').disabled = false;",
"  }",
"}",
"function formatWeeks(weeks) {",
"  const sorted = Array.from(new Set(weeks)).filter(function (w) {",
"    return Number.isInteger(w) && w >= 1 && w <= 53;",
"  }).sort(function (a, b) { return a - b; });",
"  if (!sorted.length) { return '未设置'; }",
"  const parts = [];",
"  let start = sorted[0];",
"  let prev = sorted[0];",
"  for (let i = 1; i < sorted.length; i++) {",
"    if (sorted[i] === prev + 1) { prev = sorted[i]; continue; }",
"    parts.push(start === prev ? String(start) : start + '-' + prev);",
"    start = prev = sorted[i];",
"  }",
"  parts.push(start === prev ? String(start) : start + '-' + prev);",
"  return parts.join(',');",
"}",
"function parseWeeks(text) {",
"  const raw = String(text || '').trim();",
"  if (!raw) { return null; }",
"  const result = [];",
"  let invalid = false;",
"  raw.split(/[,，\\s]+/).forEach(function (part) {",
"    if (!part) { return; }",
"    const match = part.match(/^(\\d{1,2})(?:-(\\d{1,2}))?$/);",
"    if (!match) { invalid = true; return; }",
"    const first = Number(match[1]);",
"    const last = match[2] ? Number(match[2]) : first;",
"    if (first < 1 || last > 53 || first > last) { invalid = true; return; }",
"    for (let w = first; w <= last; w++) {",
"      if (result.indexOf(w) < 0) { result.push(w); }",
"    }",
"  });",
"  if (invalid || !result.length) { return null; }",
"  return result.sort(function (a, b) { return a - b; });",
"}",
"function nextCourseCode(data) {",
"  for (let i = 1; i <= 9999; i++) {",
"    const code = String(i).padStart(4, '0');",
"    if (!data[code]) { return code; }",
"  }",
"  return null;",
"}",
"function renderCourseList(data) {",
"  const keys = Object.keys(data || {});",
"  if (!keys.length) {",
"    el('courseList').innerHTML = `<p class='hint'>当前没有课程，请手动添加或导入 JSON。</p>`;",
"    return;",
"  }",
"  el('courseList').innerHTML = keys.map(function (key) {",
"    const course = data[key] || {};",
"    const duration = (course.CourseDuration || []).map(function (item) {",
"      return item && item.Start ? ('第 ' + item.Start + '-' + item.End + ' 节') : '';",
"    }).filter(Boolean).join(' / ');",
"    const periodText = duration || '节次未设置';",
"    const timeText = (course.StartTime && course.EndTime)",
"      ? (course.StartTime + '-' + course.EndTime) : '时间未设置';",
"    const weeks = (course.CourseSessions || []).map(function (s) { return Number(s.Week); });",
"    const weekText = formatWeeks(weeks);",
"    return `<div class='course-item'>` +",
"      `<span class='code'>${esc(key)}</span>` +",
"      `<div class='info'>` +",
"        `<div class='name'>${esc(course.CourseName || '未命名课程')}</div>` +",
"        `<div class='meta'>${esc(periodText)} · ${esc(timeText)} · ${esc(course.Classroom || '地点未设置')} · ${esc(course.Teacher || '老师未设置')}</div>` +",
"        `<div class='meta'>周次：${esc(weekText)} · 共 ${weeks.length} 个时段</div>` +",
"      `</div>` +",
"      `<button class='btn btn-ghost btn-small' data-code='${esc(key)}' type='button'>删除</button>` +",
"    `</div>`;",
"  }).join('');",
"  el('courseList').querySelectorAll('button[data-code]').forEach(function (button) {",
"    button.addEventListener('click', function () { deleteTimetableCourse(button.getAttribute('data-code')); });",
"  });",
"}",
"async function loadTimetable() {",
"  try {",
"    const data = await fetch('/api/timetable').then(function (r) { return r.json(); });",
"    const keys = Object.keys(data || {});",
"    let sessions = 0;",
"    keys.forEach(function (key) {",
"      sessions += (((data[key] || {}).CourseSessions) || []).length;",
"    });",
"    el('timetableJson').value = JSON.stringify(data, null, 2);",
"    renderCourseList(data || {});",
"    el('timetableStat').textContent = keys.length",
"      ? ('已保存 ' + keys.length + ' 门课程，共 ' + sessions + ' 个上课时段')",
"      : '当前没有保存课程表，可手动添加或导入 JSON';",
"  } catch (e) {",
"    toast('课程表读取失败', false);",
"  }",
"}",
"async function submitTimetable(data) {",
"  const keys = Object.keys(data || {});",
"  const options = keys.length",
"    ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(data) }",
"    : { method: 'DELETE' };",
"  try {",
"    const res = await fetch('/api/timetable', options);",
"    const result = await res.json();",
"    toast(result.message || (res.ok ? '保存成功' : '保存失败'), res.ok);",
"    if (res.ok) { await loadTimetable(); }",
"    return res.ok;",
"  } catch (e) {",
"    toast('保存请求失败', false);",
"    return false;",
"  }",
"}",
"async function deleteTimetableCourse(code) {",
"  if (!confirm('确定删除课程 ' + code + '？')) { return; }",
"  try {",
"    const data = await fetch('/api/timetable').then(function (r) { return r.json(); });",
"    delete data[code];",
"    await submitTimetable(data);",
"  } catch (e) {",
"    toast('删除失败', false);",
"  }",
"}",
"async function clearTimetable() {",
"  if (!confirm('确定清空全部课程？此操作会写入 NVS。')) { return; }",
"  el('clearTimetableBtn').disabled = true;",
"  try {",
"    await submitTimetable({});",
"  } finally {",
"    el('clearTimetableBtn').disabled = false;",
"  }",
"}",
"async function addManualCourse() {",
"  const name = (el('courseName').value || '').trim();",
"  const classroom = (el('courseClassroom').value || '').trim();",
"  const teacher = (el('courseTeacher').value || '').trim();",
"  if (!name) { toast('请填写课程名称', false); return; }",
"  if (byteLength(name) > 40) { toast('课程名称最多 40 字节（约 13 个汉字）', false); return; }",
"  if (byteLength(classroom) > 24) { toast('上课地点最多 24 字节（约 8 个汉字）', false); return; }",
"  if (byteLength(teacher) > 24) { toast('上课老师最多 24 字节（约 8 个汉字）', false); return; }",
"  const start = Number(el('periodStart').value);",
"  const end = Number(el('periodEnd').value);",
"  if (!Number.isInteger(start) || !Number.isInteger(end) ||",
"      start < 1 || start > 99 || end < 1 || end > 99 || start > end) {",
"    toast('节次应为 1~99 的整数，且开始节次不能大于结束节次', false);",
"    return;",
"  }",
"  const startTime = el('courseStartTime').value;",
"  const endTime = el('courseEndTime').value;",
"  if (!startTime || !endTime || startTime >= endTime) {",
"    toast('结束时间必须晚于开始时间', false);",
"    return;",
"  }",
"  const weeks = parseWeeks(el('courseWeeks').value);",
"  if (!weeks) { toast('周次格式错误，请填写如 1-16 或 1,3,5-8（1~53）', false); return; }",
"  if (weeks.length > 64) { toast('单门课程最多 64 个上课时段', false); return; }",
"  el('manualAddBtn').disabled = true;",
"  try {",
"    const data = await fetch('/api/timetable').then(function (r) { return r.json(); });",
"    if (Object.keys(data || {}).length >= 16) {",
"      toast('最多保存 16 门课程，请先删除部分课程', false);",
"      return;",
"    }",
"    const code = nextCourseCode(data || {});",
"    if (!code) { toast('没有可用的 4 位课程编号', false); return; }",
"    data[code] = {",
"      CourseName: name,",
"      CourseDuration: [{ Start: start, End: end }],",
"      CourseSessions: weeks.map(function (week) { return { Week: week, Start: start, End: end }; }),",
"      Classroom: classroom,",
"      Teacher: teacher,",
"      StartTime: startTime,",
"      EndTime: endTime",
"    };",
"    const ok = await submitTimetable(data);",
"    if (ok) {",
"      el('courseName').value = '';",
"      el('courseClassroom').value = '';",
"      el('courseTeacher').value = '';",
"    }",
"  } catch (e) {",
"    toast('添加课程失败', false);",
"  } finally {",
"    el('manualAddBtn').disabled = false;",
"  }",
"}",
"function fillTemplate() {",
"  el('timetableJson').value = JSON.stringify(TIMETABLE_TEMPLATE, null, 2);",
"  el('timetableStat').textContent = '已填入模板，点击“导入并保存”后覆盖当前课程表';",
"}",
"function readTimetableFile() {",
"  const input = el('timetableFile');",
"  const file = input.files && input.files[0];",
"  if (!file) { return; }",
"  const reader = new FileReader();",
"  reader.onload = function () {",
"    el('timetableJson').value = String(reader.result || '');",
"    el('timetableStat').textContent = '已读取 ' + file.name + '，点击“导入并保存”生效';",
"  };",
"  reader.onerror = function () { toast('文件读取失败', false); };",
"  reader.readAsText(file, 'utf-8');",
"}",
"function downloadTimetable() {",
"  const text = (el('timetableJson').value || '').trim();",
"  if (!text) { toast('没有可导出的课程表', false); return; }",
"  const blob = new Blob([text], { type: 'application/json;charset=utf-8' });",
"  const url = URL.createObjectURL(blob);",
"  const link = document.createElement('a');",
"  link.href = url;",
"  link.download = 'timetable.json';",
"  document.body.appendChild(link);",
"  link.click();",
"  document.body.removeChild(link);",
"  setTimeout(function () { URL.revokeObjectURL(url); }, 1000);",
"  toast('课程表 JSON 已导出', true);",
"}",
"async function saveTimetable() {",
"  const text = (el('timetableJson').value || '').trim();",
"  if (!text) { toast('请先粘贴或选择课程表 JSON', false); return; }",
"  let data;",
"  try { data = JSON.parse(text); } catch (e) { toast('JSON 格式错误：' + e.message, false); return; }",
"  if (data === null || typeof data !== 'object' || Array.isArray(data)) {",
"    toast('根节点必须是对象（4 位数字编号 -> 课程）', false);",
"    return;",
"  }",
"  const keys = Object.keys(data);",
"  if (!keys.length) { toast('课程表不能为空，至少要有 1 门课程', false); return; }",
"  if (keys.length > 16) { toast('最多保存 16 门课程', false); return; }",
"  for (let i = 0; i < keys.length; i++) {",
"    const course = data[keys[i]];",
"    if (!/^[0-9]{4}$/.test(keys[i])) { toast('课程编号 “' + keys[i] + '” 必须是 4 位数字', false); return; }",
"    if (course === null || typeof course !== 'object' || Array.isArray(course)) {",
"      toast('课程 ' + keys[i] + ' 的内容必须是对象', false);",
"      return;",
"    }",
"  }",
"  const body = JSON.stringify(data);",
"  if (byteLength(body) > 49152) { toast('课程表 JSON 太大（最多 48 KB）', false); return; }",
"  el('timetableSaveBtn').disabled = true;",
"  try {",
"    const res = await fetch('/api/timetable', {",
"      method: 'POST',",
"      headers: { 'Content-Type': 'application/json' },",
"      body: body",
"    });",
"    const result = await res.json();",
"    toast(result.message || (res.ok ? '导入成功' : '导入失败'), res.ok);",
"    if (res.ok) { await loadTimetable(); }",
"  } catch (e) {",
"    toast('导入请求失败', false);",
"  } finally {",
"    el('timetableSaveBtn').disabled = false;",
"  }",
"}",
"el('tabAbout').addEventListener('click', function () { showTab('about'); });",
"el('tabChecklist').addEventListener('click', function () { showTab('checklist'); });",
"el('tabTimetable').addEventListener('click', function () { showTab('timetable'); });",
"el('settingsReloadBtn').addEventListener('click', loadSettings);",
"el('settingsSaveBtn').addEventListener('click', saveSettings);",
"el('manualAddBtn').addEventListener('click', addManualCourse);",
"el('clearTimetableBtn').addEventListener('click', clearTimetable);",
"el('listReloadBtn').addEventListener('click', loadChecklist);",
"el('listClearBtn').addEventListener('click', clearList);",
"el('listSaveBtn').addEventListener('click', saveChecklist);",
"el('timetableTemplateBtn').addEventListener('click', fillTemplate);",
"el('timetableReloadBtn').addEventListener('click', loadTimetable);",
"el('timetableDownloadBtn').addEventListener('click', downloadTimetable);",
"el('timetableFile').addEventListener('change', readTimetableFile);",
"el('timetableSaveBtn').addEventListener('click', saveTimetable);",
"buildListRows();",
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

/*
 * 刷屏任务：arg 传入要显示的界面编号（GUI_LAYOUT_xxx），
 * 这样保存课表/清单后都能直接跳到对应页面，改动能立刻看到。
 */
static void web_gui_refresh_task(void *arg)
{
    uint8_t layout = (uint8_t)(uintptr_t)arg;

    /* 先让 HTTP 响应发回浏览器，再刷屏（刷一帧比较慢） */
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGI(TAG, "按网页设置重绘墨水屏（第 %d 页）", (int)layout);

    if (layout < GUI_LAYOUT_COUNT) {
        layout_no = layout;
    }
    GuiUpdate();

    s_gui_refresh_busy = false;
    vTaskDelete(NULL);
}

static void web_request_gui_refresh(uint8_t layout)
{
    if (s_gui_refresh_busy) {
        return;
    }
    s_gui_refresh_busy = true;

    if (xTaskCreate(web_gui_refresh_task, "web_gui", WEB_GUI_TASK_STACK,
                    (void *)(uintptr_t)layout, 3, NULL) != pdPASS) {
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

/* ========================= 课表学期设置 ========================= */

/*
 * 读取 POST 的 JSON 请求体（自动处理分包与超时）。
 * 成功时 *body 指向 malloc 出来的、以 '\0' 结尾的缓冲区，调用者负责 free()；
 * 失败时 *status 返回建议的 HTTP 状态码，err 返回中文说明。
 */
static esp_err_t web_read_json_body(httpd_req_t *req, size_t max_len, char **body,
                                    const char **status, char *err, size_t err_size)
{
    size_t received = 0;
    int retry = 0;
    char *buf;

    *body = NULL;
    *status = "400 Bad Request";

    if (req->content_len == 0 || req->content_len >= max_len) {
        snprintf(err, err_size, "请求内容长度非法");
        return ESP_ERR_INVALID_SIZE;
    }

    buf = malloc(req->content_len + 1);
    if (buf == NULL) {
        *status = "500 Internal Server Error";
        snprintf(err, err_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }

    while (received < req->content_len) {
        int got = httpd_req_recv(req, buf + received, req->content_len - received);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++retry > 3) {
                free(buf);
                *status = "408 Request Timeout";
                snprintf(err, err_size, "读取请求内容超时");
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (got <= 0) {
            free(buf);
            snprintf(err, err_size, "读取请求内容失败");
            return ESP_ERR_INVALID_RESPONSE;
        }
        received += (size_t)got;
        retry = 0;
    }

    buf[received] = '\0';
    *body = buf;
    return ESP_OK;
}

static esp_err_t web_course_get(httpd_req_t *req)
{
    esp_err_t load_ret = Course_Load();
    char *json;
    esp_err_t ret;

    if (load_ret != ESP_OK && load_ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取课表设置 NVS 失败：%s，使用内存中的数据", esp_err_to_name(load_ret));
    }

    json = Course_ToJson();
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
    char *body = NULL;
    char err_msg[96] = { 0 };
    const char *status = "400 Bad Request";
    esp_err_t ret;

    ret = web_read_json_body(req, WEB_COURSE_BODY_MAX, &body, &status, err_msg, sizeof(err_msg));
    if (ret != ESP_OK) {
        return web_send_result(req, false, err_msg, status);
    }

    ret = Course_ApplyJson(body, err_msg, sizeof(err_msg));
    free(body);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "更新课表失败：%s（%s）", err_msg, esp_err_to_name(ret));
        return web_send_result(req, false, (err_msg[0] != '\0') ? err_msg : "保存失败",
                               "400 Bad Request");
    }

    ESP_LOGI(TAG, "网页已更新课表设置：%s", err_msg);
    web_request_gui_refresh(GUI_LAYOUT_TIMETABLE);

    return web_send_result(req, true, "学期设置已保存，墨水屏正在刷新", "200 OK");
}

/* ========================= 完整课程表导入 ========================= */

static esp_err_t web_timetable_get(httpd_req_t *req)
{
    esp_err_t load_ret = Timetable_Load();
    char *json;
    esp_err_t ret;

    if (load_ret != ESP_OK && load_ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "读取课程表 NVS 失败：%s，使用内存中的数据", esp_err_to_name(load_ret));
    }

    json = Timetable_ToJson();
    if (json == NULL) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json);

    return ret;
}

static esp_err_t web_timetable_post(httpd_req_t *req)
{
    char *body = NULL;
    char err_msg[160] = { 0 };
    const char *status = "400 Bad Request";
    esp_err_t ret;

    ret = web_read_json_body(req, WEB_TIMETABLE_BODY_MAX, &body, &status, err_msg, sizeof(err_msg));
    if (ret != ESP_OK) {
        return web_send_result(req, false, err_msg, status);
    }

    ret = Timetable_ApplyJson(body, err_msg, sizeof(err_msg));
    free(body);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "导入课程表失败：%s（%s）", err_msg, esp_err_to_name(ret));
        return web_send_result(req, false, (err_msg[0] != '\0') ? err_msg : "导入失败",
                               "400 Bad Request");
    }

    ESP_LOGI(TAG, "网页已导入课程表：%s", err_msg);
    web_request_gui_refresh(GUI_LAYOUT_TIMETABLE);
    return web_send_result(req, true, err_msg, "200 OK");
}

static esp_err_t web_timetable_delete(httpd_req_t *req)
{
    esp_err_t ret = Timetable_Clear();

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "清空课程表失败：%s", esp_err_to_name(ret));
        return web_send_result(req, false, "清空课程表失败（写入 NVS 出错）",
                               "500 Internal Server Error");
    }

    ESP_LOGI(TAG, "网页已清空课程表");
    web_request_gui_refresh(GUI_LAYOUT_TIMETABLE);
    return web_send_result(req, true, "课程表已清空，墨水屏正在刷新", "200 OK");
}

/* ========================= 清单（待办事项） ========================= */

static esp_err_t web_checklist_get(httpd_req_t *req)
{
    char *json = Checklist_ToJson();
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

static esp_err_t web_checklist_post(httpd_req_t *req)
{
    char *body = NULL;
    char err_msg[96] = { 0 };
    const char *status = "400 Bad Request";
    esp_err_t ret;

    ret = web_read_json_body(req, WEB_CHECKLIST_BODY_MAX, &body, &status, err_msg, sizeof(err_msg));
    if (ret != ESP_OK) {
        return web_send_result(req, false, err_msg, status);
    }

    ret = Checklist_ApplyJson(body, err_msg, sizeof(err_msg));
    free(body);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "更新清单失败：%s（%s）", err_msg, esp_err_to_name(ret));
        return web_send_result(req, false, (err_msg[0] != '\0') ? err_msg : "保存失败",
                               "400 Bad Request");
    }

    ESP_LOGI(TAG, "网页已更新清单：%s", err_msg);
    web_request_gui_refresh(GUI_LAYOUT_CHECKLIST);

    return web_send_result(req, true, "清单已保存，墨水屏正在刷新", "200 OK");
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
        { .uri = "/",              .method = HTTP_GET,  .handler = web_index_get,      .user_ctx = NULL },
        { .uri = "/api/about",     .method = HTTP_GET,  .handler = web_about_get,      .user_ctx = NULL },
        { .uri = "/api/course",    .method = HTTP_GET,  .handler = web_course_get,     .user_ctx = NULL },
        { .uri = "/api/course",    .method = HTTP_POST, .handler = web_course_post,    .user_ctx = NULL },
        { .uri = "/api/checklist", .method = HTTP_GET,  .handler = web_checklist_get,  .user_ctx = NULL },
        { .uri = "/api/checklist", .method = HTTP_POST, .handler = web_checklist_post, .user_ctx = NULL },
        { .uri = "/api/timetable", .method = HTTP_GET,  .handler = web_timetable_get,  .user_ctx = NULL },
        { .uri = "/api/timetable", .method = HTTP_POST, .handler = web_timetable_post, .user_ctx = NULL },
        { .uri = "/api/timetable", .method = HTTP_DELETE, .handler = web_timetable_delete, .user_ctx = NULL },
        { .uri = "/favicon.ico",   .method = HTTP_GET,  .handler = web_favicon_get,    .user_ctx = NULL },
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

    ESP_LOGI(TAG, "网页路由注册完成：关于设备 / 课表学期设置 / 清单 / 完整课程表");
    return ESP_OK;
}
