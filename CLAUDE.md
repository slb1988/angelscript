# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

这是 AngelScript 脚本引擎的官方仓库（当前版本见 `sdk/angelscript/include/angelscript.h` 中的 `ANGELSCRIPT_VERSION_STRING`，目前为 2.39.0 WIP）。AngelScript 是一个跨平台的嵌入式脚本库，用 C++ 编写，供宿主应用通过注册类型/函数的方式扩展脚本能力。

## 目录结构

所有内容都在 `sdk/` 下：

- `sdk/angelscript/` — **引擎核心库**（学习重点）
  - `include/angelscript.h` — 唯一的公开头文件，定义全部应用接口（`asIScriptEngine`、`asIScriptModule`、`asIScriptContext`、`asIScriptFunction` 等）
  - `source/` — 引擎实现，所有文件以 `as_` 前缀命名，接口的实现类以 `asC` 前缀命名（如 `asCScriptEngine`）
  - **`README.md` + `references/`（本地添加）— 引擎源码精读文档体系**：16 篇中文文档按"基础设施→前端→类型→编译→运行时"自底向上排序，全部 file:line 引用经核对，终篇是"从零实现自己的脚本引擎"路线图。深入引擎实现前先读它
  - `projects/` — 各平台/工具链的构建工程（cmake、msvc6~msvc2022、gnuc、xcode、android、meson 等）
- `sdk/add_on/` — 官方可选扩展，宿主按需注册，不属于核心库。如 `scriptarray`（脚本里的 `array<T>`）、`scriptstdstring`（`string` 类型，基于 std::string）、`scriptdictionary`、`scriptbuilder`（脚本预处理器，支持 `#include`、元数据）、`scripthandle`（`ref` 类型）、`weakref`、`debugger`（行级调试器）、`contextmgr`（协程/并发上下文管理）、`serializer`、`scripthelper`、`autowrapper`（模板自动生成 generic 包装）等
- `sdk/tests/` — 测试
  - `test_feature/` — **主回归测试套件**（约 170+ 个测试文件），改引擎后必须跑
  - `test_performance/`、`test_build_performance/`、`test_multithread/` — 性能与多线程测试
- `sdk/samples/` — 示例程序：`tutorial`（入门）、`console`、`game`（完整游戏脚本集成范例）、`events`、`coroutine`、`concurrent`、`asbuild`（离线编译脚本为字节码）、`asrun`（命令行脚本运行器，带调试器）。**`samples/README.md` 是按难度排序的学习路线图，每个示例目录里有本地添加的中文 README.md**，拆解知识点并映射到引擎源码
- `sdk/docs/` — `doxygen/` 是官方手册源码（Doxyfile + source），`articles/` 是变更记录页面

## 构建与测试

本机开发环境是 **Visual Studio 2026 Community**（`D:\Program Files\Microsoft Visual Studio\18\Community`）。核心库、全部 8 个示例、全部 4 个测试套件都已生成 **`projects/msvc2026/`** 工程（本地新增，非官方自带）：由 msvc2022 工程复制而来，工具集改为 v145，.sln 补全了 Debug/Release × Win32/x64 四个配置，且 **x64 配置的 OutDir 统一修正为 `..\..\bin\`**（官方部分工程 x64 输出到 `projects/.../x64/<Config>/`，导致 exe 找不到 bin 下的 .as 脚本；asrun/concurrent/console 官方本来就输出到 bin，其余是遗漏）。全部构建已验证通过（2026-07-12）。注意 `test_multithread` 官方工程停留在 v110 时代，msvc2026 版是重写的，其源码 `test_gc.cpp` 用的废弃接口 `asIObjectType` 已改为 `asITypeInfo`；其 `main.cpp` 已打补丁：stdin 非交互（`_isatty` 为假）时跳过结尾 `_getch()` 等待，且失败路径返回 -1（官方版成功失败都返回 0）。msvc2026 的 test_feature 工程还去掉了 `WAIT_ON_END` 宏（官方工程定义了它，测试结尾会 `_getch()` 等按键，无法脚本化运行；测试程序输出重定向到文件时是块缓冲，"卡住不动"多半是已跑完在等按键）。官方自带的 msvc2022 工程（v143）在 VS 2026 下也能直接编译，因为 VS 2026 同时装有 v143/14.44 和 v145/14.51 工具集。CMake 未在 PATH 上，但 VS 自带一份（`<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`）。

官方 msvc2022 工程也已同步修正（2026-07-12）：8 个示例的 .sln 补登记了 x64 配置、5 个示例与 test_feature/test_performance 的 x64 OutDir 改为 `..\..\bin\`、test_multithread 的 v110 老工程重写为 v143 四配置版。**msvc2022（v143）与 msvc2026（v145）两条链共享 `sdk/angelscript/lib/` 的库文件**：Debug 可混用，Release 因 LTCG（/GL）要求库与应用同工具集——切换链后先对核心库 `/t:Rebuild`。

遇到 **LNK1181**（找不到 angelscript*.lib）= 还没编对应配置的核心库；遇到 **C1047/LNK1257**（编译器版本不一致，LTCG）= 磁盘上的 .lib 是另一条工具链编的，对核心库 `/t:Rebuild` 干净重建即可。

用 MSBuild 构建（注意顺序：samples 链接的是 `sdk/angelscript/lib/` 下的预编译库，**必须先编核心库**，且示例在 IDE 里选哪个配置就需要对应配置的库）：

```
# 1. 核心库 → 产出 sdk/angelscript/lib/ 下的 .lib
& "D:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" `
  sdk\angelscript\projects\msvc2026\angelscript.sln /p:Configuration=Debug /p:Platform=x64 /m

# 2. 示例
& "...\MSBuild.exe" sdk\samples\tutorial\projects\msvc2026\tutorial.sln /p:Configuration=Debug /p:Platform=x64 /m
```

库的命名约定：`angelscript.lib`（Win32 Release）/ `angelscriptd.lib`（Win32 Debug）/ `angelscript64.lib` / `angelscript64d.lib`（x64）。msvc2026 工程里示例/测试的所有配置都输出到各自的 `bin/`（Win32 和 x64 同名互相覆盖，用哪个编哪个）；**运行时必须以 `bin/` 为工作目录**（脚本 .as 文件按相对路径加载），且示例结尾会 `getch()` 等待按键。

跨平台 CMake 方式：

构建核心库（静态库）：

```
cmake -S sdk/angelscript/projects/cmake -B build
cmake --build build --config Debug
```

构建并运行回归测试（该 CMake 工程通过 add_subdirectory 连带构建核心库和所有 add_on，是一条命令验证全部的最快方式）：

```
cmake -S sdk/tests/test_feature/projects/cmake -B build_test
cmake --build build_test --config Debug
```

可执行文件输出到 `sdk/tests/test_feature/bin/`，**必须在该目录下运行**（测试用相对路径读取 `bin/scripts/` 里的脚本文件）：

```
cd sdk/tests/test_feature/bin && ./test_feature
```

### 测试机制

- 没有测试框架。`test_feature/source/main.cpp` 的 `allTests()` 按顺序逐个调用测试函数，失败即停（fail-fast）并返回 -1；结尾通过自定义内存管理器（`InstallMemoryManager`）检查内存泄漏。
- 每个测试是 `source/test_xxx.cpp` 里的一个 `namespace TestXxx { bool Test(); }`，返回 `false` 表示通过。断言用 `utils.h` 里的 `TEST_FAILED` 宏（打印文件/行号并置 fail）。
- **运行单个测试**：没有命令行参数支持，做法是临时把该测试调用挪到（或已有惯例：新写的测试直接加在）`main.cpp` 中 `allTests()` 列表的最前面，跑到它通过即可；或临时注释掉其它调用。提交前恢复完整列表。
- 新增测试的流程：新建 `source/test_xxx.cpp` → 在 `main.cpp` 声明 namespace 并加入调用列表顶部 → 加进各构建工程（至少 `projects/cmake/CMakeLists.txt` 和 msvc 工程）。

## 核心架构（编译与执行流水线）

理解引擎的主线是这条数据流：

```
脚本源码 → asCBuilder（编译调度） → asCTokenizer/asCParser（词法/语法 → asCScriptNode AST）
        → asCCompiler（AST → 字节码） → asCByteCode（窥孔优化）
        → asCModule（存放编译产物） → asCContext::ExecuteNext()（VM 解释执行）
```

关键源文件（都在 `sdk/angelscript/source/`）：

- `as_scriptengine.cpp` — `asCScriptEngine`，全局中枢：应用类型/函数注册、模块管理、engine property 配置。文件很大，是所有对象的持有者。
- `as_builder.cpp` — 模块编译的调度者。两遍式：先解析所有脚本收集类/函数/全局变量声明（允许前向引用），再逐个编译函数体。
- `as_tokenizer.cpp` / `as_tokendef.h` — 词法分析；`as_parser.cpp` — 递归下降语法分析，产出 `asCScriptNode`（`as_scriptnode.cpp`）组成的 AST。
- `as_compiler.cpp` — 编译器核心（最大的文件之一）：类型检查、隐式转换、重载决议、生成字节码。
- `as_bytecode.cpp` — 字节码的构建与优化（窥孔优化、死代码消除）；指令定义在 `include/angelscript.h` 的 `asEBCInstr` 和 `source/as_tokendef.h` 旁的相关表里。
- `as_context.cpp` — **虚拟机**。`asCContext::ExecuteNext()` 是解释器主循环（大 switch，GCC/Clang 下可用 computed goto，见 `AS_USE_COMPUTED_GOTOS`），基于栈的执行模型；也负责异常、挂起/恢复（协程支持）、调试行回调。
- `as_callfunc*.cpp/.S/.asm` — **原生调用桥**，按 CPU 架构/ABI 一个文件（x86、x64 msvc/gcc、arm、arm64、mips、ppc、riscv64 等），负责把 VM 栈上的参数按原生调用约定传给 C++ 函数。这是移植新平台的主要工作量。不支持原生约定的平台用 `as_generic.cpp`（generic calling convention，`AS_MAX_PORTABILITY` 下的可移植兜底）。
- `as_gc.cpp` — 增量垃圾回收器，只处理循环引用；常规生命周期靠引用计数（`as_atomic.cpp` 提供原子操作）。
- `as_restore.cpp` — 字节码序列化（SaveByteCode/LoadByteCode），做的是平台无关化处理。**改动字节码指令或编译产物结构时必须同步改这里**，并注意与已保存字节码的兼容性。
- `as_module.cpp` — `asCModule`，一组脚本的编译单元；`as_scriptfunction.cpp` — 函数对象（含脚本函数的字节码）；`as_typeinfo.cpp` / `as_objecttype.cpp` / `as_datatype.cpp` — 类型系统；`as_scriptobject.cpp` — 脚本类实例的内存布局与构造/析构。
- `as_config.h` — 所有平台/编译器探测宏与移植开关，移植问题先看这里。
- `as_texts.h` — 编译器/运行时所有消息文本集中于此。

### 代码约定

- 核心库不用 STL、不抛异常，自带容器：`asCArray`、`asCMap`、`asCString`、`asCSymbolTable`（对应 `as_array.h` 等头文件），内存分配走 `asNEW`/`asDELETE` 宏（`as_memory.cpp`，支持用户自定义分配器）。
- C++11 为基线，需要兼容大量老编译器/平台（工程目录里有 msvc6、dreamcast 等），改核心代码避免新语言特性。
- 公开 API 只能加不能破坏二进制兼容（接口用纯虚类 + 版本号 `ANGELSCRIPT_VERSION` 校验）。
