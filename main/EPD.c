#include "EPD.h"
#include "EPD_Data.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

/* ==================== 引脚定义（按实际接线修改） ==================== */
#define EPD_BUSY_PIN    4
#define EPD_RST_PIN     13
#define EPD_DC_PIN      14
#define EPD_CS_PIN      27
#define EPD_SCL_PIN     26
#define EPD_SDA_PIN     25

/* ==================== 引脚操作宏（ESP-IDF） ==================== */
#define EPD_BUSY_READ()      gpio_get_level(EPD_BUSY_PIN)
#define EPD_RST_HIGH()       gpio_set_level(EPD_RST_PIN, 1)
#define EPD_RST_LOW()        gpio_set_level(EPD_RST_PIN, 0)
#define EPD_DC_HIGH()        gpio_set_level(EPD_DC_PIN, 1)
#define EPD_DC_LOW()         gpio_set_level(EPD_DC_PIN, 0)
#define EPD_CS_HIGH()        gpio_set_level(EPD_CS_PIN, 1)
#define EPD_CS_LOW()         gpio_set_level(EPD_CS_PIN, 0)
#define EPD_SCL_HIGH()       gpio_set_level(EPD_SCL_PIN, 1)
#define EPD_SCL_LOW()        gpio_set_level(EPD_SCL_PIN, 0)
#define EPD_SDA_HIGH()       gpio_set_level(EPD_SDA_PIN, 1)
#define EPD_SDA_LOW()        gpio_set_level(EPD_SDA_PIN, 0)

/* ==================== 显存（物理布局，[296][19]） ==================== */
static uint8_t EPD_DisplayBuf[EPD_PHYSICAL_HEIGHT][EPD_PHYSICAL_COLUMN_BYTES];

/* ==================== 延时（ESP-IDF） ==================== */
static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}

/* ==================== 数学工具 ==================== */
static uint32_t EPD_Pow(uint32_t X, uint32_t Y)
{
    uint32_t Result = 1;
    while (Y--) Result *= X;
    return Result;
}

static uint8_t EPD_pnpoly(uint8_t nvert, int16_t *vertx, int16_t *verty, int16_t testx, int16_t testy)
{
    int16_t i, j, c = 0;
    for (i = 0, j = nvert - 1; i < nvert; j = i++) {
        if (((verty[i] > testy) != (verty[j] > testy)) &&
            (testx < (vertx[j] - vertx[i]) * (testy - verty[i]) / (verty[j] - verty[i]) + vertx[i])) {
            c = !c;
        }
    }
    return c;
}

static uint8_t EPD_IsInAngle(int16_t X, int16_t Y, int16_t StartAngle, int16_t EndAngle)
{
    int16_t PointAngle = atan2(Y, X) / 3.14 * 180;
    if (StartAngle < EndAngle) {
        if (PointAngle >= StartAngle && PointAngle <= EndAngle) return 1;
    } else {
        if (PointAngle >= StartAngle || PointAngle <= EndAngle) return 1;
    }
    return 0;
}

/* ==================== 坐标变换（逻辑 → 物理） ==================== */
static void EPD_LogicToPhys(int16_t logicX, int16_t logicY, int16_t *physX, int16_t *physY)
{
#if (EPD_ORIENTATION == EPD_ORIENTATION_PORTRAIT)
    *physX = logicX;
    *physY = logicY;

#elif (EPD_ORIENTATION == EPD_ORIENTATION_PORTRAIT_180)
    *physX = EPD_PHYSICAL_WIDTH - 1 - logicX;
    *physY = EPD_PHYSICAL_HEIGHT - 1 - logicY;

#elif (EPD_ORIENTATION == EPD_ORIENTATION_LANDSCAPE)
    /* 横屏：顺时针旋转 90 度
     * 逻辑宽 296、高 152；物理宽 152、高 296
     * 逻辑 X(0-295) → 物理 Y(0-295)
     * 逻辑 Y(0-151) → 物理 X(0-151)（反向）
     */
    *physX = logicY;
    *physY = EPD_PHYSICAL_HEIGHT - 1 - logicX;

#elif (EPD_ORIENTATION == EPD_ORIENTATION_LANDSCAPE_180)
    *physX = EPD_PHYSICAL_WIDTH - 1 - logicY;
    *physY = logicX;

#else
    *physX = logicX;
    *physY = logicY;
#endif
}

static uint8_t EPD_IsInLogicRange(int16_t X, int16_t Y)
{
    return (X >= 0 && X < EPD_WIDTH && Y >= 0 && Y < EPD_HEIGHT) ? 1 : 0;
}

static uint8_t EPD_IsInPhysicalRange(int16_t X, int16_t Y)
{
    return (X >= 0 && X < EPD_PHYSICAL_WIDTH && Y >= 0 && Y < EPD_PHYSICAL_HEIGHT) ? 1 : 0;
}

/* ==================== 软件 SPI ==================== */
static void EPD_W_SCL(uint8_t BitValue) { BitValue ? EPD_SCL_HIGH() : EPD_SCL_LOW(); }
static void EPD_W_SDA(uint8_t BitValue) { BitValue ? EPD_SDA_HIGH() : EPD_SDA_LOW(); }

static void EPD_GPIO_Init(void)
{
    /* BUSY 输入（如果接了；未接则这里不影响，BUSY_Wait 用固定延时） */
    gpio_config_t io_in = {
        .pin_bit_mask = (1ULL << EPD_BUSY_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_in);

    /* 输出引脚 */
    gpio_config_t io_out = {
        .pin_bit_mask = (1ULL << EPD_RST_PIN) | (1ULL << EPD_DC_PIN) |
                        (1ULL << EPD_CS_PIN)  | (1ULL << EPD_SCL_PIN) |
                        (1ULL << EPD_SDA_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_out);

    delay_ms(100);

    EPD_RST_HIGH();
    EPD_DC_HIGH();
    EPD_CS_HIGH();
    EPD_SCL_HIGH();
    EPD_SDA_HIGH();
}

static void EPD_SPI_SendByte(uint8_t Byte)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        EPD_W_SCL(0);
        EPD_W_SDA(!!(Byte & (0x80 >> i)));
        delay_us(1);
        EPD_W_SCL(1);
        delay_us(1);
        EPD_W_SCL(0);
    }
}

void EPD_WriteCommand(uint8_t Command)
{
    EPD_CS_LOW();
    EPD_DC_LOW();
    EPD_SPI_SendByte(Command);
    EPD_CS_HIGH();
}

void EPD_WriteData(uint8_t Data)
{
    EPD_CS_LOW();
    EPD_DC_HIGH();
    EPD_SPI_SendByte(Data);
    EPD_CS_HIGH();
}

void EPD_BUSY_Wait(void)
{
    /* 如果 BUSY 接了 MCU，用下面这行（高电平为忙，等变低）：
     * while (EPD_BUSY_READ() != 0) delay_ms(10);
     *
     * 你的硬件 BUSY 未接 MCU，用固定延时。
     */
    delay_ms(2000);
}

/* ==================== 硬件复位 ==================== */
static void EPD_COG_Reset(void)
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

/* ==================== 初始化 ==================== */
void EPD_Init(void)
{
    EPD_GPIO_Init();
    EPD_COG_Reset();

    EPD_WriteCommand(EPD_CMD_SOFT_RESET);
    EPD_WriteData(0xE0);
    delay_ms(5);

    EPD_WriteCommand(EPD_CMD_PWR);
    EPD_WriteData(0x3F);
    EPD_WriteData(0x00);
    EPD_WriteData(0x32);
    EPD_WriteData(0x2A);
    EPD_WriteData(0x0E);
    EPD_WriteData(0x2A);

    EPD_WriteCommand(EPD_CMD_BTST);
    EPD_WriteData(0x17);
    EPD_WriteData(0x17);
    EPD_WriteData(0x17);

    EPD_WriteCommand(EPD_CMD_PWR_SEQ);
    EPD_WriteData(0x41);
    EPD_WriteData(0x00);
    EPD_WriteData(0x32);

    EPD_WriteCommand(EPD_CMD_PLL);
    EPD_WriteData(0x3C);

    EPD_WriteCommand(EPD_CMD_TSE);
    EPD_WriteData(0x00);

    EPD_WriteCommand(EPD_CMD_PSR);
    EPD_WriteData(0xCF);
    EPD_WriteData(0x8D);

    EPD_WriteCommand(EPD_CMD_CDI);
    EPD_WriteData(0x97);

    EPD_WriteCommand(EPD_CMD_WRITE_TEMPERATURE);
    EPD_WriteData(0x19);

    EPD_WriteCommand(EPD_CMD_APPLY_TEMPERATURE);
    EPD_WriteData(0x02);

    EPD_Clear();
}

/* ==================== 刷新（写 SRAM + 上电 + 刷新 + 断电） ==================== */
void EPD_Update(void)
{
    uint16_t i, j;

    EPD_WriteCommand(EPD_CMD_WRITE_IMAGE_RAM_BW);
    for (i = 0; i < EPD_PHYSICAL_HEIGHT; i++)
        for (j = 0; j < EPD_PHYSICAL_COLUMN_BYTES; j++)
            EPD_WriteData(EPD_DisplayBuf[i][j]);

    EPD_WriteCommand(EPD_CMD_WRITE_IMAGE_RAM_RW);
    for (i = 0; i < EPD_PHYSICAL_HEIGHT; i++)
        for (j = 0; j < EPD_PHYSICAL_COLUMN_BYTES; j++)
            EPD_WriteData(0x00);

    EPD_WriteCommand(EPD_CMD_POWER_ON);
    delay_ms(5);
    EPD_BUSY_Wait();

    EPD_WriteCommand(EPD_CMD_DISPLAY_REFRESH);
    delay_ms(5);
    EPD_BUSY_Wait();

    EPD_WriteCommand(EPD_CMD_POWER_OFF);
    EPD_BUSY_Wait();
}

void EPD_UpdateArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height)
{
    EPD_Update();  /* 简化：区域刷新也全屏刷 */
}

/* ==================== 清屏 ==================== */
void EPD_Clear(void)
{
    uint16_t i, j;
    for (i = 0; i < EPD_PHYSICAL_HEIGHT; i++)
        for (j = 0; j < EPD_PHYSICAL_COLUMN_BYTES; j++)
            EPD_DisplayBuf[i][j] = 0xFF;
}

void EPD_ClearArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height)
{
    int16_t i, j, physX, physY;
    uint8_t byteIndex, bitIndex;

    for (j = Y; j < Y + Height; j++) {
        for (i = X; i < X + Width; i++) {
            if (EPD_IsInLogicRange(i, j)) {
                EPD_LogicToPhys(i, j, &physX, &physY);
                if (EPD_IsInPhysicalRange(physX, physY)) {
                    byteIndex = physX / 8;
                    bitIndex = 7 - (physX % 8);
                    EPD_DisplayBuf[physY][byteIndex] |= (0x01 << bitIndex);
                }
            }
        }
    }
}

void EPD_Reverse(void)
{
    uint16_t i, j;
    for (i = 0; i < EPD_PHYSICAL_HEIGHT; i++)
        for (j = 0; j < EPD_PHYSICAL_COLUMN_BYTES; j++)
            EPD_DisplayBuf[i][j] ^= 0xFF;
}

void EPD_ReverseArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height)
{
    int16_t i, j, physX, physY;
    uint8_t byteIndex, bitIndex;

    for (j = Y; j < Y + Height; j++) {
        for (i = X; i < X + Width; i++) {
            if (EPD_IsInLogicRange(i, j)) {
                EPD_LogicToPhys(i, j, &physX, &physY);
                if (EPD_IsInPhysicalRange(physX, physY)) {
                    byteIndex = physX / 8;
                    bitIndex = 7 - (physX % 8);
                    EPD_DisplayBuf[physY][byteIndex] ^= (0x01 << bitIndex);
                }
            }
        }
    }
}

/* ==================== 字符显示 ==================== */
void EPD_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize, uint8_t Inverted)
{
    uint8_t i, j, temp, pos;
    int16_t physX, physY;
    uint8_t byteIndex, bitIndex;
    uint8_t pixel;          /* 最终要写入的位：1=黑，0=白（面板极性） */

    if (FontSize == EPD_8X16) {
        pos = Char - ' ';
        for (i = 0; i < 16; i++) {
            temp = EPD_F8x16[pos][i];
            for (j = 0; j < 8; j++) {
                if (EPD_IsInLogicRange(X + j, Y + i)) {
                    EPD_LogicToPhys(X + j, Y + i, &physX, &physY);
                    if (EPD_IsInPhysicalRange(physX, physY)) {
                        byteIndex = physX / 8;
                        bitIndex = 7 - (physX % 8);

                        /* 字模 bit=1 表示字亮点 */
                        pixel = (temp & (0x80 >> j)) ? 1 : 0;

                        /* 反色：字亮点→白(0)，背景→黑(1) */
                        if (Inverted) pixel = !pixel;

                        /* 面板极性：1=黑，0=白 */
                        if (pixel)
                            EPD_DisplayBuf[physY][byteIndex] &= ~(0x01 << bitIndex);  /* 写 0 = 白 */
                        else
                            EPD_DisplayBuf[physY][byteIndex] |=  (0x01 << bitIndex);  /* 写 1 = 黑 */
                    }
                }
            }
        }
    } else if (FontSize == EPD_6X8) {
        pos = Char - ' ';
        for (j = 0; j < 6; j++) {
            temp = EPD_F6x8[pos][j];
            for (i = 0; i < 8; i++) {
                if (EPD_IsInLogicRange(X + j, Y + i)) {
                    EPD_LogicToPhys(X + j, Y + i, &physX, &physY);
                    if (EPD_IsInPhysicalRange(physX, physY)) {
                        byteIndex = physX / 8;
                        bitIndex = 7 - (physX % 8);

                        pixel = (temp & (1 << i)) ? 1 : 0;
                        if (Inverted) pixel = !pixel;

                        if (pixel)
                            EPD_DisplayBuf[physY][byteIndex] &= ~(0x01 << bitIndex);
                        else
                            EPD_DisplayBuf[physY][byteIndex] |=  (0x01 << bitIndex);
                    }
                }
            }
        }
    }
}

void EPD_ShowString(int16_t X, int16_t Y, char *String, uint8_t FontSize, uint8_t Inverted)
{
    uint16_t XOffset = 0;
    while (*String != '\0') {
        EPD_ShowChar(X + XOffset, Y, *String, FontSize, Inverted);
        XOffset += FontSize;
        String++;
    }
}

void EPD_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize, uint8_t Inverted)
{
    uint8_t i;
    for (i = 0; i < Length; i++)
        EPD_ShowChar(X + i * FontSize, Y, Number / EPD_Pow(10, Length - i - 1) % 10 + '0', FontSize, Inverted);
}

void EPD_ShowSignedNum(int16_t X, int16_t Y, int32_t Number, uint8_t Length, uint8_t FontSize, uint8_t Inverted)
{
    uint8_t i;
    uint32_t Number1;
    if (Number >= 0) { EPD_ShowChar(X, Y, '+', FontSize, Inverted); Number1 = Number; }
    else             { EPD_ShowChar(X, Y, '-', FontSize, Inverted); Number1 = -Number; }
    for (i = 0; i < Length; i++)
        EPD_ShowChar(X + (i + 1) * FontSize, Y, Number1 / EPD_Pow(10, Length - i - 1) % 10 + '0', FontSize, Inverted);
}

void EPD_ShowHexNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize, uint8_t Inverted)
{
    uint8_t i, SingleNumber;
    for (i = 0; i < Length; i++) {
        SingleNumber = Number / EPD_Pow(16, Length - i - 1) % 16;
        if (SingleNumber < 10) EPD_ShowChar(X + i * FontSize, Y, SingleNumber + '0', FontSize, Inverted);
        else                   EPD_ShowChar(X + i * FontSize, Y, SingleNumber - 10 + 'A', FontSize, Inverted);
    }
}

void EPD_ShowBinNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize, uint8_t Inverted)
{
    uint8_t i;
    for (i = 0; i < Length; i++)
        EPD_ShowChar(X + i * FontSize, Y, Number / EPD_Pow(2, Length - i - 1) % 2 + '0', FontSize, Inverted);
}

void EPD_ShowFloatNum(int16_t X, int16_t Y, double Number, uint8_t IntLength, uint8_t FraLength, uint8_t FontSize, uint8_t Inverted)
{
    uint32_t PowNum, IntNum, FraNum;
    if (Number >= 0) EPD_ShowChar(X, Y, '+', FontSize, Inverted);
    else             { EPD_ShowChar(X, Y, '-', FontSize, Inverted); Number = -Number; }
    IntNum = Number;
    Number -= IntNum;
    PowNum = EPD_Pow(10, FraLength);
    FraNum = round(Number * PowNum);
    IntNum += FraNum / PowNum;
    EPD_ShowNum(X + FontSize, Y, IntNum, IntLength, FontSize, Inverted);
    EPD_ShowChar(X + (IntLength + 1) * FontSize, Y, '.', FontSize, Inverted);
    EPD_ShowNum(X + (IntLength + 2) * FontSize, Y, FraNum, FraLength, FontSize, Inverted);
}

void EPD_ShowImage(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image)
{
    uint16_t i, j;
    int16_t physX, physY;
    uint8_t byteIndex, bitIndex, pixel;
    uint16_t bytesPerLine = (Width + 7) / 8;

    /* 先铺白底 */
    for (j = 0; j < Height; j++) {
        for (i = 0; i < Width; i++) {
            if (EPD_IsInLogicRange(X + i, Y + j)) {
                EPD_LogicToPhys(X + i, Y + j, &physX, &physY);
                if (EPD_IsInPhysicalRange(physX, physY)) {
                    byteIndex = physX / 8;
                    bitIndex = 7 - (physX % 8);
                    EPD_DisplayBuf[physY][byteIndex] |= (0x01 << bitIndex);
                }
            }
        }
    }

    /* 画图 */
    for (j = 0; j < Height; j++) {
        for (i = 0; i < Width; i++) {
            if (EPD_IsInLogicRange(X + i, Y + j)) {
                EPD_LogicToPhys(X + i, Y + j, &physX, &physY);
                if (EPD_IsInPhysicalRange(physX, physY)) {
                    byteIndex = physX / 8;
                    bitIndex = 7 - (physX % 8);
                    pixel = (Image[j * bytesPerLine + i / 8] >> (7 - (i % 8))) & 0x01;
                    if (pixel)
                        EPD_DisplayBuf[physY][byteIndex] &= ~(0x01 << bitIndex);
                }
            }
        }
    }
}

void EPD_Printf(int16_t X, int16_t Y, uint8_t FontSize, uint8_t Inverted, char *format, ...)
{
    char String[256];
    va_list arg;
    va_start(arg, format);
    vsprintf(String, format, arg);
    va_end(arg);
    EPD_ShowString(X, Y, String, FontSize, Inverted);
}

/* ==================== 画点 ==================== */
void EPD_DrawPoint(int16_t X, int16_t Y)
{
    int16_t physX, physY;
    uint8_t byteIndex, bitIndex;

    if (EPD_IsInLogicRange(X, Y)) {
        EPD_LogicToPhys(X, Y, &physX, &physY);
        if (EPD_IsInPhysicalRange(physX, physY)) {
            byteIndex = physX / 8;
            bitIndex = 7 - (physX % 8);
            EPD_DisplayBuf[physY][byteIndex] &= ~(0x01 << bitIndex);
        }
    }
}

void EPD_ClearPoint(int16_t X, int16_t Y)
{
    int16_t physX, physY;
    uint8_t byteIndex, bitIndex;

    if (EPD_IsInLogicRange(X, Y)) {
        EPD_LogicToPhys(X, Y, &physX, &physY);
        if (EPD_IsInPhysicalRange(physX, physY)) {
            byteIndex = physX / 8;
            bitIndex = 7 - (physX % 8);
            EPD_DisplayBuf[physY][byteIndex] |= (0x01 << bitIndex);
        }
    }
}

uint8_t EPD_GetPoint(int16_t X, int16_t Y)
{
    int16_t physX, physY;
    uint8_t byteIndex, bitIndex;

    if (EPD_IsInLogicRange(X, Y)) {
        EPD_LogicToPhys(X, Y, &physX, &physY);
        if (EPD_IsInPhysicalRange(physX, physY)) {
            byteIndex = physX / 8;
            bitIndex = 7 - (physX % 8);
            if (EPD_DisplayBuf[physY][byteIndex] & (0x01 << bitIndex)) return 1;
        }
    }
    return 0;
}

/* ==================== 画线 ==================== */
void EPD_DrawLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1)
{
    int16_t x, y, dx, dy, d, incrE, incrNE, temp;
    int16_t x0 = X0, y0 = Y0, x1 = X1, y1 = Y1;
    uint8_t yflag = 0, xyflag = 0;

    if (y0 == y1) {
        if (x0 > x1) { temp = x0; x0 = x1; x1 = temp; }
        for (x = x0; x <= x1; x++) EPD_DrawPoint(x, y0);
    } else if (x0 == x1) {
        if (y0 > y1) { temp = y0; y0 = y1; y1 = temp; }
        for (y = y0; y <= y1; y++) EPD_DrawPoint(x0, y);
    } else {
        if (x0 > x1) {
            temp = x0; x0 = x1; x1 = temp;
            temp = y0; y0 = y1; y1 = temp;
        }
        if (y0 > y1) { y0 = -y0; y1 = -y1; yflag = 1; }
        if (y1 - y0 > x1 - x0) {
            temp = x0; x0 = y0; y0 = temp;
            temp = x1; x1 = y1; y1 = temp;
            xyflag = 1;
        }
        dx = x1 - x0; dy = y1 - y0;
        incrE = 2 * dy; incrNE = 2 * (dy - dx);
        d = 2 * dy - dx;
        x = x0; y = y0;

        if (yflag && xyflag) EPD_DrawPoint(y, -x);
        else if (yflag)      EPD_DrawPoint(x, -y);
        else if (xyflag)     EPD_DrawPoint(y, x);
        else                 EPD_DrawPoint(x, y);

        while (x < x1) {
            x++;
            if (d < 0) d += incrE;
            else       { y++; d += incrNE; }

            if (yflag && xyflag) EPD_DrawPoint(y, -x);
            else if (yflag)      EPD_DrawPoint(x, -y);
            else if (xyflag)     EPD_DrawPoint(y, x);
            else                 EPD_DrawPoint(x, y);
        }
    }
}

void EPD_DrawDashedLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1)
{
    const uint8_t dashLength = 3;
    const uint8_t gapLength = 2;
    int16_t x, y, dx, dy, d, incrE, incrNE, temp;
    int16_t x0 = X0, y0 = Y0, x1 = X1, y1 = Y1;
    uint8_t yflag = 0, xyflag = 0;
    uint16_t pointCount = 0;

    if (y0 == y1) {
        if (x0 > x1) { temp = x0; x0 = x1; x1 = temp; }
        for (x = x0; x <= x1; x++) {
            if (pointCount < dashLength) EPD_DrawPoint(x, y0);
            pointCount = (pointCount + 1) % (dashLength + gapLength);
        }
    } else if (x0 == x1) {
        if (y0 > y1) { temp = y0; y0 = y1; y1 = temp; }
        for (y = y0; y <= y1; y++) {
            if (pointCount < dashLength) EPD_DrawPoint(x0, y);
            pointCount = (pointCount + 1) % (dashLength + gapLength);
        }
    } else {
        if (x0 > x1) {
            temp = x0; x0 = x1; x1 = temp;
            temp = y0; y0 = y1; y1 = temp;
        }
        if (y0 > y1) { y0 = -y0; y1 = -y1; yflag = 1; }
        if (y1 - y0 > x1 - x0) {
            temp = x0; x0 = y0; y0 = temp;
            temp = x1; x1 = y1; y1 = temp;
            xyflag = 1;
        }
        dx = x1 - x0; dy = y1 - y0;
        incrE = 2 * dy; incrNE = 2 * (dy - dx);
        d = 2 * dy - dx;
        x = x0; y = y0;

        if (pointCount < dashLength) {
            if (yflag && xyflag) EPD_DrawPoint(y, -x);
            else if (yflag)      EPD_DrawPoint(x, -y);
            else if (xyflag)     EPD_DrawPoint(y, x);
            else                 EPD_DrawPoint(x, y);
        }
        pointCount = (pointCount + 1) % (dashLength + gapLength);

        while (x < x1) {
            x++;
            if (d < 0) d += incrE;
            else       { y++; d += incrNE; }

            if (pointCount < dashLength) {
                if (yflag && xyflag) EPD_DrawPoint(y, -x);
                else if (yflag)      EPD_DrawPoint(x, -y);
                else if (xyflag)     EPD_DrawPoint(y, x);
                else                 EPD_DrawPoint(x, y);
            }
            pointCount = (pointCount + 1) % (dashLength + gapLength);
        }
    }
}

/* ==================== 矩形 / 三角形 / 圆 / 弧 ==================== */
void EPD_DrawRectangle(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, uint8_t IsFilled)
{
    int16_t i, j;
    if (!IsFilled) {
        for (i = X; i < X + Width; i++) {
            EPD_DrawPoint(i, Y);
            EPD_DrawPoint(i, Y + Height - 1);
        }
        for (i = Y; i < Y + Height; i++) {
            EPD_DrawPoint(X, i);
            EPD_DrawPoint(X + Width - 1, i);
        }
    } else {
        for (i = X; i < X + Width; i++)
            for (j = Y; j < Y + Height; j++)
                EPD_DrawPoint(i, j);
    }
}

void EPD_DrawTriangle(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1,
                      int16_t X2, int16_t Y2, uint8_t IsFilled)
{
    int16_t minx = X0, miny = Y0, maxx = X0, maxy = Y0;
    int16_t i, j;
    int16_t vx[] = {X0, X1, X2};
    int16_t vy[] = {Y0, Y1, Y2};

    if (!IsFilled) {
        EPD_DrawLine(X0, Y0, X1, Y1);
        EPD_DrawLine(X0, Y0, X2, Y2);
        EPD_DrawLine(X1, Y1, X2, Y2);
    } else {
        if (X1 < minx) minx = X1;
        if (X2 < minx) minx = X2;
        if (Y1 < miny) miny = Y1;
        if (Y2 < miny) miny = Y2;
        if (X1 > maxx) maxx = X1;
        if (X2 > maxx) maxx = X2;
        if (Y1 > maxy) maxy = Y1;
        if (Y2 > maxy) maxy = Y2;

        for (i = minx; i <= maxx; i++)
            for (j = miny; j <= maxy; j++)
                if (EPD_pnpoly(3, vx, vy, i, j)) EPD_DrawPoint(i, j);
    }
}

void EPD_DrawCircle(int16_t X, int16_t Y, uint16_t Radius, uint8_t IsFilled)
{
    int16_t x, y, d, j;
    d = 1 - Radius; x = 0; y = Radius;

    EPD_DrawPoint(X + x, Y + y);
    EPD_DrawPoint(X - x, Y - y);
    EPD_DrawPoint(X + y, Y + x);
    EPD_DrawPoint(X - y, Y - x);

    if (IsFilled)
        for (j = -y; j < y; j++) EPD_DrawPoint(X, Y + j);

    while (x < y) {
        x++;
        if (d < 0) d += 2 * x + 1;
        else       { y--; d += 2 * (x - y) + 1; }

        EPD_DrawPoint(X + x, Y + y);
        EPD_DrawPoint(X + y, Y + x);
        EPD_DrawPoint(X - x, Y - y);
        EPD_DrawPoint(X - y, Y - x);
        EPD_DrawPoint(X + x, Y - y);
        EPD_DrawPoint(X + y, Y - x);
        EPD_DrawPoint(X - x, Y + y);
        EPD_DrawPoint(X - y, Y + x);

        if (IsFilled) {
            for (j = -y; j < y; j++) {
                EPD_DrawPoint(X + x, Y + j);
                EPD_DrawPoint(X - x, Y + j);
            }
            for (j = -x; j < x; j++) {
                EPD_DrawPoint(X - y, Y + j);
                EPD_DrawPoint(X + y, Y + j);
            }
        }
    }
}

void EPD_DrawArc(int16_t X, int16_t Y, uint8_t Radius, int16_t StartAngle, int16_t EndAngle, uint8_t IsFilled)
{
    int16_t x, y, d, j;
    d = 1 - Radius; x = 0; y = Radius;

    if (EPD_IsInAngle(x, y, StartAngle, EndAngle)) EPD_DrawPoint(X + x, Y + y);
    if (EPD_IsInAngle(-x, -y, StartAngle, EndAngle)) EPD_DrawPoint(X - x, Y - y);
    if (EPD_IsInAngle(y, x, StartAngle, EndAngle)) EPD_DrawPoint(X + y, Y + x);
    if (EPD_IsInAngle(-y, -x, StartAngle, EndAngle)) EPD_DrawPoint(X - y, Y - x);

    if (IsFilled)
        for (j = -y; j < y; j++)
            if (EPD_IsInAngle(0, j, StartAngle, EndAngle)) EPD_DrawPoint(X, Y + j);

    while (x < y) {
        x++;
        if (d < 0) d += 2 * x + 1;
        else       { y--; d += 2 * (x - y) + 1; }

        if (EPD_IsInAngle(x, y, StartAngle, EndAngle)) EPD_DrawPoint(X + x, Y + y);
        if (EPD_IsInAngle(y, x, StartAngle, EndAngle)) EPD_DrawPoint(X + y, Y + x);
        if (EPD_IsInAngle(-x, -y, StartAngle, EndAngle)) EPD_DrawPoint(X - x, Y - y);
        if (EPD_IsInAngle(-y, -x, StartAngle, EndAngle)) EPD_DrawPoint(X - y, Y - x);
        if (EPD_IsInAngle(x, -y, StartAngle, EndAngle)) EPD_DrawPoint(X + x, Y - y);
        if (EPD_IsInAngle(y, -x, StartAngle, EndAngle)) EPD_DrawPoint(X + y, Y - x);
        if (EPD_IsInAngle(-x, y, StartAngle, EndAngle)) EPD_DrawPoint(X - x, Y + y);
        if (EPD_IsInAngle(-y, x, StartAngle, EndAngle)) EPD_DrawPoint(X - y, Y + x);

        if (IsFilled) {
            for (j = -y; j < y; j++) {
                if (EPD_IsInAngle(x, j, StartAngle, EndAngle)) EPD_DrawPoint(X + x, Y + j);
                if (EPD_IsInAngle(-x, j, StartAngle, EndAngle)) EPD_DrawPoint(X - x, Y + j);
            }
            for (j = -x; j < x; j++) {
                if (EPD_IsInAngle(-y, j, StartAngle, EndAngle)) EPD_DrawPoint(X - y, Y + j);
                if (EPD_IsInAngle(y, j, StartAngle, EndAngle)) EPD_DrawPoint(X + y, Y + j);
            }
        }
    }
}

/* ==================== 测试图 ==================== */
void EPD_TestPattern(void)
{
    uint16_t i;
    EPD_Clear();

    for (i = 0; i < EPD_WIDTH; i++) {
        EPD_DrawPoint(i, 0);
        EPD_DrawPoint(i, EPD_HEIGHT - 1);
    }
    for (i = 0; i < EPD_HEIGHT; i++) {
        EPD_DrawPoint(0, i);
        EPD_DrawPoint(EPD_WIDTH - 1, i);
    }
    for (i = 0; i < EPD_WIDTH && i < EPD_HEIGHT; i++) {
        EPD_DrawPoint(i, i);
        EPD_DrawPoint(EPD_WIDTH - 1 - i, i);
    }
    for (i = 0; i < EPD_WIDTH; i++)  EPD_DrawPoint(i, EPD_HEIGHT / 2);
    for (i = 0; i < EPD_HEIGHT; i++) EPD_DrawPoint(EPD_WIDTH / 2, i);

    EPD_ShowString(10, 10, "TL", EPD_8X16, 1);
    EPD_ShowString(EPD_WIDTH - 40, 10, "TR", EPD_8X16, 0);
    EPD_ShowString(10, EPD_HEIGHT - 30, "BL", EPD_8X16, 1);
    EPD_ShowString(EPD_WIDTH - 40, EPD_HEIGHT - 30, "BR", EPD_8X16, 0);
    
    EPD_DrawRectangle(EPD_WIDTH / 2 - 80, EPD_HEIGHT / 2 - 16, EPD_WIDTH / 2 + 40, EPD_HEIGHT / 2 + 8, 1);
    EPD_ShowString(EPD_WIDTH / 2 - 40, EPD_HEIGHT / 2 - 8, "TEST", EPD_8X16, 1);

    EPD_Update();
}