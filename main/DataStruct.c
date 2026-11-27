/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-11-02 22:35:33
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-26 22:30:00
 * @FilePath: \epd\main\DataStruct.c
 * @Description: 课表数据结构实现：默认值 / NVS 持久化 / JSON 校验与序列化。
 *
 * JSON 结构：
 * {
 *     "Course_Lesson_Status": 0,
 *     "Course_Lesson_Type": 0,
 *     "Course_Lesson_Postion": 0,
 *     "Course_Lesson_Postion_Str": "X000",
 *     "Course_Lesson_Time": "00:00~00:00",
 *     "Semester_Week_Now": 1,
 *     "AcademicYear": "2025-2026",
 *     "Semester": 1
 * }
 *
 * NVS 记录头为 "CRS2"；上电时读到旧的 "CRS1" 记录会自动迁移。
 */
#include "DataStruct.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "COURSE";

/* NVS 存储位置：与 RTC 模块的 rtc_storage 命名空间分开 */
#define COURSE_NVS_NAMESPACE    "course_store"
#define COURSE_NVS_KEY          "lesson"
#define COURSE_NVS_MAGIC        0x43525332u     /* "CRS2"，当前版本 */
#define COURSE_NVS_MAGIC_V1     0x43525331u     /* "CRS1"，兼容旧版本 */

/* 旧版（CRS1）记录：没有 AcademicYear / Semester，仅用于自动迁移 */
typedef struct {
    uint32_t magic;
    uint8_t  status;
    uint8_t  type;
    uint8_t  postion;
    uint8_t  semester_week;
    char     postion_str[COURSE_LESSON_POSITION_STR_LEN];
    char     time[COURSE_LESSON_TIME_LEN];
} course_nvs_blob_v1_t;

/* NVS 中保存的原始数据 */
typedef struct {
    uint32_t magic;
    uint8_t  status;
    uint8_t  type;
    uint8_t  postion;
    uint8_t  semester_week;
    char     postion_str[COURSE_LESSON_POSITION_STR_LEN];
    char     time[COURSE_LESSON_TIME_LEN];
    char     academic_year[COURSE_ACADEMIC_YEAR_LEN + 1];
    uint8_t  semester;
} course_nvs_blob_t;

/* ========================= 课表全局数据（GUI 直接使用） ========================= */

course_lesson_status_t  Course_Lesson_Status = COURSE_LESSON_NOW;
course_lesson_type_t    Course_Lesson_Type = COURSE_LESSON_THEORETICAL;
course_lesson_postion_t Course_Lesson_Postion = COURSE_LESSON_TEACHING;
char                    Course_Lesson_Postion_Str[COURSE_LESSON_POSITION_STR_LEN] = { 'A', '1', '0', '2', '\0' };
char                    Course_Lesson_Time[COURSE_LESSON_TIME_LEN] = "08:00~09:40";
uint8_t                 Semester_Week_Now = 12;
char                    Course_Academic_Year[COURSE_ACADEMIC_YEAR_LEN + 1] = "";
uint8_t                 Course_Semester = COURSE_SEMESTER_NONE;

/* ========================= 内部工具 ========================= */

static esp_err_t course_fail(char *err, size_t err_size, esp_err_t code, const char *msg)
{
    if (err != NULL && err_size > 0) {
        snprintf(err, err_size, "%s", msg);
    }
    return code;
}

static bool course_digits_ok(const char *s, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

/* 教室编号：必须是 4 位可见 ASCII 字符（如 A102），且带结束符 */
static bool course_position_ok(const char *s)
{
    if (s == NULL || strlen(s) != (COURSE_LESSON_POSITION_STR_LEN - 1)) {
        return false;
    }
    for (size_t i = 0; i < COURSE_LESSON_POSITION_STR_LEN - 1; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

bool Course_IsTimeValid(const char *time_str)
{
    int h1 = 0, m1 = 0, h2 = 0, m2 = 0;

    if (time_str == NULL || strlen(time_str) != 11) {
        return false;
    }
    if (time_str[2] != ':' || time_str[5] != '~' || time_str[8] != ':') {
        return false;
    }
    if (!course_digits_ok(&time_str[0], 2) || !course_digits_ok(&time_str[3], 2) ||
        !course_digits_ok(&time_str[6], 2) || !course_digits_ok(&time_str[9], 2)) {
        return false;
    }
    if (sscanf(time_str, "%2d:%2d~%2d:%2d", &h1, &m1, &h2, &m2) != 4) {
        return false;
    }
    if (h1 > 23 || h2 > 23 || m1 > 59 || m2 > 59) {
        return false;
    }
    /* 结束时间必须晚于开始时间，否则进度条没有意义 */
    return (h1 * 60 + m1) < (h2 * 60 + m2);
}

/* 学年：空串表示未设置，否则必须是 "YYYY-YYYY"（如 2025-2026） */
static bool course_academic_year_ok(const char *year)
{
    if (year == NULL) {
        return false;
    }
    if (year[0] == '\0') {
        return true;
    }
    if (strlen(year) != COURSE_ACADEMIC_YEAR_LEN || year[4] != '-') {
        return false;
    }
    for (size_t i = 0; i < COURSE_ACADEMIC_YEAR_LEN; i++) {
        if (i != 4 && !isdigit((unsigned char)year[i])) {
            return false;
        }
    }
    return true;
}

/* ========================= 课表接口 ========================= */

void Course_Default(void)
{
    Course_Lesson_Status  = COURSE_LESSON_NOW;
    Course_Lesson_Type    = COURSE_LESSON_THEORETICAL;
    Course_Lesson_Postion = COURSE_LESSON_TEACHING;

    memset(Course_Lesson_Postion_Str, 0, sizeof(Course_Lesson_Postion_Str));
    memcpy(Course_Lesson_Postion_Str, "A102", 4);

    strlcpy(Course_Lesson_Time, "08:00~09:40", sizeof(Course_Lesson_Time));
    Semester_Week_Now = 12;
    Course_Academic_Year[0] = '\0';
    Course_Semester = COURSE_SEMESTER_NONE;
}

esp_err_t Course_Save(void)
{
    course_nvs_blob_t blob;
    nvs_handle_t handle;
    esp_err_t ret;

    memset(&blob, 0, sizeof(blob));
    blob.magic         = COURSE_NVS_MAGIC;
    blob.status        = (uint8_t)Course_Lesson_Status;
    blob.type          = (uint8_t)Course_Lesson_Type;
    blob.postion       = (uint8_t)Course_Lesson_Postion;
    blob.semester_week = Semester_Week_Now;
    memcpy(blob.postion_str, Course_Lesson_Postion_Str, sizeof(blob.postion_str));
    memcpy(blob.time, Course_Lesson_Time, sizeof(blob.time));
    memcpy(blob.academic_year, Course_Academic_Year, sizeof(blob.academic_year));
    blob.semester = Course_Semester;

    ret = nvs_open(COURSE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, COURSE_NVS_KEY, &blob, sizeof(blob));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "课表写入 NVS 失败：%s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "课表已保存：状态=%d 类型=%d 地点=%d-%s 时间=%s 周次=%d 学年=%s 学期=%d",
                 (int)Course_Lesson_Status, (int)Course_Lesson_Type,
                 (int)Course_Lesson_Postion, Course_Lesson_Postion_Str,
                 Course_Lesson_Time, (int)Semester_Week_Now,
                 Course_Academic_Year, (int)Course_Semester);
    }
    return ret;
}

esp_err_t Course_Load(void)
{
    uint8_t raw[sizeof(course_nvs_blob_t)];
    course_nvs_blob_t blob;
    nvs_handle_t handle;
    size_t len = sizeof(raw);
    uint32_t magic = 0;
    bool migrated = false;
    esp_err_t ret;

    memset(raw, 0, sizeof(raw));
    ret = nvs_open(COURSE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        /* 首次上电没有记录，属于正常情况 */
        return ret;
    }

    ret = nvs_get_blob(handle, COURSE_NVS_KEY, raw, &len);
    nvs_close(handle);
    if (ret != ESP_OK) {
        return ret;
    }

    if (len >= sizeof(magic)) {
        memcpy(&magic, raw, sizeof(magic));
    }

    memset(&blob, 0, sizeof(blob));
    if (magic == COURSE_NVS_MAGIC_V1 && len == sizeof(course_nvs_blob_v1_t)) {
        course_nvs_blob_v1_t old;

        memcpy(&old, raw, sizeof(old));
        blob.magic         = COURSE_NVS_MAGIC;
        blob.status        = old.status;
        blob.type          = old.type;
        blob.postion       = old.postion;
        blob.semester_week = old.semester_week;
        memcpy(blob.postion_str, old.postion_str, sizeof(blob.postion_str));
        memcpy(blob.time, old.time, sizeof(blob.time));
        migrated = true;
    } else if (magic == COURSE_NVS_MAGIC && len == sizeof(course_nvs_blob_t)) {
        memcpy(&blob, raw, sizeof(blob));
    } else {
        ESP_LOGW(TAG, "课表记录无效（len=%u magic=0x%08x），忽略", (unsigned)len, (unsigned)magic);
        return ESP_ERR_INVALID_STATE;
    }

    /* 兜底保证字符串结束符，再逐项校验 */
    blob.postion_str[COURSE_LESSON_POSITION_STR_LEN - 1] = '\0';
    blob.time[COURSE_LESSON_TIME_LEN - 1] = '\0';
    blob.academic_year[COURSE_ACADEMIC_YEAR_LEN] = '\0';

    if (blob.status > COURSE_LESSON_NOW ||
        blob.type > COURSE_LESSON_PRACTICAL ||
        blob.postion > COURSE_LESSON_TECHNOLOGY ||
        blob.semester_week < COURSE_SEMESTER_WEEK_MIN ||
        blob.semester_week > COURSE_SEMESTER_WEEK_MAX ||
        blob.semester > COURSE_SEMESTER_SECOND ||
        !course_position_ok(blob.postion_str) ||
        !Course_IsTimeValid(blob.time) ||
        !course_academic_year_ok(blob.academic_year)) {
        ESP_LOGW(TAG, "课表字段越界，忽略 NVS 记录");
        return ESP_ERR_INVALID_STATE;
    }

    Course_Lesson_Status  = (course_lesson_status_t)blob.status;
    Course_Lesson_Type    = (course_lesson_type_t)blob.type;
    Course_Lesson_Postion = (course_lesson_postion_t)blob.postion;
    memcpy(Course_Lesson_Postion_Str, blob.postion_str, sizeof(Course_Lesson_Postion_Str));
    memcpy(Course_Lesson_Time, blob.time, sizeof(Course_Lesson_Time));
    Semester_Week_Now = blob.semester_week;
    memcpy(Course_Academic_Year, blob.academic_year, sizeof(Course_Academic_Year));
    Course_Semester = blob.semester;

    if (migrated) {
        ESP_LOGI(TAG, "已把旧版 CRS1 课表设置迁移为 CRS2");
        ret = Course_Save();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "迁移后的课表设置写回 NVS 失败：%s", esp_err_to_name(ret));
        }
    }

    return ESP_OK;
}

esp_err_t Course_Init(void)
{
    esp_err_t ret;

    Course_Default();

    ret = Course_Load();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "未载入已保存的课表（%s），使用默认值", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "课表载入成功：状态=%d 时间=%s 地点=%s 周次=%d 学年=%s 学期=%d",
                 (int)Course_Lesson_Status, Course_Lesson_Time,
                 Course_Lesson_Postion_Str, (int)Semester_Week_Now,
                 Course_Academic_Year, (int)Course_Semester);
    }
    return ret;
}


char *Course_ToJson(void)
{
    cJSON *root;
    char *out;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "Course_Lesson_Status", (double)Course_Lesson_Status);
    cJSON_AddNumberToObject(root, "Course_Lesson_Type", (double)Course_Lesson_Type);
    cJSON_AddNumberToObject(root, "Course_Lesson_Postion", (double)Course_Lesson_Postion);
    cJSON_AddStringToObject(root, "Course_Lesson_Postion_Str", Course_Lesson_Postion_Str);
    cJSON_AddStringToObject(root, "Course_Lesson_Time", Course_Lesson_Time);
    cJSON_AddNumberToObject(root, "Semester_Week_Now", (double)Semester_Week_Now);
    cJSON_AddStringToObject(root, "AcademicYear", Course_Academic_Year);
    cJSON_AddNumberToObject(root, "Semester", (double)Course_Semester);

    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return out;
}

esp_err_t Course_ApplyJson(const char *json, char *err, size_t err_size)
{
    cJSON *root;
    const cJSON *item;
    course_lesson_status_t  status  = Course_Lesson_Status;
    course_lesson_type_t    type    = Course_Lesson_Type;
    course_lesson_postion_t postion = Course_Lesson_Postion;
    char    postion_str[COURSE_LESSON_POSITION_STR_LEN];
    char    time[COURSE_LESSON_TIME_LEN];
    char    academic_year[COURSE_ACADEMIC_YEAR_LEN + 1];
    uint8_t semester = Course_Semester;
    uint8_t week = Semester_Week_Now;
    esp_err_t ret = ESP_OK;

    if (json == NULL) {
        return course_fail(err, err_size, ESP_ERR_INVALID_ARG, "请求内容为空");
    }

    root = cJSON_Parse(json);
    if (root == NULL) {
        return course_fail(err, err_size, ESP_ERR_INVALID_ARG, "JSON 格式错误");
    }

    /* 未提交的字段沿用当前值 */
    memcpy(postion_str, Course_Lesson_Postion_Str, sizeof(postion_str));
    memcpy(time, Course_Lesson_Time, sizeof(time));
    memcpy(academic_year, Course_Academic_Year, sizeof(academic_year));

    item = cJSON_GetObjectItemCaseSensitive(root, "Course_Lesson_Status");
    if (item != NULL) {
        if (!cJSON_IsNumber(item) ||
            item->valueint < COURSE_LESSON_NULL || item->valueint > COURSE_LESSON_NOW) {
            ret = course_fail(err, err_size, ESP_ERR_INVALID_ARG, "课程状态取值应为 0~2");
            goto out;
        }
        status = (course_lesson_status_t)item->valueint;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "Course_Lesson_Type");
    if (item != NULL) {
        if (!cJSON_IsNumber(item) ||
            item->valueint < COURSE_LESSON_THEORETICAL || item->valueint > COURSE_LESSON_PRACTICAL) {
            ret = course_fail(err, err_size, ESP_ERR_INVALID_ARG, "课程类型取值应为 0~1");
            goto out;
        }
        type = (course_lesson_type_t)item->valueint;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "Course_Lesson_Postion");
    if (item != NULL) {
        if (!cJSON_IsNumber(item) ||
            item->valueint < COURSE_LESSON_TEACHING || item->valueint > COURSE_LESSON_TECHNOLOGY) {
            ret = course_fail(err, err_size, ESP_ERR_INVALID_ARG, "上课地点取值应为 0~1");
            goto out;
        }
        postion = (course_lesson_postion_t)item->valueint;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "Course_Lesson_Postion_Str");
    if (item != NULL) {
        if (!cJSON_IsString(item) || !course_position_ok(item->valuestring)) {
            ret = course_fail(err, err_size, ESP_ERR_INVALID_ARG, "教室编号必须是 4 位字符，如 A102");
            goto out;
        }
        memset(postion_str, 0, sizeof(postion_str));
        strlcpy(postion_str, item->valuestring, sizeof(postion_str));
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "Course_Lesson_Time");
    if (item != NULL) {
        if (!cJSON_IsString(item) || !Course_IsTimeValid(item->valuestring)) {
            ret = course_fail(err, err_size, ESP_ERR_INVALID_ARG, "上课时间格式应为 HH:MM~HH:MM，且结束时间晚于开始时间");
            goto out;
        }
        memset(time, 0, sizeof(time));
        strlcpy(time, item->valuestring, sizeof(time));
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "Semester_Week_Now");
    if (item != NULL) {
        if (!cJSON_IsNumber(item) ||
            item->valueint < COURSE_SEMESTER_WEEK_MIN || item->valueint > COURSE_SEMESTER_WEEK_MAX) {
            ret = course_fail(err, err_size, ESP_ERR_INVALID_ARG, "学期周次取值应为 1~53");
            goto out;
        }
        week = (uint8_t)item->valueint;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "AcademicYear");
    if (item != NULL) {
        if (!cJSON_IsString(item) || !course_academic_year_ok(item->valuestring)) {
            ret = course_fail(err, err_size, ESP_ERR_INVALID_ARG,
                              "学年格式应为 YYYY-YYYY，如 2025-2026（也可留空）");
            goto out;
        }
        memset(academic_year, 0, sizeof(academic_year));
        strlcpy(academic_year, item->valuestring, sizeof(academic_year));
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "Semester");
    if (item != NULL) {
        if (!cJSON_IsNumber(item) ||
            item->valueint < COURSE_SEMESTER_NONE || item->valueint > COURSE_SEMESTER_SECOND) {
            ret = course_fail(err, err_size, ESP_ERR_INVALID_ARG,
                              "学期取值应为 0~2（0=未设置，1=第一学期，2=第二学期）");
            goto out;
        }
        semester = (uint8_t)item->valueint;
    }

    /* 全部校验通过后再写全局变量，避免出现半更新的课表 */
    Course_Lesson_Status  = status;
    Course_Lesson_Type    = type;
    Course_Lesson_Postion = postion;
    memcpy(Course_Lesson_Postion_Str, postion_str, sizeof(Course_Lesson_Postion_Str));
    memcpy(Course_Lesson_Time, time, sizeof(Course_Lesson_Time));
    Semester_Week_Now = week;
    memcpy(Course_Academic_Year, academic_year, sizeof(Course_Academic_Year));
    Course_Semester = semester;

    ret = Course_Save();
    if (ret != ESP_OK) {
        ret = course_fail(err, err_size, ret, "已应用到内存，但写入 NVS 失败");
        goto out;
    }

    if (err != NULL && err_size > 0) {
        snprintf(err, err_size, "课表已更新");
    }

out:
    cJSON_Delete(root);
    return ret;
}



