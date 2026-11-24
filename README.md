# 挑战实验 9 · 阶段 3 中文分词与停用词过滤

本包包含阶段 3 的 C 源码、Python 预处理脚本、测试用例、性能报告和《红楼梦》120 回语料。可解压至独立目录直接运行。若要合并至已有的 `book_search` 工程，可从包中复制 `src/`、`tools/`、`config/`、`tests/` 和 `requirements.txt`，继续使用工程已有的章节数据。

## 在 Windows 运行

在解压目录打开 PowerShell。命令需要 Python 3.10 或更新版本，以及 GCC（如 MinGW-w64）。

```powershell
py -m pip install -r requirements.txt
gcc -std=c11 -Wall -Wextra -O2 src/main.c src/boolean_query.c -o book_search.exe
py tools/preprocess.py data/hongloumeng 120 build
.\book_search.exe build/tokens 120 tests/stage3_queries.txt --words
```

如果安装了 Visual Studio 的 C/C++ 工具，可以在 x64 Native Tools Command Prompt 用下面这条命令编译，其他命令中的路径参数照用。

```bat
cl /nologo /std:c11 /utf-8 /W4 /O2 /Fe:book_search.exe src\main.c src\boolean_query.c
```

第一个查询应输出 `红楼梦 -> [5, 25, 120]`。查询文件以 UTF-8 保存，布尔运算符使用大写。每行输入一个关键词或表达式，如 `贾宝玉 AND 林黛玉`。词模式按完整词查词典，请用简体词查询。括号优先，`AND` 和二元 `NOT` 同级且先于 `OR`，同级从左向右。

## 验证与复现

```powershell
py tools/check.py ./book_search.exe
py tools/evaluate.py ./book_search.exe build 120
```

第一条检查阶段 2 的 17 组原用例和阶段 3 的 13 组用例。阶段 3 的固定答案对应 `tests/fixtures/` 的 4 个小样本，使用全书时章节号会不同。第二条把阶段 2 的单字交集与阶段 3 的整词匹配都拿来对照原文连续子串，并显示同一程序的索引大小。省略 `--words` 就是原来的单字模式。

## 处理流程

1. `preprocess.py` 使用 OpenCC 将繁体章节转为简体，输出到 `build/normalized/`。
2. jieba 以精确模式切词，加载 `config/userdict.txt` 补充书中专名，过滤标点与 `config/stopwords.txt` 中的词，输出每行一词的 `build/tokens/`。
3. C 程序以词为哈希键，将按顺序读到的章号写入倒排表。同一词即使在一回出现多次，也只记录一次章号。
4. 查询直接查词的倒排表，沿用阶段 2 的双指针代码执行交集、并集和差集。

停用词文件和用户词典都可用 UTF-8 修改，修改后要重新运行预处理。也可用 `--stopwords 路径` 和 `--userdict 路径` 指定其他文件。使用空文件可分别关闭过滤和自定义词典以复现实验对照。

语料来源与拆分规则见 `corpus-source.txt`，实测数据见 `stage3_report.md`。
