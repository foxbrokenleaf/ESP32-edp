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
uint8_t DateToday = 19;
uint8_t DateHour = 23;
uint8_t DateMin = 45;
uint8_t DateSec = 5;
uint8_t DateArrary[6][7] = {
  {0, 0, 0, 0, 0, 0, 1,},
  {2, 3, 4, 5, 6, 7, 8,},
  {9, 10, 11, 12, 13, 14, 15,},
  {16, 17, 18, 19, 20, 21, 22,},
  {23, 24, 25, 26, 27, 28, 00,},
  {00, 00, 00, 00, 00, 00, 00,}
};

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
void PrevMonth(void);
void NextMonth(void);
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
  uint16_t rx_dat = 0;

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
    EPD_Init();
    
    DisplayTask();
    
    while(1)
    {
      Delay_s(20);
      PrevMonth();
      DisplayTask();
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

void UpdateDateTask(void){

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


