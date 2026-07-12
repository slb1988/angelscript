# asrun — 命令行脚本运行器（调试器、context 池、#pragma）

**难度：★★★★☆　前置：coroutine、asbuild**

一个功能齐全的 "AngelScript 解释器"：`asrun script.as` 直接运行脚本，支持命令行参数、协程、文件/socket/日期时间等标准库、**交互式行级调试器**（`-d` 或脚本里 `#pragma debug`）。它是把 AngelScript 当通用脚本语言用的完整参考实现，也是日常测试脚本片段的趁手工具。

```
用法: asrun [-d] <script file> [<args>]
调试命令: b 设断点 / s 单步 / n 下一行 / c 继续 / p 打印变量 / h 帮助
```

## 知识点拆解（对照 source/main.cpp）

### 1. Context 池 + SetContextCallbacks（main.cpp:226, 668-708）

```cpp
engine->SetContextCallbacks(RequestContextCallback, ReturnContextCallback, 0);
```

这是本示例最重要的引擎级技巧。**引擎内部**有时需要 context（初始化全局变量、调用脚本类的析构、值类型 opAssign 等），默认它临时创建/销毁；设置回调后引擎改为向宿主"借"：

- `RequestContextCallback`：从 `g_ctxPool` 取（或新建），**顺手给每个借出的 context 挂上调试器的行回调**——这就是连全局变量初始化都能调试的原因（main.cpp:400-403 的注释点明了这层因果）；
- `ReturnContextCallback`：**先 `ctx->Unprepare()` 再入池**——Unprepare 释放 context 持有的返回值等对象引用，且必须在入池前做（清理可能触发析构进而触发新的脚本调用）。

配套细节：`asEP_INIT_GLOBAL_VARS_AFTER_BUILD = false`（main.cpp:353）先关掉 Build 后自动初始化全局变量，等调试器就位后手动 `mod->ResetGlobalVars(0)`（main.cpp:403）。

### 2. 调试器 add_on（add_on/debugger/，main.cpp:326-344）

`CDebugger` 完全构建在公开 API 上，没有任何引擎私有钩子：

- **驱动**：`ctx->SetLineCallback(asMETHOD(CDebugger, LineCallback), g_dbg, asCALL_THISCALL)`——每行回调里检查断点/单步状态，命中则进入交互循环读命令；
- **看变量**：行回调里通过 `ctx->GetVarCount/GetAddressOfVar/GetVarTypeId`、调用栈通过 `ctx->GetCallstackSize/GetFunction(n)/GetLineNumber(n)`（context 的完整自省 API）；
- **打印任意类型**：`RegisterToStringCallback(typeInfo, callback)` 给 string/array/dictionary/datetime 各注册一个 to-string 函数（main.cpp:236-323），`expandMembers` 参数控制递归展开层数。

结论：**行回调 = AngelScript 调试基础设施的全部**。断点、单步、超时、性能采样都是它的应用。

### 3. #pragma 回调 + 引擎词法器复用（main.cpp:764-795）

```cpp
builder.SetPragmaCallback(PragmaCallback, 0);
```

脚本首行 `#pragma debug` 即可自开调试。亮点在解析实现：用 `engine->ParseToken()` 逐 token 扫描 pragma 文本——**宿主直接复用引擎的词法分析器**（底层就是 as_tokenizer.cpp），不用自己写字符串解析。

### 4. 杂项但重要的模式

- **手工构造脚本数组**（GetCommandLineArgs，main.cpp:639-666）：`engine->GetTypeInfoById(GetTypeIdByDecl("array<string>"))` → `CScriptArray::Create(type)` → 填元素 → 以句柄返回（注意 AddRef 约定）；
- **`asGetActiveContext()` 抛脚本异常**（main.cpp:608-611, 622-625）：宿主函数出错时 `ctx->SetException("...")`，脚本侧表现为可捕获/可报告的脚本异常——宿主函数报错的标准姿势；
- **`GetExceptionInfo(ctx, true)`**（scripthelper，main.cpp:427）：一行拿到异常的函数、行号、调用栈、变量值的格式化文本；
- **UTF-8 输出**（PrintString，main.cpp:490）：AngelScript 字符串是字节串（默认按 UTF-8 处理），Windows 控制台需转 UTF-16 用 WriteConsoleW 输出；
- bin/script.as 是个特性大杂烩：循环引用交给 GC、协程填数组、析构顺序（`PrintOnDestruct` 全局对象最后销毁）、变量遮蔽警告——适合当调试器的练习靶子。

## 底层原理

- **为什么行回调能拿到局部变量**：编译器为每个函数生成变量声明表（名字、类型、栈偏移、生存区间），随字节码存在 `asCScriptFunction`（as_scriptfunction.cpp）里；`GetAddressOfVar` 用当前栈帧基址 + 偏移直接定位。调试信息是一等公民，不是可选附加物。
- **Prepare/Unprepare 的成本**：Prepare 要在 context 栈上给函数分配栈帧并把参数区清零，Unprepare 释放持有的对象。池化避免的是 CreateContext 的内存分配，而 Prepare 每次执行都免不了——所以 `GetFunctionByDecl` 结果要缓存（tutorial 已提示），context 要池化，这两个优化在 game 示例里合体。
- **脚本异常传播**：`SetException` 置位后 ExecuteNext 中止当前指令流，逐帧回卷调用栈（清理每帧的对象），最后 Execute 返回 asEXECUTION_EXCEPTION——见 as_context.cpp 的异常处理路径。

## 构建运行

```powershell
& $msbuild projects\msvc2026\asrun.sln /p:Configuration=Debug /p:Platform=x64 /m
cd bin
.\asrun.exe script.as arg1 arg2      # script.as 里 #pragma debug 已开调试，输入 c 继续、h 帮助
```

## 思考题

1. 在调试器里 `p g_str` 打印全局变量，然后 `b script.as:xx` 给协程函数 fillArray 设断点——观察协程切换时调用栈的变化。
2. 为什么 ReturnContextCallback 里必须 Unprepare 而不能直接入池？（返回值可能持有对象引用；若下次 Prepare 前模块被 Discard，悬挂引用就崩了。）
3. exec() 把系统命令输出捕获进脚本字符串（main.cpp:532）——用它写个 asrun 脚本版的 "git status 解析器" 练手。
