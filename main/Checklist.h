/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-11-26 23:10:00
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-26 23:10:00
 * @FilePath: \epd\main\Checklist.h
 * @Description: 清单（待办清单）数据结构与持久化接口。
 *               GUI 直接读 Checklist_Items 绘制"清单"页；
 *               网页（"清单"标签页）通过 Checklist_ApplyJson() 更新并写入 NVS。
 *
 * JSON 结构：
 * {
 *     "items": [
 *         { "text": "带钥匙", "done": true },
 *         { "text": "交作业", "done": false }
 *     ],
 *     "done": 1,
 *     "total": 2,
 *     "capacity": 8
 * }
 */
#ifndef __CHECKLIST_H__
#define __CHECKLIST_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 清单容量：与墨水屏"清单"页可见行数一致（8 行 x 12 像素） */
#define CHECKLIST_ITEM_COUNT        8
/* 单项文字上限：UTF-8 字节数（13 个 GB2312 汉字约 39 字节），不含 '\0' */
#define CHECKLIST_ITEM_TEXT_LEN     40

typedef struct {
    char    text[CHECKLIST_ITEM_TEXT_LEN + 1];  /* UTF-8；空串表示该项不存在 */
    uint8_t done;                               /* 0=未完成 1=已完成 */
} checklist_item_t;

/* 定义在 Checklist.c，GUI 直接读取 */
extern checklist_item_t Checklist_Items[CHECKLIST_ITEM_COUNT];

/* ========================= 清单接口 ========================= */

/* 恢复为演示默认值（不写 NVS） */
void Checklist_Default(void);

/* 上电调用：先置默认值，再从 NVS 载入；失败时保持默认值（记录告警） */
esp_err_t Checklist_Init(void);

/* 从 NVS 载入清单 */
esp_err_t Checklist_Load(void);

/* 把当前内存中的清单写入 NVS */
esp_err_t Checklist_Save(void);

/*
 * 解析网页提交的 JSON 并应用（全部校验通过才写入全局变量），成功后自动保存 NVS。
 * items 数组最多 CHECKLIST_ITEM_COUNT 项，未提交的项保持原值；
 * text 为空串表示删除该项。
 * err/err_size：失败时返回中文错误说明（可为 NULL）。
 */
esp_err_t Checklist_ApplyJson(const char *json, char *err, size_t err_size);

/*
 * 把当前清单序列化成 JSON 字符串（cJSON_PrintUnformatted 生成）。
 * 返回值需用 cJSON_free() 释放，失败返回 NULL。
 */
char *Checklist_ToJson(void);

/*
 * 把 UTF-8 文本转换成字库芯片需要的 GB2312 编码，供墨水屏绘制使用。
 * out 容量不足时按整字截断；返回写入的字节数（不含 '\0'）。
 * 无法转换的字符写成 '?'（正常保存时 Checklist_ApplyJson() 已做校验）。
 */
size_t Checklist_Utf8ToGb2312(const char *utf8, char *out, size_t out_size);

/* 非空项总数 / 其中已完成的数量（进度显示用） */
uint8_t Checklist_TotalCount(void);
uint8_t Checklist_DoneCount(void);

#ifdef __cplusplus
}
#endif

#endif /* __CHECKLIST_H__ */
