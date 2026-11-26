/*
 * @Description: WT588F 语音芯片应用层（播放 / 停止 / 循环 / 连码 / 音量 / 信息回读）
 *
 * 芯片：WT588F02-8S-C（16 位 DSP、内置 220KB Flash、最多 1000 段语音、
 *       支持一线串口与两线串口、播放结束 IO 稳定 200ms 后休眠 <5uA）
 *
 * 分层：
 *   WT588F.c/.h      <- 本文件：面向业务，只关心“播第几段 / 音量 / 是否在播”
 *   bsp_WT588F.c/.h  <- 位时序、BUSY 检测、UART 信息回读；改引脚/时序只动 BSP 层
 *
 * 使用示例：
 *   ESP_ERROR_CHECK(WT588F_Init());      // 默认一线串口 + 单字节发码
 *   WT588F_SetVolume(12);                // 音量 0~15，上电默认最大 15
 *   WT588F_Play(3);                      // 播放第 3 段，立即返回
 *   WT588F_PlayAndWait(3, 5000);         // 播放并等播完（需要 BUSY 接线）
 *
 *   const uint16_t chain[] = { 1, 2, 3 };
 *   WT588F_PlayChain(chain, 3);          // 连码播放（最多 64 段）
 *
 * 发码方式（烧写语音工程时由上位机决定，软件只能配合）：
 *   - 地址总数 <  224 段：单字节发码，命令 0x00~0xFF
 *   - 地址总数 >= 224 段：双字节发码，命令 0xFFxx / 0x00xx（地址 0~999）
 *   本层用 WT588F_DEFAULT_ADDR_MODE 指定上电默认值，运行时可用
 *   WT588F_SetAddressMode() 修改，所有接口会自动换算成对应发码方式。
 *
 * 注意：
 *   1. 两条指令间隔 >= 2ms，BSP 层每帧后已自动插入，业务层不用再 delay。
 *   2. 打断规则：不发 F3 前缀、只发地址会立即打断当前语音；F3 + 地址是连码，
 *      播放期间收到不会打断前面的连码。
 *   3. 循环播放要先发播放指令，再发 F2；再次发 F2 停掉循环。
 *   4. BUSY 播放中为低、空闲为高，发码后 500us~65ms 才变化，
 *      WT588F_PlayAndWait() 里已按 100ms 余量等“开始播放”。
 *   5. 工程信息回读只能在空闲时发 F9，回读期间 BUSY 脚被 UART 占用，
 *      结束后 BSP 层自动还原成 GPIO 输入。
 *   6. 本工程硬件把语音芯片的 CLK 与字库/EPD 的 SCLK/SCL 复用在 GPIO26 上，
 *      由 GPIO21 经 Q9 做门控（高 = 选中语音芯片）。门控由 BSP 层在每次发码
 *      前后自动开关，业务层不用管；但发码期间（一帧约 13ms）不能同时刷 EPD
 *      或读字库，请把这几类调用放在同一个任务里顺序执行。
 */

#ifndef __WT588F_H__
#define __WT588F_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= 通信方式 ========================= */

typedef enum {
    WT588F_WIRE_ONE = 0,        /* 一线串口：只占用 DATA1（芯片 PIN2） */
    WT588F_WIRE_TWO = 1,        /* 两线串口：CLK（PIN2）+ DATA2（PIN3） */
} wt588f_wire_t;

/* ========================= 发码方式 ========================= */

typedef enum {
    WT588F_ADDR_SINGLE_BYTE = 0,    /* 单字节发码：地址 0~223，命令 0x00~0xFF */
    WT588F_ADDR_DOUBLE_BYTE = 1,    /* 双字节发码：地址 0~999，命令 0xFFxx / 0x000x */
} wt588f_addr_mode_t;

/* 上电默认值：工程里地址段数 < 224 时用单字节，>= 224 时改成 DOUBLE */
#define WT588F_DEFAULT_ADDR_MODE        WT588F_ADDR_SINGLE_BYTE

/* 上电默认通信方式（与芯片烧写时设定的一致） */
#define WT588F_DEFAULT_WIRE_MODE        WT588F_WIRE_ONE

/* ========================= 规格限制 ========================= */

#define WT588F_ADDR_MAX_SINGLE_BYTE     223     /* 单字节发码：0x00~0xDF */
#define WT588F_ADDR_MAX_DOUBLE_BYTE     999     /* 双字节发码：0x0000~0x03E7 */
#define WT588F_VOLUME_MAX               15      /* E0(最小)~EF(最大)，共 16 级 */
#define WT588F_CHAIN_MAX                64      /* 连码最多 64 段 */

/* 发码后 BUSY 变化最慢 65ms（DAC 模式），等“开始播放”给 120ms 余量 */
#define WT588F_BUSY_START_TIMEOUT_MS    120

/* ========================= 工程信息类型 ========================= */

typedef enum {
    WT588F_INFO_MAIN_PROGRAM = 0,   /* F9 00：语音芯片主控程序编号 */
    WT588F_INFO_PROJECT_NO   = 1,   /* F9 01：Bin 文件工程编号 */
    WT588F_INFO_VERSION_NO   = 2,   /* F9 02：Bin 文件版本号 */
} wt588f_info_t;

/* ========================= 初始化 / 配置 ========================= */

/**
 * @brief  初始化语音芯片（BSP 层 + 默认通信方式 + 默认发码方式）
 * @return ESP_OK 成功；其余为 BSP 层错误码
 */
esp_err_t WT588F_Init(void);

/**
 * @brief  释放总线（引脚恢复输入），再次使用需重新 Init
 */
esp_err_t WT588F_DeInit(void);

/**
 * @brief  是否已初始化
 */
bool WT588F_IsReady(void);

/**
 * @brief  设置通信方式（一线 / 两线），必须与芯片烧写时的设定一致
 */
esp_err_t WT588F_SetWireMode(wt588f_wire_t mode);

/**
 * @brief  设置发码方式（单字节 / 双字节），必须与语音工程的地址段数对应
 */
esp_err_t WT588F_SetAddressMode(wt588f_addr_mode_t mode);

/**
 * @brief  当前发码方式
 */
wt588f_addr_mode_t WT588F_GetAddressMode(void);

/**
 * @brief  当前音量（最近一次 SetVolume 的值；芯片本身不支持回读音量）
 */
uint8_t WT588F_GetVolume(void);

/* ========================= 播放控制 ========================= */

/**
 * @brief  播放指定地址的语音（不带 F3 前缀，会立即打断当前播放）
 * @param  addr 语音地址，单字节模式 0~223，双字节模式 0~999
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 地址超出当前发码方式范围
 */
esp_err_t WT588F_Play(uint16_t addr);

/**
 * @brief  连码播放：F3+addr[0]，F3+addr[1]，…… 最多 64 段
 *
 * 播放期间再次发送连码不会打断当前连码，会排在后面播。
 *
 * @param  addr  地址数组（每段都会自动加 F3 前缀）
 * @param  count 段数，1~WT588F_CHAIN_MAX
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数/地址非法
 */
esp_err_t WT588F_PlayChain(const uint16_t *addr, uint8_t count);

/**
 * @brief  连码循环播放：F4 01 + addr[0..count-1]，循环直到取消
 * @param  addr  地址数组（不带 F3 前缀，直接跟地址）
 * @param  count 段数，1~WT588F_CHAIN_MAX
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数/地址非法
 */
esp_err_t WT588F_PlayChainLoop(const uint16_t *addr, uint8_t count);

/**
 * @brief  取消连码循环播放（F4 00）
 */
esp_err_t WT588F_StopChainLoop(void);

/**
 * @brief  停止播放当前语音（FE）
 *
 * 注意：停止后如果之前开了 F2 单曲循环，也一并失效。
 */
esp_err_t WT588F_Stop(void);

/**
 * @brief  循环播放当前语音（F2）
 *
 * 规格书要求：先发播放指令（WT588F_Play），再发本命令；
 * 再次发送本命令可停掉当前循环。
 */
esp_err_t WT588F_Loop(void);

/**
 * @brief  设置音量（E0~EF / FFE0~FFEF）
 * @param  volume 0（最小）~15（最大）
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 超出 0~15
 *
 * 芯片不记忆音量：断电重启后恢复最大音量。
 */
esp_err_t WT588F_SetVolume(uint8_t volume);

/* ========================= 状态查询 / 等待 ========================= */

/**
 * @brief  是否正在播放（读 BUSY 脚）
 *
 * BUSY 未接线时恒返回 false。
 */
bool WT588F_IsBusy(void);

/**
 * @brief  等待当前语音播完
 * @param  timeout_ms 超时时间；0 = 只查一次
 * @return ESP_OK 已播完；ESP_ERR_TIMEOUT 超时；ESP_ERR_NOT_SUPPORTED 未接 BUSY
 */
esp_err_t WT588F_WaitIdle(uint32_t timeout_ms);

/**
 * @brief  播放指定语音并等待播完
 *
 * 流程：Play(addr) -> 等 BUSY 变忙（最多 WT588F_BUSY_START_TIMEOUT_MS）
 *       -> 等 BUSY 变空闲（受 timeout_ms 限制）。
 *       适合“提示音播完再继续”的场景；不关心时长时用 WT588F_Play() 即可。
 *
 * @param  addr       语音地址
 * @param  timeout_ms 整个播放过程的最长等待时间
 * @return ESP_OK 已播完
 *         ESP_ERR_TIMEOUT 超时
 *         ESP_ERR_NOT_SUPPORTED 未接 BUSY（此时只完成了发码）
 *         ESP_ERR_INVALID_ARG 地址非法
 */
esp_err_t WT588F_PlayAndWait(uint16_t addr, uint32_t timeout_ms);

/* ========================= 工程信息回读 ========================= */

/**
 * @brief  回读工程信息（第 8 章：F9 + 子命令，BUSY 脚以 9600 8N1 返回）
 *
 * 只在空闲状态有效；回读期间 BUSY 脚会被切换成 UART RX，结束后自动还原。
 * 返回内容是变长文本（工程编号/版本号），这里按“字节间空闲 gap_ms 即结束”
 * 的方式收数据。
 *
 * @param  type     信息类型
 * @param  buf      接收缓冲
 * @param  buf_size 缓冲大小
 * @param  out_len  实际收到字节数，可为 NULL
 * @param  gap_ms   字节间空闲超时，0 = BSP 默认（200ms）
 * @return ESP_OK 收到数据
 *         ESP_ERR_TIMEOUT 没收到数据
 *         ESP_ERR_INVALID_STATE 芯片正在播放 / BSP 未初始化 / UART 端口被占
 *         ESP_ERR_NOT_SUPPORTED 未接 BUSY
 *         ESP_ERR_INVALID_ARG 参数非法
 */
esp_err_t WT588F_ReadInfo(wt588f_info_t type, uint8_t *buf, size_t buf_size,
                          size_t *out_len, uint32_t gap_ms);

/* ========================= 原始指令 ========================= */

/**
 * @brief  直接发送一条原始指令（厂商后续新增命令 / 自定义用法）
 *
 * 单字节模式只接受 8 位（>0xFF 返回 ESP_ERR_INVALID_ARG）；
 * 双字节模式按 16 位发送（高字节先发）。
 *
 * @param  code 指令码（如 0xFFF2 / 0xF2）
 */
esp_err_t WT588F_SendRaw(uint16_t code);

#ifdef __cplusplus
}
#endif

#endif /* __WT588F_H__ */

