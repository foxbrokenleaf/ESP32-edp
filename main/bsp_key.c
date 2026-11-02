/*
 * @Description: 按键 BSP 层实现（ESP-IDF GPIO + esp_timer 状态机消抖）
 *
 * 消抖思路：
 *   只在“稳定态”和“抖动观察态”之间切换。读到与当前稳定态不同的电平后
 *   先进入抖动观察态，观察 BSP_KEY_DEBOUNCE_MS；期间电平回弹则退回原稳定态，
 *   持续保持则确认跳变。机械抖动不会产生任何事件。
 */

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "bsp_key.h"

static const char *TAG = "BSP_KEY";

/* ========================= 内部数据结构 ========================= */

typedef struct {
    bsp_key_state_t    state;          /* 当前状态机状态 */
    int64_t            state_us;       /* 进入当前状态的时刻（us） */
    int64_t            press_us;       /* 消抖后确认按下的时刻（us），用于长短按判定 */
    int64_t            hold_us;        /* 上一次连发事件时刻（us） */
    int                gpio_num;       /* 按键引脚，-1 表示未初始化 */
    bool               initialized;    /* 是否已初始化 */
    bool               internal_pull;  /* 实际使用的内部上/下拉配置 */
    bool               pressed;        /* 消抖后的稳定状态 */
    bool               long_fired;     /* 本次按住是否已触发长按 */
    int64_t            debounce_us;    /* 消抖时间（us） */
    int64_t            long_press_us;  /* 长按阈值（us） */
    int64_t            repeat_us;      /* 连发周期（us），0 表示关闭 */
    bsp_key_event_t    last_event;     /* 最近一次事件 */
    bsp_key_event_t    callback_event; /* 关注的事件，NONE 表示全部回调 */
    bsp_key_callback_t callback;       /* 事件回调 */
    void              *user_ctx;       /* 回调透传参数 */
} bsp_key_ctx_t;

/* 按键表：bsp_key_start() 每注册一个引脚占用一个槽位，由一个扫描任务统一轮询 */
static bsp_key_ctx_t s_keys[BSP_KEY_MAX_NUM];

static TaskHandle_t s_key_task_handle = NULL;

/* ========================= 内部辅助 ========================= */

/* 按引脚查找已注册的按键，找不到返回 NULL */
static bsp_key_ctx_t *key_find(int gpio_num)
{
    for (int i = 0; i < BSP_KEY_MAX_NUM; i++) {
        if (s_keys[i].initialized && (s_keys[i].gpio_num == gpio_num)) {
            return &s_keys[i];
        }
    }
    return NULL;
}

/* 取一个空闲槽位，按键表已满返回 NULL */
static bsp_key_ctx_t *key_find_free(void)
{
    for (int i = 0; i < BSP_KEY_MAX_NUM; i++) {
        if (!s_keys[i].initialized) {
            return &s_keys[i];
        }
    }
    return NULL;
}

/* 第一个已注册的按键（单键兼容接口的默认操作对象） */
static bsp_key_ctx_t *key_first(void)
{
    for (int i = 0; i < BSP_KEY_MAX_NUM; i++) {
        if (s_keys[i].initialized) {
            return &s_keys[i];
        }
    }
    return NULL;
}

static bool key_any_registered(void)
{
    return key_first() != NULL;
}

static const char *key_event_name(bsp_key_event_t event)
{
    switch (event) {
    case BSP_KEY_EVENT_PRESS:      return "PRESS";
    case BSP_KEY_EVENT_CLICK:      return "CLICK";
    case BSP_KEY_EVENT_LONG_PRESS: return "LONG_PRESS";
    case BSP_KEY_EVENT_LONG_HOLD:  return "LONG_HOLD";
    case BSP_KEY_EVENT_RELEASE:    return "RELEASE";
    default:                       return "NONE";
    }
}

static inline bool key_read_pressed(const bsp_key_ctx_t *key)
{
    return (gpio_get_level((gpio_num_t)key->gpio_num) == BSP_KEY_ACTIVE_LEVEL);
}

/* ========================= 对外接口 ========================= */

esp_err_t bsp_key_init(void)
{
    return bsp_key_init_ex(BSP_KEY_GPIO_NUM);
}

esp_err_t bsp_key_init_ex(int gpio_num)
{
    if (gpio_num < 0 || gpio_num > 39) {
        ESP_LOGE(TAG, "invalid GPIO%d (ESP32: 0~39)", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }
    if (gpio_num >= 6 && gpio_num <= 11) {
        ESP_LOGE(TAG, "GPIO%d is connected to SPI flash", gpio_num);
        return ESP_ERR_INVALID_ARG;
    }

    /* 同一引脚重复初始化：幂等返回 */
    if (key_find(gpio_num) != NULL) {
        return ESP_OK;
    }

    bsp_key_ctx_t *key = key_find_free();
    if (key == NULL) {
        ESP_LOGE(TAG, "no free key slot (BSP_KEY_MAX_NUM=%d)", BSP_KEY_MAX_NUM);
        return ESP_ERR_NO_MEM;
    }

    /* GPIO34~39 为纯输入引脚，没有内部上/下拉 */
    bool internal_pull = (BSP_KEY_INTERNAL_PULL != 0);
    if (internal_pull && gpio_num >= 34) {
        ESP_LOGW(TAG, "GPIO%d has no internal pull, external pull required", gpio_num);
        internal_pull = false;
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = ((uint64_t)1 << (unsigned)gpio_num),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = (internal_pull && (BSP_KEY_ACTIVE_LEVEL == 0))
                        ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (internal_pull && (BSP_KEY_ACTIVE_LEVEL != 0))
                        ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config GPIO%d failed: %s", gpio_num, esp_err_to_name(err));
        return err;
    }

    key->gpio_num       = gpio_num;
    key->internal_pull  = internal_pull;
    key->state          = BSP_KEY_STATE_IDLE;
    key->state_us       = esp_timer_get_time();
    key->press_us       = 0;
    key->hold_us        = 0;
    key->pressed        = false;
    key->long_fired     = false;
    key->last_event     = BSP_KEY_EVENT_NONE;
    key->callback       = NULL;
    key->callback_event = BSP_KEY_EVENT_NONE;
    key->user_ctx       = NULL;
    key->debounce_us    = (int64_t)BSP_KEY_DEBOUNCE_MS * 1000;
    key->long_press_us  = (int64_t)BSP_KEY_LONG_PRESS_MS * 1000;
    key->repeat_us      = (int64_t)BSP_KEY_LONG_HOLD_MS * 1000;
    key->initialized    = true;

    ESP_LOGI(TAG, "ready: GPIO%d active_%s pull_%s, debounce=%dms, long=%dms, repeat=%dms, scan=%dms",
             gpio_num, (BSP_KEY_ACTIVE_LEVEL == 0) ? "low" : "high",
             internal_pull ? ((BSP_KEY_ACTIVE_LEVEL == 0) ? "up" : "down") : "off",
             BSP_KEY_DEBOUNCE_MS, BSP_KEY_LONG_PRESS_MS,
             BSP_KEY_LONG_HOLD_MS, BSP_KEY_SCAN_PERIOD_MS);
    return ESP_OK;
}

esp_err_t bsp_key_deinit(void)
{
    bsp_key_task_stop();

    for (int i = 0; i < BSP_KEY_MAX_NUM; i++) {
        bsp_key_ctx_t *key = &s_keys[i];
        if (!key->initialized) {
            continue;
        }

        esp_err_t err = gpio_reset_pin((gpio_num_t)key->gpio_num);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "reset GPIO%d failed: %s", key->gpio_num, esp_err_to_name(err));
        }

        memset(key, 0, sizeof(*key));
        key->gpio_num = -1;
    }

    ESP_LOGI(TAG, "deinit");
    return ESP_OK;
}

esp_err_t bsp_key_start(int gpio_num, bsp_key_callback_t cb, bsp_key_event_t event)
{
    if (gpio_num == -1) {
        gpio_num = BSP_KEY_GPIO_NUM;
    }
    if ((event < BSP_KEY_EVENT_NONE) || (event > BSP_KEY_EVENT_RELEASE)) {
        ESP_LOGE(TAG, "invalid event %d", (int)event);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = bsp_key_init_ex(gpio_num);
    if (err != ESP_OK) {
        return err;
    }

    bsp_key_ctx_t *key = key_find(gpio_num);
    key->callback       = cb;
    key->user_ctx       = NULL;
    key->callback_event = event;

    err = bsp_key_task_start();
    if (err != ESP_OK) {
        key->callback       = NULL;
        key->callback_event = BSP_KEY_EVENT_NONE;
        return err;
    }

    ESP_LOGI(TAG, "started: GPIO%d, callback=%s, event=%d",
             gpio_num, (cb != NULL) ? "yes" : "no", (int)event);
    return ESP_OK;
}

void bsp_key_stop(void)
{
    bsp_key_deinit();
}

esp_err_t bsp_key_stop_gpio(int gpio_num)
{
    bsp_key_ctx_t *key = key_find(gpio_num);
    if (key == NULL) {
        ESP_LOGW(TAG, "GPIO%d not registered", gpio_num);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = gpio_reset_pin((gpio_num_t)gpio_num);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reset GPIO%d failed: %s", gpio_num, esp_err_to_name(err));
    }

    memset(key, 0, sizeof(*key));
    key->gpio_num = -1;

    /* 最后一个按键移除后不再需要扫描任务 */
    if (!key_any_registered()) {
        bsp_key_task_stop();
    }

    ESP_LOGI(TAG, "stopped: GPIO%d", gpio_num);
    return ESP_OK;
}

static bsp_key_event_t key_scan_one(bsp_key_ctx_t *key)
{
    if (!key->initialized) {
        return BSP_KEY_EVENT_NONE;
    }

    const int64_t   now_us      = esp_timer_get_time();
    const bool      raw_pressed = key_read_pressed(key);
    bsp_key_event_t event       = BSP_KEY_EVENT_NONE;

    switch (key->state) {
    case BSP_KEY_STATE_IDLE:
        /* 稳定松开：检出按下电平，进入消抖观察 */
        if (raw_pressed) {
            key->state    = BSP_KEY_STATE_DEBOUNCE_PRESS;
            key->state_us = now_us;
        }
        break;

    case BSP_KEY_STATE_DEBOUNCE_PRESS:
        if (!raw_pressed) {
            /* 抖动：电平又回去了，判定为干扰 */
            key->state    = BSP_KEY_STATE_IDLE;
            key->state_us = now_us;
        } else if (now_us - key->state_us >= key->debounce_us) {
            /* 按下电平保持足够久，确认按下 */
            key->state      = BSP_KEY_STATE_PRESSED;
            key->state_us   = now_us;
            key->press_us   = now_us;
            key->hold_us    = now_us;
            key->pressed    = true;
            key->long_fired = false;
            event           = BSP_KEY_EVENT_PRESS;
        }
        break;

    case BSP_KEY_STATE_PRESSED:
        if (!raw_pressed) {
            /* 候选松开，进入松开消抖 */
            key->state    = BSP_KEY_STATE_DEBOUNCE_RELEASE;
            key->state_us = now_us;
        } else if (now_us - key->press_us >= key->long_press_us) {
            /* 按住时间达到长按阈值 */
            key->state      = BSP_KEY_STATE_LONG_PRESSED;
            key->state_us   = now_us;
            key->hold_us    = now_us;
            key->long_fired = true;
            event           = BSP_KEY_EVENT_LONG_PRESS;
        }
        break;

    case BSP_KEY_STATE_LONG_PRESSED:
        if (!raw_pressed) {
            key->state    = BSP_KEY_STATE_DEBOUNCE_RELEASE;
            key->state_us = now_us;
        } else if ((key->repeat_us > 0) && (now_us - key->hold_us >= key->repeat_us)) {
            /* 长按保持：按周期重复上报 */
            key->hold_us = now_us;
            event        = BSP_KEY_EVENT_LONG_HOLD;
        }
        break;

    case BSP_KEY_STATE_DEBOUNCE_RELEASE:
        if (raw_pressed) {
            /* 抖动：又按住了，恢复到按下态继续计时（不打断长按计时） */
            key->state    = key->long_fired ? BSP_KEY_STATE_LONG_PRESSED
                                            : BSP_KEY_STATE_PRESSED;
            key->state_us = now_us;
            key->hold_us  = now_us;
        } else if (now_us - key->state_us >= key->debounce_us) {
            /* 松开电平保持足够久，确认松开 */
            key->state    = BSP_KEY_STATE_IDLE;
            key->state_us = now_us;
            key->pressed  = false;
            event         = key->long_fired ? BSP_KEY_EVENT_RELEASE
                                            : BSP_KEY_EVENT_CLICK;
        }
        break;

    default:
        /* 理论不可达，异常状态复位 */
        key->state    = BSP_KEY_STATE_IDLE;
        key->state_us = now_us;
        key->pressed  = false;
        break;
    }

    if (event != BSP_KEY_EVENT_NONE) {
        key->last_event = event;
        ESP_LOGD(TAG, "GPIO%d event=%s, state=%d, pressed=%d",
                 key->gpio_num, key_event_name(event),
                 (int)key->state, (int)key->pressed);

        if ((key->callback != NULL) &&
            ((key->callback_event == BSP_KEY_EVENT_NONE) ||
             (key->callback_event == event))) {
            key->callback(event, key->user_ctx);
        }
    }

    return event;
}

bsp_key_event_t bsp_key_scan(void)
{
    bsp_key_event_t first = BSP_KEY_EVENT_NONE;

    /* 一个任务轮询所有已注册按键，每个按键的状态机独立推进 */
    for (int i = 0; i < BSP_KEY_MAX_NUM; i++) {
        bsp_key_event_t event = key_scan_one(&s_keys[i]);
        if ((event != BSP_KEY_EVENT_NONE) && (first == BSP_KEY_EVENT_NONE)) {
            first = event;
        }
    }

    return first;
}

bsp_key_event_t bsp_key_get_last_event(void)
{
    bsp_key_ctx_t *key = key_first();
    return (key != NULL) ? key->last_event : BSP_KEY_EVENT_NONE;
}

bool bsp_key_is_pressed(void)
{
    bsp_key_ctx_t *key = key_first();
    return (key != NULL) && key->pressed;
}

bsp_key_state_t bsp_key_get_state(void)
{
    bsp_key_ctx_t *key = key_first();
    return (key != NULL) ? key->state : BSP_KEY_STATE_IDLE;
}

uint32_t bsp_key_get_press_time_ms(void)
{
    bsp_key_ctx_t *key = key_first();
    if ((key == NULL) || !key->pressed) {
        return 0;
    }
    return (uint32_t)((esp_timer_get_time() - key->press_us) / 1000);
}

bsp_key_event_t bsp_key_get_last_event_gpio(int gpio_num)
{
    bsp_key_ctx_t *key = key_find(gpio_num);
    return (key != NULL) ? key->last_event : BSP_KEY_EVENT_NONE;
}

bool bsp_key_is_pressed_gpio(int gpio_num)
{
    bsp_key_ctx_t *key = key_find(gpio_num);
    return (key != NULL) && key->pressed;
}

bsp_key_state_t bsp_key_get_state_gpio(int gpio_num)
{
    bsp_key_ctx_t *key = key_find(gpio_num);
    return (key != NULL) ? key->state : BSP_KEY_STATE_IDLE;
}

uint32_t bsp_key_get_press_time_ms_gpio(int gpio_num)
{
    bsp_key_ctx_t *key = key_find(gpio_num);
    if ((key == NULL) || !key->pressed) {
        return 0;
    }
    return (uint32_t)((esp_timer_get_time() - key->press_us) / 1000);
}

esp_err_t bsp_key_register_callback(bsp_key_callback_t cb, void *user_ctx)
{
    bsp_key_ctx_t *key = key_first();
    if (key == NULL) {
        ESP_LOGW(TAG, "no key registered");
        return ESP_ERR_INVALID_STATE;
    }

    key->callback       = cb;
    key->user_ctx       = (cb != NULL) ? user_ctx : NULL;
    key->callback_event = BSP_KEY_EVENT_NONE;   /* 默认所有事件都回调 */
    return ESP_OK;
}

/* ========================= 扫描任务 ========================= */

static void bsp_key_task(void *arg)
{
    (void)arg;

    const TickType_t period    = pdMS_TO_TICKS(BSP_KEY_SCAN_PERIOD_MS);
    TickType_t       last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "scan task running");

    while (1) {
        bsp_key_scan();
        /* 固定周期扫描，避免任务执行时间造成节拍漂移 */
        vTaskDelayUntil(&last_wake, period);
    }
}

esp_err_t bsp_key_task_start(void)
{
    if (!key_any_registered()) {
        ESP_LOGE(TAG, "no key registered");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_key_task_handle != NULL) {
        ESP_LOGW(TAG, "scan task already started");
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(bsp_key_task,
                                 BSP_KEY_TASK_NAME,
                                 BSP_KEY_TASK_STACK,
                                 NULL,
                                 BSP_KEY_TASK_PRIO,
                                 &s_key_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create scan task failed");
        s_key_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void bsp_key_task_stop(void)
{
    TaskHandle_t handle = s_key_task_handle;
    if (handle == NULL) {
        return;
    }
    s_key_task_handle = NULL;

    /* 允许在回调里停止自身 */
    if (xTaskGetCurrentTaskHandle() == handle) {
        ESP_LOGI(TAG, "scan task stopping itself");
        vTaskDelete(NULL);
    } else {
        vTaskDelete(handle);
        ESP_LOGI(TAG, "scan task stopped");
    }
}
