# 03 — 词法分析器（Tokenizer）

> **概括**：本篇精读 `as_tokendef.h` 与 `as_tokenizer.h/.cpp`，约 850 行代码，是整个编译器中最小、最自洽的模块。它把字符流切成 token，不维护任何跨调用状态，因此既是编译流水线的第一站，也是引擎暴露给宿主程序复用的工具（`asIScriptEngine::ParseToken`）。
>
> **难度**：★☆☆☆☆（入门友好，适合作为读编译器源码的第一课）
> **前置**：第 02 篇（引擎对象与编译流水线总览），知道 `asCScriptEngine` 持有一个 `asCTokenizer tok` 成员即可。

---

## 1. 模块定位

- **编译流水线第一站**：`asCParser`（下一篇）每前进一步都调用 `asCTokenizer::GetToken()` 取下一个 token，词法分析是**按需拉取（pull）**的，没有"先整体 tokenize 成数组"的阶段。
- **宿主可复用**：公开 API `asIScriptEngine::ParseToken()`（`as_scriptengine.cpp:1422`）就是对 `tok.GetToken()` 的一层薄封装。引擎内部也用它校验注册名字的合法性（如 `RegisterEnumValue` 在 `as_scriptengine.cpp:6446` 用 `ParseToken` 确认枚举值名是一个完整的 `asTC_IDENTIFIER`）。
- **官方示例的用法**：`sdk/samples/asrun/source/main.cpp:775` 在 `#pragma` 回调里用 `engine->ParseToken` 循环切词，过滤掉注释和空白，只留下标识符/关键字/常量，从而解析 `#pragma` 指令内容——这是"tokenizer 当通用切词器用"的典型范例。

## 2. Token 的分类体系：两层枚举

### 2.1 粗分类 asETokenClass（面向宿主）

`angelscript.h:331-339` 定义了 6 个大类：`asTC_UNKNOWN / KEYWORD / VALUE / IDENTIFIER / COMMENT / WHITESPACE`。宿主调用 `ParseToken` 只拿到这一层——足够做语法高亮、`#pragma` 解析这类粗粒度工作。

### 2.2 细分类 eTokenType（面向 Parser）

`as_tokendef.h:46-183` 的 `eTokenType` 是内部真正的 token 类型，分组组织：

- 特殊：`ttUnrecognizedToken`、`ttEnd`（行 48-50）
- 空白与注释：`ttWhiteSpace / ttOnelineComment / ttMultilineComment`（行 53-55）——注意**空白和注释也是 token**，tokenizer 不吞掉它们，跳过是 Parser 的责任（这让行号计算和文档注释提取成为可能）
- 原子值：标识符与 6 种字面量，包括 `ttHeredocStringConstant`（`"""..."""`）和 `ttNonTerminatedStringConstant`（未闭合字符串，行 58-66）
- 运算符/标点：行 69-128
- 保留关键字：行 131-182

### 2.3 关键字表 tokenWords

`as_tokendef.h:185-190` 的 `sTokenWord` 结构存 `{word, wordLength, tokenType}`，长度由宏 `asTokenDef`（行 192）在编译期用 `sizeof(str)-1` 算好，运行时零 `strlen`。

`tokenWords[]`（行 194-306）把**符号运算符和单词关键字放在同一张表里**——`"+"` 和 `"while"` 地位相同。几个值得注意的条目：

- 同义映射：`"int32"` → `ttInt`（行 280）、`"uint32"` → `ttUInt`（行 300）；`&&` 与 `and` 都是 `ttAnd`（行 219/249）
- 条件编译：定义 `AS_USE_DOUBLE_AS_FLOAT` 时 `"double"` 直接映射为 `ttFloat`（行 261-265）
- `"!is"` 是一个三字符 token（行 247），后面会看到它引出的边界问题

**最长匹配**不是靠表的书写顺序，而是靠运行时构建的跳转表按长度排序（见第 5 节）。

### 2.4 上下文关键字

`as_tokendef.h:312-329` 单独定义了 `shared`、`final`、`override`、`get/set`、`property` 等字符串常量。它们**不在 tokenWords 表里**，tokenizer 一律返回 `ttIdentifier`，由 Parser 在特定语法位置比对字符串——这样 `int final = 1;` 依然合法，语言演进时加"软关键字"不会破坏旧脚本。

## 3. 关键代码走读

### 3.1 GetToken / ParseToken 主流程

`GetToken`（`as_tokenizer.cpp:152-164`）只是参数适配；核心是 `ParseToken`（行 166-181），判定顺序是一条清晰的短路链：

```
IsWhiteSpace → IsComment → IsConstant → IsIdentifier → IsKeyWord → Unknown(长度1)
```

为什么是这个顺序：

1. **空白最先**：出现频率最高，且首字符集合（空格/\t/\r/\n/BOM）与其他类别完全不相交。
2. **注释在常量之前、且必须在关键字之前**：`//` 和 `/*` 的首字符 `/` 同时是除法运算符，若先查关键字表，`// comment` 会被切成 `ttSlash`。`IsComment`（行 223-268）只看前两个字符就能裁决。
3. **常量在标识符之前**：数字开头（或 `.` 后跟数字）与字母开头不相交，顺序其实可换；但字符串的引号必须在关键字之前处理。
4. **标识符在关键字之前**（这是最反直觉的一步，见 3.4）。
5. 全部失败则返回 `ttUnrecognizedToken`，长度硬编码为 1（行 177-178），保证 Parser 报错后还能继续前进，不会死循环。

`IsWhiteSpace`（行 183-221）还有个细节：UTF-8 BOM（EF BB BF）被当作 3 字节空白（行 186-194），所以带 BOM 的脚本文件无需宿主预处理。连续空白合并为一个 token；单行注释把结尾的 `\n` 也算进 token 长度（行 244）。

### 3.2 数字字面量的状态机

`IsConstant`（行 282-428）前半段是手写的数字状态机：

- **入口条件**（行 285）：首字符是数字，**或** `.` 后紧跟数字——所以 `.5` 是合法字面量。
- **进制前缀**（行 288-311）：`0` 开头且第二字符是 `b/o/d/x`（大小写均可）时按 2/8/10/16 进制扫描，返回 `ttBitsConstant`。注意没有 C 风格的 `0777` 八进制——必须写 `0o777`，避开了经典陷阱。
- **数字分隔符**：`IsValidSeparatorDigitInRadix`（行 270-280）允许 `1'000'000` 这种写法，条件是 `'` 两侧都必须是当前进制的合法数字（所以 `1''0`、`'1`、`1'` 都非法）。`IsDigitInRadix`（行 144-150）用 ASCII 区间减法一行判定任意 ≤36 进制的数字。
- **小数与指数**（行 320-343）：整数部分后若是 `.` 或 `e/E`，先吃小数部分，再吃可带正负号的指数。
- **后缀裁决**（行 345-358）：有 `f/F` 后缀 → `ttFloatConstant`；否则 → `ttDoubleConstant`（`AS_USE_DOUBLE_AS_FLOAT` 下也归为 float）。没有小数点也没有指数则回落为 `ttIntConstant`（行 362-364）。

**`1.x` 歧义**：行 320-330 的逻辑是"见 `.` 就贪心吃进数字"，`1.x` 会被切成 double 字面量 `1.` 加标识符 `x`。AngelScript 里数字字面量没有可访问的成员，所以这不构成问题；但如果你的语言要支持 `1.toString()`，就必须在这里加"`.` 后必须是数字才并入"的回看（对比 Ruby/Kotlin 的处理）。

### 3.3 字符串字面量：三种形态一个函数

`IsConstant` 的后半段（行 368-425）：

- **heredoc**（行 371-389）：`"""` 开头（要求剩余长度 ≥6，即至少能装下开闭各三个引号）就向后找配对的 `"""`，**中间不处理任何转义**，天然适合内嵌代码/正则。找不到则返回 `ttNonTerminatedStringConstant`。
- **普通字符串**（行 392-422）：单引号双引号同权（`quote = source[0]`，行 394），用一个 `evenSlashes` 布尔量追踪反斜杠奇偶性（行 395、417）来判断引号是否被转义——`\\"` 中的引号是真结尾，`\"` 不是。这个技巧比"跳过转义序列"的写法更短且等价。注意 tokenizer **只定界不解码**，`\n` 的真正转换在编译期字符串处理阶段。
- **多行**：普通字符串里遇到裸 `\n` 不报错，只是把类型改为 `ttMultilineStringConstant`（行 410-411），是否允许由引擎属性在编译期裁决——tokenizer 不做政策决定。
- **多字节支持**（行 399-408）：定义 `AS_DOUBLEBYTE_CHARSET` 且 `engine->ep.scanner == 0`（ASCII 模式）时，高位为 1 的字节被视为双字节字符的前导字节，连同下一字节一起吞入——防止 Big5/GBK 等编码中第二字节恰好是 `0x22`（`"`）导致字符串提前截断。

### 3.4 标识符与关键字：为什么 IsIdentifier 在前

`IsIdentifier`（行 430-466）：

- 首字符是字母/下划线，或**负值字节**且开启 `engine->ep.allowUnicodeIdentifiers`（行 440）。行 432-434 特意用 `signed char`——ARM/PPC 上 `char` 默认无符号，UTF-8 多字节序列的字节值 ≥0x80，转成 signed 后 `c < 0` 即可统一判定，这是全文件最平台相关的一行。
- 扫完整个标识符后，行 459 有一步关键回查：`IsKeyWord(source, tokenLength, tokenLength, tokenType)`——**把 sourceLength 限制为标识符自身长度**再查关键字表。若命中则返回 false，让 `ParseToken` 落到下一行的 `IsKeyWord` 正式给出关键字类型。

这个"先按标识符规则扫到底，再查表"的设计一次性解决了两个问题：`while1` 不会被切成 `while` + `1`（因为整体先被认成标识符，回查时 `sourceLength=6` 匹配 `while` 后过不了边界检查）；而 `+`、`::` 这类符号 token 不满足标识符规则，自然落到 `IsKeyWord` 处理。

`IsKeyWord`（行 468-506）本身：按首字节取桶、桶内已按长度降序排列，`strncmp` 逐个试——**第一个命中即最长匹配**（行 476-479）。行 485-497 的边界检查处理另一类粘连：`!isTrue` 里 `!is`（三字符 token）前缀匹配成功，但 token 末字符 `s` 是字母且下一字符 `T` 也是标识符字符，说明切下去会撕裂标识符，于是 `continue` 让更短的 `!` 兜底。条件里"token 末字符必须是字母数字"的限定保证了 `x=-1` 中 `=` 后面跟 `-` 不会误触发此规则。

## 4. 性能细节：keywordTable 跳转表

`as_tokenizer.h:77` 声明 `const sTokenWord **keywordTable[256]`——一个以**首字节**为索引的 256 槽跳转表。`InitJumpTable`（`as_tokenizer.cpp:74-117`）在构造时遍历 `tokenWords`：

- 每个首字符懒分配一个 32 槽的指针数组（行 90-96，注释自嘲"同首字符的关键字总不会超过 32 个吧？"——目前最多的 `i` 开头有 11 个）。
- 插入时按 `wordLength` **降序**排位（行 98-115），把"最长匹配"从算法问题变成数据布局问题：查表时线性扫桶，先长后短，首个 `strncmp` 命中即答案。
- 平均代价：一次数组索引 + 桶内几次长度受限的 `strncmp`。对 ~110 个 token 的规模，这比哈希表或 trie 的常数更小，代码也更简单。
- 表不是纯静态的：`foreach` 受引擎属性开关控制（行 84-85），宿主调 `SetEngineProperty(asEP_FOREACH_SUPPORT, ...)` 时引擎会重建跳转表（`as_scriptengine.cpp:496-498`）——关键字表是**每引擎实例**可配置的。

## 5. 设计决策与权衡

1. **手写而非生成器（flex/re2c）**：token 集合小且稳定，手写代码零依赖、可调试、可条件编译（`AS_USE_DOUBLE_AS_FLOAT`、`AS_DOUBLEBYTE_CHARSET`）、可运行时开关关键字（foreach）。生成器擅长的"复杂正则合成 DFA"在这里没有用武之地——最复杂的数字字面量状态机也就 80 行。
2. **无状态 + 按需拉取**：`GetToken` 是 `const` 方法，输入只有 `(指针, 长度)`，输出只有 `(类型, 长度)`，不记录行号、不持有游标。好处：
   - Parser 可以随意**回溯**（存个偏移量即可重扫），这为 AngelScript 语法里需要前瞻的地方（如区分声明和表达式）提供了廉价的 rewind；
   - 同一个 tokenizer 实例可被引擎、Builder、宿主并发只读复用，无需加锁；
   - 天然支持增量/片段解析——asrun 拿它切 `#pragma` 就是明证。
   代价是行号必须由调用方自己数（Parser 通过累计 token 里的换行来算），以及每次回溯要重扫一遍字符。
3. **空白/注释作为一等 token**：而不是在词法层静默跳过。这让"提取声明前的文档注释"、"精确错误列号"等需求不需要旁路通道。
4. **政策与机制分离**：tokenizer 只报告"这是个多行字符串/未闭合字符串"，是否报错由上层根据引擎属性决定。

## 6. 如果你要自己实现

最小 tokenizer 清单（对照本文件逐条打钩）：

1. 分类判定链，注意 `/` 开头先查注释再查运算符；
2. 关键字表 + 首字符索引 + 桶内按长度降序 = 最长匹配；
3. 关键字/标识符粘连：要么"先扫标识符再回查表"（AngelScript 方案，行 459），要么查表后做边界字符检查（行 485-497）——AngelScript 两个都做了，因为 `!is` 这种符号+字母混合 token 走不了第一条路；
4. 数字：进制前缀、`.5` 入口、`1.x` 的贪心歧义（想支持字面量方法调用就要回看一位）、数字分隔符两侧必须是数字、指数符号后可跟正负号；
5. 字符串：转义引号用反斜杠奇偶计数即可；未闭合字符串要返回专门的错误 token 而不是吃掉整个文件剩余部分之后不给长度；
6. 任何输入都必须前进至少 1 字节（行 178），否则错误恢复时 Parser 会死循环;
7. 平台坑：`char` 的符号性（行 432-434）、UTF-8 BOM（行 186）。

## 7. 动手实验

用公开 API 打印任意脚本的 token 流（无需接触内部头文件）：

```cpp
#include <angelscript.h>
#include <cstdio>
#include <cstring>

int main()
{
    asIScriptEngine *engine = asCreateScriptEngine();
    const char *script = "int n = 0x1F; // count\nstring s = \"\"\"raw \" text\"\"\";\nif( n !is null ) {}";
    const char *tcName[] = { "UNKNOWN", "KEYWORD", "VALUE", "IDENTIFIER", "COMMENT", "WHITESPACE" };

    size_t pos = 0, len = strlen(script);
    while( pos < len )
    {
        asUINT tokenLen = 0;
        asETokenClass tc = engine->ParseToken(script + pos, len - pos, &tokenLen);
        printf("%-10s |%.*s|\n", tcName[tc], (int)tokenLen, script + pos);
        pos += tokenLen;
    }
    engine->ShutDownAndRelease();
    return 0;
}
```

观察点：`0x1F` 是一个 `VALUE`；`// count\n` 整体（含换行）是一个 `COMMENT`；heredoc 里的孤立 `"` 没有截断字符串；`!is` 是单个 `KEYWORD`。再试着把 `!is` 改成 `!isTrue`，验证 3.4 节的边界检查。

## 8. 扩展阅读

- Crafting Interpreters — Scanning：https://craftinginterpreters.com/scanning.html （与本篇同构的教学实现，可对照"maximal munch"一节）
- AngelScript 官方手册 — 词法结构（tokens）：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_script_lexical.html
- AngelScript 官方手册 — `asIScriptEngine::ParseToken`：https://www.angelcode.com/angelscript/sdk/docs/manual/classas_i_script_engine.html
- 引擎属性列表（`asEP_ALLOW_UNICODE_IDENTIFIERS`、`asEP_ALLOW_MULTILINE_STRINGS` 等词法相关开关）：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_adv_custom_options.html

下一篇（04）：Parser——这些 token 如何被组织成脚本节点树（`asCScriptNode`），以及回溯式递归下降的实现。
