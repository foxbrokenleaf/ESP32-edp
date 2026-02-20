/**
  ******************************************************************************
  * @file    Project/STM32F10x_StdPeriph_Template/main.c 
  * @author  MCD Application Team
  * @version V3.6.0
  * @date    20-September-2021
  * @brief   Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2011 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "stm32f10x.h"
#include "stm32_eval.h"
#include <stdio.h>
#include "EPD.h"
#include "Delay.h"
#include "time.h"
#include "Key.h"

/* STM32大容量产品每页大小2KByte，中、小容量产品每页大小1KByte */
#if defined (STM32F10X_HD) || defined (STM32F10X_HD_VL) ||\
defined (STM32F10X_CL) || defined (STM32F10X_XL)
#define FLASH_PAGE_SIZE    ((uint16_t)0x800)//2048
#else
#define FLASH_PAGE_SIZE    ((uint16_t)0x400)//1024
#endif

//写入的起始地址与结束地址
#define WRITE_START_ADDR  ((uint32_t)0x08007C00)
#define WRITE_END_ADDR    ((uint32_t)0x08008000)

#ifdef USE_STM32100B_EVAL
 #include "stm32100b_eval_lcd.h"
#elif defined USE_STM3210B_EVAL
 #include "stm3210b_eval_lcd.h"
#elif defined USE_STM3210E_EVAL
 #include "stm3210e_eval_lcd.h" 
#elif defined USE_STM3210C_EVAL
 #include "stm3210c_eval_lcd.h"
#elif defined USE_STM32100E_EVAL
 #include "stm32100e_eval_lcd.h"
#endif

/** @addtogroup STM32F10x_StdPeriph_Template
  * @{
  */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
#ifdef USE_STM32100B_EVAL
  #define MESSAGE1   "STM32 MD Value Line " 
  #define MESSAGE2   " Device running on  " 
  #define MESSAGE3   "  STM32100B-EVAL    " 
#elif defined (USE_STM3210B_EVAL)
  #define MESSAGE1   "STM32 Medium Density" 
  #define MESSAGE2   " Device running on  " 
  #define MESSAGE3   "   STM3210B-EVAL    " 
#elif defined (STM32F10X_XL) && defined (USE_STM3210E_EVAL)
  #define MESSAGE1   "  STM32 XL Density  " 
  #define MESSAGE2   " Device running on  " 
  #define MESSAGE3   "   STM3210E-EVAL    "
#elif defined (USE_STM3210E_EVAL)
  #define MESSAGE1   " STM32 High Density " 
  #define MESSAGE2   " Device running on  " 
  #define MESSAGE3   "   STM3210E-EVAL    " 
#elif defined (USE_STM3210C_EVAL)
  #define MESSAGE1   " STM32 Connectivity " 
  #define MESSAGE2   " Line Device running" 
  #define MESSAGE3   " on STM3210C-EVAL   "
#elif defined (USE_STM32100E_EVAL)
  #define MESSAGE1   "STM32 HD Value Line " 
  #define MESSAGE2   " Device running on  " 
  #define MESSAGE3   "  STM32100E-EVAL    "   
#endif

/* Private macro -------------------------------------------------------------*/
// 月天数表（非闰年）
const uint8_t monthDays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
/* Private variables ---------------------------------------------------------*/
USART_InitTypeDef USART_InitStructure;
uint16_t DateYear = 2026;
uint8_t DateMonth = 2;
uint8_t DateToday = 20;
uint8_t DateHour = 14;
uint8_t DateMin = 44;
uint8_t DateSec = 32;
uint16_t DateArrary[6][7] = {
  {0, 0, 0, 0, 0, 0, 1},
  {2, 3, 4, 5, 6, 7, 8},
  {9, 10, 11, 12, 13, 14, 15},
  {16, 17, 18, 19, 20, 21, 22},
  {23, 24, 25, 26, 27, 28, 00},
  {00, 00, 00, 00, 00, 00, 00}
};

uint8_t RxDat = 0;
uint8_t TimeRead_Flag = 0;
uint8_t UpdateDay_Flag = 0;

/* Private function prototypes -----------------------------------------------*/
#ifdef __GNUC__
/* With GCC/RAISONANCE, small printf (option LD Linker->Libraries->Small printf
   set to 'Yes') calls __io_putchar() */
#define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
#define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif /* __GNUC__ */

/* Private functions ---------------------------------------------------------*/
void DisplayTask(void);
void UpdateDateTask(void);
uint8_t getMonthDays(uint16_t year, uint8_t month);
void GenerateCalendar(uint16_t year, uint8_t month);
void PrevMonth(void);
void NextMonth(void);
int InternalFlash_Test(void);
void SerialShowDate(void);
void GoToday(void);
void ShowInternalFlashData(void);
void RTC_Init(void);
void RTC_SetTime(uint8_t hour, uint8_t min, uint8_t sec);
void RTC_GetTime(void);
/**
  * @brief  Main program.
  * @param  None
  * @retval None
  */
int main(void)
{
  /*!< At this stage the microcontroller clock setting is already configured, 
       this is done through SystemInit() function which is called from startup
       file (startup_stm32f10x_xx.s) before to branch to application main.
       To reconfigure the default setting of SystemInit() function, refer to
       system_stm32f10x.c file
     */     

  /* Initialize LEDs, Key Button, LCD and COM port(USART) available on
     STM3210X-EVAL board ******************************************************/

  /* Private variables ---------------------------------------------------------*/
  uint32_t tick = 0;
  KeyEvent key_events[KEY_NUM];
  /* USARTx configured as follow:
        - BaudRate = 115200 baud  
        - Word Length = 8 Bits
        - One Stop Bit
        - No parity
        - Hardware flow control disabled (RTS and CTS signals)
        - Receive and transmit enabled
  */
  USART_InitStructure.USART_BaudRate = 115200;
  USART_InitStructure.USART_WordLength = USART_WordLength_8b;
  USART_InitStructure.USART_StopBits = USART_StopBits_1;
  USART_InitStructure.USART_Parity = USART_Parity_No;
  USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
  USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;

  STM_EVAL_COMInit(COM1, &USART_InitStructure);

  NVIC_InitTypeDef NVIC_InitStructure;
  
  /* 配置UART4中断优先级 */
  NVIC_InitStructure.NVIC_IRQChannel = USART1_IRQn;
  NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
  NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
  NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
  NVIC_Init(&NVIC_InitStructure);
  
  /* 使能接收中断 */
  USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);    

  /* Retarget the C library printf function to the USARTx, can be USART1 or USART2
     depending on the EVAL board you are using ********************************/
  printf("\n\r %s", MESSAGE1);
  printf(" %s", MESSAGE2);
  printf(" %s\n\r", MESSAGE3);

  /* Turn on leds available on STM3210X-EVAL **********************************/

  /* Add your application code here
     */

  /* Infinite loop */
    /* 初始化EPD */
    RTC_Init();
    EPD_Init();
    Key_Init();

    GenerateCalendar(DateYear, DateMonth);
    
    while(1)
    {
      switch (RxDat)
      {
      case 0x01:
        PrevMonth();
        RxDat = 0x00;
        break;
      case 0x02:
        NextMonth();
        RxDat = 0x00;
        break;  
      case 0x03:
        DateToday--;
        RxDat = 0x00;
        break;        
      case 0x04:
        DateToday++;
        RxDat = 0x00;
        break;
      case 0x05:
        ShowInternalFlashData();
        RxDat = 0x00;
        break;
      case 0x06:
        InternalFlash_Test();
        RxDat = 0x00;
        break;
      case 0x07:
        RTC_SetTime(14, 54, 00);
        RxDat = 0x00;
        break;
      case 0x08:
        SerialShowDate();
        RxDat = 0x00;
        break;
      case 0x09:
        EPD_Clear();
        EPD_Update();
        RxDat = 0x00;
        break;
      case 0x10:
        DateYear++;
        RxDat = 0x00;
        break;
      case 0x11:
        DateToday--;
        RxDat = 0x00;
        break;
      case 0x12:
        RTC_SetTime(++DateHour, DateMin, DateSec);
        RxDat = 0x00;
        break;
      case 0x13:
        RTC_SetTime(--DateHour, DateMin, DateSec);
        RxDat = 0x00;
        break;
      case 0x14:
        RTC_SetTime(DateHour, ++DateMin, DateSec);
        RxDat = 0x00;
        break;   
      case 0x15:
        RTC_SetTime(DateHour, --DateMin, DateSec);
        RxDat = 0x00;
        break;
      case 0x16:
        RTC_SetTime(DateHour, DateMin, ++DateSec);
        RxDat = 0x00;
        break;   
      case 0x17:
        RTC_SetTime(DateHour, DateMin, --DateSec);
        RxDat = 0x00;
        break;                                                           
      case 0xFF:
        DisplayTask();
        RxDat = 0x00;
        break;              
      
      default:

        break;
      }
      if(TimeRead_Flag){
        // struct tm *time;                // 定义一个时间结构体
        // time_t time_temp = RTC_GetCounter();   // 获取RTC计数器
        // time = localtime(&time_temp);   // 将Unix时间戳转换为时间结构体        
        // printf("unix:%d\n",time_temp);
        // printf("%d-%d-%d-%d-%d",time->tm_year+1900,time->tm_mon+1,time->tm_mday,time->tm_wday,time->tm_yday);
        // printf("-%d-%d-%d\n",time->tm_hour,time->tm_min,time->tm_sec);
        TimeRead_Flag = 0;
      }
      if(UpdateDay_Flag){
        UpdateDateTask();
        UpdateDay_Flag = 0;
      }

      tick += 10;
      
      Key_ScanAll(tick, key_events);
      
      // // 处理按键事件
      // for (i = 0; i < KEY_NUM; i++)
      // {
      //     if (key_events[i] == KEY_EVENT_SHORT_PRESS)
      //     {
      //         printf("Key %d short pressed!\r\n", i+1);
      //     }
      //     else if (key_events[i] == KEY_EVENT_LONG_PRESS)
      //     {
      //         printf("Key %d long pressed!\r\n", i+1);
      //     }
      // }
      
      // 也可以查询按键状态
      if (Key_IsPressed(KEY_ID_1))
      {
        // printf("Key 1 pressed!\r\n");
        PrevMonth();
        DisplayTask();
        Delay_ms(1000);
        Delay_ms(1000);
      }
      if (Key_IsPressed(KEY_ID_2))
      {
        // printf("Key 2 pressed!\r\n");
        GoToday();
        GenerateCalendar(DateYear, DateMonth);
        DisplayTask();
        Delay_ms(1000);
        Delay_ms(1000);
      }
      if (Key_IsPressed(KEY_ID_3))
      {
        // printf("Key 3 pressed!\r\n");
        NextMonth();
        DisplayTask();
        Delay_ms(1000);
        Delay_ms(1000);
      }            
      

    }
}

void DisplayTask(void){
  uint8_t i = 0;
  uint8_t j = 0;
    /* 清屏（全部白色） */
    EPD_Clear();
    
    EPD_ShowNum(0, 20, DateYear, 4, EPD_8X16);
    EPD_ShowNum(10, 43, DateMonth, 2, EPD_8X16);

    EPD_ShowNum(10, 89, DateHour, 2, EPD_8X16);
    EPD_ShowNum(10, 112, DateMin, 2, EPD_8X16);
    EPD_ShowNum(10, 135, DateSec, 2, EPD_8X16);    

    EPD_ShowString(40, 0, "Mon", EPD_8X16);
    EPD_ShowString(78, 0, "Tue", EPD_8X16);
    EPD_ShowString(118, 0, "Wed", EPD_8X16);
    EPD_ShowString(154, 0, "Thu", EPD_8X16);
    EPD_ShowString(192, 0, "Fri", EPD_8X16);
    EPD_ShowString(230, 0, "Sat", EPD_8X16);
    EPD_ShowString(268, 0, "Sun", EPD_8X16);

    for(i = 0;i < 6;i++){
      for(j = 0;j < 7;j++){
        if(DateArrary[i][j] == DateToday) EPD_DrawRectangle(33 + (j * 38), 16 + (i * 23), 38, 23, EPD_FILLED);
        if(DateArrary[i][j] != 0) EPD_ShowNum(44 + (j * 38), 20 + (i * 23), DateArrary[i][j], 2, EPD_8X16);
      }
    }

    EPD_DrawLine(0, 66, 33, 85);
    EPD_DrawLine(0, 0, 33, 15);

    EPD_DrawLine(0, 16, EPD_PHYSICAL_HEIGHT, 16);
    EPD_DrawLine(0, 39, EPD_PHYSICAL_HEIGHT, 39);
    EPD_DrawLine(0, 62, EPD_PHYSICAL_HEIGHT, 62);
    EPD_DrawLine(0, 85, EPD_PHYSICAL_HEIGHT, 85);
    EPD_DrawLine(0, 108, EPD_PHYSICAL_HEIGHT, 108);
    EPD_DrawLine(0, 131, EPD_PHYSICAL_HEIGHT, 131);

    EPD_DrawLine(33, 0, 33, EPD_PHYSICAL_WIDTH);

    EPD_DrawLine(71, 0, 71, EPD_PHYSICAL_WIDTH);
    EPD_DrawLine(109, 0, 109, EPD_PHYSICAL_WIDTH);
    EPD_DrawLine(147, 0, 147, EPD_PHYSICAL_WIDTH);
    EPD_DrawLine(185, 0, 185, EPD_PHYSICAL_WIDTH);
    EPD_DrawLine(223, 0, 223, EPD_PHYSICAL_WIDTH);
    EPD_DrawLine(261, 0, 261, EPD_PHYSICAL_WIDTH);
    
    /* 更新显示 */
    EPD_Update();
}

void UpdateTimeTask(void){

}

void UpdateDateTask(void){
  DateToday++;
  if(DateToday == getMonthDays(DateYear, DateMonth) + 1){
    DateToday = 1;
    DateMonth++;
    DateMonth %= 13;
  }
  if(DateMonth == 0){
    DateMonth = 1;
    DateYear++;
  }
  GenerateCalendar(DateYear, DateMonth);
  InternalFlash_Test();
  DisplayTask();
}

// 闰年判断
uint8_t isLeapYear(uint16_t year) {
    return (year%4==0 && year%100!=0) || year%400==0;
}

// 获取月天数
uint8_t getMonthDays(uint16_t year, uint8_t month) {
    if(month==2 && isLeapYear(year)) return 29;
    return monthDays[month-1];
}

// 计算某月1号是星期几 (返回0=星期一,1=星期二,...,6=星期日)
uint8_t getFirstDayWeek(uint16_t year, uint8_t month) {
    uint16_t y = year;
    uint8_t m = month;
    if(m == 1 || m == 2) {
        m += 12;
        y--;
    }
    // 基姆拉尔森计算公式返回0=星期一,1=星期二,...,6=星期日
    return (1 + 2*m + 3*(m+1)/5 + y + y/4 - y/100 + y/400) % 7;
}

// 生成指定年月的日历数组（星期一为第0列）
void GenerateCalendar(uint16_t year, uint8_t month) {
    uint8_t i, j;
    uint8_t days = getMonthDays(year, month);
    uint8_t firstDay = getFirstDayWeek(year, month);  // 0=星期一,6=星期日
    uint8_t date = 1;
    
    // 清空数组
    for(i=0; i<6; i++) {
        for(j=0; j<7; j++) {
            DateArrary[i][j] = 0;
        }
    }
    
    // 填充日期
    for(i=0; i<6 && date<=days; i++) {
        for(j=firstDay; j<7 && date<=days; j++) {
            DateArrary[i][j] = date++;
        }
        firstDay = 0;  // 从第二行开始从第0列填充
    }
}

// 查找日期位置
uint8_t findDate(uint8_t date, uint8_t *row, uint8_t *col) {
    uint8_t i, j;
    for(i=0; i<6; i++) {
        for(j=0; j<7; j++) {
            if(DateArrary[i][j] == date) {
                *row = i;
                *col = j;
                return 1;
            }
        }
    }
    return 0;
}

// 更新到下个月
void NextMonth(void) {
    uint8_t row, col;
    uint16_t nextYear = DateYear;
    uint8_t nextMonth = DateMonth + 1;
    
    // 年份处理
    if(nextMonth > 12) {
        nextMonth = 1;
        nextYear++;
    }
    
    // 查找当前日期位置
    if(findDate(DateToday, &row, &col)) {
        // 生成下个月的日历
        GenerateCalendar(nextYear, nextMonth);
        
        // 取下个月相同位置的日期
        uint8_t nextDate = DateArrary[row][col];
        uint8_t nextMonthDays = getMonthDays(nextYear, nextMonth);
        
        // 如果位置无效(0)或日期超出范围
        if(nextDate == 0 || nextDate > nextMonthDays) {
            // 从前往后找有效日期
            uint8_t c = col;
            while(c < 6) {
                c++;
                nextDate = DateArrary[row][c];
                if(nextDate != 0 && nextDate <= nextMonthDays) break;
            }
            
            // 如果还没找到，从下一行找（如果当前不是最后一行）
            if((nextDate == 0 || nextDate > nextMonthDays) && row < 5) {
                uint8_t r = row + 1;
                for(c=0; c<7; c++) {
                    nextDate = DateArrary[r][c];
                    if(nextDate != 0 && nextDate <= nextMonthDays) break;
                }
            }
            
            // 如果还是找不到，取下个月最后一天
            if(nextDate == 0 || nextDate > nextMonthDays) {
                nextDate = nextMonthDays;
            }
        }
        
        // 更新日期
        DateYear = nextYear;
        DateMonth = nextMonth;
        DateToday = nextDate;
    }
}

// 更新到上个月
void PrevMonth(void) {
    uint8_t row, col;
    uint16_t prevYear = DateYear;
    uint8_t prevMonth = DateMonth - 1;
    
    // 年份处理
    if(prevMonth == 0) {
        prevMonth = 12;
        prevYear--;
    }
    
    // 查找当前日期位置
    if(findDate(DateToday, &row, &col)) {
        // 生成上个月的日历
        GenerateCalendar(prevYear, prevMonth);
        
        // 取上个月相同位置的日期
        uint8_t prevDate = DateArrary[row][col];
        uint8_t prevMonthDays = getMonthDays(prevYear, prevMonth);
        
        // 如果位置无效(0)或日期超出范围
        if(prevDate == 0 || prevDate > prevMonthDays) {
            // 从后往前找有效日期
            uint8_t c = col;
            while(c > 0) {
                c--;
                prevDate = DateArrary[row][c];
                if(prevDate != 0 && prevDate <= prevMonthDays) break;
            }
            
            // 如果还没找到，从上一行找
            if((prevDate == 0 || prevDate > prevMonthDays) && row > 0) {
                uint8_t r = row - 1;
                for(c=0; c<7; c++) {
                    prevDate = DateArrary[r][c];
                    if(prevDate != 0 && prevDate <= prevMonthDays) break;
                }
            }
            
            // 如果还是找不到，取上个月最后一天
            if(prevDate == 0 || prevDate > prevMonthDays) {
                prevDate = prevMonthDays;
            }
        }
        
        // 更新日期
        DateYear = prevYear;
        DateMonth = prevMonth;
        DateToday = prevDate;
    }
}

// 初始化日历（程序启动时调用）
void InitCalendar(void) {
    GenerateCalendar(DateYear, DateMonth);
}

void SerialShowDate(void){
  uint8_t i = 0;
  uint8_t j = 0;  
  printf("Year = %04d\r\n", DateYear);
  printf("Month = %02d\r\n", DateMonth);
  printf("DateToday = %02d\r\n", DateToday);
  printf("Hour = %02d\r\n", DateHour);
  printf("Min = %02d\r\n", DateMin);
  printf("Sec = %02d\r\n", DateSec);    
  for(i = 0;i < 6;i++){
    for(j = 0;j < 7;j++) printf("%02d ", DateArrary[i][j]);
    printf("\r\n");
  }    
}

/**
* @brief  InternalFlash_Test,对内部FLASH进行读写测试
* @param  None
* @retval None
*/
int InternalFlash_Test(void)
{
  uint32_t Address = 0x00;        //记录写入的地址
  uint8_t i = 0;
  uint8_t j = 0;
  uint16_t tmp = 0;



  FLASH_Status FLASHStatus = FLASH_COMPLETE; //记录每次擦除的结果
  // TestStatus MemoryProgramStatus = PASSED;//记录整个测试结果


  /* 解锁 */
  FLASH_Unlock();


  /* 清空所有标志位 */
  FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);

  /* 按页擦除*/
  FLASHStatus = FLASH_ErasePage(WRITE_START_ADDR);

  /* 向内部FLASH写入数据 */
  Address = WRITE_START_ADDR;

  if(FLASH_ProgramHalfWord(Address, DateYear) == FLASH_COMPLETE) Address = Address + 2;
  if(FLASH_ProgramHalfWord(Address, (DateToday << 8) | DateMonth) == FLASH_COMPLETE) Address = Address + 2;
  if(FLASH_ProgramHalfWord(Address, (DateMin << 8) | DateHour) == FLASH_COMPLETE) Address = Address + 2;
  if(FLASH_ProgramHalfWord(Address, DateSec) == FLASH_COMPLETE) Address = Address + 2;
  // printf("Write to flash!\r\n");
  // printf("Year = %04X\r\n", DateYear);
  // printf("Month | Day = %02X\r\n", (DateMonth << 8) | DateToday);
  // printf("Hour | Min = %02X\r\n", (DateHour << 8) | DateMin);
  // printf("Sec = %02X\r\n", DateSec);  
  for(i = 0;i < 6;i++){
    for(j = 0;(j < 7) && (FLASHStatus == FLASH_COMPLETE);j++){
      tmp = (DateArrary[i][j] << 8) | (DateArrary[i][j] >> 8);
      FLASHStatus = FLASH_ProgramHalfWord(Address, tmp);
      // printf(" = %04X\r\n",DateArrary[i][j]);
      // printf("%04X ",tmp);
      Address = Address + 2;
    }
    // printf("\r\n");
  }

  FLASH_Lock();

  /* 检查写入的数据是否正确 */
  // Address = WRITE_START_ADDR;

  // while ((Address < WRITE_END_ADDR)) {
  //   if ((*(__IO uint32_t*) Address) != Data) {
  //     // MemoryProgramStatus = FAILED;
  //   }
  //   Address += 4;
  // }
  return 0;
}

void GoToday(void){
  uint32_t address = WRITE_START_ADDR;
  DateYear = *(__IO uint16_t*) address;
  address += 2;  
  DateMonth = *(__IO uint8_t*) address++;
  DateToday = *(__IO uint8_t*) address++;  
}

void ShowInternalFlashData(void){
  uint32_t address = WRITE_START_ADDR;
  uint8_t i = 0;
  uint8_t j = 0;
  uint8_t tmp_hour = 0;
  uint8_t tmp_min = 0;
  uint8_t tmp_sec = 0;

  DateYear = *(__IO uint16_t*) address;
  printf("Year = %04d\r\n", *(__IO uint16_t*) address);
  address += 2;
  DateMonth = *(__IO uint8_t*) address++;
  printf("Month = %02d\r\n", DateMonth);
  DateToday = *(__IO uint8_t*) address++;
  printf("Day = %02d\r\n", DateToday);
  tmp_hour = *(__IO uint8_t*) address++;
  printf("Hour = %02d\r\n", tmp_hour);
  tmp_min = *(__IO uint8_t*) address++;
  printf("Min = %02d\r\n", tmp_min);
  tmp_sec = *(__IO uint8_t*) address++;
  printf("Sec = %02d\r\n", tmp_sec);
  RTC_SetTime(tmp_hour, tmp_min, tmp_sec);
  address = address + 2;
  for(i = 0;i < 6;i++){
    for(j = 0;j < 7;j++){
      DateArrary[i][j] = *(__IO uint8_t*) address;
      printf("%02d ", *(__IO uint8_t*) address);
      address = address + 2;
    }
    printf("\r\n");
  }  
}

void RTC_Init(void){
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP,ENABLE); // 使能PWR和BKP时钟
	PWR_BackupAccessCmd(ENABLE);                            // 使能RTC和后备寄存器访问
//	RCC_LSEConfig(RCC_LSE_ON);
//	while(RCC_GetFlagStatus(RCC_FLAG_LSERDY) == RESET);
//	
//	RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);
	
	RCC_LSICmd(ENABLE);                                     // 使能LSI时钟
	
	while(RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET);     // 等待LSI时钟准备就绪
	
	RCC_RTCCLKConfig(RCC_RTCCLKSource_LSI);                 // 选择LSI作为RTC时钟源
	
	RCC_RTCCLKCmd(ENABLE);                                  // 使能RTC时钟
	
	RTC_WaitForSynchro();                                   // 等待RTC时钟同步
	
	RTC_WaitForLastTask();                                  // 等待上一次RTC操作完成
	
	RTC_ITConfig(RTC_IT_SEC,ENABLE);                        // 使能秒中断
	
	RTC_WaitForLastTask();                                  // 等待上一次RTC操作完成
	
	//RTC_SetPrescaler(32768-1);
	RTC_SetPrescaler(40000-1);                              //设定RTC频率            
	
	RTC_WaitForLastTask();                                  // 等待上一次RTC操作完成
	
	NVIC_InitTypeDef NVIC_InitStructure;                    // 定义NVIC_InitTypeDef结构体
	NVIC_InitStructure.NVIC_IRQChannel = RTC_IRQn;          // 选择RTC中断
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;   // 设置抢占优先级
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;      // 设置子优先级
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;         // 使能中断
	
	NVIC_Init(&NVIC_InitStructure);                         // 初始化NVIC_InitTypeDef结构体

}


void RTC_SetTime(uint8_t hour, uint8_t min, uint8_t sec)
{
  struct tm time_start;               // 定义一个时间结构体,该结构体内存储的时间格式为正常时间
  time_start.tm_year = 70;    // 设置年,这个年指的是当前年数减1900的得到数字，范围为0-128
  time_start.tm_mon = 0;      // 设置月,这个月指的是当前月数减1得到的数字，范围为0-12，因为Unix时间是从1月开始算的
  time_start.tm_mday = 1;    // 设置日
  time_start.tm_hour = hour;    // 设置小时
  time_start.tm_min = min;      // 设置分钟
  time_start.tm_sec = sec;      // 设置秒数
  // printf("Set Unxi = %d\r\n", mktime(&time_start));
	RTC_SetCounter(mktime(&time_start));       // 设置RTC计数器
	RTC_WaitForLastTask();      // 等待上一次RTC操作完成
}


void RTC_GetTime(void)                  // 获取RTC时间
{
	struct tm *time;                // 定义一个时间结构体
	time_t time_temp = RTC_GetCounter();   // 获取RTC计数器
	time = localtime(&time_temp);   // 将Unix时间戳转换为时间结构体
  DateHour = time->tm_hour;
  DateMin = time->tm_min;
  DateSec = time->tm_sec;
  // printf("unix:%d\n",time_temp);
	// printf("%d-%d-%d-%d-%d",time->tm_year+1900,time->tm_mon+1,time->tm_mday,time->tm_wday,time->tm_yday);
	// printf("-%d-%d-%d\n",time->tm_hour,time->tm_min,time->tm_sec);
}

void RTC_IRQHandler()                           // RTC中断处理函数
{
	if(RTC_GetITStatus(RTC_IT_SEC) != RESET)    // 判断是否是秒中断
	{
		TimeRead_Flag = 1;                      // 设置读秒标志位
    RTC_GetTime();
    if(DateHour == 23 && DateMin == 59 && DateSec == 59) UpdateDay_Flag = 1;;
		RTC_ClearITPendingBit(RTC_IT_SEC);      // 清除秒中断标志位
		RTC_WaitForLastTask();                  // 等待最后一个任务结束
	}
}



/**
  * @brief  Retargets the C library printf function to the USART.
  * @param  None
  * @retval None
  */
PUTCHAR_PROTOTYPE
{
  /* Place your implementation of fputc here */
  /* e.g. write a character to the USART */
  USART_SendData(EVAL_COM1, (uint8_t) ch);

  /* Loop until the end of transmission */
  while (USART_GetFlagStatus(EVAL_COM1, USART_FLAG_TC) == RESET)
  {}

  return ch;
}

#ifdef  USE_FULL_ASSERT

/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t* file, uint32_t line)
{ 
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */

  /* Infinite loop */
  while (1)
  {
  }
}
#endif

/**
  * @}
  */



void USART1_IRQHandler(void){
    uint8_t data = 0;
    
    if(USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        /* 读取接收到的数据 */
        data = USART_ReceiveData(USART1);
        
        /* 这里可以处理接收到的数据 */
        RxDat = data;
        // USART_SendData(USART1, data);
        // ... 你的处理代码 ...
        
        /* 清除中断标志 */
        USART_ClearITPendingBit(USART1, USART_IT_RXNE);
    }
    
    /* 其他中断处理 */
    if(USART_GetITStatus(USART1, USART_IT_TXE) != RESET)
    {
        /* 发送中断处理 */
        USART_ClearITPendingBit(USART1, USART_IT_TXE);
    }
    
    if(USART_GetITStatus(USART1, USART_IT_ORE) != RESET)
    {
        /* 过载错误处理 */
        USART_ClearITPendingBit(USART1, USART_IT_ORE);
    }
}
