/**
 * @file    key_driver.c
 * @brief   STM32F10x 多按键驱动模块
 * @details 支持5个按键，每个按键独立检测短按、长按，自带消抖处理
 */

#include "stm32f10x.h"
#include "stdio.h"
#include "Key.h"



/* 按键引脚定义 - 可根据实际硬件修改 */
// 按键1
#define KEY1_PORT           GPIOB
#define KEY1_PIN            GPIO_Pin_7
#define KEY1_CLK_ENABLE()   RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE)

// 按键2
#define KEY2_PORT           GPIOB
#define KEY2_PIN            GPIO_Pin_8
#define KEY2_CLK_ENABLE()   RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE)

// 按键3
#define KEY3_PORT           GPIOB
#define KEY3_PIN            GPIO_Pin_9
#define KEY3_CLK_ENABLE()   RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE)

// 按键4
#define KEY4_PORT           GPIOA
#define KEY4_PIN            GPIO_Pin_3
#define KEY4_CLK_ENABLE()   RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE)

// 按键5
#define KEY5_PORT           GPIOA
#define KEY5_PIN            GPIO_Pin_4
#define KEY5_CLK_ENABLE()   RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE)

/* 按键引脚映射表 */
typedef struct {
    GPIO_TypeDef* port;
    uint16_t pin;
} KeyPinMap;

static const KeyPinMap g_key_pin_map[KEY_NUM] = {
    {KEY1_PORT, KEY1_PIN},
    {KEY2_PORT, KEY2_PIN},
    {KEY3_PORT, KEY3_PIN},
    // {KEY4_PORT, KEY4_PIN},
    // {KEY5_PORT, KEY5_PIN}
};

/* 按键状态定义 */
#define KEY_PRESS           0   // 假设按键按下为低电平（外部上拉）
#define KEY_RELEASE         1

/* 按键参数配置 */
#define KEY_DEBOUNCE_TIME   20  // 消抖时间 (ms)
#define KEY_LONG_PRESS_TIME 1000// 长按时间 (ms)

/* 按键状态机 */
typedef enum {
    KEY_STATE_IDLE,         // 空闲状态
    KEY_STATE_DEBOUNCE,     // 消抖状态
    KEY_STATE_PRESSED,      // 按下状态
    KEY_STATE_LONG_PRESS    // 长按状态
} KeyState;

/* 按键数据结构 */
typedef struct {
    KeyState state;         // 当前状态
    uint32_t press_time;    // 按下时间戳
    uint8_t valid;          // 按键有效标志
} KeyStruct;

static KeyStruct g_keys[KEY_NUM];  // 5个按键实例

/**
 * @brief   所有按键GPIO初始化
 */
void Key_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    uint8_t i;
    
    /* 配置GPIO结构体 */
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;  // 上拉输入
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    
    /* 逐个初始化按键引脚 */
    for (i = 0; i < KEY_NUM; i++)
    {
        /* 使能对应GPIO时钟 - 这里简化处理，实际应该根据端口分别使能 */
        if (g_key_pin_map[i].port == GPIOA)
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
        else if (g_key_pin_map[i].port == GPIOB)
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
        else if (g_key_pin_map[i].port == GPIOC)
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
        else if (g_key_pin_map[i].port == GPIOD)
            RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD, ENABLE);
        
        GPIO_InitStructure.GPIO_Pin = g_key_pin_map[i].pin;
        GPIO_Init(g_key_pin_map[i].port, &GPIO_InitStructure);
    }
    
    /* 初始化所有按键状态 */
    for (i = 0; i < KEY_NUM; i++)
    {
        g_keys[i].state = KEY_STATE_IDLE;
        g_keys[i].press_time = 0;
        g_keys[i].valid = 0;
    }
}

/**
 * @brief   获取指定按键的电平状态
 * @param   key_id 按键ID (0-4)
 * @retval  0:按下 1:释放
 */
static uint8_t Key_GetLevel(uint8_t key_id)
{
    if (key_id >= KEY_NUM) return KEY_RELEASE;
    
    return GPIO_ReadInputDataBit(g_key_pin_map[key_id].port, 
                                  g_key_pin_map[key_id].pin);
}

/**
 * @brief   单个按键扫描函数
 * @param   key_id 按键ID (0-4)
 * @param   current_time 当前系统时间戳（ms）
 * @retval  按键事件类型
 */
static KeyEvent Key_ScanSingle(uint8_t key_id, uint32_t current_time)
{
    uint8_t key_level = Key_GetLevel(key_id);
    KeyEvent event = KEY_EVENT_NONE;
    KeyStruct *key = &g_keys[key_id];
    
    switch (key->state)
    {
        case KEY_STATE_IDLE:
            /* 检测到按键按下 */
            if (key_level == KEY_PRESS)
            {
                key->state = KEY_STATE_DEBOUNCE;
                key->press_time = current_time;
            }
            break;
            
        case KEY_STATE_DEBOUNCE:
            /* 消抖检测 */
            if (key_level == KEY_PRESS)
            {
                /* 消抖时间到，确认按键有效按下 */
                if (current_time - key->press_time >= KEY_DEBOUNCE_TIME)
                {
                    key->state = KEY_STATE_PRESSED;
                    key->valid = 1;
                }
            }
            else
            {
                /* 消抖期间按键释放，回到空闲 */
                key->state = KEY_STATE_IDLE;
            }
            break;
            
        case KEY_STATE_PRESSED:
            /* 检测按键释放或长按 */
            if (key_level == KEY_RELEASE)
            {
                /* 按键释放，产生短按事件 */
                if (key->valid)
                {
                    event = KEY_EVENT_SHORT_PRESS;
                    key->valid = 0;
                }
                key->state = KEY_STATE_IDLE;
            }
            else if (current_time - key->press_time >= KEY_LONG_PRESS_TIME)
            {
                /* 达到长按时间，产生长按事件 */
                if (key->valid)
                {
                    event = KEY_EVENT_LONG_PRESS;
                    key->valid = 0;
                }
                key->state = KEY_STATE_LONG_PRESS;
            }
            break;
            
        case KEY_STATE_LONG_PRESS:
            /* 长按状态，等待按键释放 */
            if (key_level == KEY_RELEASE)
            {
                key->state = KEY_STATE_IDLE;
            }
            break;
    }
    
    return event;
}

/**
 * @brief   所有按键扫描函数，需要在定时中断或主循环中定期调用（建议10ms调用一次）
 * @param   current_time 当前系统时间戳（ms）
 * @param   events 用于返回所有按键的事件状态数组
 */
void Key_ScanAll(uint32_t current_time, KeyEvent *events)
{
    uint8_t i;
    
    if (events == NULL) return;
    
    for (i = 0; i < KEY_NUM; i++)
    {
        events[i] = Key_ScanSingle(i, current_time);
    }
}

/**
 * @brief   获取指定按键是否处于按下状态（不含消抖）
 * @param   key_id 按键ID (0-4)
 * @retval  1:按下 0:释放
 */
uint8_t Key_IsPressed(uint8_t key_id)
{
    if (key_id >= KEY_NUM) return 0;
    
    return (g_keys[key_id].state == KEY_STATE_PRESSED || 
            g_keys[key_id].state == KEY_STATE_LONG_PRESS);
}

/**
 * @brief   获取任意按键是否处于按下状态
 * @retval  1:有按键按下 0:无按键按下
 */
uint8_t Key_AnyPressed(void)
{
    uint8_t i;
    
    for (i = 0; i < KEY_NUM; i++)
    {
        if (g_keys[i].state == KEY_STATE_PRESSED || 
            g_keys[i].state == KEY_STATE_LONG_PRESS)
        {
            return 1;
        }
    }
    return 0;
}
