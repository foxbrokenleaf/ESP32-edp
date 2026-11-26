/*
 * @Description: 整机上电自检实现（NVS / 字库 / EPD / 语音 / 按键 / RTC / 内存）
 *
 * 逐项独立判断，互不影响；结果同时给串口和墨水屏。
 * 每一项失败时 detail 里都会给出可定位的信息（错误码 / 引脚号 / 电平）。
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

#include "app_selftest.h"
#include "EPD.h"
#include "GT30L32S4W.h"
#include "bsp_GT30L32S4W.h"
#include "WT588F.h"
#include "bsp_rtc.h"

static const char *TAG = "SELFTEST";

/* ========================= 内部工具 ========================= */

/**
 * @brief  追加一条结果；报表已满时丢弃并告警
 * @param  fmt detail 的 printf 格式，可为 NULL
 */
static app_selftest_item_t *selftest_add(app_selftest_report_t *rep, const char *name,
                                        app_selftest_state_t state, const char *fmt, ...)
{
    if (rep->count >= APP_SELFTEST_MAX_ITEMS) {
        ESP_LOGW(TAG, "report full, drop item %s", name);
        return NULL;
    }

    app_selftest_item_t *item = &rep->item[rep->count++];
    memset(item, 0, sizeof(*item));
    strncpy(item->name, name, APP_SELFTEST_NAME_LEN - 1);
    item->state = state;

    if (fmt != NULL) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(item->detail, APP_SELFTEST_DETAIL_LEN, fmt, args);
        va_end(args);
    }
    return item;
}

/* ========================= 单项自检 ========================= */

/* NVS：分区写满 / 版本升级时 nvs_flash_init 会返回特定错误码 */
static void selftest_nvs(app_selftest_report_t *rep)
{
#if !APP_SELFTEST_TEST_NVS
    selftest_add(rep, "NVS", APP_SELFTEST_STATE_SKIP, "disabled");
#else
    esp_err_t err = nvs_flash_init();

    if (err == ESP_OK) {
        selftest_add(rep, "NVS", APP_SELFTEST_STATE_PASS, "ok");
    } else if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        selftest_add(rep, "NVS", APP_SELFTEST_STATE_FAIL, "need erase");
    } else {
        selftest_add(rep, "NVS", APP_SELFTEST_STATE_FAIL, "%s", esp_err_to_name(err));
    }
#endif
}

/* 字库：初始化 + 读回 'A' 的 8x16 字模与参考数据比对 */
static void selftest_font(app_selftest_report_t *rep)
{
#if !APP_SELFTEST_TEST_FONT
    selftest_add(rep, "FONT", APP_SELFTEST_STATE_SKIP, "disabled");
#else
    esp_err_t err = GT30L32S4W_Init();
    if (err == ESP_OK) {
        err = GT30L32S4W_SelfTest();
    }

    if (err == ESP_OK) {
        selftest_add(rep, "FONT", APP_SELFTEST_STATE_PASS, "clk26 si25 cs23");
    } else if (!bsp_gt30l32s4w_is_ready()) {
        selftest_add(rep, "FONT", APP_SELFTEST_STATE_FAIL, "init: %s", esp_err_to_name(err));
    } else {
        selftest_add(rep, "FONT", APP_SELFTEST_STATE_FAIL, "read back bad");
    }
#endif
}

/*
 * EPD：硬件上没有读回通路，只能人眼确认。
 * 结果页（app_selftest_draw）就是它的自检画面：
 * 边框/文字清晰、无整屏反色、无残影即通过。
 */
static void selftest_epd(app_selftest_report_t *rep)
{
#if !APP_SELFTEST_TEST_EPD
    selftest_add(rep, "EPD", APP_SELFTEST_STATE_SKIP, "disabled");
#else
    selftest_add(rep, "EPD", APP_SELFTEST_STATE_MANUAL, "look at page");
#endif
}

/*
 * 语音：WT588F 自检（F9 工程信息回读）+ 播一段 + 采样 BUSY。
 *
 * 第一步最硬：发 F9 00（单字节）/ FFF9 0000（双字节），芯片把主控程序编号
 * 以 9600 从 BUSY(PIN 1) 回传。这一条能同时证明供电、发码通道（DATA/CLK + 门控）、
 * BUSY 回传全都通；读不到就是芯片没在听，再播也白搭。
 * 第二步看 BUSY 变忙：说明芯片收到播放命令并在播（此时无声则查功放/喇叭）。
 */
static void selftest_voice(app_selftest_report_t *rep)
{
#if !APP_SELFTEST_TEST_VOICE
    selftest_add(rep, "VOICE", APP_SELFTEST_STATE_SKIP, "disabled");
#else
    esp_err_t err = WT588F_Init();
    if (err != ESP_OK) {
        selftest_add(rep, "VOICE", APP_SELFTEST_STATE_FAIL, "init: %s", esp_err_to_name(err));
        return;
    }

    char prog[12] = "?";

#if APP_SELFTEST_TEST_VOICE_INFO
    /* F9 00/01/02：回读主控程序编号、工程编号、版本号（后两项由 WT588F 打进串口） */
    static wt588f_selftest_t info;
    err = WT588F_SelfTest(&info);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        selftest_add(rep, "VOICE", APP_SELFTEST_STATE_SKIP, "no busy pin");
        return;
    }
    if (err != ESP_OK) {
        selftest_add(rep, "VOICE", APP_SELFTEST_STATE_FAIL, "F9: %s", esp_err_to_name(err));
        return;
    }
    WT588F_InfoToText(info.main_program, info.main_program_len, prog, sizeof(prog));
    if (prog[0] == '\0') {
        prog[0] = '?';
        prog[1] = '\0';
    }
#endif /* APP_SELFTEST_TEST_VOICE_INFO */

    err = WT588F_Play(APP_SELFTEST_VOICE_ADDR);
    if (err != ESP_OK) {
        selftest_add(rep, "VOICE", APP_SELFTEST_STATE_FAIL, "play %u: %s",
                     (unsigned)APP_SELFTEST_VOICE_ADDR, esp_err_to_name(err));
        return;
    }

    /* 发码后 BUSY 最快 500us、最慢 65ms 才变化，按 50ms 步长采样 */
    const uint32_t step_ms = 50;
    for (uint32_t waited = 0; waited < APP_SELFTEST_VOICE_TIMEOUT_MS; waited += step_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        if (WT588F_IsBusy()) {
            selftest_add(rep, "VOICE", APP_SELFTEST_STATE_PASS, "prog %.6s,%ums",
                         prog, (unsigned)(waited + step_ms));
            return;
        }
    }

    selftest_add(rep, "VOICE", APP_SELFTEST_STATE_FAIL, "prog %.6s no busy", prog);
#endif
}

/*
 * 按键：空闲时三路都应为高。
 * 先把引脚配成纯输入（GPIO34~39 没有内部上下拉，必须靠外部上拉），
 * 若读不到高就等 APP_SELFTEST_KEY_LOW_MS 再读一次，排除抖动/刚好按下。
 */
static void selftest_key(app_selftest_report_t *rep)
{
#if !APP_SELFTEST_TEST_KEY
    selftest_add(rep, "KEY", APP_SELFTEST_STATE_SKIP, "disabled");
#else
    static const int pins[] = APP_SELFTEST_KEY_PINS;
    const int num = (int)(sizeof(pins) / sizeof(pins[0]));

    gpio_config_t cfg = {
        .pin_bit_mask = 0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < num; i++) {
        cfg.pin_bit_mask |= (1ULL << pins[i]);
    }
    gpio_config(&cfg);

    char levels[APP_SELFTEST_DETAIL_LEN];
    int  low = 0;
    int  pos = 0;

    for (int pass = 0; pass < 2; pass++) {
        low = 0;
        pos = 0;
        for (int i = 0; i < num; i++) {
            int level = gpio_get_level((gpio_num_t)pins[i]);
            if (!level) {
                low |= (1 << i);
            }
            if (pos < (int)sizeof(levels) - 1) {
                levels[pos++] = level ? '1' : '0';
            }
        }
        levels[pos] = '\0';

        if (low == 0) {
            break;
        }
        if (pass == 0) {        /* 可能刚好按下，等一会儿再确认一次 */
            vTaskDelay(pdMS_TO_TICKS(APP_SELFTEST_KEY_LOW_MS));
        }
    }

    if (low == 0) {
        selftest_add(rep, "KEY", APP_SELFTEST_STATE_PASS, "idle %s", levels);
    } else {
        for (int i = 0; i < num; i++) {
            if (low & (1 << i)) {
                selftest_add(rep, "KEY", APP_SELFTEST_STATE_FAIL, "gpio%d low", pins[i]);
                break;
            }
        }
    }
#endif
}

/* RTC：初始化 + 判断时间是否已经可用（首次上电/掉电后是 1970） */
static void selftest_rtc(app_selftest_report_t *rep)
{
#if !APP_SELFTEST_TEST_RTC
    selftest_add(rep, "RTC", APP_SELFTEST_STATE_SKIP, "disabled");
#else
    esp_err_t err = bsp_rtc_init();
    if (err != ESP_OK) {
        selftest_add(rep, "RTC", APP_SELFTEST_STATE_FAIL, "init: %s", esp_err_to_name(err));
        return;
    }

    time_t now = rtc_get_time_seconds();
    if (now < 1600000000) {     /* 2020-09-13 之前视为未同步 */
        selftest_add(rep, "RTC", APP_SELFTEST_STATE_SKIP, "no time, sntp later");
        return;
    }

    char buf[24];
    if (rtc_format_time(buf, sizeof(buf), "%m-%d %H:%M") == ESP_OK) {
        selftest_add(rep, "RTC", APP_SELFTEST_STATE_PASS, "%s", buf);
    } else {
        selftest_add(rep, "RTC", APP_SELFTEST_STATE_PASS, "ok");
    }
#endif
}

/* 内存：内部 RAM 剩余是刷屏/字模缓冲的基础，PSRAM 有就一起报出来 */
static void selftest_mem(app_selftest_report_t *rep)
{
#if !APP_SELFTEST_TEST_MEM
    selftest_add(rep, "MEM", APP_SELFTEST_STATE_SKIP, "disabled");
#else
    uint32_t int_kb   = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    uint32_t psram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    if (int_kb < APP_SELFTEST_MIN_FREE_HEAP_KB) {
        selftest_add(rep, "MEM", APP_SELFTEST_STATE_FAIL, "int %uKB low", (unsigned)int_kb);
    } else if (psram_kb > 0) {
        selftest_add(rep, "MEM", APP_SELFTEST_STATE_PASS, "int%uK psram%uK",
                     (unsigned)int_kb, (unsigned)psram_kb);
    } else {
        selftest_add(rep, "MEM", APP_SELFTEST_STATE_PASS, "int %uKB", (unsigned)int_kb);
    }
#endif
}

/* ========================= 对外接口 ========================= */

const char *app_selftest_state_str(app_selftest_state_t state)
{
    switch (state) {
    case APP_SELFTEST_STATE_PASS:   return "PASS";
    case APP_SELFTEST_STATE_FAIL:   return "FAIL";
    case APP_SELFTEST_STATE_MANUAL: return "LOOK";
    case APP_SELFTEST_STATE_SKIP:
    default:                        return "SKIP";
    }
}

esp_err_t app_selftest_run(app_selftest_report_t *report)
{
    if (report == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(report, 0, sizeof(*report));
    int64_t start_us = esp_timer_get_time();

    ESP_LOGI(TAG, "----- self test begin -----");

    selftest_nvs(report);
    selftest_font(report);
    selftest_epd(report);
    selftest_voice(report);
    selftest_key(report);
    selftest_rtc(report);
    selftest_mem(report);

    report->fail   = 0;
    report->manual = 0;
    for (uint8_t i = 0; i < report->count; i++) {
        if (report->item[i].state == APP_SELFTEST_STATE_FAIL) {
            report->fail++;
        } else if (report->item[i].state == APP_SELFTEST_STATE_MANUAL) {
            report->manual++;
        }
    }
    report->elapsed_ms = (uint32_t)((esp_timer_get_time() - start_us) / 1000);

    return (report->fail == 0) ? ESP_OK : ESP_FAIL;
}

bool app_selftest_all_pass(const app_selftest_report_t *report)
{
    return (report != NULL) && (report->fail == 0);
}

void app_selftest_print(const app_selftest_report_t *report)
{
    if (report == NULL) {
        return;
    }

    ESP_LOGI(TAG, "%-7s %-4s %s", "ITEM", "RES", "DETAIL");
    for (uint8_t i = 0; i < report->count; i++) {
        const app_selftest_item_t *item = &report->item[i];
        ESP_LOGI(TAG, "%-7s %-4s %s", item->name,
                 app_selftest_state_str(item->state), item->detail);
    }
    ESP_LOGI(TAG, "----- %u items, %u fail, %u manual, %ums -> %s -----",
             (unsigned)report->count, (unsigned)report->fail, (unsigned)report->manual,
             (unsigned)report->elapsed_ms,
             (report->fail == 0) ? "OK" : "FAILED");
}

void app_selftest_draw(const app_selftest_report_t *report)
{
    if (report == NULL) {
        return;
    }

    /* 8x16 字体：37 字符/行（296px），9 行（152px），最多 7 项 + 标题 + 分隔线 */
    char line[40];
    char summary[16];

    EPD_Clear();

    if (report->fail == 0) {
        snprintf(summary, sizeof(summary), "%u ITEMS OK", (unsigned)report->count);
    } else {
        snprintf(summary, sizeof(summary), "%u FAIL", (unsigned)report->fail);
    }
    snprintf(line, sizeof(line), "%-24s%13s", APP_SELFTEST_TITLE, summary);
    EPD_ShowString(0, 0, line, EPD_8X16, (report->fail == 0) ? 0 : 1);

    EPD_DrawLine(0, 17, EPD_WIDTH - 1, 17);

    uint8_t rows = (report->count > 7) ? 7 : report->count;
    for (uint8_t i = 0; i < rows; i++) {
        const app_selftest_item_t *item = &report->item[i];
        snprintf(line, sizeof(line), "%-6s%-5s%.23s", item->name,
                 app_selftest_state_str(item->state), item->detail);
        EPD_ShowString(0, 20 + i * 16, line, EPD_8X16, 0);
    }

    EPD_Update();
}

esp_err_t app_selftest_run_and_show(app_selftest_report_t *report)
{
    static app_selftest_report_t local;     /* 不占 main 任务栈 */
    app_selftest_report_t *rep = (report != NULL) ? report : &local;

    esp_err_t err = app_selftest_run(rep);
    app_selftest_print(rep);
    app_selftest_draw(rep);
    return err;
}


