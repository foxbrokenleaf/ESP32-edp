# tools/ 辅助脚本

这些脚本只在开发/验证时使用，不参与固件构建。

| 脚本 | 作用 |
| --- | --- |
| `gen_gb2312_map.py` | 生成 `main/gb2312_map.bin`（严格 GB2312 的 Unicode → GB2312 映射表，7445 项 × 4 字节）。只有在需要重建映射表时才运行。 |
| `verify_gb2312_map.py` | 用 Python 内置的严格 `gb2312` 编解码器逐项校验 `main/gb2312_map.bin`：条目数、升序、编码是否一致、有没有漏项。 |
| `check_web_page.py` | 不烧板子检查网页：把 `main/http.c` 里的 `INDEX_HTML[]` 还原成真实页面，用 `node --check` 校验 JS 语法，检查 JS 用到的元素 id 是否存在、`/api/xxx` 是否都在 `routes[]` 里注册。 |
| `test_gb2312_map.c` | `gb2312_map.c` 的主机端（PC）单元测试：映射表自检、二分查找、UTF-8 解码、整串转 GB2312。 |
| `gbk_tools.ps1` | 源码编码转换（GBK ⇄ UTF-8），项目里 `main/` 的编码并不统一，编辑前先转成 UTF-8、改完转回 GBK 最稳妥。 |

## 常用命令（在项目根目录执行）

```powershell
# 校验映射表 / 网页 / 编码转换逻辑
python tools\verify_gb2312_map.py
python tools\check_web_page.py

# 主机端单元测试（需要 MinGW gcc）
gcc -std=gnu11 -Wall -Wextra -I main tools\test_gb2312_map.c main\gb2312_map.c -o build\test_gb2312_map.exe
.\build\test_gb2312_map.exe

# 编码转换示例（GBK -> UTF-8 编辑 -> 存回 GBK）
powershell -ExecutionPolicy Bypass -File tools\gbk_tools.ps1 -Mode to-utf8 -Path main\GUI.c -OutFile .tmp_GUI.c
powershell -ExecutionPolicy Bypass -File tools\gbk_tools.ps1 -Mode to-gbk  -Path .tmp_GUI.c -OutFile main\GUI.c
```

## 清单（checklist）数据的约定

- 墨水屏字库芯片 GT30L32S4W 只认 GB2312，所以网页提交的 UTF-8 文字在绘制前
  会用 `Gb2312_FromUtf8()` 转码；保存时 `Checklist_ApplyJson()` 已经拒绝
  GB2312 之外的字符（emoji、GBK 扩展字等）。
- 每项最多 `CHECKLIST_ITEM_TEXT_LEN`（40）字节 UTF-8，约 13 个汉字。
- NVS：命名空间 `list_store`、键 `items`、记录头 `LST1`。
