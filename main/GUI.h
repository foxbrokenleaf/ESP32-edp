/*
 * @Author: 'foxbrokenleaf' '2353919304@qq.com'
 * @Date: 2026-10-19 20:12:05
 * @LastEditors: 'foxbrokenleaf' '2353919304@qq.com'
 * @LastEditTime: 2026-11-26 21:40:00
 * @FilePath: \epd\main\GUI.h
 * @Description: 界面（GUI）对外接口。
 *               页面在 GUI.c 里实现：0=完整课表, 1=时钟, 2=日历, 3=清单，
 *               用 layout_no 选页，改 GUI_LAYOUT_COUNT 即可增删页面。
 */
#ifndef __GUI_H__
#define __GUI_H__

#include <stdint.h>

/* 各界面在 layout_no 里的编号（网页保存后也会直接跳到对应页面） */
#define GUI_LAYOUT_TIMETABLE    0       /* 完整课程表（网页导入的 NVS JSON） */
#define GUI_LAYOUT_CLOCK        1
#define GUI_LAYOUT_CALENDAR     2
#define GUI_LAYOUT_CHECKLIST    3

/* 界面总数：底部页码点、按键翻页都会跟着这个宏走 */
#define GUI_LAYOUT_COUNT        4

/* 当前显示的界面编号（0 ~ GUI_LAYOUT_COUNT-1） */
extern uint8_t layout_no;

/* 按 layout_no 重绘整个屏幕 */
void GuiUpdate(void);

/* 各界面（GUI.c 内部使用，也允许外部单独调用） */
void CourseGUI(void);
void ClockGUI(void);
void CalendarGUI(void);
void ListGUI(void);

/* 课程表页：按 week（学期周次）筛选 NVS 中导入的课程并绘制 */
void TimetableGUI(uint8_t week);

#endif /* __GUI_H__ */
