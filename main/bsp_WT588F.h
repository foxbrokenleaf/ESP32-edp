/*
 * @Description: WT588F 语音芯片 BSP 层（一线/两线串口位时序 + BUSY 检测 + 信息回读）
 *
 * 芯片：WT588F02-8S-C（唯创知音，SOP8 封装，规格书 V1.03）
 *
 * 硬件连接（本工程实际电路：CLK 与字库/EPD 共用，用 Q9 做 CS 门控复用）
 *
 *   ESP32 GPIO26 (CLK)  ──R22 10K──┐
 *                                  ├─ Q9 (NPN) 基极
 *   ESP32 GPIO21 (CS)   ───────────┼─ Q9 集电极
 *                                  └─ Q9 发射极 = AU_CLK_O ─┬─ 芯片 PIN2 (IO2/DATA1/CLK2)
 *                                                            └─ R21 10K ─> GND
 *   ESP32 GPIO25 (DATA) ────────────> 芯片 PIN3 (IO3/DATA2)   （一线模式不用）
 *   ESP32 GPIO32 (BUSY) <─────────── 芯片 PIN1 (IO1/BUSY)     （播放中低、空闲高）
 *   芯片 PIN4 (IO4)     ────────────> AU_DOWNLOAD（下载器烧写口，不接 MCU）
 *   芯片 PIN5 PWMN / PIN7 PWMP ─────> 喇叭输出，本驱动不控制
 *   芯片 PIN6 VDD / PIN8 GND
 *
 * Q9 接成“CLK 与 CS 的与门”（射极跟随器）：
 *   CS = 0（未选中）：集电极无供电，AU_CLK_O ≈ 0V。芯片完全收不到时钟/数据，
 *                     所以字库/EPD 在 GPIO26/25 上跑总线也不会误触发语音芯片。
 *   CS = 1（选中）  ：AU_CLK_O 跟随 CLK，高电平 = 3.3V - Vbe ≈ 2.6V。
 * 本层在每次发码前把 CS 拉高、发完拉低，并且“帧前 DATA 先高 5ms”是在 CS 有效
 * 之后做的，芯片看到的就是干净的 5ms 高电平（门控关闭时它只看到低电平，正好
 * 对应规格书“若 DATA 为低，必须先拉高 5ms 再发”的场景）。
 *
 * 硬件提醒：2.6V 只比芯片 VIH(≈0.7×VDD≈2.31V) 高 0.3V。若实测通信不稳，
 *   先用示波器量 AU_CLK_O 的高电平（可调用 bsp_wt588f_bus_test() 打波形）；
 *   低于 2.4V 建议把 Q9 换成 74LVC1G08（真与门）或 74LVC1G125（OE=CS 缓冲器）。
 *
 * 共用总线（非常重要）：
 *   GPIO26/25 上还接着字库 GT30L32S4W 的 SCLK/SI 和 EPD 的软件 SPI SCL/SDA。
 *   门控只能保证“没被选中时语音芯片不受干扰”，解决不了两边同时发码的问题：
 *   语音发码期间（一帧约 13ms，忙等不可打断）绝对不能同时刷 EPD / 读字库，
 *   否则两边波形都会被打断。请把语音调用与 EPD/GUI/字库调用放在同一个任务里
 *   顺序执行，或用一把全局互斥锁把它们串起来。
 *
 * 注意：规格书明确要求“请勿给 IO2 添加上拉”，上电时为高电平有几率让芯片
 *       进入非工作模式。因此本层把 DATA1/CLK2 的 MCU 内部上拉关掉。
 *
 * 一、一线串口（6.2.1 / 6.2.4）
 *   1. 每帧发送前 DATA 必须先拉高 >= 5ms（规格书：若 DATA 为低，必须先拉高
 *      5ms，否则某些条件下不响应；本层每帧都做，保证稳定）。
 *   2. 起帧：DATA 拉低 5ms（允许 5~20ms）。
 *   3. 数据位：先高后低，用高低电平比例表示数值
 *        位 = 1 -> 高 600us + 低 200us（3:1）
 *        位 = 0 -> 高 200us + 低 600us（1:3）
 *      允许范围 100us:300us ~ 400us:1200us，本层取规格书推荐值。
 *   4. 字节内低位先发；双字节指令先发高字节再发低字节，
 *      且每个字节各自成一个帧（各带 5ms 起帧），对应规格书
 *      “双字节一条指令 20.4ms = 10.2ms × 2”的说明。
 *   5. 单字节一帧总耗时约 13.4ms（5ms + 8×0.8ms + 2ms）。
 *
 * 二、两线串口（6.2.5 / 6.2.8）
 *   1. 每发一个字节前：CLK 若为低，先拉高 >= 5ms，再拉低 5ms 起帧。
 *   2. DATA 在 CLK 上升沿被采样，字节内低位先发。
 *   3. CLK 高/低各 350us（周期 700us，允许 200us~3.2ms）。
 *   4. 双字节同样是两个 8 位帧（每帧都有拉高/拉低 5ms）。
 *
 * 三、通用时序 / 电气注意
 *   1. 上电初始化约 40ms，期间不响应指令（本层 init 里等这段时间）。
 *   2. 两条指令间隔最小 2ms，本层每帧结束自动插入 BSP_WT588F_CMD_GAP_MS。
 *   3. 播放结束后 IO 电平保持稳定 200ms，芯片进入休眠（<5uA）。
 *      所以本层帧结束后把输出脚拉低（芯片 IO 内部 220K 下拉，低电平不灌电流）。
 *   4. 发码后 BUSY 变化时间：PWM 模式 500us~35ms，DAC 模式 30ms~65ms，
 *      所以判断“是否开始播放”要给 100ms 以上余量，见 WT588F_BUSY_START_TIMEOUT_MS。
 *   5. 全部延时用 esp_rom_delay_us() 忙等实现：本工程 CONFIG_FREERTOS_HZ=100，
 *      vTaskDelay 最小粒度 10ms，无法产生 us 级时序；一帧约 13ms 忙等可接受。
 *
 * 四、工程信息回读（第 8 章）
 *   发 F9H + 子命令后，芯片把主控程序编号 / 工程编号 / 版本号以 9600 8N1
 *   从 BUSY（PIN1）脚输出。因此回读时要用 UART RX 接管 BUSY 脚，用完再还原
 *   成普通输入。分三步调用，顺序见 bsp_wt588f_info_readback_begin/_read/_end。
 */

#ifndef __BSP_WT588F_H__
#define __BSP_WT588F_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= 引脚定义 ========================= */

/*
 * CS（门控使能，Q9 集电极）：高 = 选中语音芯片（Q9 导通，CLK 可达芯片）
 *   低 = 关闭门控（AU_CLK_O 被 R21 拉到地，语音芯片与 26/25 总线隔离）
 *   设为 -1 表示板上没有门控电路（CLK 直连芯片），此时本层不驱动任何片选脚。
 *   注意：该脚必须是能输出的 GPIO（0~33，避开 6~11）。
 */
#define BSP_WT588F_CS_PIN               21
#define BSP_WT588F_CS_ACTIVE_LEVEL      1       /* 高有效；电路改了极性改这里 */

/*
 * DATA1/CLK2（经 Q9 门控后接芯片 PIN2）
 *   一线模式：它是 DATA（驱动源仍是 MCU，只是被 CS 门控）；
 *   两线模式：它是 CLK（同样被 CS 门控）。
 *   注意：该脚不能加上拉（见文件头说明）。
 *   该脚与字库 SCLK / EPD SCL 共用（GPIO26），发码期间独占。
 */
#define BSP_WT588F_DATA1_CLK2_PIN       26

/*
 * DATA2（芯片 PIN3）：两线模式的 DATA，一线模式不用
 *   与字库 SI / EPD SDA 共用（GPIO25）。因此一线模式下本层会把它释放成
 *   高阻输入（不驱动），避免把字库/EPD 的空闲电平拉低；只有切到两线模式
 *   才配置成推挽输出。设为 -1 表示不接（此时只能跑一线模式）。
 */
#define BSP_WT588F_DATA2_PIN            25

/*
 * BUSY（芯片 PIN1）：播放中为低、空闲为高
 *   设为 -1 表示硬件没接：此时 is_busy() 恒为 false，wait_idle() 返回
 *   ESP_ERR_NOT_SUPPORTED，工程信息回读同样不可用。
 */
#define BSP_WT588F_BUSY_PIN             32

/* ========================= 通信方式 ========================= */

/*
 * 芯片的控制方式（一线/两线、单字节/双字节发码）是烧写语音工程时在芯片里
 * 设定好的，软件只能配合。上电默认值由下面这个宏决定，运行时可用
 * bsp_wt588f_set_wire_mode() 改。
 */
typedef enum {
    BSP_WT588F_WIRE_ONE = 0,        /* 一线串口：只用 DATA1（PIN2） */
    BSP_WT588F_WIRE_TWO = 1,        /* 两线串口：CLK（PIN2）+ DATA（PIN3） */
} bsp_wt588f_wire_t;

#define BSP_WT588F_DEFAULT_WIRE_MODE    BSP_WT588F_WIRE_ONE

/* ========================= 时序参数 ========================= */
/* 单位：ms / us；全部在规格书允许范围内，取推荐值 */

#define BSP_WT588F_IDLE_HIGH_MS         5       /* 每帧前拉高 >= 5ms */
#define BSP_WT588F_START_LOW_MS         5       /* 起帧拉低 5~20ms */
#define BSP_WT588F_BIT_ONE_HIGH_US      600     /* 位 1：高 600us */
#define BSP_WT588F_BIT_ONE_LOW_US       200     /* 位 1：低 200us */
#define BSP_WT588F_BIT_ZERO_HIGH_US     200     /* 位 0：高 200us */
#define BSP_WT588F_BIT_ZERO_LOW_US      600     /* 位 0：低 600us */
#define BSP_WT588F_CLK_HALF_US          350     /* 两线：CLK 高/低各 350us */
#define BSP_WT588F_CMD_GAP_MS           2       /* 两条指令间隔 >= 2ms */

/* 上电初始化时间（规格书 40ms），init 时等待，保证第一条指令被响应 */
#define BSP_WT588F_POWER_ON_MS          40

/* ========================= BUSY ========================= */

/* 播放中 BUSY 为低电平；若实际硬件极性相反，改这里即可 */
#define BSP_WT588F_BUSY_ACTIVE_LEVEL    0

/* ========================= 指令码（规格书 6.2.2 / 6.2.6） ========================= */

#define BSP_WT588F_CMD_VOLUME_BASE      0xE0    /* E0~EF：音量 0~15（双字节 FFE0~FFEF） */
#define BSP_WT588F_CMD_LOOP             0xF2    /* 循环播放当前语音（FFF2） */
#define BSP_WT588F_CMD_CHAIN            0xF3    /* 连码播放前缀，F3 + 地址（FFF3 + 地址） */
#define BSP_WT588F_CMD_CHAIN_LOOP       0xF4    /* 连码循环：F4 01 开始 / F4 00 取消（FFF4 0001/0000） */
#define BSP_WT588F_CMD_STOP             0xFE    /* 停止播放当前语音（FFFE） */
#define BSP_WT588F_CMD_INFO             0xF9    /* 回读工程信息：F9 + 子命令（FFF9 + 000x） */

#define BSP_WT588F_INFO_MAIN_PROGRAM    0x00    /* F9 00：主控程序编号 */
#define BSP_WT588F_INFO_PROJECT_NO      0x01    /* F9 01：Bin 文件工程编号 */
#define BSP_WT588F_INFO_VERSION_NO      0x02    /* F9 02：Bin 文件版本号 */

#define BSP_WT588F_CHAIN_LOOP_ON        0x01    /* F4 01 */
#define BSP_WT588F_CHAIN_LOOP_OFF       0x00    /* F4 00 */

/* ========================= 信息回读（UART） ========================= */

/*
 * 回读时 BUSY 脚作为 UART RX，9600 8N1。
 * BSP_WT588F_INFO_UART_PORT 是 uart_port_t 的数值：0/1/2 对应 UART_NUM_0/1/2。
 * 默认用 UART1，若工程里 UART1 已被占用请改成空闲的端口。
 */
#define BSP_WT588F_INFO_UART_PORT       1
#define BSP_WT588F_INFO_BAUD_RATE       9600
#define BSP_WT588F_INFO_RX_BUF_SIZE     512
#define BSP_WT588F_INFO_GAP_MS          200     /* 字节间最大空闲时间，超时即认为回读结束 */


/* ========================= 接口 ========================= */

/**
 * @brief  初始化 BSP 层（配置引脚、创建互斥锁、等待芯片上电初始化 40ms）
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 引脚配置非法；ESP_ERR_NO_MEM 锁创建失败
 */
esp_err_t bsp_wt588f_init(void);

/**
 * @brief  释放 BSP 层：引脚恢复为输入（释放总线），删除互斥锁
 * @return ESP_OK 成功
 */
esp_err_t bsp_wt588f_deinit(void);

/**
 * @brief  查询是否已初始化
 */
bool bsp_wt588f_is_ready(void);

/**
 * @brief  设置通信方式（一线 / 两线），并把总线置于空闲电平
 * @param  mode BSP_WT588F_WIRE_ONE / BSP_WT588F_WIRE_TWO
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 模式非法；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t bsp_wt588f_set_wire_mode(bsp_wt588f_wire_t mode);

/**
 * @brief  当前通信方式
 */
bsp_wt588f_wire_t bsp_wt588f_get_wire_mode(void);

/**
 * @brief  发送一个 8 位帧（一线：DATA 时序；两线：CLK/DATA 时序）
 *
 * 内部自动完成“拉高 5ms -> 拉低 5ms -> 8 位（低位先发）-> 拉低 + 2ms 间隔”，
 * 调用方只需要关心要发哪个字节。
 *
 * @param  byte 待发送字节
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t bsp_wt588f_send_byte(uint8_t byte);

/**
 * @brief  发送一个 16 位指令（两个 8 位帧，先高字节后低字节）
 *
 * 与规格书“双字节一条指令 20.4ms = 10.2ms × 2”一致：高/低字节各成帧，
 * 每帧都有自己的 5ms 起帧电平。整条指令发送期间持锁，不会被其它任务打断。
 *
 * @param  word 待发送的 16 位数据（如 0xFFF3 / 0x0001）
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t bsp_wt588f_send_word(uint16_t word);

/**
 * @brief  读 BUSY 引脚原始电平（1=高，0=低）
 *
 * BUSY 未接（BSP_WT588F_BUSY_PIN < 0）时返回 1（视为空闲）。
 */
uint8_t bsp_wt588f_busy_level(void);

/**
 * @brief  芯片是否正在播放（BUSY == ACTIVE_LEVEL）
 */
bool bsp_wt588f_is_busy(void);

/**
 * @brief  等待播完（BUSY 变回空闲）
 * @param  timeout_ms 超时时间；0 表示只查一次不等待
 * @return ESP_OK 已空闲
 *         ESP_ERR_TIMEOUT 超时仍在播放
 *         ESP_ERR_NOT_SUPPORTED BUSY 未接
 *         ESP_ERR_INVALID_STATE 未初始化
 *
 * 注：轮询用 vTaskDelay(1)，本工程 tick=100Hz 时约 10ms 一次，语音段通常
 *     上百 ms，精度足够，且不会长时间忙等占 CPU。
 */
esp_err_t bsp_wt588f_wait_idle(uint32_t timeout_ms);

/* ========================= 门控 / 硬件自检 ========================= */

/**
 * @brief  手动开/关 CS 门控（诊断用；正常发码时由 send_byte/send_word 自动控制）
 *
 * @param  selected true = 选中语音芯片（Q9 导通，CLK 可到达芯片）
 *                  false = 关闭门控（AU_CLK_O 被 R21 拉到地）
 * @return ESP_OK 成功；ESP_ERR_NOT_SUPPORTED 板上没有 CS 脚（BSP_WT588F_CS_PIN < 0）；
 *         ESP_ERR_INVALID_STATE 未初始化
 *
 * 注意：保持 selected = true 时语音芯片会看到 CLK（GPIO26）上的全部翻转，
 *       所以只有确认此刻没有字库/EPD 操作时才能手动打开门控。
 */
esp_err_t bsp_wt588f_cs_set(bool selected);

/**
 * @brief  门控 + 时钟波形测试（示波器 / 万用表测 Q9 电路用）
 *
 * 连续 duration_ms 毫秒、约 60~100Hz 的周期波形：
 *   门控关 3ms（AU_CLK_O 应为 ≈0V）
 *   -> 门控开，CLK 上打 8 个 400us 的脉冲（AU_CLK_O 跟随，高电平 ≈3.3V-Vbe≈2.6V）
 *   -> 门控关，空闲 -> 下一周期
 *
 * 一根探头挂在 AU_CLK_O 上就能同时确认：
 *   1) 门控开时有没有波形、高电平够不够高（低于 2.4V 建议换 74LVC1G08/125）；
 *   2) 门控关时是否真的回到 0V（回不到 0V 说明 Q9 基极还在被驱动或 Q9 击穿）。
 * 测试结束会恢复“门控关 + 总线空闲”状态。
 *
 * @param  duration_ms 测试时长（ms），0 按 1000ms 处理
 * @return ESP_OK 完成；ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t bsp_wt588f_bus_test(uint32_t duration_ms);

/**
 * @brief  信息回读第一步：把 BUSY 脚切换成 UART RX（9600 8N1）
 *
 * 必须在发送 F9 指令之前调用，否则芯片已经开始回传，会丢掉开头的字节。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_SUPPORTED BUSY 未接；
 *         ESP_ERR_INVALID_STATE 未初始化或该 UART 端口已被占用；
 *         其余为 UART 驱动错误码
 */
esp_err_t bsp_wt588f_info_readback_begin(void);

/**
 * @brief  信息回读第二步：读取芯片回传的数据
 *
 * 回传长度规格书没有写死（是变长文本），因此这里按“连续读到 gap_ms 内没有
 * 新字节就认为结束”的方式收数据，缓冲区满也会提前结束。
 *
 * @param  buf      接收缓冲
 * @param  buf_size 缓冲大小
 * @param  out_len  实际收到的字节数，可为 NULL
 * @param  gap_ms   字节间空闲超时，0 表示用 BSP_WT588F_INFO_GAP_MS
 * @return ESP_OK 收到数据
 *         ESP_ERR_TIMEOUT 一个字节都没收到
 *         ESP_ERR_INVALID_ARG 参数非法
 *         ESP_ERR_INVALID_STATE 未调用 begin
 */
esp_err_t bsp_wt588f_info_readback_read(uint8_t *buf, size_t buf_size,
                                        size_t *out_len, uint32_t gap_ms);

/**
 * @brief  信息回读第三步：卸载 UART，把 BUSY 脚还原成普通输入
 * @return ESP_OK 成功
 */
esp_err_t bsp_wt588f_info_readback_end(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_WT588F_H__ */
