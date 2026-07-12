# console — 交互式脚本控制台（增量编译与引擎自省）

**难度：★★☆☆☆　前置：tutorial**

一个游戏内控制台（quake 式 console）的原型：用户在命令行输入任意表达式立即求值，还能在运行时动态添加/删除脚本函数和变量。演示 AngelScript 作为"应用内 REPL"的用法。

## 运行效果

```
> exec 1 + 2 * 3
7
> addvar int myVar = 42
Variable added.
> exec myVar + 1
43
> addfunc int sq(int x) { return x*x; }
Function added.
> exec sq(9)
81
> listvars / listfuncs / delvar / delfunc / quit
```

## 知识点拆解（对照 source/main.cpp）

### 1. 全局属性绑定：C++ 变量直通脚本（main.cpp:158-162）

```cpp
float g_gravity;
engine->RegisterGlobalProperty("float g_gravity", &g_gravity);
```

脚本里读写 `g_gravity` 就是**直接读写这个 C++ 变量的内存**——没有拷贝、没有同步回调。这是游戏引擎暴露 cvar（配置变量）的标准做法。

### 2. 注册重载函数：asFUNCTIONPR（main.cpp:171-177）

C++ 的 `grab` 有 7 个重载，`asFUNCTION(grab)` 无法区分取哪个，必须用 `asFUNCTIONPR(grab, (int), void)` 显式指定参数表和返回类型来取函数指针。

`_grab` 系列是个巧妙的技巧：`exec` 命令把用户输入包装成 `_grab(表达式)` 再编译执行（main.cpp:188）。**编译器的重载决议**会根据表达式的类型自动选中对应的 `_grab` 重载，从而把任意类型的求值结果打印出来——利用编译器本身做类型分发。

### 3. 立即执行语句：ExecuteString（main.cpp:192）

`ExecuteString(engine, code, mod)` 来自 **scripthelper** add_on。它的内部实现值得一读（add_on/scripthelper/scripthelper.cpp）：把语句包进一个临时函数 `void ExecuteString(){ ... }`，用 `mod->CompileFunction` 即时编译，执行完丢弃。传入模块参数使得语句能访问该模块的全局变量——这就是 exec 能引用 addvar 添加的变量的原因。

### 4. 增量编译：运行时修改模块（main.cpp:199-304）

这是本示例最核心的知识点。模块 Build 之后**仍可动态增删内容**：

| 操作 | API |
|------|-----|
| 添加全局变量 | `mod->CompileGlobalVar("addvar", "int v = 1;", 0)` |
| 添加函数 | `mod->CompileFunction("addfunc", code, 0, asCOMP_ADD_TO_MODULE, &func)` |
| 删除 | `mod->RemoveGlobalVar(index)` / `mod->RemoveFunction(func)` |

注意细节：
- `asCOMP_ADD_TO_MODULE` 标志表示编译结果并入模块（否则只是临时函数）；
- `CompileFunction` 通过出参返回 `asIScriptFunction*`，**用完必须 Release**（main.cpp:271）；
- 删除函数后调用 `engine->GarbageCollect()`（main.cpp:303）——脚本函数可能递归（自己引用自己），是潜在的循环引用，需要 GC 兜底。这暗示了一个底层事实：**`asCScriptFunction` 本身也是 GC 管理的对象**。

### 5. 引擎自省（反射）：ListVariables/ListFunctions（main.cpp:306-365）

引擎侧和模块侧各有一套枚举 API：

- 引擎注册项：`GetGlobalPropertyCount/GetGlobalPropertyByIndex`、`GetGlobalFunctionCount/GetGlobalFunctionByIndex`；
- 模块内容：`mod->GetGlobalVarCount/GetGlobalVarDeclaration`、`GetFunctionCount/GetFunctionByIndex`；
- `engine->GetTypeDeclaration(typeId)` 把 typeId 还原成可读的类型名。

typeId 是 AngelScript 类型系统的运行时句柄（int），后面 asrun/game 示例里会大量出现。

## 底层原理

- **RegisterGlobalProperty** 在引擎里创建 `asCGlobalProperty`（as_globalproperty.cpp）记录地址，编译器遇到该标识符时直接生成访问这个绝对地址的字节码——所以它比注册 get/set 函数快得多。
- **CompileFunction** 走的是与 Build 相同的 `asCBuilder`/`asCCompiler` 流水线（as_module.cpp → asCModule::CompileFunction），只是输入是单个函数、跳过全局声明收集阶段。
- **重载决议**发生在编译期：`asCCompiler::MatchFunctions`（as_compiler.cpp）按参数类型给每个候选打分（精确匹配 > 基本类型隐式转换 > 需构造转换），`_grab` 技巧正是复用了这套机制。
- RegisterScriptArray(engine, **false**)：第二个参数表示不把 array 设为默认数组类型（`int[]` 语法不启用，只能写 `array<int>`）。

## 构建运行

```powershell
& $msbuild projects\msvc2026\console.sln /p:Configuration=Debug /p:Platform=x64 /m
cd bin; .\console.exe
```

## 思考题

1. `exec string("abc") + "def"` 为什么能打印？（重载决议选中 `_grab(const string &in)`。）
2. addfunc 添加一个调用自身的递归函数再 delfunc，为什么不会内存泄漏？（GC 处理函数对象的循环引用。）
3. 为什么 `ConfigureEngine` 结尾调用 `ClearMessageCallback`（main.cpp:180），而 `ExecString` 失败时仍能知道出错？（看 ExecuteString 的返回值约定；这也导致本示例的编译错误细节不打印——可以自己改进它。）
