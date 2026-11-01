/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-19 20:12:00
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-01 22:19:06
 * @FilePath: \epd\main\GUI.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "GUI.h"
#include "EPD.h"
#include "GT30L32S4W.h"

void CourseGUI(void){
    EPD_Clear();
    
    //上课状态
    EPD_ShowChineseChar(0, 0, Font_32x32_E6ADA3, EPD_32X32, 0);
    EPD_ShowChineseChar(0, 32, Font_32x32_E59CA8, EPD_32X32, 0);
    EPD_ShowChineseChar(0, 64, Font_32x32_E4B88A, EPD_32X32, 0);
    EPD_ShowChineseChar(0, 96, Font_32x32_E8AFBE, EPD_32X32, 0);        

    //课程名称
    GT30L32S4W_ShowString(48, 32, "Python\xB8\xDF\xBC\xB6\xD3\xA6\xD3\xC3", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_32X32, 0);
    GT30L32S4W_ShowString(112, 64, "\xC0\xED\xC2\xDB\xBF\xCE", GT30L32S4W_FONT_ASCII_16X32, GT30L32S4W_FONT_GB2312_32X32, 0);

    //上课地点
    EPD_ShowChineseChar(276, 0, Font_16x16_E7A791, EPD_16X16, 0);
    EPD_ShowChar(280, 16, 'D', EPD_8X16, 0);
    EPD_ShowChar(280, 32, '-', EPD_8X16, 0);
    EPD_ShowChar(280, 48, '4', EPD_8X16, 0);
    EPD_ShowChar(280, 64, '0', EPD_8X16, 0);
    EPD_ShowChar(280, 80, '8', EPD_8X16, 0);

    //上课时间
    //固定
    EPD_ShowChar(155, 131, 'S', EPD_8X16, 0);
    EPD_ShowChar(251, 131, 'E', EPD_8X16, 0);
    //变动
    EPD_ShowString(163, 131, "15:35~17:10", EPD_8X16, 0);

    //第n周
    EPD_ShowChineseChar(276, 100, Font_16x16_E783AB, EPD_16X16, 0);
    EPD_ShowNum(276, 116, 2, 2, EPD_8X16, 0);   //n
    EPD_ShowChineseChar(276, 132, Font_16x16_E783AB, EPD_16X16, 0);

    //时间
    EPD_ShowNum(1, 132, 2026, 4, EPD_8X16, 0);  //年
    EPD_ShowNum(33, 132, 10, 2, EPD_8X16, 0);   //月
    EPD_ShowNum(49, 132, 20, 2, EPD_8X16, 0);   //日

    EPD_ShowNum(73, 132, 0, 2, EPD_8X16, 0);   //时
    EPD_ShowChar(89, 132, ':', EPD_8X16, 0);
    EPD_ShowNum(97, 132, 2, 2, EPD_8X16, 0);   //分

    //框线
    EPD_DrawLine(143, 128, 143, 151);
    EPD_DrawLine(128, 128, 128, 151);
    EPD_DrawLine(47, 0, 47, 128);
    EPD_DrawLine(32, 0, 32, 128);
    EPD_DrawLine(272, 0, 272, 151);
    EPD_DrawLine(0, 128, 272, 128);

    //美化

    EPD_Update();
}