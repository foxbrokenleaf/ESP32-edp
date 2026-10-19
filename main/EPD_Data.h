/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:36:25
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-10-19 23:36:15
 * @FilePath: \epd\main\EPD_Data.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef __EPD_DATA_H
#define __EPD_DATA_H

#include <stdint.h>

/* ASCII 字模数据 */
extern const uint8_t EPD_F8x16[][16];
extern const uint8_t EPD_F6x8[][6];

/* 图像测试数据 */
extern const uint8_t EPD_Image_Test[];

extern const uint8_t Font_16x16_E783AB[32];     //烫

extern const uint8_t Font_32x32_E783AB[128];    //烫

extern const uint8_t Font_32x32_E58786[128];   // 准
extern const uint8_t Font_32x32_E5A487[128];   // 备
extern const uint8_t Font_32x32_E4B88A[128];   // 上
extern const uint8_t Font_32x32_E8AFBE[128];   // 课
extern const uint8_t Font_32x32_E6ADA3[128];   // 正
extern const uint8_t Font_32x32_E59CA8[128];   // 在
extern const uint8_t Font_32x32_E697A0[128];   // 无
extern const uint8_t Font_16x16_E69599[32];   // 教 (16x16)
extern const uint8_t Font_32x32_E69599[128];   // 教 (32x32)
extern const uint8_t Font_16x16_E7A791[32];  // 科 (16x16)
extern const uint8_t Font_32x32_E7A791[128];  // 科 (32x32)
extern const uint8_t Font_32x32_E7AE97[128];  // 算
extern const uint8_t Font_32x32_E6B395[128];  // 法
extern const uint8_t Font_32x32_E58886[128];  // 分
extern const uint8_t Font_32x32_E69E90[128];  // 析
extern const uint8_t Font_32x32_E4B88E[128];  // 与
extern const uint8_t Font_32x32_E8AEBE[128];  // 设
extern const uint8_t Font_32x32_E8AEA1[128];  // 计
extern const uint8_t Font_32x32_E5AE9E[128];  // 实
extern const uint8_t Font_32x32_E8B7B5[128];  // 践
extern const uint8_t Font_32x32_E8BDAF[128];  // 软
extern const uint8_t Font_32x32_E4BBB6[128];  // 件
extern const uint8_t Font_32x32_E5B7A5[128];  // 工
extern const uint8_t Font_32x32_E7A88B[128];  // 程
extern const uint8_t Font_32x32_E9AB98[128];  // 高
extern const uint8_t Font_32x32_E7BAA7[128];  // 级
extern const uint8_t Font_32x32_E4BD93[128];  // 体
extern const uint8_t Font_32x32_E7B3BB[128];  // 系
extern const uint8_t Font_32x32_E69EB6[128];  // 架
extern const uint8_t Font_32x32_E69E84[128];  // 构
extern const uint8_t Font_32x32_E69CBA[128];  // 机
extern const uint8_t Font_32x32_E7BB84[128];  // 组
extern const uint8_t Font_32x32_E68890[128];  // 成
extern const uint8_t Font_32x32_E58E9F[128];  // 原
extern const uint8_t Font_32x32_E79086[128];  // 理
extern const uint8_t Font_32x32_E7A7BB[128];  // 移
extern const uint8_t Font_32x32_E58AA8[128];  // 动
extern const uint8_t Font_32x32_E5BA94[128];  // 应
extern const uint8_t Font_32x32_E794A8[128];  // 用
extern const uint8_t Font_32x32_E5BC80[128];  // 开
extern const uint8_t Font_32x32_E58F91[128];  // 发
extern const uint8_t Font_32x32_E68A80[128];  // 技
extern const uint8_t Font_32x32_E69CAF[128];  // 术

#endif /* __EPD_DATA_H */