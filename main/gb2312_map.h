/*
 * @Description: Unicode -> GB2312 映射表接口。
 *
 * 字库芯片 GT30L32S4W 只接受 GB2312 编码，而网页提交的清单文字是 UTF-8，
 * 所以在绘制前需要把每个 Unicode 码点换算成 GB2312 双字节编码。
 *
 * 映射数据由 tools/gen_gb2312_map.py 生成 main/gb2312_map.bin，
 * 再通过 CMakeLists.txt 的 EMBED_FILES 打进固件（约 30 KB，一个映射 4 字节）。
 */
#ifndef __GB2312_MAP_H__
#define __GB2312_MAP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * @brief  在内置映射表里查找 Unicode 码点对应的 GB2312 编码。
 * @param  unicode   Unicode 码点（只支持 BMP，0x80 以上的字符才有意义）
 * @param  gb_code   输出 GB2312 双字节编码，高字节为区码、低字节为位码
 * @return true=找到，false=不在 GB2312 字符集内
 */
bool Gb2312_FromUnicode(uint32_t unicode, uint16_t *gb_code);

/*
 * @brief  与 Gb2312_FromUnicode() 相同，但显式传入映射表，便于在 PC 上做单元测试。
 * @param  map       映射表起始地址（每项 4 字节：uint16 unicode + uint16 gb2312，小端）
 * @param  map_size  映射表字节数
 */
bool Gb2312_FromUnicodeData(const uint8_t *map, size_t map_size,
                            uint32_t unicode, uint16_t *gb_code);

/* 内置映射表条目数（每项 4 字节），便于日志与自检 */
size_t Gb2312_MapCount(void);

/*
 * @brief  解码一个 UTF-8 字符。
 * @param  cursor  输入输出：指向当前字节，成功时前移到下一个字符
 * @param  end     文本结束位置
 * @return 码点（>= 0）；-1 表示编码非法（过长编码 / 代理区 / BMP 以外 / 截断）
 */
int32_t Gb2312_Utf8Next(const uint8_t **cursor, const uint8_t *end);

/*
 * @brief  把整串 UTF-8 文本转换成字库芯片需要的 GB2312 编码。
 * @param  out       输出缓冲区（out_size 要为 '\0' 留位置）
 * @param  out_size  输出缓冲区大小
 * @return 写入的字节数（不含 '\0'）；容量不足时按整字截断
 *
 * 说明：GB2312 里没有的字符写成 '?'。正常流程里 Checklist_ApplyJson()
 * 已经做过校验，不会走到这里，保留兜底避免整行文字消失。
 */
size_t Gb2312_FromUtf8(const char *utf8, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* __GB2312_MAP_H__ */
