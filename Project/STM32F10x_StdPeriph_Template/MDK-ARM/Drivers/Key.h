#ifndef __KEY_DRIVER_H
#define __KEY_DRIVER_H

#include "stm32f10x.h"

/* 按键数量 */
#define KEY_NUM             3

/* 按键ID定义 */
typedef enum {
    KEY_ID_1 = 0,
    KEY_ID_2,
    KEY_ID_3,
    KEY_ID_4,
    KEY_ID_5,
    KEY_ID_MAX
} KeyID;

/* 按键事件类型 */
typedef enum {
    KEY_EVENT_NONE = 0,         // 无事件
    KEY_EVENT_SHORT_PRESS,      // 短按
    KEY_EVENT_LONG_PRESS        // 长按
} KeyEvent;

/* 初始化所有按键 */
void Key_Init(void);

/* 扫描所有按键，返回事件数组 */
void Key_ScanAll(uint32_t current_time, KeyEvent *events);

/* 查询指定按键是否按下 */
uint8_t Key_IsPressed(uint8_t key_id);

/* 查询是否有任意按键按下 */
uint8_t Key_AnyPressed(void);

#endif /* __KEY_DRIVER_H */