/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-11-27 10:00:00
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-27 10:00:00
 * @FilePath: \epd\main\Timetable.h
 * @Description: 完整课程表（JSON 导入）数据结构与持久化接口。
 *
 * 网页 / API 导入的 JSON 结构（兼容用户提供的 JSON Schema）：
 * {
 *     "0101": {
 *         "CourseName": "高等数学",
 *         "Day": 1,
 *         "CourseDuration": [ { "Start": 1, "End": 2 } ],
 *         "CourseSessions": [
 *             { "Week": 1, "Start": 1, "End": 2 },
 *             { "Week": 2, "Start": 1, "End": 2 }
 *         ],
 *         "Classroom": "A102",
 *         "Teacher": "张老师",
 *         "StartTime": "08:00",       // 可选，HH:MM
 *         "EndTime": "09:40"          // 可选，HH:MM
 *     }
 * }
 *
 * 说明：
 *   - 根对象每个键必须是 4 位数字（课程编号，原样保存，不解释含义）；
 *   - 每门课必须包含 CourseName / CourseDuration / CourseSessions /
 *     Classroom / Teacher 五个字段，可选 Day / StartTime / EndTime；
 *   - Day 缺省时按 0（星期未设置）保存，旧版 JSON 仍可导入；
 *   - 每门课只描述一个星期，同一课程多天上课请拆成多门课；
 *   - CourseDuration 可以为空数组，CourseSessions 至少 1 项；
 *   - 受固件资源限制：最多 TIMETABLE_COURSE_MAX 门课，每门最多
 *     TIMETABLE_DURATION_MAX 个 CourseDuration、TIMETABLE_SESSION_MAX 个
 *     CourseSessions；节次 1~TIMETABLE_PERIOD_MAX，周次 1~53，星期 1~7。
 *
 * 数据保存在 NVS 命名空间 timetable / 键 courses 中，记录头 "TMT3"。
 * 上电时如读到旧的 "TMT1" / "TMT2" 记录会自动迁移，旧课程不会丢失；
 * 旧记录没有星期信息，Day 迁移为 0，不参与“当前/下一节”筛选。
 */
#ifndef __TIMETABLE_H__
#define __TIMETABLE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= 资源上限 ========================= */

/* 最多保存的课程门数（16 门，覆盖常见一学期课程） */
#define TIMETABLE_COURSE_MAX            16
/* 每门课最多保存的 CourseDuration 项数 */
#define TIMETABLE_DURATION_MAX          8
/* 每门课最多保存的 CourseSessions 项数（约 16 周 x 4 节） */
#define TIMETABLE_SESSION_MAX           64

/* 课程编号：4 位数字 + '\0' */
#define TIMETABLE_CODE_LEN              5
/* 课程名称：UTF-8 字节数上限（约 13 个汉字），不含 '\0' */
#define TIMETABLE_NAME_LEN              40
/* 教室 / 教师：UTF-8 字节数上限，不含 '\0' */
#define TIMETABLE_CLASSROOM_LEN         24
#define TIMETABLE_TEACHER_LEN           24
/* 上下课时间："HH:MM" 5 字符 + '\0'，可为空串 */
#define TIMETABLE_TIME_LEN              5

/* 节次 / 周次范围（Schema 只要求 >= 1；这里按墨水屏和存储空间限制） */
#define TIMETABLE_PERIOD_MIN            1
#define TIMETABLE_PERIOD_MAX            99
#define TIMETABLE_WEEK_MIN              1
#define TIMETABLE_WEEK_MAX              53
/* 星期：1=周一 ... 7=周日；0 仅用于兼容没有星期的旧记录 */
#define TIMETABLE_DAY_MIN               1
#define TIMETABLE_DAY_MAX               7

/* ========================= 数据结构 ========================= */

/* CourseDuration 的一项：Start / End 为节次 */
typedef struct {
    uint8_t start;
    uint8_t end;
} timetable_duration_t;

/* CourseSessions 的一项：Week 为周次，Start / End 为节次 */
typedef struct {
    uint8_t week;
    uint8_t start;
    uint8_t end;
} timetable_session_t;

typedef struct {
    char     code[TIMETABLE_CODE_LEN];          /* 4 位数字课程编号 */
    char     name[TIMETABLE_NAME_LEN + 1];      /* UTF-8 */
    char     classroom[TIMETABLE_CLASSROOM_LEN + 1]; /* UTF-8，可为空串 */
    char     teacher[TIMETABLE_TEACHER_LEN + 1];     /* UTF-8，可为空串 */
    char     start_time[TIMETABLE_TIME_LEN + 1];     /* "HH:MM"，可为空串 */
    char     end_time[TIMETABLE_TIME_LEN + 1];       /* "HH:MM"，可为空串 */
    uint8_t  duration_count;
    timetable_duration_t duration[TIMETABLE_DURATION_MAX];
    uint8_t  session_count;                     /* 至少 1 项 */
    timetable_session_t sessions[TIMETABLE_SESSION_MAX];
    uint8_t  day;                               /* 1=周一 ... 7=周日，0=未设置（旧数据） */
} timetable_course_t;

/* 定义在 Timetable.c，课程页按 RTC 时间从这里筛选当前 / 下一节课程 */
extern timetable_course_t Timetable_Courses[TIMETABLE_COURSE_MAX];
extern uint8_t            Timetable_Count;

/* ========================= 课表接口 ========================= */

/* 清空内存中的课程表（不写 NVS） */
void Timetable_Default(void);

/* 上电调用：先清空，再从 NVS 载入；失败时保持空表（记录告警） */
esp_err_t Timetable_Init(void);

/* 从 NVS 载入课程表 */
esp_err_t Timetable_Load(void);

/* 把当前内存中的课程表写入 NVS */
esp_err_t Timetable_Save(void);

/* 清空课程表并写入 NVS（网页“清空课程表”/删除最后一门课使用） */
esp_err_t Timetable_Clear(void);

/*
 * 解析网页提交的完整课程表 JSON 并应用：
 * 先完整校验，再写入 NVS，全部成功后才替换内存中的课程表；
 * 校验或 NVS 写入失败时，内存中的课程表保持不变。
 * err/err_size：失败时返回中文错误说明（可为 NULL）。
 */
esp_err_t Timetable_ApplyJson(const char *json, char *err, size_t err_size);

/*
 * 把当前课程表序列化成与导入格式一致的 JSON 字符串。
 * 空表返回 "{}"。返回值需用 cJSON_free() 释放，失败返回 NULL。
 */
char *Timetable_ToJson(void);

/* 上课时段（CourseSessions）总数，便于日志与网页提示 */
uint32_t Timetable_SessionCount(void);

#ifdef __cplusplus
}
#endif

#endif /* __TIMETABLE_H__ */
