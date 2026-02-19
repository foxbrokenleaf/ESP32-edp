#ifndef __EPD_H
#define __EPD_H

#include <stdint.h>
#include "EPD_Data.h"

/*屏幕尺寸定义*********************/
#define EPD_WIDTH               152     // 屏幕宽度像素
#define EPD_HEIGHT              296     // 屏幕高度像素
#define EPD_COLUMN_BYTES        19      // 每行字节数 (152/8)

/*字体大小定义*********************/
#define EPD_8X16                8       // 宽8像素，高16像素
#define EPD_6X8                 6       // 宽6像素，高8像素

/*填充模式定义*********************/
#define EPD_UNFILLED            0
#define EPD_FILLED               1

/*函数声明*********************/

/*初始化函数*/
void EPD_Init(void);

/*更新函数*/
void EPD_Update(void);
void EPD_UpdateArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height);

/*显存控制函数*/
void EPD_Clear(void);
void EPD_ClearArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height);
void EPD_Reverse(void);
void EPD_ReverseArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height);

/*显示函数*/
void EPD_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize);
void EPD_ShowString(int16_t X, int16_t Y, char *String, uint8_t FontSize);
void EPD_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);
void EPD_ShowSignedNum(int16_t X, int16_t Y, int32_t Number, uint8_t Length, uint8_t FontSize);
void EPD_ShowHexNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);
void EPD_ShowBinNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize);
void EPD_ShowFloatNum(int16_t X, int16_t Y, double Number, uint8_t IntLength, uint8_t FraLength, uint8_t FontSize);
void EPD_ShowImage(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image);
void EPD_Printf(int16_t X, int16_t Y, uint8_t FontSize, char *format, ...);

/*绘图函数*/
void EPD_DrawPoint(int16_t X, int16_t Y);
uint8_t EPD_GetPoint(int16_t X, int16_t Y);
void EPD_DrawLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1);
void EPD_DrawDashedLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1);
void EPD_DrawRectangle(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, uint8_t IsFilled);
void EPD_DrawTriangle(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, int16_t X2, int16_t Y2, uint8_t IsFilled);
void EPD_DrawCircle(int16_t X, int16_t Y, uint16_t Radius, uint8_t IsFilled);

/*硬件操作函数（供内部调用）*/
void EPD_WriteCommand(uint8_t Command);
void EPD_WriteData(uint8_t Data);
void EPD_BUSY_Wait(void);

#endif