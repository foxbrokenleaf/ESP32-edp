/*
 * @Description: 按键 BSP 层（状态机消抖，支持短按 / 长按 / 长按连发）
 *
 * 硬件连接（可在此处修改）：
 *   KEY -> GPIO0（开发板 BOOT 键，按下为低电平，内部上拉）
 *
 * 说明：
 *   1. 消抖使用有限状态机，全程不阻塞：每次扫描只读一次引脚电平，
 *      电平连续保持 BSP_KEY_DEBOUNCE_MS 才认为状态真的改变。
 *   2. 计时基准为 esp_timer_get_time()（微秒），与 FreeRTOS tick 频率
 *      （本项目 CONFIG_FREERTOS_HZ=100）无关，时间精度不受扫描周期影响。
 *   3. 状态机流转：
 *
 *        IDLE --检出按下--> DEBOUNCE_PRESS --稳定--> PRESSED --按住超时--> LONG_PRESSED
 *         ^                     | 电平回弹             |                        |
 *         |                     +--> IDLE              | 松开                   | 松开 / 连发
 *         |                                           v                        v
 *         +<---- 稳定松开 ---- DEBOUNCE_RELEASE <------+------------------------+
 *                              (短按 -> CLICK，长按 -> RELEASE)
 *
 *   4. 每次扫描最多产生 1 个事件，事件含义：
 *        PRESS        消抖后确认按下（按下瞬间）
 *        CLICK        短按结束：按下后未到长按阈值就松开
 *        LONG_PRESS   按住达到 BSP_KEY_LONG_PRESS_MS（长按触发一次）
 *        LONG_HOLD    长按保持：按住期间每 BSP_KEY_LONG_HOLD_MS 触发一次
 *        RELEASE      长按之后松开（短按松开上报的是 CLICK）
 *
 * 使用方式（三选一）：
 *   a) 轮询：bsp_key_init() 后由 app 按固定周期调用 bsp_key_scan()，返回值即事件；
 *   b) 分步：bsp_key_init() + bsp_key_register_callback() + bsp_key_task_start()；
 *   c) 一步到位：bsp_key_start(gpio_num, callback, event)，
 *      内部完成 GPIO 初始化、回调注册、扫描任务创建，event 指定关注的事件。
 *
 * 多按键（最多 BSP_KEY_MAX_NUM 个，共用一个扫描任务轮流扫描）：
 *   bsp_key_start(17, sw3_key, BSP_KEY_EVENT_PRESS);
 *   bsp_key_start(18, sw2_key, BSP_KEY_EVENT_PRESS);
 *   bsp_key_start(19, sw1_key, BSP_KEY_EVENT_PRESS);
 *   每个引脚有独立的消抖状态机、长按计时和回调，互不影响；
 *   用不同的回调函数即可区分是哪个按键。
 *
 * 示例（所有事件都回调）：
 *   static void on_key(bsp_key_event_t event, void *ctx)
 *   {
 *       switch (event) {
 *       case BSP_KEY_EVENT_CLICK:      layout_no = !layout_no; break;
 *       case BSP_KEY_EVENT_LONG_PRESS: EPD_Clear();            break;
 *       default: break;
 *       }
 *   }
 *
 *   bsp_key_start(0, on_key, BSP_KEY_EVENT_NONE);
 *
 * 示例（只关心单击）：
 *   bsp_key_start(0, on_key, BSP_KEY_EVENT_CLICK);
 *
 * 注意：
 *   1. GPIO0 是 strapping 引脚，上电/复位瞬间按住会被当成 BOOT 键进入下载模式，
 *      上电后再按不受影响。若产品外壳上电时可能压住按键，请改用普通 GPIO
 *      （如 GPIO32/33/16/17 等未占用引脚）。
 *   2. bsp_key_scan() 会扫描按键表里的所有按键，但只返回其中一个事件；
 *      多按键场景请使用回调方式，不要用轮询返回值。
 *   3. 回调运行在扫描任务上下文，不要在里面做耗时操作（如整屏 EPD 刷新），
 *      建议只置标志 / 发队列，由业务任务执行耗时动作。
 */

#ifndef __BSP_KEY_H__
#define __BSP_KEY_H__

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= 引脚定义 ========================= */

/*
 * 按键引脚：
 *   0  -> 大多数 ESP32 开发板的 BOOT 键（已有外部上拉）
 *   34~39 为纯输入引脚，无内部上下拉，若使用需把 BSP_KEY_INTERNAL_PULL 设为 0
 */
#define BSP_KEY_GPIO_NUM          0

/* 按下时的电平：0 = 低电平有效（按键另一端接 GND），1 = 高电平有效 */
#define BSP_KEY_ACTIVE_LEVEL      0

/* 是否启用片内上/下拉（低电平有效时上拉，高电平有效时下拉） */
#define BSP_KEY_INTERNAL_PULL     1

/* ========================= 按键数量 ========================= */

/* 最多同时管理的按键个数（bsp_key_start() 每调用一次占用一个槽位） */
#define BSP_KEY_MAX_NUM           4

/* ========================= 时间参数 ========================= */

#define BSP_KEY_DEBOUNCE_MS       20      /* 消抖确认时间 */
#define BSP_KEY_LONG_PRESS_MS     2000    /* 长按判定阈值 */
#define BSP_KEY_LONG_HOLD_MS      500     /* 长按连发周期，0 表示关闭连发 */
#define BSP_KEY_SCAN_PERIOD_MS    10      /* 扫描周期（任务模式下生效） */

/* ========================= 任务参数 ========================= */

#define BSP_KEY_TASK_NAME         "bsp_key_task"
#define BSP_KEY_TASK_STACK        4096
#define BSP_KEY_TASK_PRIO         5

/* ========================= 配置检查 ========================= */

#if (BSP_KEY_GPIO_NUM < 0) || (BSP_KEY_GPIO_NUM > 39)
#error "BSP_KEY_GPIO_NUM: 非法引脚，ESP32 可用 GPIO 为 0~39"
#endif

#if (BSP_KEY_GPIO_NUM >= 6) && (BSP_KEY_GPIO_NUM <= 11)
#error "BSP_KEY_GPIO_NUM: GPIO6~GPIO11 连接 SPI Flash，不能用作按键"
#endif

#if (BSP_KEY_INTERNAL_PULL) && (BSP_KEY_GPIO_NUM >= 34)
#error "BSP_KEY_GPIO_NUM: GPIO34~GPIO39 无内部上下拉，请把 BSP_KEY_INTERNAL_PULL 设为 0"
#endif

#if (BSP_KEY_ACTIVE_LEVEL != 0) && (BSP_KEY_ACTIVE_LEVEL != 1)
#error "BSP_KEY_ACTIVE_LEVEL: 只能为 0（低电平有效）或 1（高电平有效）"
#endif

#if (BSP_KEY_MAX_NUM < 1)
#error "BSP_KEY_MAX_NUM: 至少为 1"
#endif

#if (BSP_KEY_SCAN_PERIOD_MS <= 0)
#error "BSP_KEY_SCAN_PERIOD_MS: 扫描周期必须大于 0"
#endif

#if (BSP_KEY_DEBOUNCE_MS < BSP_KEY_SCAN_PERIOD_MS)
#error "BSP_KEY_DEBOUNCE_MS: 消抖时间不应小于扫描周期"
#endif

#if (BSP_KEY_LONG_PRESS_MS <= BSP_KEY_DEBOUNCE_MS)
#error "BSP_KEY_LONG_PRESS_MS: 长按阈值必须大于消抖时间"
#endif

#if (BSP_KEY_LONG_HOLD_MS < 0)
#error "BSP_KEY_LONG_HOLD_MS: 不能为负数，0 表示关闭长按连发"
#endif

/* ========================= 状态 / 事件 ========================= */

/* 状态机内部状态，调试时可用 bsp_key_get_state() 查看 */
typedef enum {
    BSP_KEY_STATE_IDLE = 0,          /* 稳定：松开 */
    BSP_KEY_STATE_DEBOUNCE_PRESS,    /* 抖动中：刚检出按下，等待稳定 */
    BSP_KEY_STATE_PRESSED,           /* 稳定：按下（短按计时中） */
    BSP_KEY_STATE_LONG_PRESSED,      /* 稳定：长按已触发（可连发） */
    BSP_KEY_STATE_DEBOUNCE_RELEASE,  /* 抖动中：刚检出松开，等待稳定 */
} bsp_key_state_t;

/* 对外事件，bsp_key_scan() 的返回值 / 回调参数 */
typedef enum {
    BSP_KEY_EVENT_NONE = 0,          /* 无事件 */
    BSP_KEY_EVENT_PRESS,             /* 消抖后确认按下 */
    BSP_KEY_EVENT_CLICK,             /* 短按结束（松开时上报） */
    BSP_KEY_EVENT_LONG_PRESS,        /* 长按触发（按住达到阈值时上报一次） */
    BSP_KEY_EVENT_LONG_HOLD,         /* 长按保持（按住期间周期上报） */
    BSP_KEY_EVENT_RELEASE,           /* 长按后松开 */
} bsp_key_event_t;

/* 事件回调，在 bsp_key_scan() 所在的上下文（任务或用户任务）中执行 */
typedef void (*bsp_key_callback_t)(bsp_key_event_t event, void *user_ctx);

/* ========================= 接口 ========================= */

/**
 * @brief  初始化默认按键 GPIO（可重复调用，已初始化时直接返回 ESP_OK）
 * @return ESP_OK 成功
 */
esp_err_t bsp_key_init(void);

/**
 * @brief  初始化指定 GPIO 的按键（运行期指定引脚，电平极性沿用 BSP_KEY_ACTIVE_LEVEL）
 * @param  gpio_num 按键 GPIO 编号（0~39），不能是 6~11（接 SPI Flash）
 * @return ESP_OK              成功（该引脚已注册时直接返回 ESP_OK）
 *         ESP_ERR_INVALID_ARG 引脚非法
 *         ESP_ERR_NO_MEM      按键表已满（BSP_KEY_MAX_NUM）
 * @note   1. 只初始化 GPIO，不注册回调、不创建任务，适合配合 bsp_key_scan() 轮询。
 *         2. GPIO34~39 无内部上下拉，会自动关闭内部上下拉并打印警告，
 *            使用这些引脚时必须由硬件提供外部上/下拉。
 *         3. 最多可注册 BSP_KEY_MAX_NUM 个不同引脚。
 */
esp_err_t bsp_key_init_ex(int gpio_num);

/**
 * @brief  一步完成：初始化指定 GPIO、注册回调、创建周期扫描任务
 * @param  gpio_num 按键 GPIO 编号（0~39）；传 -1 使用 BSP_KEY_GPIO_NUM
 * @param  cb       事件回调，可为 NULL（只扫描不通知）
 * @param  event    只在该事件发生时调用回调；BSP_KEY_EVENT_NONE 表示所有事件
 * @return ESP_OK              成功
 *         ESP_ERR_INVALID_ARG 引脚或事件非法
 *         ESP_ERR_NO_MEM      按键表已满（最多 BSP_KEY_MAX_NUM 个）或任务创建失败
 * @note   1. 可在同一个任务里对多个不同引脚各调用一次，实现多按键；
 *            所有按键共用一个扫描任务，每个按键的状态机/回调相互独立。
 *         2. 对同一引脚重复调用只会更新回调与关注的事件，不会重复占用槽位、
 *            也不会重复创建任务。
 */
esp_err_t bsp_key_start(int gpio_num, bsp_key_callback_t cb, bsp_key_event_t event);

/**
 * @brief  停止扫描任务并删除全部按键（与 bsp_key_start() 配对使用）
 */
void bsp_key_stop(void);

/**
 * @brief  只停止并删除指定按键，其余按键继续工作
 * @param  gpio_num 按键 GPIO 编号
 * @return ESP_OK           成功
 *         ESP_ERR_NOT_FOUND 该引脚未注册
 * @note   删除最后一个按键时会同时停止扫描任务。
 *         扫描任务运行期间如需删除按键，请从按键回调中调用（与扫描同一上下文）；
 *         从其它任务调用请先 bsp_key_task_stop()。
 */
esp_err_t bsp_key_stop_gpio(int gpio_num);

/**
 * @brief  反初始化：停止扫描任务并释放全部按键 GPIO
 * @return ESP_OK 成功
 */
esp_err_t bsp_key_deinit(void);

/**
 * @brief  按键扫描（状态机核心），需按固定周期调用，建议 10ms 左右
 * @return 本次扫描产生的事件；无事件返回 BSP_KEY_EVENT_NONE
 * @note   会依次扫描按键表中所有已注册按键（回调照常按各自配置触发），
 *         但返回值只反映其中的一个事件；多按键请不要依赖返回值，改用回调。
 *         轮询方式的按键不要同时启动扫描任务（bsp_key_task_start()）。
 */
bsp_key_event_t bsp_key_scan(void);

/**
 * @brief  默认按键（第一个注册的按键）最近一次产生的事件（不会清除）
 */
bsp_key_event_t bsp_key_get_last_event(void);

/**
 * @brief  默认按键当前消抖后的稳定状态：true = 按下
 */
bool bsp_key_is_pressed(void);

/**
 * @brief  默认按键当前状态机状态
 */
bsp_key_state_t bsp_key_get_state(void);

/**
 * @brief  默认按键当前按住的时长（毫秒），未按下返回 0
 */
uint32_t bsp_key_get_press_time_ms(void);

/**
 * @brief  指定按键最近一次产生的事件
 * @param  gpio_num 按键 GPIO 编号
 * @return 事件；该引脚未注册时返回 BSP_KEY_EVENT_NONE
 */
bsp_key_event_t bsp_key_get_last_event_gpio(int gpio_num);

/**
 * @brief  指定按键当前是否处于按下状态
 * @param  gpio_num 按键 GPIO 编号
 * @return true = 按下；未注册返回 false
 */
bool bsp_key_is_pressed_gpio(int gpio_num);

/**
 * @brief  指定按键当前状态机状态
 * @param  gpio_num 按键 GPIO 编号
 * @return 状态；未注册返回 BSP_KEY_STATE_IDLE
 */
bsp_key_state_t bsp_key_get_state_gpio(int gpio_num);

/**
 * @brief  指定按键当前按住的时长（毫秒）
 * @param  gpio_num 按键 GPIO 编号
 * @return 毫秒数；未注册或未按下返回 0
 */
uint32_t bsp_key_get_press_time_ms_gpio(int gpio_num);

/**
 * @brief  给默认按键注册事件回调（传 NULL 表示取消）
 * @param  cb       回调函数
 * @param  user_ctx 回调透传参数
 * @return ESP_OK               成功
 *         ESP_ERR_INVALID_STATE 还没有注册任何按键
 * @note   注册后所有事件都会回调；若只想接收某个事件，请用 bsp_key_start()。
 */
esp_err_t bsp_key_register_callback(bsp_key_callback_t cb, void *user_ctx);

/**
 * @brief  启动内部扫描任务（周期 BSP_KEY_SCAN_PERIOD_MS，自动消抖）
 * @return ESP_OK 成功，ESP_ERR_NO_MEM 任务创建失败
 */
esp_err_t bsp_key_task_start(void);

/**
 * @brief  停止内部扫描任务
 */
void bsp_key_task_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_KEY_H__ */
