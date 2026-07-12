# 11 — 虚拟机（asCContext::ExecuteNext）

> **文件**：`as_context.cpp`（6952 行）/ `as_context.h`（288 行）
> **难度**：★★★★★
> **前置**：09（字节码指令集）、10（编译器如何生成字节码）
>
> 本篇是全系列的核心：编译器生成的字节码最终在这里被逐条执行。asCContext 同时承担
> 三重身份——解释器主循环的宿主、脚本调用栈的管理者、宿主程序观察脚本执行的窗口
>（调试/异常/挂起 API 全在这个类上）。

---

## 1. 模块定位：context = 一个可挂起的执行栈

一个 `asIScriptEngine` 可以创建任意多个 `asCContext`，每个 context 拥有**自己独立的
数据栈和调用栈**，engine 只保存共享的类型信息与函数字节码。这正是 AngelScript 支持
协程（coroutine）与"多脚本线程"的基础：挂起一个 context（`Suspend()`），切换到另一个
context 继续 `Execute()`，各自的执行现场互不干扰——现场就保存在 context 的成员里
（as_context.h:196-247）：

- `m_status`：状态机（as_context.h:196）
- `m_callStack`：调用栈，每帧 9 个 PTR（as_context.h:205，`CALLSTACK_FRAME_SIZE = 9`，as_context.h:255）
- `m_stackBlocks` + `m_stackIndex`：分块增长的数据栈（as_context.h:208-212）
- `m_regs`：虚拟寄存器组 `asSVMRegisters`（as_context.h:247）

`asSVMRegisters` 定义在 angelscript.h:1424-1434，公开给 JIT 编译器使用：

| 寄存器 | 作用 |
|---|---|
| `programPointer` | 指向当前字节码指令（相当于 PC） |
| `stackFramePointer` | 当前函数栈帧基址（相当于 FP，局部变量以负偏移访问） |
| `stackPointer` | 栈顶（相当于 SP，**向低地址增长**） |
| `valueRegister` | 64 位临时寄存器，存放基本类型返回值/比较结果 |
| `objectRegister` + `objectType` | 对象临时寄存器：存对象返回值及其类型 |
| `doProcessSuspend` | 是否需要在 SUSPEND 指令处做挂起/行回调检查 |

## 2. 执行模型：分块栈与栈帧布局

### 2.1 stackBlocks 分块增长（ReserveStackSpace，as_context.cpp:1987-2096）

数据栈不是一整块 `realloc` 的内存——那会使栈上所有指针失效。它是一个块数组：
第 0 块大小为 `engine->ep.initContextStackSize`（as_context.cpp:2001），之后每块
**指数翻倍**（`m_stackBlockSize << m_stackIndex`，as_context.cpp:2056）。当前块放不下
时（as_context.cpp:2033 的 while 判断，注意预留 `RESERVE_STACK = 2*AS_PTR_SIZE`，
as_context.h:252），就切到下一块，并把新函数的参数区 memcpy 过去（由调用方
PrepareScriptFunction 完成，as_context.cpp:2130-2136）。超过
`ep.maximumContextStackSize` 则抛 `TXT_STACK_OVERFLOW`（as_context.cpp:2046）。

### 2.2 一个栈帧的布局

栈向低地址生长；`stackFramePointer`（fp）指向参数区起点，局部变量在 fp **下方**
以负偏移访问（字节码里写作 `l_fp - asBC_SWORDARG0(l_bc)`）：

```
高地址
+---------------------------+
|  调用者的栈帧 ...          |
+---------------------------+  <- 调用前的 stackPointer
|  [this 指针]  (若为方法)    |   fp[0]
|  [返回值缓冲区指针] (若对象  |   fp[AS_PTR_SIZE]
|   按值返回, DoesReturnOnStack)|
|  参数0, 参数1, ...          |   fp[offset]，正偏移
+---------------------------+  <- stackFramePointer (fp)
|  局部变量 / 对象变量指针     |   fp[-stackOffset]，负偏移
|  (variableSpace 个 DWORD)   |
+---------------------------+  <- 进入函数后的 stackPointer
|  表达式临时值、待传实参 ...  |   继续向下压
+---------------------------+  <- 运行中的 sp
低地址        （每格为 asDWORD，4 字节）
```

这个布局由两处代码建立：
- **最外层函数**：`Prepare()` 里 `m_regs.stackFramePointer = m_regs.stackPointer - m_argumentsSize - m_returnValueSize`（as_context.cpp:805-808），并把参数区清零（as_context.cpp:811）；若对象按值返回，还在参数区写入返回缓冲区地址（as_context.cpp:813-821）。
- **脚本内调用**：`PrepareScriptFunction()` 把 `fp = sp`（此时实参已被调用者压好，as_context.cpp:2140），把堆上对象变量槽清零为 null（as_context.cpp:2145-2155），再 `sp -= variableSpace` 留出局部变量区（as_context.cpp:2158）。

## 3. Prepare 与 SetArg*：调用前的手工布栈

`Prepare(func)`（as_context.cpp:693-824）流程：状态检查 → 清理上次执行残留
（CleanStack/CleanReturnObject，as_context.cpp:711-716）→ 计算
`m_argumentsSize = 参数空间 + this 指针 + 返回缓冲指针`（as_context.cpp:766-773）→
`ReserveStackSpace(参数 + 返回值 + scriptData->stackNeeded)`（as_context.cpp:783）→
置 `m_status = asEXECUTION_PREPARED`（as_context.cpp:801）。若重复 Prepare 同一函数，
会跳过大部分设置（as_context.cpp:741-745）。

`SetArgDWord(arg, value)`（as_context.cpp:1152-1193）演示了参数如何落栈：类型校验后，
从 0 开始累加偏移——先跳过 this 指针和返回缓冲指针（as_context.cpp:1178-1184），
再累加前面各参数的 `GetSizeOnStackDWords()`（as_context.cpp:1186-1187），最后
`*(asDWORD*)&m_regs.stackFramePointer[offset] = value`（as_context.cpp:1190）。
整个 SetArg* 家族（Byte/Word/QWord/Float/Double/Address/Object/VarType）只是类型检查
与写入宽度不同，寻址逻辑完全一致；`SetObject()` 则写 fp[0]（as_context.cpp:1043）。

## 4. Execute 与解释器主循环

`Execute()`（as_context.cpp:1523-1612）：置 `asEXECUTION_ACTIVE` → 把自己压入线程本地
的活动 context 栈（`asPushActiveContext`，as_context.cpp:1537，嵌套过深则抛
TXT_TOO_MANY_NESTED_CALLS，as_context.cpp:1541-1542）→ 首次执行时
`SetProgramPointer()`（as_context.cpp:1543-1544；它处理 delegate/虚函数解析，脚本函数
走 PrepareScriptFunction，系统函数直接 `CallSystemFunction`，as_context.cpp:1678-1727）
→ 核心循环：

```cpp
while (m_status == asEXECUTION_ACTIVE)
{
    ExecuteNext();                                   // as_context.cpp:1550-1558
    if (m_status == asEXECUTION_EXCEPTION && m_exceptionWillBeCaught)
        CleanStack(true);                            // 回卷到 catch 块后继续循环
}
```

循环退出后做自动 GC 步进（按新建对象数执行等量步数，as_context.cpp:1572-1586），
最后按 m_status 返回 FINISHED/ABORTED/SUSPENDED/EXCEPTION（as_context.cpp:1591-1611）。

### 4.1 switch vs computed goto

`ExecuteNext()`（as_context.cpp:2243-5046，约 2800 行）用宏做了双形态分发
（as_context.cpp:2233-2241）：

```cpp
#if AS_USE_COMPUTED_GOTOS
#define INSTRUCTION(x) case_##x
#define NEXT_INSTRUCTION() goto *(const void*) dispatch_table[*(asBYTE*)l_bc]
#define BEGIN() NEXT_INSTRUCTION();
#else
#define INSTRUCTION(x) case x
#define NEXT_INSTRUCTION() break
#define BEGIN() switch( *(const asBYTE*)l_bc )
#endif
```

- **switch 形态**：`for(;;) { switch(*(asBYTE*)l_bc) { case ...: break; } }`
  （as_context.cpp:2325、2342）。每条指令结束 `break` 回到循环头，重新做一次
  范围检查 + 跳转表查找 + 跳转。源码注释特意强调 case 必须**连续无空洞**，这样编译器
  只需一次查表（as_context.cpp:2337-2341）；MSVC 下还用 `__assume(0)` 告诉编译器
  default 不可达（as_context.cpp:5019-5025）。
- **computed goto 形态**（GCC≥6 / clang，见 as_config.h:110-112、457-461、671-678；
  clang-cl 在 Windows 上也启用）：函数开头定义 256 项的静态标签地址表 `dispatch_table`
  （as_context.cpp:2250-2318，未用的 201-255 号填 `&&INSTRUCTION(FAULT)`），每条指令
  末尾直接 `goto *dispatch_table[下一条指令的操作码]`。

性能差别在**分支预测**：switch 形态所有指令共享同一个间接跳转点，CPU 的间接分支
预测器只能记住"上一次跳去哪"，命中率极低；computed goto 让每条指令拥有自己的跳转点，
预测器能学到"ADDi 之后常来 CpyVtoR4"这类真实的指令相关性，典型解释器能快 15-25%。
此外还省掉了 switch 的边界检查和回到循环头的无条件跳转。

主循环把三个热寄存器缓存为局部变量 `l_bc / l_sp / l_fp`（as_context.cpp:2321-2323），
让编译器把它们放进物理寄存器；**只有在可能被外界观察或需要跨函数时才写回 m_regs**
——这是通读所有 case 时反复出现的模式。

## 5. 指令实现走读（9 条）

1. **asBC_PshV4（压变量，as_context.cpp:2368-2372）**——最小指令的样子：
   `--l_sp; *l_sp = *(l_fp - asBC_SWORDARG0(l_bc)); l_bc++;`。变量按 fp 负偏移取，
   压栈是指针递减，指令长 1 DWORD。

2. **asBC_ADDi（整数加法，as_context.cpp:3667-3670）**——三地址形式：
   `*(int*)(l_fp - ARG0) = *(int*)(l_fp - ARG1) + *(int*)(l_fp - ARG2)`，源和目的都是
   局部变量槽，不经过求值栈——这是编译器优化后的"寄存器式"算术（09 篇讲过 bcINSTR_wW_rW_rW 编码）。

3. **asBC_DIVi（除法带陷阱，as_context.cpp:3682-3710）**——先取除数，为 0 时**把
   l_bc/l_sp/l_fp 写回 m_regs**（异常处理器需要精确现场），然后
   `SetInternalException(TXT_DIVIDE_BY_ZERO); return;`；还专门检查 `0x80000000 / -1`
   的溢出（as_context.cpp:3696-3706）。所有可能出错的指令都遵循"先写回、再抛异常、
   立即 return"的套路。

4. **asBC_JMP / asBC_JZ（跳转，as_context.cpp:2485-2498）**——相对跳转：
   `l_bc += 2 + asBC_INTARG(l_bc)`；条件版读 `m_regs.valueRegister`（CMPi 等比较指令把
   -1/0/1 写进它，as_context.cpp:2938）决定加偏移还是顺序 +2。

5. **asBC_CALL（脚本调脚本，as_context.cpp:2432-2456）**——帧切换全过程：取函数 id、
   `l_bc += 2` 让返回地址指向下一条指令 → 写回三寄存器 →
   `CallScriptFunction(m_engine->scriptFunctions[i])` → 把 m_regs 重新读回局部变量 →
   若状态不再是 ACTIVE（挂起/异常）则 `return` 退出 ExecuteNext。

6. **asBC_RET（返回，as_context.cpp:2459-2482）**——若调用栈为空或栈顶是嵌套调用
   标记帧（`s[0] == 0`，as_context.cpp:2462-2463），置 `asEXECUTION_FINISHED` 并 return；
   否则 `PopCallState()` 恢复调用者的 5 个寄存器，再 `l_sp += w` 弹掉本次调用的实参
   （w 是指令内嵌的参数字数，as_context.cpp:2469-2480）。

7. **asBC_CALLSYS（进入原生桥，as_context.cpp:3008-3047）**——写回寄存器后
   `l_sp += CallSystemFunction(i, this)`（原生调用桥在 as_callfunc.cpp:627，返回它消耗的
   实参 DWORD 数，桥内部按调用约定把栈上参数搬进 CPU 寄存器/原生栈——见第 12 篇）。
   调用返回后检查 `m_doSuspend` 与状态变化（系统函数可能调用了 SetException 或 Suspend）。

8. **asBC_ALLOC（对象创建，as_context.cpp:3138-3210+）**——双分支：**脚本类**走
   `m_engine->CallAlloc(objType)` 分配 + `ScriptObject_Construct` 预初始化 + 把对象指针
   压栈后 `CallScriptFunction(构造函数)`（as_context.cpp:3152-3169），构造函数是脚本代码，
   在同一循环里继续解释；**应用类**只分配内存，构造行为（如有）作为系统函数调用
   （as_context.cpp:3183 起）。注意 as_context.cpp:3160-3161：把新对象地址写进调用者
   预留的目标变量槽——在构造函数执行**之前**，这样构造中抛异常时栈回卷器能找到它。

9. **asBC_CALLINTF（接口/虚方法分发，as_context.cpp:3933-3957 →
   CallInterfaceMethod，as_context.cpp:2171-2231）**——AngelScript 的"虚表"：从栈顶取
   对象（null 则抛异常并置 `m_needToCleanupArgs = true`，让异常处理器清掉已压的实参，
   as_context.cpp:2175-2181）；若是接口方法，**线性搜索**对象类型的 `interfaces` 数组
   找到该接口的 VFT 偏移（as_context.cpp:2196-2205，源码注释承认应改二分），
   `realFunc = objType->virtualFunctionTable[func->vfTableIdx + offset]`
   （as_context.cpp:2216）；类自身虚方法则直接按 `vfTableIdx` 查表
   （as_context.cpp:2226）。最后 `CallScriptFunction(realFunc)`。

10. **asBC_SUSPEND（协作点，as_context.cpp:3110-3136）**——快路径只有
   `if(!doProcessSuspend) { l_bc++; NEXT; }`；慢路径先触发行回调
   （CallLineCallback，as_context.cpp:3113-3120），再检查 `m_doSuspend`：写回寄存器、
   置 `asEXECUTION_SUSPENDED`、return——**挂起就这么简单**，因为全部现场本来就在
   m_regs 和 m_callStack 里。编译器默认在每个语句边界插入 SUSPEND（可用
   asEP_BUILD_WITHOUT_LINE_CUES 关闭）。

另外值得一看：**asBC_JitEntry**（as_context.cpp:4337-4367）——有 JIT 函数且参数非 0
时把控制权交给 JIT 代码（传入 `&m_regs`），否则当 nop 滑过；未识别操作码统一落到
`case_FAULT`（as_context.cpp:5040-5047），写回现场后抛 TXT_UNRECOGNIZED_BYTE_CODE。

## 6. 调用与返回：三条路径的帧管理

- **脚本 → 脚本**：`CallScriptFunction`（as_context.cpp:2099-2113）=
  `PushCallState()` + 切换 `m_currentFunction` / `programPointer` +
  `PrepareScriptFunction()`。`PushCallState`（as_context.cpp:1840-1883）向 m_callStack
  压 5 个值：旧 fp、当前函数、返回地址（pc）、旧 sp、stackIndex（帧大小 9 是为兼容
  PushState 的标记帧）；源码注释解释了为什么先存进局部数组 `s[5]` 再拷贝——规避指针
  别名分析、减少 cache 抖动（as_context.cpp:1861-1866）。`PopCallState`
  （as_context.cpp:1886-1907）逆向恢复。
- **脚本 → 原生**：不压调用帧！CALLSYS 直接进 `CallSystemFunction`（as_callfunc.cpp:627），
  原生函数用的是 **C++ 真实线程栈**；脚本这边只在返回后把 sp 加上被消耗的参数大小。
  正因如此，原生函数执行期间 `m_callingSystemFunction` 记录着它（供 SetException 判断合法性）。
- **原生 → 脚本（嵌套调用）**：宿主在被脚本调用的 C++ 函数里想再执行脚本，若复用同一个
  context，需 `PushState()`（as_context.cpp:1730-1794）：先 `PushCallState()` 压入当前
  脚本帧，再压一个 **s[0]=0 的标记帧**，其中保存 m_callingSystemFunction、
  m_initialFunction、原始栈指针、valueRegister/objectRegister 等 9 项
  （as_context.cpp:1764-1775），随后状态回到 UNINITIALIZED 供重新 Prepare。
  RET 看到标记帧就停止返回（as_context.cpp:2462-2467），CleanStack 的回卷也在标记帧
  处止步（as_context.cpp:5165-5167）。`PopState`（as_context.cpp:1798-1837）逆向恢复
  并把状态置回 ACTIVE。`IsNested()` 就是数标记帧（as_context.cpp:234）。

## 7. 异常与清理

`SetException(descr, allowCatch)`（as_context.cpp:5050-5058）只允许在系统函数执行期间
调用（`m_callingSystemFunction == 0` 则返回错误）；内部与 VM 自身的陷阱一样走
`SetInternalException`（as_context.cpp:5060-5092）：置 `asEXECUTION_EXCEPTION`、记录
异常函数/行号/列号（行号从字节码位置反查调试信息，as_context.cpp:5076）、用
`FindExceptionTryCatch()` 预判是否会被 try/catch 捕获（as_context.cpp:5088）、触发
异常回调。注意它**并不立即回卷栈**——主循环因状态改变而退出各层 ExecuteNext，栈回卷
发生在 Execute 循环里（被捕获时，as_context.cpp:1556-1557）或下一次 Prepare/Unprepare
时（未捕获时保留现场，这样宿主还能用自省 API 打印完整调用栈！）。

回卷由 `CleanStack`（as_context.cpp:5149-5181）驱动：逐帧调 `CleanStackFrame` +
`PopCallState`，直到被捕获或遇到嵌套标记帧。`CleanStackFrame`
（as_context.cpp:5526-5723）做四件事：清理调用中途的实参（CleanArgsOnStack，
as_context.cpp:5376）；查本帧 tryCatchInfo 决定是否在此捕获（as_context.cpp:5553-5568，
捕获则把 pc/sp 直接设到 catch 块，as_context.cpp:5671-5676）；**析构局部对象**；最后
析构按值传入的参数对象（as_context.cpp:5688-5720）。

析构局部对象时的难点：一个栈槽在异常发生时到底有没有活对象？**堆上对象**好办——槽里
是指针，非 null 就释放（as_context.cpp:5618-5652）；**栈上值对象**（inline 分配）无法
从内存内容判断，必须靠编译期生成的 `objVariableInfo` 表：`DetermineLiveObjects`
（as_context.cpp:5260-5374）拿当前 pc 在表中回放 asOBJ_INIT/asOBJ_UNINIT/
asBLOCK_BEGIN/asBLOCK_END 事件，算出每个变量的存活计数，只有 `liveObjects[n] > 0`
才调用析构（as_context.cpp:5656-5663）。这就是第 10 篇中编译器为每次对象构造/析构
发射生存期标记的**消费端**。返回值对象由 `CleanReturnObject`（as_context.cpp:5100-5147）
单独清理（区分栈上返回、引用类型 Release、值类型 destruct+free）。

## 8. 挂起 / 恢复与状态机

`asEContextState` 的主干迁移：UNINITIALIZED --Prepare--> PREPARED --Execute-->
ACTIVE --SUSPEND 指令--> SUSPENDED --再次 Execute--> ACTIVE --RET 到底--> FINISHED；
旁路有 EXCEPTION、ABORTED、ERROR、DESERIALIZATION。

`Suspend()`（as_context.cpp:1507-1520）只置三个标志（m_doSuspend、
m_externalSuspendRequest、doProcessSuspend），**可以从另一个线程安全调用**；真正的
停车发生在 VM 下次执行到 SUSPEND 指令（或 CALLSYS 返回、PrepareScriptFunction）时。
`Abort()`（as_context.cpp:1489-1504）额外置 m_doAbort，Execute 退出时转为 ABORTED。
samples/events 里"脚本跑一半让出、宿主稍后续跑"的效果，底层就是：SUSPEND 指令处
写回 m_regs → Execute 返回 asEXECUTION_SUSPENDED → 宿主随时再调 Execute()，
因为 programPointer 非零，直接从断点继续主循环。

## 9. 调用栈自省 API（调试器的数据来源）

- `GetCallstackSize()`（as_context.cpp:1910-1916）：`1 + m_callStack.GetLength()/9`，
  stackLevel 0 是最深的当前函数。
- `GetFunction(level)`（as_context.cpp:1919-1929）：level 0 取 m_currentFunction，
  否则从调用帧的 `s[1]` 取。
- `GetLineNumber(level)`（as_context.cpp:1932-1984）：拿该帧的 pc（上层帧要 `bytePos -= 1`
  才落在 CALL 指令上，as_context.cpp:1951-1953），减去字节码基址得偏移，再查函数的
  行号调试表；返回值低 20 位是行、高位是列。
- `GetVarCount/GetVar/GetAddressOfVar`（as_context.cpp:6002-6180）：变量元数据来自
  `func->scriptData->variables`，地址 = `stackFramePointer[-stackOffset]`（堆上对象再
  解一次引用）。`IsVarInScope`（as_context.cpp:5184）同样靠 objVariableInfo 的
  asOBJ_VARDECL 标记判断。

调试器套路：SetLineCallback（as_context.cpp:5770）里检查断点 → 命中则用上述 API 展示
栈与变量 → 需要暂停就调 Suspend()。

## 10. 如果你要自己实现

1. **先写 switch 版最小循环**：`while(true) switch(*pc++)`，指令集只要
   PUSH/ADD/JMP/JZ/CALL/RET/HALT 七条就能跑图灵完备的程序；确认正确性后再改
   computed goto（保持 AngelScript 这种双形态宏是好方法，随时可切回 switch 调试）。
2. **把 pc/sp/fp 放局部变量**，只在"逃逸点"（调用、异常、挂起）写回上下文对象——
   这一条对解释器性能的影响常常大于分发方式本身。
3. **栈布局先简单后紧凑**：先学 AngelScript 用统一的 DWORD 栈 + fp 相对寻址；
   参数区由调用者压好、fp 一指即成帧，返回时靠指令内嵌的参数大小弹栈，不需要
   单独的"清栈"指令序列。
4. **异常安全从第一天设计**：每条可能陷阱的指令都要能在"精确现场"停下；对象生存期
   信息必须由编译器生成（运行时无法从栈内存猜出值对象是否已构造）。
5. **调用栈与数据栈分离**（AngelScript 即如此）：控制信息不混在数据栈里，回卷、
   自省、序列化都简单得多。

## 11. 动手实验

1. **单步看寄存器**：在 as_context.cpp:3668（asBC_ADDi）下断点，跑
   `sdk/samples/tutorial`。观察 `l_fp - asBC_SWORDARG1(l_bc)` 处的两个操作数、写回
   槽位的变化；再在 as_context.cpp:2445（CALL → CallScriptFunction）断点，对比调用
   前后 m_regs 三个指针和 m_callStack.GetLength() 的变化，验证第 6 节的帧布局。
2. **数指令**：在 ExecuteNext 的 `for(;;)` 顶部（as_context.cpp:2325）加一个静态计数器
   （或直接开 AS_DEBUG，用现成的 `stats.Instr(...)` 统计，as_context.cpp:2330），跑一次
   tutorial 的 Execute，看看一个几行的脚本函数要执行多少条字节码；再对比开关
   asEP_BUILD_WITHOUT_LINE_CUES 前后 SUSPEND 指令的数量差。
3. **观察挂起**：给 context 设一个每 N 行调用 `ctx->Suspend()` 的行回调，验证 Execute
   返回 asEXECUTION_SUSPENDED 后再次 Execute 能无缝续跑（正是 samples/events 的做法）。

## 12. 扩展阅读

- Crafting Interpreters, "A Virtual Machine"：https://craftinginterpreters.com/a-virtual-machine.html
  ——单文件栈式 VM 的完整教学实现，与本篇的 switch 形态一一对应。
- Wikipedia, "Threaded code"：https://en.wikipedia.org/wiki/Threaded_code
  ——direct/indirect/token threading 的理论谱系；computed goto 即 token threading 的
  C 语言实现。
- 下一篇（12）：CallSystemFunction 原生调用桥——CALLSYS 指令跨过 as_callfunc.cpp:627
  之后，参数如何按 ABI 搬进 CPU 寄存器。
