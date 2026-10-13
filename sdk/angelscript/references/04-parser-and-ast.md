# 04 — 语法分析器与 AST（asCParser / asCScriptNode）

> **系列**：AngelScript 源码精读（16 篇）· 第 04 篇
> **难度**：★★★☆☆
> **前置**：第 03 篇（词法分析器 asCTokenizer）——parser 逐 token 调用 tokenizer，不了解 sToken/eTokenType 会读不下去
> **核心文件**：`as_parser.h/cpp`（约 4900 行）、`as_scriptnode.h/cpp`（不到 200 行）

一句话概括：asCParser 是一个手写的**递归下降解析器**，把 token 流变成由单一节点类 asCScriptNode 组成的树；它刻意做得"很薄"——不解析运算符优先级、不存文本、支持"只找边界不进内部"的浅解析——把语义留给 builder 和 compiler。

---

## 1. 模块定位

```
源码文本 --asCTokenizer--> token 流 --asCParser--> asCScriptNode 树 --+--> asCBuilder（收集声明，第一遍）
                                                                      +--> asCCompiler（编译函数体，第二遍）
```

parser 没有独立的"词法阶段"：它按需调用 `engine->tok.GetToken()`（as_parser.cpp:1035），解析到哪 token 化到哪。下游有两个消费者：

- **asCBuilder**：调用 `ParseScript()` 得到整个脚本的树，从中摘出类、函数、全局变量等**声明**（as_builder.cpp:378 `parser.GetScriptNode()`）；
- **asCCompiler**：编译某个函数体时，再 new 一个 asCParser，对函数体那段源码**重新做完整解析**（as_compiler.cpp:797-800）。

也就是说同一段函数体源码会被扫两次——第一次只数花括号找结尾，第二次才真正建树。这是 AngelScript 两遍编译的内存换时间策略（见第 5 节）。

## 2. AST 节点设计：一个类打天下

### 2.1 单一节点类 + 枚举区分

AngelScript 不用"每种语法结构一个多态子类"的经典 OO 设计，而是只有一个 `asCScriptNode`（as_scriptnode.h:109-137），用枚举 `eScriptNode`（as_scriptnode.h:47-98，snScript/snFunction/snExpression/snIf... 共 49 种）区分种类：

```cpp
// as_scriptnode.h:123-132
eScriptNode nodeType;     // 节点种类（snIf、snExprValue...）
eTokenType  tokenType;    // 关联 token 类型（如运算符节点存 ttPlus）
size_t      tokenPos;     // 源码起始偏移
size_t      tokenLength;  // 覆盖长度
asCScriptNode *parent, *next, *prev, *firstChild, *lastChild;
```

取舍：多态节点类型安全、访问字段直观，但类数量爆炸、序列化/池化困难；单一类 + 枚举则**分配释放极其统一**——所有节点等大，可以走同一个内存池。代价是消费者要靠"约定"理解结构（如 snFunction 的第一个孩子是返回类型、然后是标识符、参数表……），as_compiler 里到处是 `node->firstChild->next` 这样的裸导航。

### 2.2 first-child / next-sibling 表示法

任意叉树用二叉链表示：每个节点只有 `firstChild/lastChild` 加兄弟双向链 `next/prev`，孩子数量不定也无需数组。遍历"第 n 个孩子"就是沿 `next` 走 n 步——消费端代码里常见 `node->firstChild->next->next`。

配套操作只有三个：

- `AddChildLast()`（as_scriptnode.cpp:131-154）：尾插，并顺手调用 `UpdateSourcePos` 扩张父区间；对空指针（内存耗尽时 CreateNode 返回 0）静默忽略（:134），让 OOM 错误延迟到 `errorWhileParsing` 统一处理；
- `DisconnectParent()`（as_scriptnode.cpp:156-175）：把节点从父链摘下——builder 靠它"偷走"子树（比如把默认参数表达式节点摘下来单独保存），避免复制；
- `CreateCopy()`（as_scriptnode.cpp:76-100）：深拷贝，共享类/mixin 跨模块复用声明时用。

### 2.3 节点只记 (position, length)，不存文本

节点里没有任何字符串。`UpdateSourcePos()`（as_scriptnode.cpp:107-129）在每次加孩子/吃 token 时扩张 `(tokenPos, tokenLength)` 区间，使父节点区间总能覆盖所有孩子。要文本时按需切片：`tempString.Assign(&script->code[t.pos], t.length)`（as_parser.cpp:2555）。

为什么这样设计：① 零字符串拷贝，节点固定 72 字节左右；② 源码本身由 asCScriptCode 持有且在编译期不变，位置即身份；③ 错误报告需要的行列号可由 `script->ConvertPosToRowCol()` 从 pos 换算（as_parser.cpp:1075）；④ 浅解析出的"函数体节点"只需 (pos,len) 就能让第二遍解析知道从哪重启（`sourcePos = in_block->tokenPos`，as_parser.cpp:2818）。

### 2.4 内存池

`CreateNode()`（as_parser.cpp:142-153）从 `engine->memoryMgr.AllocScriptNode()` 拿内存后 placement-new；`Destroy()`（as_scriptnode.cpp:59-74）先递归销毁孩子再把内存还给池。池是个简单的自由链表（as_memory.cpp:215-243，`scriptNodePool` 空了才走 `userAlloc`）。析构函数被声明为 protected（as_scriptnode.h:134-136）强制走 `Destroy()`，防止绕过池直接 delete。

## 3. 递归下降的组织

as_parser.cpp 开头的注释（44-48 行）约定了 BNF 记号，且每个 Parse 函数上方都有 `// BNF:n:` 注释标注它实现的产生式（官方手册的语法参考就是从这些注释生成的）。主干调用树：

```
ParseScript(bool inBlock)                    as_parser.cpp:2595
 ├─ ParseImport / ParseEnumeration / ParseTypedef / ParseClass
 │  ParseMixin / ParseInterface / ParseFuncDef / ParseNamespace / ParseUsing
 ├─ ParseDeclaration（全局变量）              :4157
 └─ ParseFunction                            :3282
     ├─ ParseType + ParseTypeMod（返回类型）  :499 / :449
     ├─ ParseIdentifier                      :765
     ├─ ParseParameterList                   :787
     └─ SuperficiallyParseStatementBlock ★   :3911（只找 '}'，不建语句树）

（第二遍，由 compiler 触发）
ParseStatementBlock                          :3958
 └─ ParseStatement                           :4243
     ├─ ParseIf/For/ForEach/While/DoWhile/Switch/Return/Break/Continue/TryCatch
     ├─ ParseStatementBlock（嵌套块，递归）
     ├─ ParseDeclaration（局部变量，经 IsVarDecl() 判定 :2988）
     └─ ParseExpressionStatement             :4284
         └─ ParseAssignment                  :2124
             └─ ParseCondition（?:）          :2149
                 └─ ParseExpression          :2182
                     └─ ParseExprTerm        :2209
                         ├─ ParseExprPreOp*  :2277
                         ├─ ParseExprValue   :1624（常量/变量/函数调用/cast/lambda/括号）
                         └─ ParseExprPostOp* :2298（. [] () ++ --）
```

分派靠**先窥后退**：`GetToken(&t); RewindTo(&t);` 是全文件最高频的手势。`RewindTo`（as_parser.cpp:1054-1065）只缓存一枚 `lastToken` 避免重复 tokenize；更远距离的回溯（如 `IsVarDecl()`、`IsFuncDecl()`、`IsLambda()` 这些多 token 前瞻判定函数）就老老实实重扫。

**表达式与运算符优先级——parser 不管**。`ParseExpression`（as_parser.cpp:2182-2206）就是个循环：

```cpp
node->AddChildLast(ParseExprTerm());
for(;;) {
    if( !IsOperator(t.type) ) return node;
    node->AddChildLast(ParseExprOperator());
    node->AddChildLast(ParseExprTerm());
}
```

产出的 snExpression 是**扁平的**：`term op term op term ...` 交替排列的孩子链，`a + b * c` 在树里和 `a * b + c` 结构完全相同。优先级由 **asCCompiler** 处理：`ConvertToPostFix()`（as_compiler.cpp:10734）用运算符栈把这条链转成后缀序列（Dijkstra 调度场算法），`GetPrecedence()`（as_compiler.cpp:15134）给出优先级表，然后 `CompilePostFixExpression()`（as_compiler.cpp:10771）线性编译。这与"优先级爬升/每级一个 ParseXxx 函数"的教科书做法都不同——是第三条路：**解析器出扁平链，消费者做调度场**。

两处例外在 parser 内解决：赋值的右结合靠 `ParseAssignment` 尾部递归调用自身（as_parser.cpp:2141），三元 `?:` 在 `ParseCondition` 里显式处理（:2159-2174）。

**长前瞻判定函数**。C 系语法著名的"`T * a;` 是声明还是表达式"一类歧义，AngelScript 用一族 `IsXxx()` 布尔函数解决，它们向前扫任意多 token 后必定回退，不产生节点：

- `IsVarDecl()`（as_parser.cpp:2988）：语句块里区分变量声明与表达式语句；
- `IsFuncDecl(bool isMethod)`（:3112）：顶层/类体里区分函数与虚属性/变量；
- `IsVirtualPropertyDecl()`（:3069）、`IsLambda()`（:1760）、`IsFunctionCall()`（:2079）；
- 底层工具 `FindTokenAfterType()`（:1334）跳过一个完整类型（含 const、scope、模板参数、`@`、`[]`），`IsTemplateTypeList()`（:1446）解决 `a < b` 与模板列表 `A<B>` 的尖括号歧义。

这使文法在工程上保持 LL(1) 的骨架（每个 Parse 函数只窥 1-3 个 token 分派），把非 LL(1) 的局部全部隔离进这些可回退的判定函数里。

## 4. 两遍解析的支持：Superficially 变体

builder 第一遍只需要**声明**（签名、类型名、成员列表），函数体和变量初始化表达式在这一遍毫无用处却占大头。于是 parser 提供三个"只找边界"的浅解析：

- `SuperficiallyParseStatementBlock()`（as_parser.cpp:3911-3955）：吃掉 `{`，然后**纯数花括号层级**（level++/--），遇 level==0 收工。产出的 snStatementBlock 没有任何孩子，只有覆盖整个函数体的 (pos,len)。`ParseFunction` 的最后一步就是它（:3373-3375，注释明说"statements will be parsed on request by the compiler"）。
- `SuperficiallyParseVarInit()`（:3828-3909）：全局变量/成员变量的 `= expr` 或 `(args)` 初始化，只配平括号找到 `,` 或 `;`。
- `SuperficiallyParseExpression()`（:905-1005）：默认参数表达式，用一个字符栈配平 `()`/`{}` 直到裸露的 `,` 或 `)`。

第二遍的对应入口是三个公开重载：`ParseStatementBlock(script, block)`（:2808，注意 `sourcePos = in_block->tokenPos` 从浅节点记录的位置重启，且置 `checkValidTypes = true`）、`ParseVarInit`（:3779）、`ParseExpression`（:2511）。asCCompiler::CompileFunction 在 as_compiler.cpp:797-798 现场构造 parser 完成第二遍。

另有一组"应用接口专用"入口：`ParseFunctionDefinition`（as_parser.cpp:109，供 RegisterGlobalFunction 解析 C++ 侧声明字符串，`isParsingAppInterface = true` 允许 `?` 类型）、`ParseDataType`（:155）、`ParsePropertyDeclaration`（:219）、`ParseTemplateDecl`（:190）——它们复用同一套 Parse* 函数，解析完都检查必须到 `ttEnd`。

## 5. 错误处理与恢复

两个标志（as_parser.h:184-185）：

- `isSyntaxError`：当前产生式已坏，各 Parse 函数在每次子调用后 `if( isSyntaxError ) return node;` 快速弹出（半成品节点仍挂在树上，保证能被 Destroy 统一回收）；
- `errorWhileParsing`：整趟解析是否出过错，决定公开入口最终返回 -1（:2494）。

`Error()`（as_parser.cpp:1067-1079）做三件事：`RewindTo(token)` 把读头退回错误处、置两个标志、`ConvertPosToRowCol` 后经 `builder->WriteError` 输出。错误消息用 `ExpectedToken/ExpectedOneOf`（:1145/:1163）+ `InsteadFound`（:1193）两条组合，形如 "Expected ';'" + "Instead found '}'"。

**恢复（同步）点**：源码里并没有叫 SkipStatement 的函数，恢复逻辑内联在两个循环解析器里，模式相同——

- `ParseScript(bool)`（:2665-2688）：出错后线性吃 token 直到 `;`、`{` 或 EOF；若停在 `{` 则数层级跳过整个块（连坏掉的类体一起跳），然后 `isSyntaxError = false;` 继续解析下一个顶层声明；
- `ParseStatementBlock`（:4005-4040）：同样跳到 `;`/`{`/`}`/EOF，`}` 时 RewindTo 留给外层循环正常收尾，EOF 时报 "unexpected end of file" 并附 `Info(TXT_WHILE_PARSING_STATEMENT_BLOCK, &start)` 指回块的起点。

效果：一次编译能报出多个顶层错误和函数内多个语句错误，但**一个语句内部只报第一处**（快速弹出）。

## 6. 关键代码走读

**(1) GetToken 的按需词法 + 注释过滤**（as_parser.cpp:1009-1046）
先查 `lastToken.pos == sourcePos` 的单枚缓存（RewindTo 存进去的），否则调 tokenizer；`do...while` 循环把 `ttWhiteSpace/ttOnelineComment/ttMultilineComment` 直接吞掉——所以 AST 里天然没有注释节点，builder 想要文档注释得自己回源码找。

**(2) ParseExprValue 的歧义处理**（as_parser.cpp:1624-1719）
表达式原子的分派是 parser 里最"脏"的地方：见到标识符要依次判 lambda（`IsLambda()` :1760，前瞻到 `)` 后是否 `{`）、模板类型、`type[]()` 构造调用、函数调用、普通变量访问。1678-1680 行的注释很诚实："Just 'type()' isn't considered a construct call... The compiler will have to sort this out, since the parser doesn't have enough information."——parser 只做语法可判定的部分，语义歧义整体下放。

**(3) ParseFunction 尾部的浅解析交接**（as_parser.cpp:3364-3377）
签名解析完后窥一眼：是 `;` 就是 external shared 声明，直接收；否则 `SuperficiallyParseStatementBlock()`。这一行就是两遍编译在 parser 侧的全部接口。

**(4) UpdateSourcePos 的区间合并**（as_scriptnode.cpp:107-129）
父区间取自身与新孩子区间的并（向左延伸调整 pos+length，向右只加 length）。配合 AddChildLast 的调用点，任何节点的 (pos,len) 恒覆盖其子树——错误定位、浅解析重启、CreateCopy 都建立在这条不变式上。

**(5) 顶层错误恢复循环**（as_parser.cpp:2601-2688）
注意双层结构：内层 `while( !isSyntaxError )` 正常分派，外层 `for(;;)` 兜住错误、跳同步点、清标志再进内层。2608-2615 行还有个小优化：先跳过 shared/external/final/abstract 修饰词再看真正的决定性 token，免得每个分支都要考虑修饰词前缀。

## 7. 如果你要自己实现

- **节点**：单一 struct + kind 枚举 + (pos,len) + firstChild/lastChild/next/prev/parent 五指针，配一个等大对象的自由链表池。不存文本、不存行号（按需换算），你会得到一个分配零碎片、销毁一趟递归的 AST。
- **解析器骨架**：每个产生式一个 `Node* ParseX()`；统一的 `GetToken/RewindTo(单 token 缓存)`；分派全部走"窥一个（偶尔两三个）token 再回退"。需要更长前瞻的判定（变量声明 vs 表达式）单独写 `IsXxx()` 布尔函数，扫完必须回退。
- **优先级**：如果 AST 的消费者就是你自己的编译器，AngelScript 的"扁平链 + 消费端调度场"完全够用且解析器最简单；如果 AST 要给外部工具用（格式化、静态分析），还是建正确嵌套的二元树（优先级爬升）更友好——这是本模块最值得权衡的取舍。
- **错误恢复**：`isSyntaxError` 快速弹出 + 在"列表型"产生式（顶层声明列表、语句列表）设同步点（`;` `{` `}` EOF），跳块时记得配平嵌套花括号。半成品节点留在树上由统一 Destroy 回收，比出错就地释放简单得多。
- **两遍支持**：浅解析只需要"配平括号找边界"，但要处理三个坑：字符串里的括号（靠 tokenizer 天然解决）、未闭合字符串（ttNonTerminatedStringConstant，:3939）、模板尖括号 `<` 与小于号的歧义（`IsTemplateTypeList`，:3856）。

## 8. 动手实验

1. **看树的形成**：在 `asCParser::ParseScript(asCScriptCode*)`（as_parser.cpp:2486）下断点，编译一个小脚本，返回前在调试器里展开 `scriptNode`，沿 firstChild/next 手画一遍树；对照 `ParseFunction` 确认函数体只是个没有孩子的 snStatementBlock。再在 as_compiler.cpp:798 下断点看第二遍如何从 `blockBegin->tokenPos` 重启。
2. **观察编译产物**：在 as_config.h 里定义 `AS_DEBUG` 重编库（MSVC Debug 配置默认已定义），运行任意样例（如 sdk/samples/asbuild），工作目录会出现 `AS_DEBUG/` 文件夹，每个脚本函数一个 txt（as_compiler.cpp:266-269 附近的 `byteCode.DebugOutput(...)`），内含最终字节码——可以对照扁平表达式链被调度场重排后的求值顺序。as_debug.h 本身主要是 `AS_PROFILE` 下的 TimeIt 剖析器（as_debug.h:254），parser 入口 `ParseStatementBlock` 就埋了点（as_parser.cpp:2810）。
3. **故意写错**：一个脚本里放两处错误（一处在顶层声明、一处在函数体内），确认两处都被报出；再把两处错误放进同一条语句，确认只报第一处——印证第 5 节的恢复粒度。
4. **验证扁平链**：解析 `a + b * c`，在调试器里看 snExpression 的五个孩子（term op term op term），确认 parser 没建任何优先级嵌套；然后在 `asCCompiler::ConvertToPostFix`（as_compiler.cpp:10734）下断点看它变成 `a b c * +`。

## 9. 扩展阅读

- Crafting Interpreters — Parsing Expressions（递归下降 + 优先级爬升的对照实现）：https://craftinginterpreters.com/parsing-expressions.html
- Wikipedia — Recursive descent parser / Shunting-yard algorithm（后者正是 ConvertToPostFix 的原型）
- AngelScript 官方手册 — Script language grammar（由源码里的 `// BNF:` 注释生成，与 as_parser.cpp 一一对应）：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_script_bnf.html
- 下一篇（05）：asCBuilder——两遍编译的第一遍如何消费这棵树、注册类型与函数签名。
