/*
 * @Description: 整机上电自检（Power-On Self Test）
 *
 * 一次性检查本板所有外设，结果同时给串口和墨水屏：
 *
 *   NVS    nvs_flash_init() 是否正常（分区写满/版本升级会在这里暴露）
 *   FONT   GT30L32S4W 初始化 + 读回已知字模比对（接线/供电/HOLD# 问题）
 *   EPD    只能人眼确认：结果页本身就是 EPD 的自检画面
 *   VOICE  WT588F 自检：F9 00/01/02 回读主控程序编号（证明芯片在线且收发都通），
 *          再播一段采样 BUSY（区分“没收到码”和“收到码但没出声”）
 *   KEY    按键空闲电平是否为高（外部上拉是否装好、有没有卡住/短路）
 *   RTC    bsp_rtc 初始化 + 当前时间是否已同步（首次上电/SD卡时间不恢复时会是 1970）
 *   MEM    内部 RAM / PSRAM 剩余量（跑 GUI 前留个基线，方便对比是否泄漏）
 *
 * 设计要点：
 *   1. 每一项独立判断，前一项失败不影响后面的检查（NVS 除外）；
 *   2. 不假设调用者已经初始化过外设：各 BSP 的 init 都是幂等的，
 *      内部再调一次只做检查，不会重新配置引脚；
 *   3. 语音项会真的出声，量产的板子可以关掉 APP_SELFTEST_TEST_VOICE；
 *   4. 结果页只用内置 8x16 英文字模（EPD_Data.c），不依赖字库芯片，
 *      这样字库自检失败时结果页仍然能显示。
 *
 * 用法：
 *   static app_selftest_report_t report;
 *   app_selftest_run(&report);       // 只检查，不动屏幕
 *   app_selftest_print(&report);     // 串口表格
 *   app_selftest_draw(&report);      // 画结果页（会全刷一次，约 6s）
 *
 *   或者一步到位：
 *   app_selftest_run_and_show(NULL);
 *
 * 注意：EPD 自检依赖调用者已经 EPD_Init()；本模块不会重新初始化屏幕。
 */

#ifndef __APP_SELFTEST_H__
#define __APP_SELFTEST_H__

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= 配置 ========================= */

/* 单项开关：不需要的项目置 0，会记为 SKIP */
#define APP_SELFTEST_TEST_NVS           1
#define APP_SELFTEST_TEST_FONT          1
#define APP_SELFTEST_TEST_EPD           1       /* 只登记“待目检”，不额外刷屏 */
/* 语音自检：F9 工程信息回读 + 播一段观察 BUSY */
#define APP_SELFTEST_TEST_VOICE         1
#define APP_SELFTEST_TEST_VOICE_INFO    1       /* 1 = 先做 F9 00/01/02 回读，证明芯片在线 */
#define APP_SELFTEST_TEST_KEY           1
#define APP_SELFTEST_TEST_RTC           1
#define APP_SELFTEST_TEST_MEM           1

/* 内存自检：内部 RAM 剩余低于该值判为失败（EPD 缓冲区在内部 RAM 里，太低会刷屏失败） */
#define APP_SELFTEST_MIN_FREE_HEAP_KB   32

/*
 * 语音自检：播一段并观察 BUSY
 *   地址必须与语音工程一致（请与 main.c 的 APP_VOICE_TEST_ADDR 保持相同值）。
 *   双字节发码时把 WT588F_SetAddressMode() 放在本模块之前调用即可。
 */
#define APP_SELFTEST_VOICE_ADDR         1
#define APP_SELFTEST_VOICE_TIMEOUT_MS   3000    /* 等 BUSY 变忙的最长时间 */

/* 按键自检：空闲时全部为高（按键另一端接 GND，外部上拉） */
#define APP_SELFTEST_KEY_PINS           { 34, 36, 39 }
#define APP_SELFTEST_KEY_LOW_MS         300     /* 持续为低多久判定为异常 */

/* 结果页标题 */
#define APP_SELFTEST_TITLE              "SELFTEST"

/* ========================= 结果 ========================= */

typedef enum {
    APP_SELFTEST_STATE_SKIP = 0,    /* 未测试（开关关掉 / 硬件没接 / 条件不满足） */
    APP_SELFTEST_STATE_PASS,        /* 通过 */
    APP_SELFTEST_STATE_FAIL,        /* 失败 */
    APP_SELFTEST_STATE_MANUAL,      /* 需人眼或耳朵确认（EPD 只能这样） */
} app_selftest_state_t;

#define APP_SELFTEST_MAX_ITEMS          8
#define APP_SELFTEST_NAME_LEN           8
#define APP_SELFTEST_DETAIL_LEN         28

typedef struct {
    char                 name[APP_SELFTEST_NAME_LEN];      /* 项目名，如 "FONT" */
    app_selftest_state_t state;
    char                 detail[APP_SELFTEST_DETAIL_LEN];  /* 串口/屏幕上的补充信息 */
} app_selftest_item_t;

typedef struct {
    app_selftest_item_t item[APP_SELFTEST_MAX_ITEMS];
    uint8_t             count;      /* 实际检查了几项 */
    uint8_t             fail;       /* FAIL 数量 */
    uint8_t             manual;     /* MANUAL 数量 */
    uint32_t            elapsed_ms; /* 整个自检耗时 */
} app_selftest_report_t;

/* ========================= 接口 ========================= */

/**
 * @brief  执行全部自检项（不打印、不刷屏）
 * @param  report 结果输出，内部先清零；不能为 NULL
 * @return ESP_OK   没有 FAIL 项（MANUAL 不算失败）
 *         ESP_FAIL 至少一项 FAIL
 *         ESP_ERR_INVALID_ARG report 为 NULL
 *
 * @note   语音项会真的发码出声；本函数内部有 vTaskDelay，不要在中断/临界区调用。
 */
esp_err_t app_selftest_run(app_selftest_report_t *report);

/**
 * @brief  执行自检 + 串口打印 + 画结果页（最常用的一个入口）
 * @param  report 可为 NULL，内部用静态变量暂存
 * @return 同 app_selftest_run()
 *
 * @note   会全刷一次墨水屏（EPD_BUSY_CONNECTED=0 时约 6s），
 *         且要求调用前已经 EPD_Init()。
 */
esp_err_t app_selftest_run_and_show(app_selftest_report_t *report);

/**
 * @brief  把结果打印成表格（每个项目一行）
 */
void app_selftest_print(const app_selftest_report_t *report);

/**
 * @brief  把结果画到墨水屏（一次全刷，纯 ASCII，不依赖字库芯片）
 *
 * 版式（296x152 横屏，8x16 字体）：
 *   SELFTEST                    12 ITEMS OK
 *   --------------------------------------
 *   NVS    PASS ok
 *   FONT   PASS ok
 *   ...
 */
void app_selftest_draw(const app_selftest_report_t *report);

/**
 * @brief  是否全部通过（FAIL 为 0）
 */
bool app_selftest_all_pass(const app_selftest_report_t *report);

/**
 * @brief  状态字符串："PASS" / "FAIL" / "SKIP" / "LOOK"
 */
const char *app_selftest_state_str(app_selftest_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* __APP_SELFTEST_H__ */
