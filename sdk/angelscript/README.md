# AngelScript 引擎源码学习指南

这是一套**自底向上**精读 AngelScript 引擎实现的文档体系。目标不止是"会用"，而是吃透每个模块的构造，最终有能力**亲手实现一个自己的脚本引擎**。

- 引擎公开接口只有一个头文件：`include/angelscript.h`
- 全部实现在 `source/` 下约 80 个 `as_*` 文件中
- 模块精读文档在 [`references/`](references/) 目录，共 16 篇，编号即推荐阅读顺序

## 学习地图（从基础到高层）

```
                        ┌────────────────────────────────────┐
   第四层：运行时         │ 11 虚拟机   12 脚本对象   13 原生调用桥 │
                        │ 14 GC      15 字节码序列化            │
                        ├────────────────────────────────────┤
   第三层：编译           │ 07 Builder  08 编译器   09 字节码     │
                        │ 10 模块与函数对象                     │
                        ├────────────────────────────────────┤
   第二层：语言前端与类型  │ 03 词法器   04 语法器/AST  05 类型系统 │
                        │ 06 引擎中枢与注册 API                 │
                        ├────────────────────────────────────┤
   第一层：基础设施       │ 01 内存与容器   02 配置与可移植层      │
                        └────────────────────────────────────┘
                          16 终篇：从零实现你自己的脚本引擎
```

| # | 文档 | 主题 | 核心源文件 | 难度 |
|---|------|------|-----------|------|
| 01 | [内存与基础容器](references/01-memory-and-containers.md) | asNEW、asCArray/asCString/asCMap/asCSymbolTable、原子引用计数 | as_memory, as_array.h, as_string, as_map.h, as_atomic | ★★ |
| 02 | [配置与可移植层](references/02-config-and-portability.md) | as_config.h 宏体系、公开 API 的二进制兼容设计 | as_config.h, angelscript.h | ★★ |
| 03 | [词法分析器](references/03-tokenizer.md) | token 分类、关键字跳转表、字面量状态机 | as_tokenizer, as_tokendef.h | ★★ |
| 04 | [语法分析器与 AST](references/04-parser-and-ast.md) | 递归下降、first-child/next-sibling 节点、两遍解析支持 | as_parser, as_scriptnode | ★★★ |
| 05 | [类型系统](references/05-type-system.md) | asCDataType/asCObjectType/typeId 三层、asOBJ_* 标志 | as_datatype, as_typeinfo, as_objecttype | ★★★ |
| 06 | [引擎中枢与注册 API](references/06-engine-and-registration.md) | Register* 全家桶的实现、模板类型、字符串工厂 | as_scriptengine, as_configgroup | ★★★ |
| 07 | [编译调度器 Builder](references/07-builder.md) | 两遍编译、前向引用、增量编译 | as_builder, as_scriptcode | ★★★★ |
| 08 | [编译器核心](references/08-compiler.md) | 表达式 IR、重载决议、隐式转换、控制流、生命周期 | as_compiler, as_variablescope | ★★★★★ |
| 09 | [字节码与优化器](references/09-bytecode.md) | 指令集编码、窥孔优化、栈大小计算 | as_bytecode, angelscript.h(asEBCInstr) | ★★★★ |
| 10 | [模块与函数对象](references/10-module-and-function.md) | 编译产物容器、函数元数据、调试信息 | as_module, as_scriptfunction | ★★★ |
| 11 | [虚拟机](references/11-context-vm.md) | ExecuteNext 解释循环、栈帧、异常、挂起 | as_context | ★★★★★ |
| 12 | [脚本对象模型](references/12-script-objects.md) | 实例内存布局、工厂、继承/接口/虚表、weakref | as_scriptobject | ★★★★ |
| 13 | [原生调用桥](references/13-native-calling.md) | ABI 手工实现、x64 调用约定、generic 兜底 | as_callfunc*, as_generic | ★★★★★ |
| 14 | [垃圾回收器](references/14-gc.md) | 引用计数 + 增量循环检测（试删除法） | as_gc | ★★★★ |
| 15 | [字节码序列化](references/15-serialization.md) | 指针→索引重写、跨平台格式、加载校验 | as_restore | ★★★★ |
| 16 | [从零实现你自己的脚本引擎](references/16-build-your-own.md) | 综合路线图：里程碑拆解与最小实现顺序 | （全部） | ★★★★★ |

## 两条阅读动线

**动线 A：数据流（推荐首选）** —— 跟着一段脚本的一生走：

```
源码文本 → [03 词法] token 流 → [04 语法] AST → [07 调度 + 08 编译] → [09 字节码]
        → [10 模块] 存放 → [11 VM] 执行 → [13 原生桥] 调 C++ → [14 GC] 回收
```

先按 01→16 顺序通读一遍（每篇 20-40 分钟），再挑 08/11/13 三座大山二刷精读。

**动线 B：问题驱动** —— 从你在 samples 里见过的现象向下钻：

| samples 里的现象 | 对应文档 |
|-----------------|---------|
| `Print("a" + x)` 为什么能选对重载 | 08 编译器（重载决议） |
| Suspend/yield 协程为什么廉价 | 11 虚拟机（栈是堆内存块） |
| `asCALL_CDECL` 传裸函数指针就能被脚本调 | 13 原生调用桥 |
| 循环引用为什么不泄漏 | 14 GC |
| asbuild 离线编译的 .abc 文件里是什么 | 15 序列化 + 09 字节码 |
| 注册接口时那串声明字符串谁在解析 | 06 注册 API（复用 04 的 parser） |

## 学习方法建议

1. **文档 + 源码 + 调试器三开**：每篇文档的 file:line 引用在 VS 2026 里直接对照；"动手实验"一节永远比阅读有效。推荐的第一个断点：`asCContext::ExecuteNext`（as_context.cpp）跑 tutorial，单步十分钟胜读十页。
2. **用 test_feature 当靶场**：每个特性都有对应测试（tests/test_feature/source/test_*.cpp），想看引擎如何处理某语法，找到对应测试文件跑单个测试再下断点（方法见根目录 CLAUDE.md）。
3. **改一行验证理解**：比如给窥孔优化器注释掉一个模式，跑 test_feature 看什么坏了——回归套件就是你的安全网。
4. **终极检验**：读完后做第 16 篇的路线图练习——用任意语言实现一个 mini 脚本引擎（哪怕只有 int 类型 + 函数调用 + generic 绑定），你会在每一步遇到 AngelScript 已经优雅解决过的问题。

## 全局速查

- **官方手册**（使用视角）：https://www.angelcode.com/angelscript/sdk/docs/manual/index.html
- **官方源码仓库**：https://github.com/anjo76/angelscript
- **语言/引擎设计通识**（强烈推荐配套阅读）：Crafting Interpreters — https://craftinginterpreters.com/ （免费在线书，其 clox 部分与 AngelScript 的"手写编译器 + 字节码栈机"路线高度同构）
- 引擎错误消息全集：`source/as_texts.h`（grep 报错文本定位引擎行为的捷径）
- 版本与变更：`include/angelscript.h` 的 `ANGELSCRIPT_VERSION_STRING`、`../docs/articles/changes*.html`
