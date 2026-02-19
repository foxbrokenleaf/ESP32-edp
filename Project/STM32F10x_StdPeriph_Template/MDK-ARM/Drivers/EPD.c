#include "EPD.h"
#include "EPD_Data.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include "stm32f10x.h"

/**
  * 数据存储格式：
  * 纵向8点，高位在下，先从左到右，再从上到下
  * 每一个Bit对应一个像素点
  * 
  * 坐标轴定义：
  * 左上角为(0, 0)点
  * 横向向右为X轴，取值范围：0~151
  * 纵向向下为Y轴，取值范围：0~295
  * 
  *       0             X轴           151 
  *      .-------------------------------> (152,0)
  *    0 |
  *      |
  *      |
  *      |
  *  Y轴 |
  *      |
  *      |
  *      |
  *  295 |
  *      v (0,296)
  */

/*全局变量*********************/
/**
  * EPD显存数组
  * 所有的显示函数，都只是对此显存数组进行读写
  * 随后调用EPD_Update函数或EPD_UpdateArea函数
  * 才会将显存数组的数据发送到EPD硬件，进行显示
  */
uint8_t EPD_DisplayBuf[EPD_HEIGHT][EPD_COLUMN_BYTES];

/*引脚定义（根据实际硬件连接修改）*********************/
#define EPD_BUSY_PORT   GPIOA
#define EPD_BUSY_PIN    GPIO_Pin_5
#define EPD_RST_PORT    GPIOA
#define EPD_RST_PIN     GPIO_Pin_4
#define EPD_DC_PORT     GPIOA
#define EPD_DC_PIN      GPIO_Pin_3
#define EPD_CS_PORT     GPIOA
#define EPD_CS_PIN      GPIO_Pin_2
#define EPD_SCL_PORT    GPIOA
#define EPD_SCL_PIN     GPIO_Pin_1
#define EPD_SDA_PORT    GPIOA
#define EPD_SDA_PIN     GPIO_Pin_0

/*引脚操作宏*********************/
#define EPD_BUSY_READ()      GPIO_ReadInputDataBit(EPD_BUSY_PORT, EPD_BUSY_PIN)
#define EPD_RST_HIGH()       GPIO_SetBits(EPD_RST_PORT, EPD_RST_PIN)
#define EPD_RST_LOW()        GPIO_ResetBits(EPD_RST_PORT, EPD_RST_PIN)
#define EPD_DC_HIGH()        GPIO_SetBits(EPD_DC_PORT, EPD_DC_PIN)
#define EPD_DC_LOW()         GPIO_ResetBits(EPD_DC_PORT, EPD_DC_PIN)
#define EPD_CS_HIGH()        GPIO_SetBits(EPD_CS_PORT, EPD_CS_PIN)
#define EPD_CS_LOW()         GPIO_ResetBits(EPD_CS_PORT, EPD_CS_PIN)
#define EPD_SCL_HIGH()       GPIO_SetBits(EPD_SCL_PORT, EPD_SCL_PIN)
#define EPD_SCL_LOW()        GPIO_ResetBits(EPD_SCL_PORT, EPD_SCL_PIN)
#define EPD_SDA_HIGH()       GPIO_SetBits(EPD_SDA_PORT, EPD_SDA_PIN)
#define EPD_SDA_LOW()        GPIO_ResetBits(EPD_SDA_PORT, EPD_SDA_PIN)

/*命令定义*********************/
#define CMD_SOFT_RESET              0x00
#define CMD_PSR                     0x00
#define CMD_WRITE_TEMPERATURE       0xE5
#define CMD_APPLY_TEMPERATURE       0xE0
#define CMD_WRITE_IMAGE_RAM_BW       0x10
#define CMD_WRITE_IMAGE_RAM_RW       0x13
#define CMD_POWER_ON                 0x04
#define CMD_DISPLAY_REFRESH          0x12
#define CMD_TURN_OFF_DC              0x02

/*延时函数（简单实现）*********************/
static void delay_ms(uint32_t ms)
{
    uint32_t i;
    while(ms--)
    {
        for(i = 0; i < 8000; i++);
    }
}

static void delay_us(uint32_t us)
{
    uint32_t i;
    while(us--)
    {
        for(i = 0; i < 8; i++);
    }
}

/*工具函数*********************/

/**
  * 函    数：次方函数
  * 参    数：X 底数，Y 指数
  * 返 回 值：X的Y次方
  */
static uint32_t EPD_Pow(uint32_t X, uint32_t Y)
{
    uint32_t Result = 1;
    while(Y--)
    {
        Result *= X;
    }
    return Result;
}

/**
  * 函    数：判断指定点是否在指定多边形内部
  * 参    数：nvert 多边形的顶点数
  * 参    数：vertx verty 包含多边形顶点的x和y坐标的数组
  * 参    数：testx testy 测试点的X和y坐标
  * 返 回 值：指定点是否在指定多边形内部，1：在内部，0：不在内部
  */
static uint8_t EPD_pnpoly(uint8_t nvert, int16_t *vertx, int16_t *verty, int16_t testx, int16_t testy)
{
    int16_t i, j, c = 0;
    
    for (i = 0, j = nvert - 1; i < nvert; j = i++)
    {
        if (((verty[i] > testy) != (verty[j] > testy)) &&
            (testx < (vertx[j] - vertx[i]) * (testy - verty[i]) / (verty[j] - verty[i]) + vertx[i]))
        {
            c = !c;
        }
    }
    return c;
}

/**
  * 函    数：判断指定点是否在指定角度内部
  * 参    数：X Y 指定点的坐标
  * 参    数：StartAngle EndAngle 起始角度和终止角度，范围：-180~180
  *           水平向右为0度，水平向左为180度或-180度，下方为正数，上方为负数，顺时针旋转
  * 返 回 值：指定点是否在指定角度内部，1：在内部，0：不在内部
  */
static uint8_t EPD_IsInAngle(int16_t X, int16_t Y, int16_t StartAngle, int16_t EndAngle)
{
    int16_t PointAngle;
    PointAngle = atan2(Y, X) / 3.14 * 180;
    if (StartAngle < EndAngle)
    {
        if (PointAngle >= StartAngle && PointAngle <= EndAngle)
        {
            return 1;
        }
    }
    else
    {
        if (PointAngle >= StartAngle || PointAngle <= EndAngle)
        {
            return 1;
        }
    }
    return 0;
}

/*引脚配置*********************/

/**
  * 函    数：EPD写SCL高低电平
  * 参    数：BitValue 要写入SCL的电平值，范围：0/1
  * 返 回 值：无
  */
void EPD_W_SCL(uint8_t BitValue)
{
    if(BitValue)
        EPD_SCL_HIGH();
    else
        EPD_SCL_LOW();
}

/**
  * 函    数：EPD写SDA高低电平
  * 参    数：BitValue 要写入SDA的电平值，范围：0/1
  * 返 回 值：无
  */
void EPD_W_SDA(uint8_t BitValue)
{
    if(BitValue)
        EPD_SDA_HIGH();
    else
        EPD_SDA_LOW();
}

/**
  * 函    数：EPD引脚初始化
  * 参    数：无
  * 返 回 值：无
  */
void EPD_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    uint32_t i, j;
    
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    
    /*配置BUSY为输入上拉*/
    GPIO_InitStructure.GPIO_Pin = EPD_BUSY_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(EPD_BUSY_PORT, &GPIO_InitStructure);
    
    /*配置其他引脚为推挽输出*/
    GPIO_InitStructure.GPIO_Pin = EPD_RST_PIN | EPD_DC_PIN | EPD_CS_PIN | 
                                  EPD_SCL_PIN | EPD_SDA_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(EPD_RST_PORT, &GPIO_InitStructure);
    
    /*在初始化前，加入适量延时，待EPD供电稳定*/
    for (i = 0; i < 1000; i ++)
    {
        for (j = 0; j < 1000; j ++);
    }
    
    /*初始状态*/
    EPD_RST_HIGH();
    EPD_DC_HIGH();
    EPD_CS_HIGH();
    EPD_SCL_HIGH();
    EPD_SDA_HIGH();
}

/*通信协议*********************/

/**
  * 函    数：SPI发送一个字节
  * 参    数：Byte 要发送的一个字节数据，范围：0x00~0xFF
  * 返 回 值：无
  */
void EPD_SPI_SendByte(uint8_t Byte)
{
    uint8_t i;
    
    for(i = 0; i < 8; i++)
    {
        EPD_W_SCL(0);
        EPD_W_SDA(!!(Byte & (0x80 >> i)));
        delay_us(1);
        EPD_W_SCL(1);
        delay_us(1);
        EPD_W_SCL(0);
    }
}

/**
  * 函    数：EPD写命令
  * 参    数：Command 要写入的命令值，范围：0x00~0xFF
  * 返 回 值：无
  */
void EPD_WriteCommand(uint8_t Command)
{
    EPD_CS_LOW();
    EPD_DC_LOW();           // 命令模式
    EPD_SPI_SendByte(Command);
    EPD_CS_HIGH();
}

/**
  * 函    数：EPD写数据
  * 参    数：Data 要写入的数据值，范围：0x00~0xFF
  * 返 回 值：无
  */
void EPD_WriteData(uint8_t Data)
{
    EPD_CS_LOW();
    EPD_DC_HIGH();          // 数据模式
    EPD_SPI_SendByte(Data);
    EPD_CS_HIGH();
}

/**
  * 函    数：EPD等待BUSY信号
  * 参    数：无
  * 返 回 值：无
  */
void EPD_BUSY_Wait(void)
{
    while(EPD_BUSY_READ() != Bit_SET)
    {
        delay_ms(10);
    }
}

/*硬件配置*********************/

/**
  * 函    数：EPD硬复位
  * 参    数：无
  * 返 回 值：无
  */
void EPD_COG_Reset(void)
{
    delay_ms(5);
    EPD_RST_HIGH();
    delay_ms(5);
    EPD_RST_LOW();
    delay_ms(10);
    EPD_RST_HIGH();
    delay_ms(5);
    EPD_CS_HIGH();
    delay_ms(5);
}

/**
  * 函    数：EPD SRAM模式（刷新显示）
  * 参    数：无
  * 返 回 值：无
  */
void EPD_SRAM_Mode(void)
{
    delay_ms(50);
    EPD_WriteCommand(CMD_POWER_ON);
    EPD_WriteCommand(CMD_POWER_ON);
    delay_ms(5);
    EPD_BUSY_Wait();
    
    EPD_WriteCommand(CMD_DISPLAY_REFRESH);
    EPD_WriteCommand(CMD_DISPLAY_REFRESH);
    delay_ms(5);
    EPD_BUSY_Wait();
    
    EPD_WriteCommand(CMD_TURN_OFF_DC);
    EPD_WriteCommand(CMD_TURN_OFF_DC);
    EPD_BUSY_Wait();
    
    EPD_DC_LOW();
    EPD_CS_LOW();
    EPD_RST_LOW();
    EPD_CS_HIGH();
}

/**
  * 函    数：EPD初始化
  * 参    数：无
  * 返 回 值：无
  */
void EPD_Init(void)
{
    EPD_GPIO_Init();
    
    EPD_COG_Reset();
    
    EPD_WriteCommand(CMD_SOFT_RESET);
    EPD_WriteData(0xE0);
    delay_ms(5);
    
    EPD_WriteCommand(CMD_WRITE_TEMPERATURE);
    EPD_WriteData(0x19);
    
    EPD_WriteCommand(CMD_APPLY_TEMPERATURE);
    EPD_WriteData(0x02);
    
    EPD_WriteCommand(CMD_PSR);
    EPD_WriteData(0xCF);
    EPD_WriteData(0x8D);
    
    EPD_Clear();                // 清空显存数组
}

/*功能函数*********************/

/**
  * 函    数：将EPD显存数组更新到EPD屏幕
  * 参    数：无
  * 返 回 值：无
  */
void EPD_Update(void)
{
    uint16_t i, j;
    
    /*写入黑白图像数据*/
    EPD_WriteCommand(CMD_WRITE_IMAGE_RAM_BW);
    for(i = 0; i < EPD_HEIGHT; i++)
    {
        for(j = 0; j < EPD_COLUMN_BYTES; j++)
        {
            EPD_WriteData(EPD_DisplayBuf[i][j]);
        }
    }
    
    /*红色通道全0（不使用）*/
    EPD_WriteCommand(CMD_WRITE_IMAGE_RAM_RW);
    for(i = 0; i < EPD_HEIGHT; i++)
    {
        for(j = 0; j < EPD_COLUMN_BYTES; j++)
        {
            EPD_WriteData(0x00);
        }
    }
    
    EPD_SRAM_Mode();                // 刷新显示
}

/**
  * 函    数：将EPD显存数组部分更新到EPD屏幕
  * 参    数：X 指定区域左上角的横坐标，屏幕区域：0~151
  * 参    数：Y 指定区域左上角的纵坐标，屏幕区域：0~295
  * 参    数：Width 指定区域的宽度
  * 参    数：Height 指定区域的高度
  * 返 回 值：无
  */
void EPD_UpdateArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height)
{
    /*墨水屏刷新需要整屏刷新，这里直接调用全屏更新*/
    /*如果希望优化，可以只更新指定区域的数据*/
    EPD_Update();
}

/**
  * 函    数：将EPD显存数组全部清零
  * 参    数：无
  * 返 回 值：无
  */
void EPD_Clear(void)
{
    uint16_t i, j;
    for(i = 0; i < EPD_HEIGHT; i++)
    {
        for(j = 0; j < EPD_COLUMN_BYTES; j++)
        {
            EPD_DisplayBuf[i][j] = 0x00;
        }
    }
}

/**
  * 函    数：将EPD显存数组部分清零
  * 参    数：X 指定区域左上角的横坐标
  * 参    数：Y 指定区域左上角的纵坐标
  * 参    数：Width 指定区域的宽度
  * 参    数：Height 指定区域的高度
  * 返 回 值：无
  */
void EPD_ClearArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height)
{
    int16_t i, j;
    uint8_t byteIndex, bitIndex;
    
    for(j = Y; j < Y + Height; j++)
    {
        for(i = X; i < X + Width; i++)
        {
            if(i >= 0 && i < EPD_WIDTH && j >= 0 && j < EPD_HEIGHT)
            {
                byteIndex = i / 8;
                bitIndex = 7 - (i % 8);
                EPD_DisplayBuf[j][byteIndex] &= ~(0x01 << bitIndex);
            }
        }
    }
}

/**
  * 函    数：将EPD显存数组全部取反
  * 参    数：无
  * 返 回 值：无
  */
void EPD_Reverse(void)
{
    uint16_t i, j;
    for(i = 0; i < EPD_HEIGHT; i++)
    {
        for(j = 0; j < EPD_COLUMN_BYTES; j++)
        {
            EPD_DisplayBuf[i][j] ^= 0xFF;
        }
    }
}

/**
  * 函    数：将EPD显存数组部分取反
  * 参    数：X 指定区域左上角的横坐标
  * 参    数：Y 指定区域左上角的纵坐标
  * 参    数：Width 指定区域的宽度
  * 参    数：Height 指定区域的高度
  * 返 回 值：无
  */
void EPD_ReverseArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height)
{
    int16_t i, j;
    uint8_t byteIndex, bitIndex;
    
    for(j = Y; j < Y + Height; j++)
    {
        for(i = X; i < X + Width; i++)
        {
            if(i >= 0 && i < EPD_WIDTH && j >= 0 && j < EPD_HEIGHT)
            {
                byteIndex = i / 8;
                bitIndex = 7 - (i % 8);
                EPD_DisplayBuf[j][byteIndex] ^= (0x01 << bitIndex);
            }
        }
    }
}

/**
  * 函    数：EPD显示一个字符
  * 参    数：X 指定字符左上角的横坐标
  * 参    数：Y 指定字符左上角的纵坐标
  * 参    数：Char 指定要显示的字符
  * 参    数：FontSize 指定字体大小
  *           范围：EPD_8X16		宽8像素，高16像素
  *                 EPD_6X8		宽6像素，高8像素
  * 返 回 值：无
  */
void EPD_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize)
{
    if(FontSize == EPD_8X16)
    {
        /*将ASCII字模库EPD_F8x16的指定数据以8*16的图像格式显示*/
        EPD_ShowImage(X, Y, 8, 16, EPD_F8x16[Char - ' ']);
    }
    else if(FontSize == EPD_6X8)
    {
        /*将ASCII字模库EPD_F6x8的指定数据以6*8的图像格式显示*/
        EPD_ShowImage(X, Y, 6, 8, EPD_F6x8[Char - ' ']);
    }
}

/**
  * 函    数：EPD显示字符串
  * 参    数：X 指定字符串左上角的横坐标
  * 参    数：Y 指定字符串左上角的纵坐标
  * 参    数：String 指定要显示的字符串
  * 参    数：FontSize 指定字体大小
  * 返 回 值：无
  */
void EPD_ShowString(int16_t X, int16_t Y, char *String, uint8_t FontSize)
{
    uint16_t XOffset = 0;
    
    while(*String != '\0')
    {
        EPD_ShowChar(X + XOffset, Y, *String, FontSize);
        XOffset += FontSize;
        String++;
    }
}

/**
  * 函    数：EPD显示数字（十进制，正整数）
  * 参    数：X 指定数字左上角的横坐标
  * 参    数：Y 指定数字左上角的纵坐标
  * 参    数：Number 指定要显示的数字
  * 参    数：Length 指定数字的长度
  * 参    数：FontSize 指定字体大小
  * 返 回 值：无
  */
void EPD_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize)
{
    uint8_t i;
    for(i = 0; i < Length; i++)
    {
        EPD_ShowChar(X + i * FontSize, Y, 
                     Number / EPD_Pow(10, Length - i - 1) % 10 + '0', FontSize);
    }
}

/**
  * 函    数：EPD显示有符号数字（十进制，整数）
  * 参    数：X 指定数字左上角的横坐标
  * 参    数：Y 指定数字左上角的纵坐标
  * 参    数：Number 指定要显示的数字
  * 参    数：Length 指定数字的长度
  * 参    数：FontSize 指定字体大小
  * 返 回 值：无
  */
void EPD_ShowSignedNum(int16_t X, int16_t Y, int32_t Number, uint8_t Length, uint8_t FontSize)
{
    uint8_t i;
    uint32_t Number1;
    
    if(Number >= 0)
    {
        EPD_ShowChar(X, Y, '+', FontSize);
        Number1 = Number;
    }
    else
    {
        EPD_ShowChar(X, Y, '-', FontSize);
        Number1 = -Number;
    }
    
    for(i = 0; i < Length; i++)
    {
        EPD_ShowChar(X + (i + 1) * FontSize, Y, 
                     Number1 / EPD_Pow(10, Length - i - 1) % 10 + '0', FontSize);
    }
}

/**
  * 函    数：EPD显示十六进制数字（十六进制，正整数）
  * 参    数：X 指定数字左上角的横坐标
  * 参    数：Y 指定数字左上角的纵坐标
  * 参    数：Number 指定要显示的数字
  * 参    数：Length 指定数字的长度
  * 参    数：FontSize 指定字体大小
  * 返 回 值：无
  */
void EPD_ShowHexNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize)
{
    uint8_t i, SingleNumber;
    for(i = 0; i < Length; i++)
    {
        SingleNumber = Number / EPD_Pow(16, Length - i - 1) % 16;
        
        if(SingleNumber < 10)
        {
            EPD_ShowChar(X + i * FontSize, Y, SingleNumber + '0', FontSize);
        }
        else
        {
            EPD_ShowChar(X + i * FontSize, Y, SingleNumber - 10 + 'A', FontSize);
        }
    }
}

/**
  * 函    数：EPD显示二进制数字（二进制，正整数）
  * 参    数：X 指定数字左上角的横坐标
  * 参    数：Y 指定数字左上角的纵坐标
  * 参    数：Number 指定要显示的数字
  * 参    数：Length 指定数字的长度
  * 参    数：FontSize 指定字体大小
  * 返 回 值：无
  */
void EPD_ShowBinNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize)
{
    uint8_t i;
    for(i = 0; i < Length; i++)
    {
        EPD_ShowChar(X + i * FontSize, Y, 
                     Number / EPD_Pow(2, Length - i - 1) % 2 + '0', FontSize);
    }
}

/**
  * 函    数：EPD显示浮点数字（十进制，小数）
  * 参    数：X 指定数字左上角的横坐标
  * 参    数：Y 指定数字左上角的纵坐标
  * 参    数：Number 指定要显示的数字
  * 参    数：IntLength 整数位长度
  * 参    数：FraLength 小数位长度
  * 参    数：FontSize 指定字体大小
  * 返 回 值：无
  */
void EPD_ShowFloatNum(int16_t X, int16_t Y, double Number, uint8_t IntLength, uint8_t FraLength, uint8_t FontSize)
{
    uint32_t PowNum, IntNum, FraNum;
    
    if(Number >= 0)
    {
        EPD_ShowChar(X, Y, '+', FontSize);
    }
    else
    {
        EPD_ShowChar(X, Y, '-', FontSize);
        Number = -Number;
    }
    
    IntNum = Number;
    Number -= IntNum;
    PowNum = EPD_Pow(10, FraLength);
    FraNum = round(Number * PowNum);
    IntNum += FraNum / PowNum;
    
    EPD_ShowNum(X + FontSize, Y, IntNum, IntLength, FontSize);
    EPD_ShowChar(X + (IntLength + 1) * FontSize, Y, '.', FontSize);
    EPD_ShowNum(X + (IntLength + 2) * FontSize, Y, FraNum, FraLength, FontSize);
}

/**
  * 函    数：EPD显示图像
  * 参    数：X 指定图像左上角的横坐标
  * 参    数：Y 指定图像左上角的纵坐标
  * 参    数：Width 指定图像的宽度
  * 参    数：Height 指定图像的高度
  * 参    数：Image 指定要显示的图像
  * 返 回 值：无
  */
void EPD_ShowImage(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image)
{
    uint16_t i, j;
    uint8_t byteIndex, bitIndex;
    uint8_t pixel;
    
    /*将图像所在区域清空*/
    EPD_ClearArea(X, Y, Width, Height);
    
    for(j = 0; j < Height; j++)
    {
        for(i = 0; i < Width; i++)
        {
            if(X + i >= 0 && X + i < EPD_WIDTH && Y + j >= 0 && Y + j < EPD_HEIGHT)
            {
                byteIndex = (X + i) / 8;
                bitIndex = 7 - ((X + i) % 8);
                
                /*读取图像数据中的像素*/
                /*图像数据按字节存储，每个字节8个像素，高位对应左边像素*/
                pixel = (Image[j * ((Width + 7) / 8) + i / 8] >> (7 - (i % 8))) & 0x01;
                
                if(pixel)
                {
                    EPD_DisplayBuf[Y + j][byteIndex] |= (0x01 << bitIndex);
                }
            }
        }
    }
}

/**
  * 函    数：EPD使用printf函数打印格式化字符串
  * 参    数：X 指定格式化字符串左上角的横坐标
  * 参    数：Y 指定格式化字符串左上角的纵坐标
  * 参    数：FontSize 指定字体大小
  * 参    数：format 指定要显示的格式化字符串
  * 返 回 值：无
  */
void EPD_Printf(int16_t X, int16_t Y, uint8_t FontSize, char *format, ...)
{
    char String[256];
    va_list arg;
    va_start(arg, format);
    vsprintf(String, format, arg);
    va_end(arg);
    EPD_ShowString(X, Y, String, FontSize);
}

/**
  * 函    数：EPD在指定位置画一个点
  * 参    数：X 指定点的横坐标
  * 参    数：Y 指定点的纵坐标
  * 返 回 值：无
  */
void EPD_DrawPoint(int16_t X, int16_t Y)
{
    uint8_t byteIndex, bitIndex;
    
    if(X >= 0 && X < EPD_WIDTH && Y >= 0 && Y < EPD_HEIGHT)
    {
        byteIndex = X / 8;
        bitIndex = 7 - (X % 8);
        EPD_DisplayBuf[Y][byteIndex] |= (0x01 << bitIndex);
    }
}

/**
  * 函    数：EPD获取指定位置点的值
  * 参    数：X 指定点的横坐标
  * 参    数：Y 指定点的纵坐标
  * 返 回 值：指定位置点是否处于点亮状态，1：点亮，0：熄灭
  */
uint8_t EPD_GetPoint(int16_t X, int16_t Y)
{
    uint8_t byteIndex, bitIndex;
    
    if(X >= 0 && X < EPD_WIDTH && Y >= 0 && Y < EPD_HEIGHT)
    {
        byteIndex = X / 8;
        bitIndex = 7 - (X % 8);
        if(EPD_DisplayBuf[Y][byteIndex] & (0x01 << bitIndex))
        {
            return 1;
        }
    }
    return 0;
}

/**
  * 函    数：EPD画线
  * 参    数：X0, Y0 起点坐标
  * 参    数：X1, Y1 终点坐标
  * 返 回 值：无
  */
void EPD_DrawLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1)
{
    int16_t x, y, dx, dy, d, incrE, incrNE, temp;
    int16_t x0 = X0, y0 = Y0, x1 = X1, y1 = Y1;
    uint8_t yflag = 0, xyflag = 0;
    
    if(y0 == y1)        // 横线
    {
        if(x0 > x1) {temp = x0; x0 = x1; x1 = temp;}
        for(x = x0; x <= x1; x++)
        {
            EPD_DrawPoint(x, y0);
        }
    }
    else if(x0 == x1)   // 竖线
    {
        if(y0 > y1) {temp = y0; y0 = y1; y1 = temp;}
        for(y = y0; y <= y1; y++)
        {
            EPD_DrawPoint(x0, y);
        }
    }
    else                // 斜线（Bresenham算法）
    {
        if(x0 > x1)
        {
            temp = x0; x0 = x1; x1 = temp;
            temp = y0; y0 = y1; y1 = temp;
        }
        
        if(y0 > y1)
        {
            y0 = -y0;
            y1 = -y1;
            yflag = 1;
        }
        
        if(y1 - y0 > x1 - x0)
        {
            temp = x0; x0 = y0; y0 = temp;
            temp = x1; x1 = y1; y1 = temp;
            xyflag = 1;
        }
        
        dx = x1 - x0;
        dy = y1 - y0;
        incrE = 2 * dy;
        incrNE = 2 * (dy - dx);
        d = 2 * dy - dx;
        x = x0;
        y = y0;
        
        if(yflag && xyflag){EPD_DrawPoint(y, -x);}
        else if(yflag)     {EPD_DrawPoint(x, -y);}
        else if(xyflag)    {EPD_DrawPoint(y, x);}
        else               {EPD_DrawPoint(x, y);}
        
        while(x < x1)
        {
            x++;
            if(d < 0)
            {
                d += incrE;
            }
            else
            {
                y++;
                d += incrNE;
            }
            
            if(yflag && xyflag){EPD_DrawPoint(y, -x);}
            else if(yflag)     {EPD_DrawPoint(x, -y);}
            else if(xyflag)    {EPD_DrawPoint(y, x);}
            else               {EPD_DrawPoint(x, y);}
        }
    }
}

/**
  * 函    数：EPD画虚线
  * 参    数：X0, Y0 起点坐标
  * 参    数：X1, Y1 终点坐标
  * 返 回 值：无
  */
void EPD_DrawDashedLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1)
{
    const uint8_t dashLength = 3;
    const uint8_t gapLength = 2;
    
    int16_t x, y, dx, dy, d, incrE, incrNE, temp;
    int16_t x0 = X0, y0 = Y0, x1 = X1, y1 = Y1;
    uint8_t yflag = 0, xyflag = 0;
    uint16_t pointCount = 0;
    
    if(y0 == y1)
    {
        if(x0 > x1) {temp = x0; x0 = x1; x1 = temp;}
        
        for(x = x0; x <= x1; x++)
        {
            if(pointCount < dashLength)
            {
                EPD_DrawPoint(x, y0);
            }
            pointCount = (pointCount + 1) % (dashLength + gapLength);
        }
    }
    else if(x0 == x1)
    {
        if(y0 > y1) {temp = y0; y0 = y1; y1 = temp;}
        
        for(y = y0; y <= y1; y++)
        {
            if(pointCount < dashLength)
            {
                EPD_DrawPoint(x0, y);
            }
            pointCount = (pointCount + 1) % (dashLength + gapLength);
        }
    }
    else
    {
        if(x0 > x1)
        {
            temp = x0; x0 = x1; x1 = temp;
            temp = y0; y0 = y1; y1 = temp;
        }
        
        if(y0 > y1)
        {
            y0 = -y0;
            y1 = -y1;
            yflag = 1;
        }
        
        if(y1 - y0 > x1 - x0)
        {
            temp = x0; x0 = y0; y0 = temp;
            temp = x1; x1 = y1; y1 = temp;
            xyflag = 1;
        }
        
        dx = x1 - x0;
        dy = y1 - y0;
        incrE = 2 * dy;
        incrNE = 2 * (dy - dx);
        d = 2 * dy - dx;
        x = x0;
        y = y0;
        
        if(pointCount < dashLength)
        {
            if(yflag && xyflag){EPD_DrawPoint(y, -x);}
            else if(yflag)     {EPD_DrawPoint(x, -y);}
            else if(xyflag)    {EPD_DrawPoint(y, x);}
            else               {EPD_DrawPoint(x, y);}
        }
        pointCount = (pointCount + 1) % (dashLength + gapLength);
        
        while(x < x1)
        {
            x++;
            if(d < 0)
            {
                d += incrE;
            }
            else
            {
                y++;
                d += incrNE;
            }
            
            if(pointCount < dashLength)
            {
                if(yflag && xyflag){EPD_DrawPoint(y, -x);}
                else if(yflag)     {EPD_DrawPoint(x, -y);}
                else if(xyflag)    {EPD_DrawPoint(y, x);}
                else               {EPD_DrawPoint(x, y);}
            }
            pointCount = (pointCount + 1) % (dashLength + gapLength);
        }
    }
}

/**
  * 函    数：EPD画矩形
  * 参    数：X, Y 左上角坐标
  * 参    数：Width 宽度
  * 参    数：Height 高度
  * 参    数：IsFilled 是否填充
  * 返 回 值：无
  */
void EPD_DrawRectangle(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, uint8_t IsFilled)
{
    int16_t i, j;
    
    if(!IsFilled)
    {
        for(i = X; i < X + Width; i++)
        {
            EPD_DrawPoint(i, Y);
            EPD_DrawPoint(i, Y + Height - 1);
        }
        for(i = Y; i < Y + Height; i++)
        {
            EPD_DrawPoint(X, i);
            EPD_DrawPoint(X + Width - 1, i);
        }
    }
    else
    {
        for(i = X; i < X + Width; i++)
        {
            for(j = Y; j < Y + Height; j++)
            {
                EPD_DrawPoint(i, j);
            }
        }
    }
}

/**
  * 函    数：EPD画三角形
  * 参    数：X0,Y0, X1,Y1, X2,Y2 三个顶点坐标
  * 参    数：IsFilled 是否填充
  * 返 回 值：无
  */
void EPD_DrawTriangle(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, 
                      int16_t X2, int16_t Y2, uint8_t IsFilled)
{
    int16_t minx = X0, miny = Y0, maxx = X0, maxy = Y0;
    int16_t i, j;
    int16_t vx[] = {X0, X1, X2};
    int16_t vy[] = {Y0, Y1, Y2};
    
    if(!IsFilled)
    {
        EPD_DrawLine(X0, Y0, X1, Y1);
        EPD_DrawLine(X0, Y0, X2, Y2);
        EPD_DrawLine(X1, Y1, X2, Y2);
    }
    else
    {
        if(X1 < minx) {minx = X1;}
        if(X2 < minx) {minx = X2;}
        if(Y1 < miny) {miny = Y1;}
        if(Y2 < miny) {miny = Y2;}
        
        if(X1 > maxx) {maxx = X1;}
        if(X2 > maxx) {maxx = X2;}
        if(Y1 > maxy) {maxy = Y1;}
        if(Y2 > maxy) {maxy = Y2;}
        
        for(i = minx; i <= maxx; i++)
        {
            for(j = miny; j <= maxy; j++)
            {
                if(EPD_pnpoly(3, vx, vy, i, j))
                {
                    EPD_DrawPoint(i, j);
                }
            }
        }
    }
}

/**
  * 函    数：EPD画圆
  * 参    数：X, Y 圆心坐标
  * 参    数：Radius 半径
  * 参    数：IsFilled 是否填充
  * 返 回 值：无
  */
void EPD_DrawCircle(int16_t X, int16_t Y, uint16_t Radius, uint8_t IsFilled)
{
    int16_t x, y, d, j;
    
    d = 1 - Radius;
    x = 0;
    y = Radius;
    
    EPD_DrawPoint(X + x, Y + y);
    EPD_DrawPoint(X - x, Y - y);
    EPD_DrawPoint(X + y, Y + x);
    EPD_DrawPoint(X - y, Y - x);
    
    if(IsFilled)
    {
        for(j = -y; j < y; j++)
        {
            EPD_DrawPoint(X, Y + j);
        }
    }
    
    while(x < y)
    {
        x++;
        if(d < 0)
        {
            d += 2 * x + 1;
        }
        else
        {
            y--;
            d += 2 * (x - y) + 1;
        }
        
        EPD_DrawPoint(X + x, Y + y);
        EPD_DrawPoint(X + y, Y + x);
        EPD_DrawPoint(X - x, Y - y);
        EPD_DrawPoint(X - y, Y - x);
        EPD_DrawPoint(X + x, Y - y);
        EPD_DrawPoint(X + y, Y - x);
        EPD_DrawPoint(X - x, Y + y);
        EPD_DrawPoint(X - y, Y + x);
        
        if(IsFilled)
        {
            for(j = -y; j < y; j++)
            {
                EPD_DrawPoint(X + x, Y + j);
                EPD_DrawPoint(X - x, Y + j);
            }
            for(j = -x; j < x; j++)
            {
                EPD_DrawPoint(X - y, Y + j);
                EPD_DrawPoint(X + y, Y + j);
            }
        }
    }
}

/**
  * 函    数：EPD画圆弧
  * 参    数：X, Y 圆心坐标
  * 参    数：Radius 半径
  * 参    数：StartAngle 起始角度
  * 参    数：EndAngle 终止角度
  * 参    数：IsFilled 是否填充
  * 返 回 值：无
  */
void EPD_DrawArc(int16_t X, int16_t Y, uint8_t Radius, int16_t StartAngle, int16_t EndAngle, uint8_t IsFilled)
{
    int16_t x, y, d, j;
    
    d = 1 - Radius;
    x = 0;
    y = Radius;
    
    if(EPD_IsInAngle(x, y, StartAngle, EndAngle)) {EPD_DrawPoint(X + x, Y + y);}
    if(EPD_IsInAngle(-x, -y, StartAngle, EndAngle)) {EPD_DrawPoint(X - x, Y - y);}
    if(EPD_IsInAngle(y, x, StartAngle, EndAngle)) {EPD_DrawPoint(X + y, Y + x);}
    if(EPD_IsInAngle(-y, -x, StartAngle, EndAngle)) {EPD_DrawPoint(X - y, Y - x);}
    
    if(IsFilled)
    {
        for(j = -y; j < y; j++)
        {
            if(EPD_IsInAngle(0, j, StartAngle, EndAngle)) {EPD_DrawPoint(X, Y + j);}
        }
    }
    
    while(x < y)
    {
        x++;
        if(d < 0)
        {
            d += 2 * x + 1;
        }
        else
        {
            y--;
            d += 2 * (x - y) + 1;
        }
        
        if(EPD_IsInAngle(x, y, StartAngle, EndAngle)) {EPD_DrawPoint(X + x, Y + y);}
        if(EPD_IsInAngle(y, x, StartAngle, EndAngle)) {EPD_DrawPoint(X + y, Y + x);}
        if(EPD_IsInAngle(-x, -y, StartAngle, EndAngle)) {EPD_DrawPoint(X - x, Y - y);}
        if(EPD_IsInAngle(-y, -x, StartAngle, EndAngle)) {EPD_DrawPoint(X - y, Y - x);}
        if(EPD_IsInAngle(x, -y, StartAngle, EndAngle)) {EPD_DrawPoint(X + x, Y - y);}
        if(EPD_IsInAngle(y, -x, StartAngle, EndAngle)) {EPD_DrawPoint(X + y, Y - x);}
        if(EPD_IsInAngle(-x, y, StartAngle, EndAngle)) {EPD_DrawPoint(X - x, Y + y);}
        if(EPD_IsInAngle(-y, x, StartAngle, EndAngle)) {EPD_DrawPoint(X - y, Y + x);}
        
        if(IsFilled)
        {
            for(j = -y; j < y; j++)
            {
                if(EPD_IsInAngle(x, j, StartAngle, EndAngle)) {EPD_DrawPoint(X + x, Y + j);}
                if(EPD_IsInAngle(-x, j, StartAngle, EndAngle)) {EPD_DrawPoint(X - x, Y + j);}
            }
            for(j = -x; j < x; j++)
            {
                if(EPD_IsInAngle(-y, j, StartAngle, EndAngle)) {EPD_DrawPoint(X - y, Y + j);}
                if(EPD_IsInAngle(y, j, StartAngle, EndAngle)) {EPD_DrawPoint(X + y, Y + j);}
            }
        }
    }
}