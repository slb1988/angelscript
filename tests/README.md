# AngelScript async/await 前端测试（VS2022 独立工程）

本目录为 `6e6f713b..HEAD` 的 async/await 前端改动提供**可独立运行、可断点调试**的测试工程。

## 构建与运行

```bat
:: 命令行构建（或直接用 VS2022 打开 projects/vs2022/angelscript_async_tests.sln）
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" ^
    projects\vs2022\angelscript_async_tests.sln -p:Configuration=Debug -p:Platform=x64

:: 运行（输出 [PASS]/[FAIL]，全部通过时退出码为 0）
projects\vs2022\x64\Debug\angelscript_async_tests.exe
```

## 在 VS2022 中调试

1. 打开 `projects/vs2022/angelscript_async_tests.sln`，F5 启动。
2. 常用断点位置（对应各提交）：
   - `source/as_parser.cpp` → `IsAsyncFunctionModifier()` / `IsAwaitExpression()`：async/await 向前看识别
   - `source/as_parser.cpp` → `ParseExprTerm()` 内 `snAwait` 分支：await 挂为前置运算符节点
   - `source/as_parser.cpp` → `IsVarDecl()` 开头的 await 拦截：语句二义性消除（提交 8db9830b）
   - `source/as_builder.cpp` → `RegisterScriptFunction()` 内 `asTRAIT_ASYNC` 块：7 条注册期校验（提交 0045f1d1）
   - `source/as_builder.cpp` → `RegisterScriptFunction()` 内 `ttAmp` 语法树扫描：显式引用参数检测（提交 255a3d6f）
   - `source/as_compiler.cpp` → `CompileExpressionTerm()` 内 `snAwait` 块：编译期环境/类型检查 + lowering 熔断器
3. 想让某个测试单独跑：在 `tests/main.cpp` 的 `main()` 里注释其它用例，或在目标用例函数里下断点。

## 测试覆盖与提交的对应关系

| 测试 | 覆盖提交 |
|---|---|
| EP 属性默认关/可开 | 6e6f713b（基线）+ 02ea5bcc |
| await 作变量名兼容、非 async 函数 await 容错解析 | 0045f1d1 |
| async 方法解析/注册/共存 | 32e5e57a |
| 全局/接口/mixin/属性访问器/返回类型/引用返回/allow_discard 拒绝 | 0045f1d1 |
| `&out`、`const FVector &in` 参数拒绝 vs 按值参数放行 | 0045f1d1 → 255a3d6f（关键回归） |
| `await t;` 不误判为变量声明 | 8db9830b |
| 合法 await 撞 "state-machine lowering" 熔断器 | 0045f1d1（lowering 在引擎集成层） |
| async 默认 nodiscard | 32e5e57a |
| 特性关闭时 async 语法失效 | 02ea5bcc |

## 重要：为独立运行所做的适配

这个仓库是 UE fork 的 vendor VM 源码，无法原样脱离 UE 编译/运行。本测试工程做了以下适配
（改动均有 `NOTE(standalone-test)` 注释，可用 git diff 查看）：

1. **`tests/ue_stubs/`**：UE 头文件桩（`CoreMinimal.h` 的 TArray/TMap/TMultiMap/FMemory/
   FCrc/check/ensure、`Misc/MemStack.h`、`HAL/PlatformAtomics.h`、`UObject/*`、
   `AngelscriptManager/Settings/CodeModule`）。只实现 fork 源码实际用到的 API 子集。
2. **`source/` 三处 unity-build 掩盖的问题**（独立编译必须修）：
   - `as_bytecode.h`：补 `class asCBuilder;` 前置声明；
   - `as_bytecode.cpp`：补 `#include "as_builder.h"`；
   - `as_module.cpp`：补 `#include "as_scriptobject.h"` + 一处 `check(...)` 缺的分号；
   - `as_string_util.cpp`：补回单参数版 `asStringScanDouble`（头里有声明，实现只在 UE 层）。
3. **`source/as_module.cpp` 的 `Build()`**：fork 把 `builder->Build()` 拆成了分段 API
   （`BuildParallelParseScripts`→`BuildGenerateTypes`→`BuildLayoutClasses`→
   `BuildAllocateGlobalVariables`→`BuildLayoutFunctions`→`BuildGenerateFunctions`→
   `BuildCompileCode`，UE 插件层跨模块驱动）。定义 `AS_STANDALONE_BUILD_DRIVER` 时
   （本测试工程已定义）在 `Build()` 内顺序执行全部阶段；不影响 UE 构建路径。
4. **重新应用了被回退的提交 `3c30219d`**（`git revert --no-commit 779b0201`，未提交）：
   fork 把 `asOBJ_GC` 定义为 0，导致 `$func` 的 5 个 GC 行为注册失败、
   `configFailed=true`，**任何模块都无法构建**。这是该修复提交存在的意义；
   回退后独立环境完全不可用。若要在 UE 中验证，请先确认该修复在真实工程里无害。
5. **Debug 配置定义了 `NDEBUG`**：与 UE 构建行为一致（UE Debug 默认不开 CRT 调试断言），
   否则引擎初始化中的历史 `asASSERT` 会弹窗挂起。仍保留 `/Zi` 调试信息。

## 这个 fork 与上游 AngelScript 的语法差异（写测试脚本时注意）

- **`@` 句柄 token 被移除**（`as_tokendef.h` 中注释掉）：句柄靠
  `asOBJ_IMPLICIT_HANDLE` 隐式表达。注册类型时带该标志 + 引擎属性
  `asEP_ALLOW_IMPLICIT_HANDLE_TYPES=1`，脚本里直接写 `FTask M()` 即句柄返回。
- **`null` 不存在**，用 `nullptr`。
- **绑定声明里 `float` 报错**，用 `float32`/`float64`。
- **脚本 `class` 需要预注册 `AddPreClassData`**（UE 用来挂 UObject 影子类型），
  否则报 "Structs cannot be base classes for anything"；`struct` 不受此限。
- **`mixin` 语法是 `mixin <函数定义>`**，不是上游的 `mixin class`。
- **接口体内不认 `async`**（独立解析路径），builder 的接口校验目前不可达。

## 已知边界（本仓库范围内未实现）

- async 函数体的**状态机 lowering 不在本仓库**（引擎集成层，见提交 83f6a668 的
  P4 CL 137443）。因此含 `await` 的 async 函数必然在编译期报
  "Async function reached bytecode compilation before state-machine lowering"——
  这是故意的熔断器，不是 bug。
- 不打桩真实 `UObject`，脚本对象的运行时（实例化脚本类等）不在测试范围内。
