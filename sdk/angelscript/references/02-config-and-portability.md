# 02 — 配置与可移植层（as_config.h 与公开 API 设计）

> 一句话概括：本篇精读 AngelScript 的"编译期适配中枢" `as_config.h` 与唯一公开头文件 `angelscript.h`，理解一个跨 30+ 平台的引擎如何用宏体系描述 ABI 差异、用纯虚接口 + 版本号保证二进制兼容。
>
> 难度：★★☆☆☆（以阅读预处理宏为主，但涉及的 ABI 概念会在 13/14 篇原生调用约定中反复用到）
> 前置：01（源码地图与构建方式）

---

## 1. 模块定位

AngelScript 支持的目标平台横跨 Windows/Linux/macOS/BSD、iOS/Android、以及 PS2/PS3/PSP/PSVita、XBox360、Wii/WiiU/Switch、Dreamcast 等游戏主机，CPU 覆盖 x86/x64、ARM/ARM64、MIPS、PPC、SH4、SPARC、RISC-V64、E2K。这些平台的差异不只是"有没有某个系统 API"，而是深入到 C++ ABI 层面：

- 成员函数指针有多大？虚函数、多继承、虚继承各存什么？
- 一个带析构函数的结构体按值传参时，编译器实际怎么传？
- 返回一个小结构体，是走寄存器还是隐藏指针？谁负责弹栈？

因为 AngelScript 的招牌能力是**原生调用约定**（直接调用应用注册的 C++ 函数，不需要包装函数），它必须在编译期就"知道"这些答案。`as_config.h` 就是把所有答案集中在一个头文件里的地方：内部所有 `.cpp` 都先包含它，它在末尾再包含公开头 `angelscript.h`（source/as_config.h:1454），因此配置宏对整个库全局生效。

另一端，`include/angelscript.h` 是**唯一**需要暴露给应用程序的头文件。它的二进制兼容策略是经典的 COM 式三板斧：

1. **纯虚接口**：应用只通过 `asIScriptEngine` 等抽象类的虚表调用库，不依赖任何内部类的大小与布局；
2. **版本号握手**：`asCreateScriptEngine(asDWORD version = ANGELSCRIPT_VERSION)` 在运行时校验头文件版本与库二进制版本是否兼容（include/angelscript.h:568）；
3. **只加不改**：同一个次版本号内接口只在尾部追加，虚表槽位次序不变，因此应用可以直接换 DLL 而不用重新编译（补丁号只需满足"应用 ≤ 库"）。

## 2. as_config.h 的宏体系：五个层次

文件前 370 行是纯文档：按"特性开关 → 库链接方式 → 编译器差异 → 编译器识别方法 → CPU 差异 → 目标系统 → 调用约定能力"分节解释每个宏的语义（source/as_config.h:44-369）。真正的探测逻辑从第 373 行 `// Detect compiler` 开始，结构是**先按编译器分大块，块内再按 OS 与 CPU 细分**：

```
默认值（377-384） → __EMSCRIPTEN__ / __BORLANDC__ / _MSC_VER / __MWERKS__ /
__SNC__ / __GNUC__（内含 Apple、Windows-MinGW、Linux、BSD、Android、Haiku…）/
__SUNPRO_CC → 硬件兜底检测（1370+） → 一致性收尾（1393-1412）
```

五层宏各举真实例子：

**(1) OS 层**：`AS_WIN`、`AS_LINUX`、`AS_MAC`、`AS_ANDROID` 等。例如 MSVC 分支中 `#if defined(WIN32) || defined(_WIN32) || defined(_WIN64)` 则定义 `AS_WIN`（source/as_config.h:504-506）；GNUC 分支里 Linux 的判断特意排除 Android：`defined(__linux__) && !defined(ANDROID) && !defined(__ANDROID__)`（source/as_config.h:943）。

**(2) 编译器层**：`asVSNPRINTF` 是典型例子——MSVC9 之前映射到 `_vsnprintf`，之后映射到带 `_TRUNCATE` 的安全版 `vsnprintf_s`（source/as_config.h:488-492），GNUC 则直接用 `vsnprintf`（source/as_config.h:660）。

**(3) CPU 架构层**：注意 `AS_X64_MSVC` 与 `AS_X64_GCC` 是**编译器 × 架构**的组合宏，而不是单纯的架构宏——同样的 x86-64 芯片，Windows x64 ABI 和 System V ABI 的传参规则完全不同，所以 MSVC 的 `_M_X64` 定义 `AS_X64_MSVC`（source/as_config.h:520-527），而 Linux 的 `__x86_64__` 定义 `AS_X64_GCC`（source/as_config.h:960-970），二者会分别选中 `as_callfunc_x64_msvc.cpp/asm` 与 `as_callfunc_x64_gcc.cpp` 这两套完全不同的原生调用实现。MinGW 又是第三种：`AS_X64_MINGW`（source/as_config.h:930-935）。

**(4) 调用约定能力层**：描述 ABI 细节的一组宏。例如 MSVC x64 定义 `AS_LARGE_OBJS_PASSED_BY_REF` + `AS_LARGE_OBJ_MIN_SIZE 3`（超过 2 个 DWORD 的对象一律按引用传，source/as_config.h:524-525），而 Linux x64 的阈值是 5 且还定义 `SPLIT_OBJS_BY_MEMBER_TYPES`（按成员类型拆到整数/浮点寄存器，source/as_config.h:962-967）——这正是两套 x64 ABI 的核心差异。`COMPLEX_MASK` / `COMPLEX_RETURN_MASK` 则声明"哪些 `asOBJ_APP_*` 特征会让对象变成必须隐式按引用传/在内存中返回"，每个平台各有一份定义（如 MSVC x64 在 source/as_config.h:526-527）。

**(5) 特性开关层**：
- `AS_MAX_PORTABILITY`：关闭一切平台相关代码，只留 `asCALL_GENERIC` 通用调用约定（source/as_config.h:78-80）。它既可以被显式定义（Emscripten 直接强制，source/as_config.h:389-391），也是最后的自动兜底：如果扫完全部架构宏一个都没定义，就自动进入最大可移植模式（source/as_config.h:1393-1399）。这保证了库在**任何**未知平台上都能先编译通过、跑起来。
- `AS_NO_THISCALL_FUNCTOR_METHOD`：默认在文件开头就定义为"不支持"（source/as_config.h:384），随后每个**验证过**的平台用 `#undef` 白名单式打开（如 MSVC x64 在 source/as_config.h:522）。"默认关闭、测过才开"是整个文件的安全哲学。
- `AS_USE_COMPUTED_GOTOS`：VM 主循环是否用 computed goto 分发字节码。GCC ≥ 6 与 clang（含 clang-cl，见最近提交 3fc00a6a）默认开启（source/as_config.h:456-462、671-679），MSVC 原生编译器不支持该扩展。
- 线程能力的级联收尾：没有原子指令就禁多线程（`AS_NO_ATOMIC` ⇒ `AS_NO_THREADS`，source/as_config.h:1403-1405）；没选定线程实现也禁（source/as_config.h:1410-1412）。

文件最后定义 `asASSERT`（Android 上用 `__android_log_print`+`exit(1)` 模拟，其他平台就是 `assert`，source/as_config.h:1415-1433）以及 VM 取参的内部宏 `ARG_DW`/`BCARG_PTR` 等（source/as_config.h:1441-1448）。

## 3. angelscript.h 的结构

按行号浏览一遍公开头的组织（共约 2100 行）：

- **命名空间开关**：`AS_USE_NAMESPACE` 决定是否包进 `namespace AngelScript`，通过 `BEGIN_AS_NAMESPACE` 等三个宏实现（include/angelscript.h:47-55）。
- **版本号**：`ANGELSCRIPT_VERSION 23900` 与 `"2.39.0 WIP"`（include/angelscript.h:61-62）。编码规则是 `major*10000 + minor*100 + patch`。
- **基础类型**：`asBYTE/asWORD/asDWORD/asQWORD/asPWORD` 等（include/angelscript.h:86-113）。细节值得玩味：LP64 平台上 `asDWORD` 是 `unsigned int`，非 LP64 上却是 `unsigned long`（32 位 Windows 上 long 也是 32 位）；`asQWORD` 在老 MSVC 上要用 `unsigned __int64`；`asPWORD` 用 `uintptr_t`，仅 MSVC6/Marmalade/clang-cl 例外用 `size_t`。这些类型的实际宽度会在引擎创建时被断言验证（见下节）。
- **枚举分区**：`asERetCodes`（include/angelscript.h:118）、`asEEngineProp`（152）、`asECallConvTypes`（199）、`asEObjTypeFlags`（213）、`asEBehaviours`（270）、`asEContextState`（300）、`asETypeIdFlags`（342）等应用常用枚举集中在前 400 行；而字节码指令枚举 `asEBCInstr`（1463）和 `asEBCType`（1678）放在文件尾部——它们只有 JIT 编译器作者才需要。
- **注册宏族**：`asOFFSET`（440）、`asFUNCTION`（442）、`asFUNCTIONPR`（448/450）、`asMETHOD`/`asMETHODPR`（504-505），下节细讲。
- **导出声明**：`AS_API` 按静态库/DLL 导出/DLL 导入分别展开为空、`__declspec(dllexport/dllimport)` 或 GCC 的 visibility 属性（include/angelscript.h:546-561），随后是 `extern "C"` 的全局函数区：`asCreateScriptEngine`、`asGetLibraryVersion`、`asGetLibraryOptions`、线程与内存管理函数（568-594）。用 `extern "C"` 是为了 DLL 手动 `GetProcAddress` 时有稳定的符号名。
- **接口区**：`asIScriptEngine`（668）、`asIScriptModule`（831）、`asIScriptContext`（902）、`asIScriptGeneric`（1000）、`asIScriptObject`（1041）、`asITypeInfo`（1071）、`asIScriptFunction`（1148）、`asIBinaryStream`（1228）、`asILockableSharedBool`（1238）、JIT 接口（1436-1453）。全部是纯虚类，析构受保护，生命周期由 `AddRef/Release` 引用计数管理。

## 4. 关键代码走读

### 4.1 asCreateScriptEngine 的版本握手（source/as_scriptengine.cpp:233-273）

```cpp
AS_API asIScriptEngine *asCreateScriptEngine(asDWORD version)
{
    if( (version/10000)     != (ANGELSCRIPT_VERSION/10000) )     return 0; // 主版本必须相等
    if( (version/100)%100   != (ANGELSCRIPT_VERSION/100)%100 )   return 0; // 次版本必须相等
    if( (version%100)       >  (ANGELSCRIPT_VERSION%100) )       return 0; // 应用补丁号 ≤ 库补丁号
    ...
    return asNEW(asCScriptEngine)();
}
```

三条规则精确表达了兼容承诺：接口只在**补丁版本**内保持向后兼容（库比应用新可以，反之不行），主/次版本一变即拒绝。`version` 参数默认值就是应用编译时头文件里的 `ANGELSCRIPT_VERSION`（include/angelscript.h:568），所以应用什么都不用写，握手自动完成；失败时**返回空指针而不是抛异常**。随后的一串 `asASSERT` 验证 `as_config.h`/`angelscript.h` 里的类型宽度假设在本平台真的成立——`sizeof(asDWORD)==4`、`sizeof(asPWORD)==sizeof(void*)`、`sizeof(bool)==AS_SIZEOF_BOOL`，以及用 `memcmp` 实测大小端与 `AS_BIG_ENDIAN` 一致（source/as_scriptengine.cpp:246-268）。配置错了，Debug 版第一时间就在这里崩给你看。

### 4.2 asFUNCTION/asMETHOD：把任意函数指针装进 asSFuncPtr

C++ 标准不允许把成员函数指针转成 `void*`（它可能比数据指针大得多），所以 AngelScript 定义了一个"能装下任何已知函数指针"的搬运结构 `asSFuncPtr`（include/angelscript.h:458-483）：核心是一个 25 字节的 union（"已知最大的成员函数指针是 MSVC 64 位的 20 字节，8 字节对齐后 24 字节"）加一个 `flag` 字节（1=generic、2=全局函数、3=成员方法）。

- `asFUNCTION(f)` 展开为模板函数 `asFunctionPtr(f)`（include/angelscript.h:442），它构造 `flag=2` 的 asSFuncPtr 并把函数指针 reinterpret 成统一的 `asFUNCTION_t` 存入（include/angelscript.h:1262-1279）；针对 `asGENFUNC_t` 有一个模板特化，自动打上 `flag=1` 的 generic 标记（1282-1289）——这就是为什么注册 generic 函数不需要显式区分的原因。
- `asFUNCTIONPR(f,p,r)` 先 `static_cast` 到带完整签名的指针类型再转存（include/angelscript.h:450），用于给重载函数消歧。
- `asMETHOD(c,m)` 是最精巧的一个（include/angelscript.h:504）：`asSMethodPtr<sizeof(void (c::*)())>::Convert(...)` 用**成员函数指针本身的 sizeof 作为模板参数**分发。主模板体内声明负长度数组制造编译错误（include/angelscript.h:1301-1315），只有针对具体尺寸的特化（单指针大小、+1 int、+2 int……对应无继承/多继承/虚继承的 MSVC 指针布局，include/angelscript.h:1318-1358）才会逐字节 `CopyMethodPtr` 拷进 union。后续 `as_callfunc.cpp` 再配合 `MULTI_BASE_OFFSET`/`VIRTUAL_BASE_OFFSET` 等宏（source/as_config.h:445-451）从这 25 个字节里解码出真实入口与 this 调整量——这是第 13 篇的主题。

### 4.3 as_debug.h：调试输出与内建性能剖析

`as_debug.h` 只做两件事。其一，当定义了 `AS_DEBUG` 时为各平台准备 `_mkdir`（GNUC 映射到 `mkdir(dir, S_IRWXU)`，source/as_debug.h:41-79），因为编译器/字节码模块会把每个编译出的函数的字节码 dump 到 `AS_DEBUG/` 目录下的文本文件里；Wii/WinCE/PSVita 等没有对应 API 的平台被逐层 `#ifndef` 排除。其二，MSVC + `AS_PROFILE` 下提供一个 RAII 剖析器：`TimeIt(x)` 宏创建 `CProfilerScope`，用 `QueryPerformanceCounter` 计时、以 `"scope|subscope"` 字符串为键累计进 `asCMap`，进程退出时写出 `AS_DEBUG/profiling_summary.txt`（source/as_debug.h:85-254）；未启用时 `TimeIt(x)` 展开为空（source/as_debug.h:261）。

## 5. 设计决策与权衡

- **C++03 基线 + 可选 C++11**：库主体不要求 C++11。`AS_CAN_USE_CPP11` 由一串编译器版本判断得出（include/angelscript.h:428-436），仅用来启用锦上添花的能力（如更好的类型萃取）；`asCreateScriptEngine` 里连 64 位字面量都用移位拼（source/as_scriptengine.cpp:262-265）。代价是大量本可用 `static_assert`/`type_traits` 表达的东西要靠"负数组长度"这类技巧。
- **不依赖异常**：公开 API 全部以负数错误码（`asERetCodes`）或空指针报错；`AS_NO_EXCEPTIONS` 让库能在关异常的工具链（PS3、PSVita、部分 ARM）上编译（source/as_config.h:98-99、633、979）。
- **白名单式能力开启**：所有危险能力（thiscall functor、原生调用约定、computed goto）默认关闭，被实测验证过的平台逐个 `#undef` 打开；未知平台自动坠入 `AS_MAX_PORTABILITY` 安全网。牺牲的是新平台的开箱性能，换来的是"永远能编译、永远行为正确"。
- **接口稳定优先于内聚**：把 JIT 才用的 `asEBCInstr` 也放进唯一公开头里，头文件很大，但应用永远只需 `#include <angelscript.h>`，且 `asGetLibraryOptions()` 可在运行时报告库编译时的全部特性开关（source/as_scriptengine.cpp:80+），方便应用校验自己与库的配置一致。

## 6. 如果你要自己实现一个脚本引擎

1. **单一公开头 + 纯虚接口 + C 工厂函数**：应用侧只见抽象类和 `extern "C"` 的创建函数，内部实现随便改布局；这是不用 COM 也能拿到的最便宜的 ABI 稳定方案（即所谓 hourglass 模式的简化版）。
2. **版本号做成整数并在工厂函数里握手**，规则要能区分"重新编译才能升级"（major/minor）与"直接换 DLL 即可"（patch）。
3. **把平台假设写成宏 + 在运行时断言这些假设**：`as_config.h` 负责"声称"，`asCreateScriptEngine` 里的 `asASSERT(sizeof(...))` 负责"验证"，两者缺一不可——移植到新平台时后者是最快的报警器。
4. **给自己留一个 AS_MAX_PORTABILITY 式的兜底路径**（纯 C++ 的 generic 调用），让原生 ABI 适配成为优化项而非阻塞项。
5. **固定宽度类型自己定义**（asDWORD 等），不要让公开接口的布局依赖 `int/long` 这类平台浮动类型。

## 7. 动手实验

1. **追踪一个能力宏的影响面**：在 sdk/angelscript 下运行 `rg -l AS_X64_MSVC source/`，再对比 `rg -l AS_X64_GCC source/`，观察两套 x64 ABI 分别激活哪些 `as_callfunc_*` 文件；同法看 `rg -n "AS_NO_THISCALL_FUNCTOR_METHOD" source/` 能找到它在 as_callfunc.cpp 与 as_scriptengine.cpp 里拦截了哪些注册路径。
2. **故意传错版本号**：`asCreateScriptEngine(20000)`（2.0.0）应返回空指针；`asCreateScriptEngine(ANGELSCRIPT_VERSION + 1)`（补丁号高于库）也返回空；`asCreateScriptEngine(ANGELSCRIPT_VERSION - 1)`（若补丁号不为 0）则成功——对照 source/as_scriptengine.cpp:236-243 验证三条规则。
3. **强制最大可移植模式**：给编译加 `-DAS_MAX_PORTABILITY` 重新构建，然后调用 `asGetLibraryOptions()` 打印结果，并尝试用 `asCALL_CDECL` 注册一个函数，观察返回 `asNOT_SUPPORTED`。
4. **打开 AS_DEBUG**：定义 `AS_DEBUG` 构建后编译任意脚本，检查工作目录下生成的 `AS_DEBUG/` 目录里的字节码 dump 文件（这是第 09 篇编译器与第 10 篇字节码的重要调试手段）。

## 8. 扩展阅读

- AngelScript 官方手册（配置库、版本约定、注册接口）：https://www.angelcode.com/angelscript/sdk/docs/manual/index.html
- 手册中 "Custom options / compile-time configuration" 与 "Calling convention" 章节（从上面首页导航进入，讲解本篇宏的官方语义）
- Wikipedia — Application binary interface：https://en.wikipedia.org/wiki/Application_binary_interface
- Wikipedia — x86 calling conventions（对照 AS_X64_MSVC 与 AS_X64_GCC 的差异）：https://en.wikipedia.org/wiki/X86_calling_conventions
- Wikipedia — Opaque pointer / Component Object Model（纯虚接口做 ABI 边界的思想来源）：https://en.wikipedia.org/wiki/Component_Object_Model
- 编译器预定义宏速查（as_config.h:164 引用的同类资料）：https://sourceforge.net/p/predef/wiki/Home/

---

下一篇（03）：内存与基础容器 —— asCArray/asCString/asCMap 与自定义内存分配（asNEW/asDELETE）。
