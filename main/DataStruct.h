/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-11-02 22:35:33
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-26 22:30:00
 * @FilePath: \epd\main\DataStruct.h
 * @Description: 课表数据结构与持久化接口。
 *               GUI 直接读这些全局变量绘制课程表页；
 *               HTTP（网页“设置课程表”）通过 Course_ApplyJson() 更新并写入 NVS。
 *
 * JSON 结构（与 DataStruct.c 中的示例保持一致）：
 * {
 *     "Course_Lesson_Status": 0,          // 0=无课 1=准备上课 2=正在上课
 *     "Course_Lesson_Type": 0,            // 0=理论课 1=实践课
 *     "Course_Lesson_Postion": 0,         // 0=教学楼 1=科技楼
 *     "Course_Lesson_Postion_Str": "X000",// 4 位教室编号
 *     "Course_Lesson_Time": "00:00~00:00",// 上课时间 HH:MM~HH:MM
 *     "Semester_Week_Now": 0              // 学期第几周
 * }
 */
#ifndef __DATASTRUCT_H__
#define __DATASTRUCT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= 课程状态 ========================= */

typedef enum {
    COURSE_LESSON_NULL = 0,     /* 今日无课 */
    COURSE_LESSON_READY,        /* 准备上课 */
    COURSE_LESSON_NOW           /* 正在上课 */
} course_lesson_status_t;

/* ========================= 课程类型 ========================= */

typedef enum {
    COURSE_LESSON_THEORETICAL = 0,  /* 理论课 */
    COURSE_LESSON_PRACTICAL         /* 实践课 */
} course_lesson_type_t;

/* ========================= 上课地点 ========================= */

typedef enum {
    COURSE_LESSON_TEACHING = 0,     /* 教学楼 */
    COURSE_LESSON_TECHNOLOGY        /* 科技楼 */
} course_lesson_postion_t;

/* 教室编号：4 位可见 ASCII + '\0'，GUI 只使用 [1..3] 作为房间号 */
#define COURSE_LESSON_POSITION_STR_LEN   5
/* 上课时间："HH:MM~HH:MM" 最长 11 字符 + '\0' */
#define COURSE_LESSON_TIME_LEN           12
/* 学期周次范围 */
#define COURSE_SEMESTER_WEEK_MIN         1
#define COURSE_SEMESTER_WEEK_MAX         30

/* ========================= 课表全局数据 ========================= */
/* 定义在 DataStruct.c，GUI 直接使用，修改后调用 GuiUpdate() 重绘 */

extern course_lesson_status_t  Course_Lesson_Status;
extern course_lesson_type_t    Course_Lesson_Type;
extern course_lesson_postion_t Course_Lesson_Postion;
extern char                    Course_Lesson_Postion_Str[COURSE_LESSON_POSITION_STR_LEN];
extern char                    Course_Lesson_Time[COURSE_LESSON_TIME_LEN];
extern uint8_t                 Semester_Week_Now;

/* ========================= 课表接口 ========================= */

/* 恢复为演示默认值（不写 NVS） */
void Course_Default(void);

/* 上电调用：先置默认值，再从 NVS 载入，失败时保持默认值（记录告警） */
esp_err_t Course_Init(void);

/* 从 NVS 载入课表 */
esp_err_t Course_Load(void);

/* 把当前内存中的课表写入 NVS */
esp_err_t Course_Save(void);

/*
 * 解析网页提交的 JSON 并应用（全部校验通过才写入全局变量），成功后自动保存 NVS。
 * 字段可以只提交一部分，未提交的字段保持原值。
 * err/err_size：失败时返回中文错误说明（可为 NULL）。
 */
esp_err_t Course_ApplyJson(const char *json, char *err, size_t err_size);

/*
 * 把当前课表序列化成 JSON 字符串（cJSON_PrintUnformatted 生成）。
 * 返回值需用 cJSON_free() 释放，失败返回 NULL。
 */
char *Course_ToJson(void);

/* 校验 "HH:MM~HH:MM" 格式与取值范围（结束时间必须晚于开始时间） */
bool Course_IsTimeValid(const char *time_str);

#ifdef __cplusplus
}
#endif

#endif /* __DATASTRUCT_H__ */
