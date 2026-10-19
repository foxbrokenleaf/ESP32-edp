/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-18 21:36:25
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-10-19 07:56:24
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

#endif /* __EPD_DATA_H */