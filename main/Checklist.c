/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-11-26 23:10:00
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-26 23:10:00
 * @FilePath: \epd\main\Checklist.c
 * @Description: 清单数据结构实现：默认值 / NVS 持久化 / JSON 校验与序列化。
 *
 * JSON 结构：
 * {
 *     "items": [
 *         { "text": "带钥匙", "done": true },
 *         { "text": "",       "done": false }
 *     ],
 *     "done": 1,
 *     "total": 4,
 *     "capacity": 8
 * }
 *
 * 说明：
 *   - items 最多 8 项，可以少于 8 项，没有提交的项保持原值；
 *   - text 为空表示删除该项；done 支持 true/false 或 0/1；
 *   - text 必须是 UTF-8，且每个字符都能在 GB2312（字库芯片）里找到，
 *     长度不超过 CHECKLIST_ITEM_TEXT_LEN 字节（约 13 个汉字）。
 */
#include "Checklist.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "gb2312_map.h"
#include "nvs.h"

static const char *TAG = "CHECKLIST";

/* NVS 存储位置：与课表（course_store）、RTC（rtc_storage）分开 */
#define CHECKLIST_NVS_NAMESPACE   "list_store"
#define CHECKLIST_NVS_KEY         "items"
#define CHECKLIST_NVS_MAGIC       0x4C535431u   /* "LST1"，用于识别记录版本/损坏 */

/* NVS 中保存的原始数据 */
typedef struct {
    uint32_t magic;
    uint8_t  count;
    uint8_t  done[CHECKLIST_ITEM_COUNT];
    char     text[CHECKLIST_ITEM_COUNT][CHECKLIST_ITEM_TEXT_LEN + 1];
} checklist_nvs_blob_t;

/* ========================= 清单全局数据（GUI 直接使用） ========================= */

checklist_item_t Checklist_Items[CHECKLIST_ITEM_COUNT];

/* 首次上电的演示数据（可在网页里随意改） */
static const char *const s_default_text[CHECKLIST_ITEM_COUNT] = {
    "带钥匙",
    "带校园卡",
    "完成课程作业",
    "背 20 个单词",
    "",
    "",
    "",
    "",
};

static const uint8_t s_default_done[CHECKLIST_ITEM_COUNT] = { 1, 0, 0, 0, 0, 0, 0, 0 };

/* ========================= 内部工具 ========================= */

static esp_err_t checklist_fail(char *err, size_t err_size, esp_err_t code, const char *msg)
{
    if (err != NULL && err_size > 0) {
        snprintf(err, err_size, "%s", msg);
    }
    return code;
}

static esp_err_t checklist_failf(char *err, size_t err_size, esp_err_t code,
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

/*
 * 校验一项文字：长度、UTF-8 合法性、控制字符、字库可显示性。
 * index 从 0 开始，只用于错误提示。
 */
static bool checklist_text_valid(const char *text, char *err, size_t err_size, int index)
{
    const uint8_t *p;
    const uint8_t *end;

    if (text == NULL) {
        return true;
    }

    p = (const uint8_t *)text;
    end = p + strlen(text);

    if ((size_t)(end - p) > CHECKLIST_ITEM_TEXT_LEN) {
        checklist_failf(err, err_size, ESP_ERR_INVALID_SIZE,
                        "第 %d 项文字太长（最多 %d 字节，约 13 个汉字）",
                        index + 1, CHECKLIST_ITEM_TEXT_LEN);
        return false;
    }

    while (p < end) {
        int32_t cp = Gb2312_Utf8Next(&p, end);
        uint16_t gb_code;

        if (cp < 0) {
            checklist_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "第 %d 项不是合法的 UTF-8 文本", index + 1);
            return false;
        }
        if (cp < 0x20 || cp == 0x7F) {
            checklist_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "第 %d 项含有控制字符", index + 1);
            return false;
        }
        if (cp >= 0x80 && !Gb2312_FromUnicode((uint32_t)cp, &gb_code)) {
            checklist_failf(err, err_size, ESP_ERR_INVALID_ARG,
                            "第 %d 项含有字库不支持的字符（仅支持 GB2312 汉字/英文/数字）",
                            index + 1);
            return false;
        }
    }

    return true;
}

/* ========================= 清单接口 ========================= */

void Checklist_Default(void)
{
    memset(Checklist_Items, 0, sizeof(Checklist_Items));

    for (size_t i = 0; i < CHECKLIST_ITEM_COUNT; i++) {
        strlcpy(Checklist_Items[i].text, s_default_text[i], sizeof(Checklist_Items[i].text));
        Checklist_Items[i].done = s_default_done[i];
    }
}

uint8_t Checklist_TotalCount(void)
{
    uint8_t count = 0;

    for (size_t i = 0; i < CHECKLIST_ITEM_COUNT; i++) {
        if (Checklist_Items[i].text[0] != '\0') {
            count++;
        }
    }
    return count;
}

uint8_t Checklist_DoneCount(void)
{
    uint8_t count = 0;

    for (size_t i = 0; i < CHECKLIST_ITEM_COUNT; i++) {
        if (Checklist_Items[i].text[0] != '\0' && Checklist_Items[i].done != 0) {
            count++;
        }
    }
    return count;
}

size_t Checklist_Utf8ToGb2312(const char *utf8, char *out, size_t out_size)
{
    return Gb2312_FromUtf8(utf8, out, out_size);
}

esp_err_t Checklist_Save(void)
{
    checklist_nvs_blob_t blob;
    nvs_handle_t handle;
    esp_err_t ret;

    memset(&blob, 0, sizeof(blob));
    blob.magic = CHECKLIST_NVS_MAGIC;

    for (size_t i = 0; i < CHECKLIST_ITEM_COUNT; i++) {
        if (Checklist_Items[i].text[0] == '\0') {
            continue;
        }
        strlcpy(blob.text[i], Checklist_Items[i].text, sizeof(blob.text[i]));
        blob.done[i] = (Checklist_Items[i].done != 0) ? 1 : 0;
        blob.count++;
    }

    ret = nvs_open(CHECKLIST_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败：%s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(handle, CHECKLIST_NVS_KEY, &blob, sizeof(blob));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "清单写入 NVS 失败：%s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "清单已保存：%d 项，已完成 %d 项",
                 (int)Checklist_TotalCount(), (int)Checklist_DoneCount());
    }
    return ret;
}

esp_err_t Checklist_Load(void)
{
    checklist_nvs_blob_t blob;
    checklist_item_t items[CHECKLIST_ITEM_COUNT];
    nvs_handle_t handle;
    size_t len = sizeof(blob);
    esp_err_t ret;

    ret = nvs_open(CHECKLIST_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        /* 首次上电没有记录，属于正常情况 */
        return ret;
    }

    ret = nvs_get_blob(handle, CHECKLIST_NVS_KEY, &blob, &len);
    nvs_close(handle);
    if (ret != ESP_OK) {
        return ret;
    }

    if (len != sizeof(blob) || blob.magic != CHECKLIST_NVS_MAGIC ||
        blob.count > CHECKLIST_ITEM_COUNT) {
        ESP_LOGW(TAG, "清单记录无效（len=%u），忽略", (unsigned)len);
        return ESP_ERR_INVALID_STATE;
    }

    memset(items, 0, sizeof(items));
    for (size_t i = 0; i < CHECKLIST_ITEM_COUNT; i++) {
        blob.text[i][CHECKLIST_ITEM_TEXT_LEN] = '\0';
        if (!checklist_text_valid(blob.text[i], NULL, 0, (int)i)) {
            ESP_LOGW(TAG, "第 %d 项无效，忽略 NVS 记录", (int)i + 1);
            return ESP_ERR_INVALID_STATE;
        }
        strlcpy(items[i].text, blob.text[i], sizeof(items[i].text));
        items[i].done = (blob.done[i] != 0) ? 1 : 0;
    }

    memcpy(Checklist_Items, items, sizeof(items));
    return ESP_OK;
}

esp_err_t Checklist_Init(void)
{
    esp_err_t ret;

    Checklist_Default();

    ret = Checklist_Load();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "未载入已保存的清单（%s），使用默认值", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "清单载入成功：%d 项，已完成 %d 项",
                 (int)Checklist_TotalCount(), (int)Checklist_DoneCount());
    }
    return ret;
}

char *Checklist_ToJson(void)
{
    cJSON *root;
    cJSON *items;
    char *out;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "done", (double)Checklist_DoneCount());
    cJSON_AddNumberToObject(root, "total", (double)Checklist_TotalCount());
    cJSON_AddNumberToObject(root, "capacity", (double)CHECKLIST_ITEM_COUNT);

    items = cJSON_AddArrayToObject(root, "items");
    if (items == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    for (size_t i = 0; i < CHECKLIST_ITEM_COUNT; i++) {
        cJSON *item = cJSON_CreateObject();

        if (item == NULL) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddStringToObject(item, "text", Checklist_Items[i].text);
        cJSON_AddBoolToObject(item, "done", Checklist_Items[i].done != 0);
        cJSON_AddItemToArray(items, item);
    }

    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return out;
}

esp_err_t Checklist_ApplyJson(const char *json, char *err, size_t err_size)
{
    cJSON *root;
    const cJSON *array;
    const cJSON *entry;
    checklist_item_t items[CHECKLIST_ITEM_COUNT];
    esp_err_t ret = ESP_OK;
    int index = 0;

    if (json == NULL) {
        return checklist_fail(err, err_size, ESP_ERR_INVALID_ARG, "请求内容为空");
    }

    root = cJSON_Parse(json);
    if (root == NULL) {
        return checklist_fail(err, err_size, ESP_ERR_INVALID_ARG, "JSON 格式错误");
    }

    array = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(array)) {
        ret = checklist_fail(err, err_size, ESP_ERR_INVALID_ARG, "缺少 items 数组");
        goto out;
    }

    /* 未提交的项沿用当前值 */
    memcpy(items, Checklist_Items, sizeof(items));

    cJSON_ArrayForEach(entry, array) {
        const cJSON *text_item;
        const cJSON *done_item;
        char    text[CHECKLIST_ITEM_TEXT_LEN + 1];
        uint8_t done;

        /* 超出容量的项直接忽略，保证旧版网页也能正常工作 */
        if (index >= CHECKLIST_ITEM_COUNT) {
            break;
        }
        if (!cJSON_IsObject(entry)) {
            ret = checklist_failf(err, err_size, ESP_ERR_INVALID_ARG,
                                  "items 的第 %d 个元素应为对象", index + 1);
            goto out;
        }

        strlcpy(text, items[index].text, sizeof(text));
        done = items[index].done;

        text_item = cJSON_GetObjectItemCaseSensitive(entry, "text");
        if (text_item != NULL) {
            if (!cJSON_IsString(text_item)) {
                ret = checklist_failf(err, err_size, ESP_ERR_INVALID_ARG,
                                      "第 %d 项的 text 应为字符串", index + 1);
                goto out;
            }
            if (!checklist_text_valid(text_item->valuestring, err, err_size, index)) {
                ret = ESP_ERR_INVALID_ARG;
                goto out;
            }
            memset(text, 0, sizeof(text));
            strlcpy(text, text_item->valuestring, sizeof(text));
        }

        done_item = cJSON_GetObjectItemCaseSensitive(entry, "done");
        if (done_item != NULL) {
            if (cJSON_IsBool(done_item)) {
                done = cJSON_IsTrue(done_item) ? 1 : 0;
            } else if (cJSON_IsNumber(done_item)) {
                done = (done_item->valueint != 0) ? 1 : 0;
            } else {
                ret = checklist_failf(err, err_size, ESP_ERR_INVALID_ARG,
                                      "第 %d 项的 done 应为 true/false", index + 1);
                goto out;
            }
        }

        strlcpy(items[index].text, text, sizeof(items[index].text));
        items[index].done = done;
        index++;
    }

    /* 全部校验通过后再写全局变量，避免出现半更新的清单 */
    memcpy(Checklist_Items, items, sizeof(items));

    ret = Checklist_Save();
    if (ret != ESP_OK) {
        ret = checklist_fail(err, err_size, ret, "已应用到内存，但写入 NVS 失败");
        goto out;
    }

    if (err != NULL && err_size > 0) {
        snprintf(err, err_size, "清单已更新（%d 项，已完成 %d 项）",
                 (int)Checklist_TotalCount(), (int)Checklist_DoneCount());
    }

out:
    cJSON_Delete(root);
    return ret;
}
