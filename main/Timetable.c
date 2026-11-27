/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-11-27 10:00:00
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-27 10:00:00
 * @FilePath: \epd\main\Timetable.c
 * @Description: 完整课程表（JSON 导入）实现：Schema 校验 / NVS 持久化 / 序列化。
 *
 * JSON 结构见 Timetable.h。4 位数字键按不透明课程编号处理，原样保存。
 */
#include "Timetable.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "gb2312_map.h"
#include "nvs.h"

static const char *TAG = "TIMETABLE";

/* NVS 存储位置：与课节（course_store）、清单（list_store）分开 */
#define TIMETABLE_NVS_NAMESPACE   "timetable"
#define TIMETABLE_NVS_KEY         "courses"
#define TIMETABLE_NVS_MAGIC       0x544D5433u   /* "TMT3"，当前版本（增加 Day） */
#define TIMETABLE_NVS_MAGIC_V2    0x544D5432u   /* "TMT2"，兼容旧版本（无 Day） */
#define TIMETABLE_NVS_MAGIC_V1    0x544D5431u   /* "TMT1"，兼容旧版本（无时间、无 Day） */

/* NVS 中保存的记录头，后面紧跟 count 个 timetable_course_t */
typedef struct {
    uint32_t magic;
    uint8_t  count;
} timetable_nvs_header_t;

/* 旧版（TMT1）课程结构：没有 StartTime / EndTime，仅用于自动迁移 */
typedef struct {
    uint8_t start;
    uint8_t end;
} timetable_duration_v1_t;

typedef struct {
    char     code[TIMETABLE_CODE_LEN];
    char     name[TIMETABLE_NAME_LEN + 1];
    char     classroom[TIMETABLE_CLASSROOM_LEN + 1];
    char     teacher[TIMETABLE_TEACHER_LEN + 1];
    uint8_t  duration_count;
    timetable_duration_v1_t duration[TIMETABLE_DURATION_MAX];
    uint8_t  session_count;
    timetable_session_t sessions[TIMETABLE_SESSION_MAX];
} timetable_course_v1_t;

/* 旧版（TMT2）课程结构：有 StartTime / EndTime，但没有 Day */
typedef struct {
    char     code[TIMETABLE_CODE_LEN];
    char     name[TIMETABLE_NAME_LEN + 1];
    char     classroom[TIMETABLE_CLASSROOM_LEN + 1];
    char     teacher[TIMETABLE_TEACHER_LEN + 1];
    char     start_time[TIMETABLE_TIME_LEN + 1];
    char     end_time[TIMETABLE_TIME_LEN + 1];
    uint8_t  duration_count;
    timetable_duration_t duration[TIMETABLE_DURATION_MAX];
    uint8_t  session_count;
    timetable_session_t sessions[TIMETABLE_SESSION_MAX];
} timetable_course_v2_t;

/* ========================= 课表全局数据 ========================= */

timetable_course_t Timetable_Courses[TIMETABLE_COURSE_MAX];
uint8_t            Timetable_Count = 0;

/* ========================= 错误信息工具 ========================= */

static esp_err_t timetable_fail(char *err, size_t err_size, esp_err_t code, const char *msg)
{
    if (err != NULL && err_size > 0) {
        snprintf(err, err_size, "%s", msg);
    }
    return code;
}

static esp_err_t timetable_failf(char *err, size_t err_size, esp_err_t code,
                                 const char *fmt, ...)
{
    if (err != NULL && err_size > 0) {
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(err, err_size, fmt, ap);
        va_end(ap);
    }
    return code;
}

/* ========================= 字段校验工具 ========================= */

static bool timetable_code_ok(const char *code)
{
    if (code == NULL || strlen(code) != (TIMETABLE_CODE_LEN - 1)) {
        return false;
    }
    for (size_t i = 0; i < TIMETABLE_CODE_LEN - 1; i++) {
        if (code[i] < '0' || code[i] > '9') {
            return false;
        }
    }
    return true;
}

/*
 * 校验 "HH:MM" 时间文本（24 小时制）。
 * 空串由调用者决定是否允许，本函数只接受完整格式。
 */
static bool timetable_time_valid(const char *time_str)
{
    int hour = 0;
    int minute = 0;

    if (time_str == NULL || strlen(time_str) != TIMETABLE_TIME_LEN || time_str[2] != ':') {
        return false;
    }
    if (!isdigit((unsigned char)time_str[0]) || !isdigit((unsigned char)time_str[1]) ||
        !isdigit((unsigned char)time_str[3]) || !isdigit((unsigned char)time_str[4])) {
        return false;
    }
    if (sscanf(time_str, "%2d:%2d", &hour, &minute) != 2) {
        return false;
    }
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

/* 比较两个已通过 timetable_time_valid() 的时间，结束时间必须晚于开始时间 */
static bool timetable_time_range_valid(const char *start, const char *end)
{
    int start_min;
    int end_min;

    if (!timetable_time_valid(start) || !timetable_time_valid(end)) {
        return false;
    }
    start_min = ((start[0] - '0') * 10 + (start[1] - '0')) * 60 +
                (start[3] - '0') * 10 + (start[4] - '0');
    end_min = ((end[0] - '0') * 10 + (end[1] - '0')) * 60 +
              (end[3] - '0') * 10 + (end[4] - '0');
    return start_min < end_min;
}

/*
 * 校验 UTF-8 文本：长度、合法性、控制字符、字库可显示性。
 * 字库芯片只认 GB2312，因此导入时就拒绝无法显示成字模的字符。
 */
static bool timetable_text_valid(const char *text, size_t max_len, bool allow_empty,
                                 const char *code, const char *field,
                                 char *err, size_t err_size)
{
    const uint8_t *p;
    const uint8_t *end;
    size_t len;

    if (text == NULL) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的 %s 不能为空", code, field);
        return false;
    }

    len = strlen(text);
    if (len > max_len) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_SIZE,
                        "课程 %s 的 %s 太长（最多 %u 字节）",
                        code, field, (unsigned)max_len);
        return false;
    }
    if (len == 0) {
        if (allow_empty) {
            return true;
        }
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的 %s 不能为空", code, field);
        return false;
    }

    p = (const uint8_t *)text;
    end = p + len;
    while (p < end) {
        int32_t cp = Gb2312_Utf8Next(&p, end);
        uint16_t gb_code;

        if (cp < 0) {
            timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "课程 %s 的 %s 不是合法的 UTF-8 文本", code, field);
            return false;
        }
        if (cp < 0x20 || cp == 0x7F) {
            timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "课程 %s 的 %s 含有控制字符", code, field);
            return false;
        }
        if (cp >= 0x80 && !Gb2312_FromUnicode((uint32_t)cp, &gb_code)) {
            timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "课程 %s 的 %s 含有字库不支持的字符（仅支持 GB2312）",
                            code, field);
            return false;
        }
    }

    return true;
}

/* Schema 的 additionalProperties: false：对象里只允许 allowed 中的字段 */
static bool timetable_keys_allowed(const cJSON *obj, const char *const *allowed,
                                   size_t allowed_count, const char *code,
                                   char *err, size_t err_size)
{
    const cJSON *child;

    cJSON_ArrayForEach(child, obj) {
        bool found = false;

        if (child->string == NULL) {
            return false;
        }
        for (size_t i = 0; i < allowed_count; i++) {
            if (strcmp(child->string, allowed[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "课程 %s 含有不支持的字段 \"%s\"", code, child->string);
            return false;
        }
    }

    return true;
}

/* 取必填字符串字段，校验后复制到 dst（dst_size 已包含 '\0'） */
static bool timetable_required_string(const cJSON *obj, const char *key, const char *code,
                                      bool allow_empty, size_t max_len,
                                      char *dst, size_t dst_size,
                                      char *err, size_t err_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (item == NULL) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 缺少 %s 字段", code, key);
        return false;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的 %s 应为字符串", code, key);
        return false;
    }
    if (!timetable_text_valid(item->valuestring, max_len, allow_empty, code, key,
                              err, err_size)) {
        return false;
    }

    memset(dst, 0, dst_size);
    strlcpy(dst, item->valuestring, dst_size);
    return true;
}

/* 取可选的 "HH:MM" 字段；不存在或空串时写入空串 */
static bool timetable_optional_time(const cJSON *obj, const char *key, const char *code,
                                    char *dst, size_t dst_size,
                                    char *err, size_t err_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    memset(dst, 0, dst_size);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的 %s 应为字符串", code, key);
        return false;
    }
    if (item->valuestring[0] == '\0') {
        return true;
    }
    if (!timetable_time_valid(item->valuestring)) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的 %s 格式应为 HH:MM", code, key);
        return false;
    }
    strlcpy(dst, item->valuestring, dst_size);
    return true;
}

/* 取必填数组字段 */
static const cJSON *timetable_required_array(const cJSON *obj, const char *key,
                                             const char *code,
                                             char *err, size_t err_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (item == NULL) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 缺少 %s 字段", code, key);
        return NULL;
    }
    if (!cJSON_IsArray(item)) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的 %s 应为数组", code, key);
        return NULL;
    }
    return item;
}

/* 取整数字段（Schema 要求 integer），并检查范围 */
static bool timetable_int_field(const cJSON *obj, const char *key, int min, int max,
                                const char *where, int *out,
                                char *err, size_t err_size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    double value;
    int    ivalue;

    if (item == NULL) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "%s 缺少 %s 字段", where, key);
        return false;
    }
    if (!cJSON_IsNumber(item)) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "%s 的 %s 应为整数", where, key);
        return false;
    }

    value = item->valuedouble;
    if (value < (double)min || value > (double)max) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "%s 的 %s 应为 %d~%d 的整数", where, key, min, max);
        return false;
    }
    ivalue = (int)value;
    if ((double)ivalue != value) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "%s 的 %s 应为整数", where, key);
        return false;
    }

    *out = ivalue;
    return true;
}


/* 解析 CourseDuration[] 的一项 */
static bool timetable_parse_duration(const cJSON *entry, const char *code, int index,
                                     timetable_duration_t *out,
                                     char *err, size_t err_size)
{
    static const char *const allowed[] = { "Start", "End" };
    char where[48];
    int start = 0;
    int end = 0;

    if (!cJSON_IsObject(entry)) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的 CourseDuration[%d] 应为对象", code, index);
        return false;
    }
    if (!timetable_keys_allowed(entry, allowed, sizeof(allowed) / sizeof(allowed[0]),
                                code, err, err_size)) {
        return false;
    }

    snprintf(where, sizeof(where), "课程 %s 的 CourseDuration[%d]", code, index);
    if (!timetable_int_field(entry, "Start", TIMETABLE_PERIOD_MIN, TIMETABLE_PERIOD_MAX,
                             where, &start, err, err_size) ||
        !timetable_int_field(entry, "End", TIMETABLE_PERIOD_MIN, TIMETABLE_PERIOD_MAX,
                             where, &end, err, err_size)) {
        return false;
    }
    if (start > end) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "%s 的 Start 不能大于 End", where);
        return false;
    }

    out->start = (uint8_t)start;
    out->end   = (uint8_t)end;
    return true;
}

/* 解析 CourseSessions[] 的一项 */
static bool timetable_parse_session(const cJSON *entry, const char *code, int index,
                                    timetable_session_t *out,
                                    char *err, size_t err_size)
{
    static const char *const allowed[] = { "Week", "Start", "End" };
    char where[48];
    int week  = 0;
    int start = 0;
    int end   = 0;

    if (!cJSON_IsObject(entry)) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的 CourseSessions[%d] 应为对象", code, index);
        return false;
    }
    if (!timetable_keys_allowed(entry, allowed, sizeof(allowed) / sizeof(allowed[0]),
                                code, err, err_size)) {
        return false;
    }

    snprintf(where, sizeof(where), "课程 %s 的 CourseSessions[%d]", code, index);
    if (!timetable_int_field(entry, "Week", TIMETABLE_WEEK_MIN, TIMETABLE_WEEK_MAX,
                             where, &week, err, err_size) ||
        !timetable_int_field(entry, "Start", TIMETABLE_PERIOD_MIN, TIMETABLE_PERIOD_MAX,
                             where, &start, err, err_size) ||
        !timetable_int_field(entry, "End", TIMETABLE_PERIOD_MIN, TIMETABLE_PERIOD_MAX,
                             where, &end, err, err_size)) {
        return false;
    }
    if (start > end) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "%s 的 Start 不能大于 End", where);
        return false;
    }

    out->week  = (uint8_t)week;
    out->start = (uint8_t)start;
    out->end   = (uint8_t)end;
    return true;
}


/* 解析一门课程 */
static bool timetable_parse_course(const char *code, const cJSON *obj,
                                   timetable_course_t *out,
                                   char *err, size_t err_size)
{
    static const char *const allowed[] = {
        "CourseName", "Day", "CourseDuration", "CourseSessions", "Classroom",
        "Teacher", "StartTime", "EndTime"
    };
    const cJSON *array;
    const cJSON *entry;
    char where[24];
    int index;
    int day;

    memset(out, 0, sizeof(*out));
    if (!timetable_code_ok(code)) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程编号 \"%s\" 必须是 4 位数字", (code != NULL) ? code : "");
        return false;
    }
    memcpy(out->code, code, TIMETABLE_CODE_LEN - 1);
    out->code[TIMETABLE_CODE_LEN - 1] = '\0';

    if (!cJSON_IsObject(obj)) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的内容应为对象", code);
        return false;
    }
    if (!timetable_keys_allowed(obj, allowed, sizeof(allowed) / sizeof(allowed[0]),
                                code, err, err_size)) {
        return false;
    }

    if (!timetable_required_string(obj, "CourseName", code, false, TIMETABLE_NAME_LEN,
                                   out->name, sizeof(out->name), err, err_size) ||
        !timetable_required_string(obj, "Classroom", code, true, TIMETABLE_CLASSROOM_LEN,
                                   out->classroom, sizeof(out->classroom), err, err_size) ||
        !timetable_required_string(obj, "Teacher", code, true, TIMETABLE_TEACHER_LEN,
                                   out->teacher, sizeof(out->teacher), err, err_size)) {
        return false;
    }

    /* Day 可选：缺省为 0，兼容旧版 JSON；出现时必须是 1~7 */
    out->day = 0;
    if (cJSON_GetObjectItemCaseSensitive(obj, "Day") != NULL) {
        snprintf(where, sizeof(where), "课程 %s", code);
        if (!timetable_int_field(obj, "Day", TIMETABLE_DAY_MIN, TIMETABLE_DAY_MAX,
                                 where, &day, err, err_size)) {
            return false;
        }
        out->day = (uint8_t)day;
    }

    if (!timetable_optional_time(obj, "StartTime", code, out->start_time,
                                 sizeof(out->start_time), err, err_size) ||
        !timetable_optional_time(obj, "EndTime", code, out->end_time,
                                 sizeof(out->end_time), err, err_size)) {
        return false;
    }
    if ((out->start_time[0] == '\0') != (out->end_time[0] == '\0')) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的 StartTime 与 EndTime 必须同时提供", code);
        return false;
    }
    if (out->start_time[0] != '\0' &&
        !timetable_time_range_valid(out->start_time, out->end_time)) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的结束时间必须晚于开始时间", code);
        return false;
    }

    array = timetable_required_array(obj, "CourseDuration", code, err, err_size);
    if (array == NULL) {
        return false;
    }
    index = cJSON_GetArraySize(array);
    if (index > TIMETABLE_DURATION_MAX) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_SIZE,
                        "课程 %s 的 CourseDuration 最多 %d 项",
                        code, TIMETABLE_DURATION_MAX);
        return false;
    }
    out->duration_count = (uint8_t)index;
    index = 0;
    cJSON_ArrayForEach(entry, array) {
        if (!timetable_parse_duration(entry, code, index, &out->duration[index],
                                      err, err_size)) {
            return false;
        }
        index++;
    }

    array = timetable_required_array(obj, "CourseSessions", code, err, err_size);
    if (array == NULL) {
        return false;
    }
    index = cJSON_GetArraySize(array);
    if (index < 1) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                        "课程 %s 的 CourseSessions 至少要有 1 项", code);
        return false;
    }
    if (index > TIMETABLE_SESSION_MAX) {
        timetable_failf(err, err_size, ESP_ERR_INVALID_SIZE,
                        "课程 %s 的 CourseSessions 最多 %d 项",
                        code, TIMETABLE_SESSION_MAX);
        return false;
    }
    out->session_count = (uint8_t)index;
    index = 0;
    cJSON_ArrayForEach(entry, array) {
        if (!timetable_parse_session(entry, code, index, &out->sessions[index],
                                     err, err_size)) {
            return false;
        }
        index++;
    }

    return true;
}


/* ========================= 默认值 / 查询 ========================= */

void Timetable_Default(void)
{
    memset(Timetable_Courses, 0, sizeof(Timetable_Courses));
    Timetable_Count = 0;
}

static uint32_t timetable_count_sessions(const timetable_course_t *courses, uint8_t count)
{
    uint32_t total = 0;

    for (uint8_t i = 0; i < count; i++) {
        total += courses[i].session_count;
    }
    return total;
}

uint32_t Timetable_SessionCount(void)
{
    return timetable_count_sessions(Timetable_Courses, Timetable_Count);
}

/* ========================= NVS 持久化 ========================= */

/*
 * 校验从 NVS 读出的课程记录，并补齐字符串结束符。
 * 通过后写到 dst；err 可为 NULL（加载时只记日志）。
 */
static bool timetable_blob_course_valid(const timetable_course_t *src,
                                        timetable_course_t *dst,
                                        char *err, size_t err_size)
{
    timetable_course_t tmp = *src;

    tmp.code[TIMETABLE_CODE_LEN - 1] = '\0';
    tmp.name[TIMETABLE_NAME_LEN] = '\0';
    tmp.classroom[TIMETABLE_CLASSROOM_LEN] = '\0';
    tmp.teacher[TIMETABLE_TEACHER_LEN] = '\0';
    tmp.start_time[TIMETABLE_TIME_LEN] = '\0';
    tmp.end_time[TIMETABLE_TIME_LEN] = '\0';

    if (!timetable_code_ok(tmp.code)) {
        timetable_fail(err, err_size, ESP_ERR_INVALID_STATE, "NVS 课程编号无效");
        return false;
    }
    if (!timetable_text_valid(tmp.name, TIMETABLE_NAME_LEN, false, tmp.code,
                              "CourseName", err, err_size) ||
        !timetable_text_valid(tmp.classroom, TIMETABLE_CLASSROOM_LEN, true, tmp.code,
                              "Classroom", err, err_size) ||
        !timetable_text_valid(tmp.teacher, TIMETABLE_TEACHER_LEN, true, tmp.code,
                              "Teacher", err, err_size)) {
        return false;
    }

    if ((tmp.start_time[0] == '\0') != (tmp.end_time[0] == '\0') ||
        (tmp.start_time[0] != '\0' &&
         (!timetable_time_valid(tmp.start_time) || !timetable_time_valid(tmp.end_time) ||
          !timetable_time_range_valid(tmp.start_time, tmp.end_time)))) {
        timetable_fail(err, err_size, ESP_ERR_INVALID_STATE, "NVS 上下课时间无效");
        return false;
    }

    if (tmp.day > TIMETABLE_DAY_MAX) {
        timetable_fail(err, err_size, ESP_ERR_INVALID_STATE, "NVS Day 越界");
        return false;
    }

    if (tmp.duration_count > TIMETABLE_DURATION_MAX) {
        timetable_fail(err, err_size, ESP_ERR_INVALID_STATE, "NVS CourseDuration 数量越界");
        return false;
    }
    for (uint8_t i = 0; i < tmp.duration_count; i++) {
        if (tmp.duration[i].start < TIMETABLE_PERIOD_MIN ||
            tmp.duration[i].end > TIMETABLE_PERIOD_MAX ||
            tmp.duration[i].start > tmp.duration[i].end) {
            timetable_fail(err, err_size, ESP_ERR_INVALID_STATE, "NVS CourseDuration 越界");
            return false;
        }
    }

    if (tmp.session_count < 1 || tmp.session_count > TIMETABLE_SESSION_MAX) {
        timetable_fail(err, err_size, ESP_ERR_INVALID_STATE, "NVS CourseSessions 数量越界");
        return false;
    }
    for (uint8_t i = 0; i < tmp.session_count; i++) {
        if (tmp.sessions[i].week < TIMETABLE_WEEK_MIN ||
            tmp.sessions[i].week > TIMETABLE_WEEK_MAX ||
            tmp.sessions[i].start < TIMETABLE_PERIOD_MIN ||
            tmp.sessions[i].end > TIMETABLE_PERIOD_MAX ||
            tmp.sessions[i].start > tmp.sessions[i].end) {
            timetable_fail(err, err_size, ESP_ERR_INVALID_STATE, "NVS CourseSessions 越界");
            return false;
        }
    }

    *dst = tmp;
    return true;
}


/*
 * 把指定课程数组写入 NVS。导入时先调用本函数，成功后再替换内存数据，
 * 因此 NVS 写入失败不会破坏当前正在使用的课程表。
 */
static esp_err_t timetable_save_records(const timetable_course_t *courses, uint8_t count)
{
    timetable_nvs_header_t *header;
    nvs_handle_t handle;
    uint8_t *blob;
    size_t payload_len;
    size_t blob_len;
    esp_err_t ret;

    if (count > TIMETABLE_COURSE_MAX || (count > 0 && courses == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    payload_len = (size_t)count * sizeof(timetable_course_t);
    blob_len = sizeof(timetable_nvs_header_t) + payload_len;

    blob = calloc(1, blob_len);
    if (blob == NULL) {
        ESP_LOGE(TAG, "保存课程表时内存不足");
        return ESP_ERR_NO_MEM;
    }

    header = (timetable_nvs_header_t *)blob;
    header->magic = TIMETABLE_NVS_MAGIC;
    header->count = count;
    if (payload_len > 0) {
        memcpy(blob + sizeof(*header), courses, payload_len);
    }

    ret = nvs_open(TIMETABLE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败：%s", esp_err_to_name(ret));
        free(blob);
        return ret;
    }

    ret = nvs_set_blob(handle, TIMETABLE_NVS_KEY, blob, blob_len);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    free(blob);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "课程表写入 NVS 失败：%s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "课程表已保存：%d 门课程，%u 个上课时段，%u 字节",
                 (int)count, (unsigned)timetable_count_sessions(courses, count),
                 (unsigned)blob_len);
    }
    return ret;
}

esp_err_t Timetable_Save(void)
{
    return timetable_save_records(Timetable_Courses, Timetable_Count);
}

esp_err_t Timetable_Clear(void)
{
    esp_err_t ret = timetable_save_records(NULL, 0);

    if (ret == ESP_OK) {
        Timetable_Default();
        ESP_LOGI(TAG, "课程表已清空并写入 NVS");
    }
    return ret;
}


esp_err_t Timetable_Load(void)
{
    nvs_handle_t handle;
    timetable_nvs_header_t header;
    timetable_course_t *courses = NULL;
    uint8_t *blob = NULL;
    size_t len = 0;
    size_t course_size;
    uint8_t legacy = 0;                 /* 0=TMT3，1=TMT1，2=TMT2 */
    esp_err_t ret;

    ret = nvs_open(TIMETABLE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        /* 首次上电没有记录，属于正常情况 */
        return ret;
    }

    ret = nvs_get_blob(handle, TIMETABLE_NVS_KEY, NULL, &len);
    if (ret != ESP_OK) {
        nvs_close(handle);
        return ret;
    }
    blob = malloc(len);
    if (blob == NULL) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    ret = nvs_get_blob(handle, TIMETABLE_NVS_KEY, blob, &len);
    nvs_close(handle);
    if (ret != ESP_OK) {
        free(blob);
        return ret;
    }

    if (len < sizeof(header)) {
        free(blob);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(&header, blob, sizeof(header));

    if (header.magic == TIMETABLE_NVS_MAGIC_V1) {
        course_size = sizeof(timetable_course_v1_t);
        legacy = 1;
    } else if (header.magic == TIMETABLE_NVS_MAGIC_V2) {
        course_size = sizeof(timetable_course_v2_t);
        legacy = 2;
    } else if (header.magic == TIMETABLE_NVS_MAGIC) {
        course_size = sizeof(timetable_course_t);
    } else {
        course_size = 0;
    }

    if (course_size == 0 ||
        header.count > TIMETABLE_COURSE_MAX ||
        len != sizeof(header) + (size_t)header.count * course_size) {
        ESP_LOGW(TAG, "课程表记录无效（len=%u count=%u），忽略",
                 (unsigned)len, (unsigned)header.count);
        free(blob);
        return ESP_ERR_INVALID_STATE;
    }

    if (header.count > 0) {
        const uint8_t *payload = blob + sizeof(header);

        courses = calloc(header.count, sizeof(timetable_course_t));
        if (courses == NULL) {
            free(blob);
            return ESP_ERR_NO_MEM;
        }

        for (uint8_t i = 0; i < header.count; i++) {
            timetable_course_t conv;

            memset(&conv, 0, sizeof(conv));
            if (legacy == 1) {
                timetable_course_v1_t raw;

                memcpy(&raw, payload + (size_t)i * sizeof(raw), sizeof(raw));
                memcpy(conv.code, raw.code, sizeof(conv.code));
                memcpy(conv.name, raw.name, sizeof(conv.name));
                memcpy(conv.classroom, raw.classroom, sizeof(conv.classroom));
                memcpy(conv.teacher, raw.teacher, sizeof(conv.teacher));
                conv.duration_count = raw.duration_count;
                for (uint8_t j = 0; j < TIMETABLE_DURATION_MAX; j++) {
                    conv.duration[j].start = raw.duration[j].start;
                    conv.duration[j].end   = raw.duration[j].end;
                }
                conv.session_count = raw.session_count;
                memcpy(conv.sessions, raw.sessions, sizeof(conv.sessions));
            } else if (legacy == 2) {
                timetable_course_v2_t raw;

                memcpy(&raw, payload + (size_t)i * sizeof(raw), sizeof(raw));
                memcpy(&conv, &raw, sizeof(raw));
            } else {
                memcpy(&conv, payload + (size_t)i * sizeof(conv), sizeof(conv));
            }

            if (!timetable_blob_course_valid(&conv, &courses[i], NULL, 0)) {
                ESP_LOGW(TAG, "课程表第 %d 门课程无效，忽略 NVS 记录", (int)i + 1);
                free(courses);
                free(blob);
                return ESP_ERR_INVALID_STATE;
            }
        }
    }

    memset(Timetable_Courses, 0, sizeof(Timetable_Courses));
    if (header.count > 0) {
        memcpy(Timetable_Courses, courses, (size_t)header.count * sizeof(timetable_course_t));
    }
    Timetable_Count = header.count;

    free(courses);
    free(blob);

    if (legacy != 0) {
        ESP_LOGI(TAG, "已把旧版 TMT%u 课程表迁移为 TMT3（%d 门课程，星期未设置）",
                 (unsigned)legacy, (int)Timetable_Count);
        ret = timetable_save_records(Timetable_Courses, Timetable_Count);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "迁移后的课程表写回 NVS 失败：%s", esp_err_to_name(ret));
        }
    }

    return ESP_OK;
}

esp_err_t Timetable_Init(void)
{
    esp_err_t ret;

    Timetable_Default();

    ret = Timetable_Load();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "未载入已保存的课程表（%s），使用空表", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "课程表载入成功：%d 门课程，%u 个上课时段",
                 (int)Timetable_Count, (unsigned)Timetable_SessionCount());
    }
    return ret;
}


/* ========================= 动态 JSON 输出缓冲 ========================= */
/*
 * 完整课表最多 16 x 64 个时段，用 cJSON 建树会产生大量小节点；
 * ESP32 无 PSRAM，这里改成直接拼接字符串，峰值内存只有最终文本本身。
 */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   ok;
} timetable_json_buf_t;

static void timetable_json_reserve(timetable_json_buf_t *jb, size_t extra)
{
    size_t need;
    size_t cap;
    char *grown;

    if (!jb->ok) {
        return;
    }
    need = jb->len + extra + 1;
    if (need <= jb->cap) {
        return;
    }

    cap = (jb->cap > 0) ? jb->cap : 512;
    while (cap < need) {
        size_t next = cap + cap / 2;

        if (next <= cap) {
            cap = need;
            break;
        }
        cap = next;
    }
    grown = realloc(jb->buf, cap);
    if (grown == NULL) {
        jb->ok = false;
        return;
    }
    jb->buf = grown;
    jb->cap = cap;
}

static void timetable_json_raw(timetable_json_buf_t *jb, const char *text, size_t len)
{
    timetable_json_reserve(jb, len);
    if (!jb->ok) {
        return;
    }
    memcpy(jb->buf + jb->len, text, len);
    jb->len += len;
    jb->buf[jb->len] = '\0';
}

static void timetable_json_puts(timetable_json_buf_t *jb, const char *text)
{
    timetable_json_raw(jb, text, strlen(text));
}

static void timetable_json_number(timetable_json_buf_t *jb, unsigned value)
{
    char tmp[8];
    int len = snprintf(tmp, sizeof(tmp), "%u", value);

    if (len > 0) {
        timetable_json_raw(jb, tmp, (size_t)len);
    }
}

/* 输出带引号的 JSON 字符串，转义 "、\ 以及控制字符（UTF-8 字节原样输出） */
static void timetable_json_string(timetable_json_buf_t *jb, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)text;

    timetable_json_raw(jb, "\"", 1);
    while (jb->ok && p != NULL && *p != '\0') {
        unsigned char c = *p++;

        if (c == '"' || c == '\\') {
            char esc[2] = { '\\', (char)c };
            timetable_json_raw(jb, esc, 2);
        } else if (c < 0x20) {
            char esc[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 0x0F] };
            timetable_json_raw(jb, esc, 6);
        } else {
            char ch = (char)c;
            timetable_json_raw(jb, &ch, 1);
        }
    }
    timetable_json_raw(jb, "\"", 1);
}

/* ========================= JSON 序列化 ========================= */

char *Timetable_ToJson(void)
{
    timetable_json_buf_t jb = { .buf = NULL, .len = 0, .cap = 0, .ok = true };

    timetable_json_puts(&jb, "{");
    for (uint8_t i = 0; i < Timetable_Count && jb.ok; i++) {
        const timetable_course_t *course = &Timetable_Courses[i];

        if (i > 0) {
            timetable_json_puts(&jb, ",");
        }
        timetable_json_string(&jb, course->code);
        timetable_json_puts(&jb, ":{\"CourseName\":");
        timetable_json_string(&jb, course->name);

        timetable_json_puts(&jb, ",\"CourseDuration\":[");
        for (uint8_t j = 0; j < course->duration_count; j++) {
            if (j > 0) {
                timetable_json_puts(&jb, ",");
            }
            timetable_json_puts(&jb, "{\"Start\":");
            timetable_json_number(&jb, course->duration[j].start);
            timetable_json_puts(&jb, ",\"End\":");
            timetable_json_number(&jb, course->duration[j].end);
            timetable_json_puts(&jb, "}");
        }

        timetable_json_puts(&jb, "],\"CourseSessions\":[");
        for (uint8_t j = 0; j < course->session_count; j++) {
            if (j > 0) {
                timetable_json_puts(&jb, ",");
            }
            timetable_json_puts(&jb, "{\"Week\":");
            timetable_json_number(&jb, course->sessions[j].week);
            timetable_json_puts(&jb, ",\"Start\":");
            timetable_json_number(&jb, course->sessions[j].start);
            timetable_json_puts(&jb, ",\"End\":");
            timetable_json_number(&jb, course->sessions[j].end);
            timetable_json_puts(&jb, "}");
        }

        timetable_json_puts(&jb, "],\"Classroom\":");
        timetable_json_string(&jb, course->classroom);
        timetable_json_puts(&jb, ",\"Teacher\":");
        timetable_json_string(&jb, course->teacher);
        if (course->day >= TIMETABLE_DAY_MIN && course->day <= TIMETABLE_DAY_MAX) {
            timetable_json_puts(&jb, ",\"Day\":");
            timetable_json_number(&jb, course->day);
        }
        if (course->start_time[0] != '\0' && course->end_time[0] != '\0') {
            timetable_json_puts(&jb, ",\"StartTime\":");
            timetable_json_string(&jb, course->start_time);
            timetable_json_puts(&jb, ",\"EndTime\":");
            timetable_json_string(&jb, course->end_time);
        }
        timetable_json_puts(&jb, "}");
    }
    timetable_json_puts(&jb, "}");

    if (!jb.ok || jb.buf == NULL) {
        free(jb.buf);
        return NULL;
    }
    return jb.buf;
}



/* ========================= 顶层对象解析 ========================= */
/*
 * 根对象是 "4 位编号 -> 课程对象" 的扁平结构，逐门课解析可以避免
 * 一次性为整份课表建立 cJSON 树（ESP32 无 PSRAM，大课表容易内存不足）。
 */

static const char *timetable_skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

/* p 指向 '{'，返回匹配的 '}' 之后的位置；字符串内的括号不计入 */
static const char *timetable_match_object(const char *p, const char *end)
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (; p < end; p++) {
        char c = *p;

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                return p + 1;
            }
        }
    }
    return NULL;
}

static bool timetable_parse_root(const char *json, timetable_course_t *courses,
                                 uint8_t *out_count, char *err, size_t err_size)
{
    const char *p = json;
    const char *end = json + strlen(json);
    size_t parsed = 0;

    /* 容忍 UTF-8 BOM（部分 Windows 工具导出的 JSON 会带 BOM） */
    if ((size_t)(end - p) >= 3 && (uint8_t)p[0] == 0xEF &&
        (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) {
        p += 3;
    }

    p = timetable_skip_ws(p, end);
    if (p >= end || *p != '{') {
        timetable_fail(err, err_size, ESP_ERR_INVALID_ARG,
                       "课程表根节点应为对象（4 位数字编号 -> 课程）");
        return false;
    }

    p = timetable_skip_ws(p + 1, end);
    if (p < end && *p == '}') {
        timetable_fail(err, err_size, ESP_ERR_INVALID_ARG,
                       "课程表不能为空，至少要有 1 门课程");
        return false;
    }
    if (p >= end) {
        timetable_fail(err, err_size, ESP_ERR_INVALID_ARG, "JSON 格式错误：根对象未闭合");
        return false;
    }

    for (;;) {
        const char *key_start;
        const char *key_end;
        const char *value_start;
        const char *value_end;
        char code[TIMETABLE_CODE_LEN];
        size_t key_len;
        cJSON *obj;

        if (p >= end || *p != '"') {
            timetable_fail(err, err_size, ESP_ERR_INVALID_ARG,
                           "根对象的键必须是 4 位数字字符串");
            return false;
        }

        key_start = p + 1;
        key_end = key_start;
        while (key_end < end && *key_end != '"') {
            if (*key_end == '\\') {
                timetable_fail(err, err_size, ESP_ERR_INVALID_ARG,
                               "根对象的键必须是 4 位数字字符串");
                return false;
            }
            key_end++;
        }
        if (key_end >= end) {
            timetable_fail(err, err_size, ESP_ERR_INVALID_ARG,
                           "JSON 格式错误：字符串未闭合");
            return false;
        }

        key_len = (size_t)(key_end - key_start);
        if (key_len != TIMETABLE_CODE_LEN - 1) {
            timetable_fail(err, err_size, ESP_ERR_INVALID_ARG,
                           "根对象的键必须是 4 位数字字符串");
            return false;
        }
        memcpy(code, key_start, key_len);
        code[key_len] = '\0';
        if (!timetable_code_ok(code)) {
            timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "课程编号 \"%s\" 必须是 4 位数字", code);
            return false;
        }
        for (size_t i = 0; i < parsed; i++) {
            if (strcmp(courses[i].code, code) == 0) {
                timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                                "课程编号 %s 重复", code);
                return false;
            }
        }
        if (parsed >= TIMETABLE_COURSE_MAX) {
            timetable_failf(err, err_size, ESP_ERR_INVALID_SIZE,
                            "课程表最多支持 %d 门课程", TIMETABLE_COURSE_MAX);
            return false;
        }

        p = timetable_skip_ws(key_end + 1, end);
        if (p >= end || *p != ':') {
            timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "课程 %s 缺少冒号", code);
            return false;
        }

        p = timetable_skip_ws(p + 1, end);
        if (p >= end || *p != '{') {
            timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "课程 %s 的内容应为对象", code);
            return false;
        }
        value_start = p;
        value_end = timetable_match_object(p, end);
        if (value_end == NULL) {
            timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "JSON 格式错误：课程 %s 的对象未闭合", code);
            return false;
        }

        obj = cJSON_ParseWithLength(value_start, (size_t)(value_end - value_start));
        if (obj == NULL) {
            timetable_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "课程 %s 的 JSON 格式错误", code);
            return false;
        }
        if (!timetable_parse_course(code, obj, &courses[parsed], err, err_size)) {
            cJSON_Delete(obj);
            return false;
        }
        cJSON_Delete(obj);
        parsed++;

        p = timetable_skip_ws(value_end, end);
        if (p >= end) {
            timetable_fail(err, err_size, ESP_ERR_INVALID_ARG,
                           "JSON 格式错误：根对象未闭合");
            return false;
        }
        if (*p == ',') {
            p = timetable_skip_ws(p + 1, end);
            continue;
        }
        if (*p == '}') {
            p++;
            break;
        }
        timetable_fail(err, err_size, ESP_ERR_INVALID_ARG,
                       "JSON 格式错误：课程之间缺少逗号");
        return false;
    }

    p = timetable_skip_ws(p, end);
    if (p != end) {
        timetable_fail(err, err_size, ESP_ERR_INVALID_ARG,
                       "JSON 格式错误：根对象后有多余内容");
        return false;
    }

    *out_count = (uint8_t)parsed;
    return true;
}

/* ========================= JSON 导入 ========================= */

esp_err_t Timetable_ApplyJson(const char *json, char *err, size_t err_size)
{
    timetable_course_t *courses = NULL;
    uint8_t count = 0;
    esp_err_t ret = ESP_OK;

    if (json == NULL) {
        return timetable_fail(err, err_size, ESP_ERR_INVALID_ARG, "请求内容为空");
    }

    courses = calloc(TIMETABLE_COURSE_MAX, sizeof(timetable_course_t));
    if (courses == NULL) {
        return timetable_fail(err, err_size, ESP_ERR_NO_MEM, "内存不足");
    }

    if (!timetable_parse_root(json, courses, &count, err, err_size)) {
        ret = ESP_ERR_INVALID_ARG;
        goto out;
    }

    /*
     * 先写 NVS，成功后再替换内存数据：
     * 校验或写入失败时，当前正在使用的课程表保持不变。
     */
    ret = timetable_save_records(courses, count);
    if (ret != ESP_OK) {
        timetable_failf(err, err_size, ret, "写入 NVS 失败（%s），内存中的课程表未改变",
                        esp_err_to_name(ret));
        goto out;
    }

    memset(Timetable_Courses, 0, sizeof(Timetable_Courses));
    memcpy(Timetable_Courses, courses, (size_t)count * sizeof(timetable_course_t));
    Timetable_Count = count;

    if (err != NULL && err_size > 0) {
        snprintf(err, err_size, "课程表已导入：%u 门课程，%u 个上课时段",
                 (unsigned)Timetable_Count, (unsigned)Timetable_SessionCount());
    }

out:
    free(courses);
    return ret;
}

