# 08 — 编译器核心（asCCompiler：AST → 字节码）

> **系列**：AngelScript 源码精读（16 篇）· 第 08 篇
> **难度**：★★★★★（全系列最难——单文件 18593 行，类型系统、重载决议、对象生命周期在这里汇合）
> **前置**：第 04 篇（AST）、第 05 篇（类型系统 asCDataType）、第 07 篇（builder 符号环境）、第 09 篇（asCByteCode，本篇的输出端，可对照读）
> **核心文件**：`as_compiler.h/cpp`（501 + 18593 行）、`as_variablescope.h/cpp`（87 + 142 行）

一句话概括：asCCompiler 是一个**每次只编译一个函数**的单遍树遍历编译器——递归下降地走 AST，每个子表达式的编译结果装在一个 `asCExprContext`（类型信息 + 一段还没拼接的字节码）里向上传，运算符节点把左右两个 context 合并成一个；没有独立的语义分析遍，也没有中端 IR，类型检查、重载决议、隐式转换、临时变量生命周期全部在这一遍里就地完成。

---

## 1. 模块定位

```
asCBuilder（第 07 篇）
   │ 对每个函数调用一次
   ▼
asCCompiler::CompileFunction(builder, script, parameterNames, funcNode, outFunc, classDecl)
   │  输入：函数体 AST 子树 + builder 提供的符号环境（全局函数/类型/属性查询）
   ▼
outFunc->scriptData->byteCode（最终字节码）+ variables/lineNumbers/objVariablePos（调试与异常清理信息）
```

- 编译器实例是**一次性**的：`Reset()`（as_compiler.cpp:126）清空全部状态，builder 对每个函数复用一个 asCCompiler 对象。
- 入口除了 `CompileFunction`（as_compiler.cpp:766）还有三个变体：`CompileDefaultConstructor`（274）、`CompileDefaultCopyConstructor`（152）、`CompileFactory`（331），以及编译全局变量初始化表达式的 `CompileGlobalVariable`（1510）。
- 有趣的细节：函数体的 AST 并不是 builder 传进来的现成树——CompileFunction 里**重新 parse 一遍**函数体（as_compiler.cpp:797-800 `parser.ParseStatementBlock(script, blockBegin)`），因为第一遍 builder 只做了浅解析（省内存，见第 04 篇）。

## 2. 中间表示：asCExprContext / asCExprValue（理解全文件的钥匙）

编译器没有三地址码这类 IR，它的"IR"就是**伴随每个子表达式流动的一个结构体**（as_compiler.h:140-176）：

```cpp
struct asCExprContext {
    asCByteCode  bc;          // 这个子表达式已生成、但还没拼进函数的字节码片段
    asCExprValue type;        // 值的静态描述（见下）
    int property_get, property_set;   // 若这是 get_/set_ 虚拟属性访问，暂存 accessor 函数 id
    asCArray<asSDeferredParam> deferredParams; // 函数调用后需要延后清理/回写的参数
    asCScriptNode *exprNode;  // 出错时报行号用
    asCString methodName, enumValue;  // 尚未消歧的函数名/枚举值符号
    ...
};
struct asCExprValue {         // as_compiler.h:59-119
    asCDataType dataType;
    bool isLValue : 1;        // 能否被赋值
    bool isTemporary : 1;     // 是否为编译器分配的临时变量（用完要 Release）
    bool isConstant : 1;      // 编译期常量（值就存在下面的 union 里）
    bool isVariable : 1;      // 值在某个栈变量里（stackOffset 有效）
    bool isExplicitHandle : 1;
    ...
    int stackOffset;          // 局部变量偏移，或全局变量索引
    union { asQWORD qwordValue; double doubleValue; ... };  // 常量值
};
```

关键点：

1. **值有四种存在形态**，由标志位组合表达：编译期常量（`isConstant`，字节码里还什么都没有）、栈变量（`isVariable`）、引用（`dataType.IsReference()`，地址在值寄存器或栈顶）、栈顶值。很多辅助函数就是做形态迁移：`ConvertToVariable`（as_compiler.cpp:16030，把常量变成 `asBC_SetV4` 写入临时变量、把引用 `asBC_RDR4` 读入变量）、`ConvertToTempVariable`（16004）、`ConvertToReference`（13974）、`Dereference`（6717）。
2. **字节码是"挂"在表达式上的**：编译 `a+b` 时，左右子树各自的 `ctx->bc` 先独立存在，`CompileMathOperator` 决定好求值顺序后用 `MergeExprBytecode(ctx, lctx); MergeExprBytecode(ctx, rctx)`（16343-16344）拼接，再补上一条 `ADDi`。这让编译器可以**自由调整子表达式的求值顺序**（复合赋值时先右后左，16335-16336），也能在"只探测不生成"模式下工作（重载决议时 `generateCode=false`）。
3. **常量折叠是形态之一而不是独立 pass**：只要 `lctx->type.isConstant && rctx->type.isConstant`（16309），运算符函数直接算出结果塞回 `ctx->type.SetConstantDW(...)`，一条字节码都不发（见第 7 节）。
4. **"待定符号"也挂在 context 上**：`methodName` 存尚未决议的函数名（`funcdef@ f = myFunc;` 里 `myFunc` 编译时还不知道选哪个重载，等赋值目标类型出现才由 `DetermineSingleFunc`（1612）或 ImplicitConvObjectRef 的 funcdef 分支（8244-8293）敲定）；`enumValue` 存歧义枚举名；`isAnonymousInitList` 表示 `{1,2,3}` 这种要等目标类型才能编译的初始化列表（10887-10893 先记下 AST，ImplicitConversion 7910-7920 再回头编译）。这是单遍编译器处理"类型自下而上、期望自上而下"矛盾的手法：把没法立刻决定的东西原样带着走。

`asCExprValue` 的常量值存在匿名 union 里且强制走 `SetConstantB/W/DW/QW/F/D` 访问器（as_compiler.h:107-118 注释说明是为了大小端安全）。两个 context 的合并原语是 `MergeExprBytecode`（只拼字节码）与 `MergeExprBytecodeAndType`（连同类型一起转移，10818 表达式栈收敛时用）。

## 3. 栈帧与变量模型

### 3.1 栈帧布局（SetupParametersAndReturnVariable，as_compiler.cpp:484）

偏移以 DWORD 计，**参数在非正偏移，局部变量从 +1 开始**：

```
offset  -N..0 : this 指针（成员函数，489 行 stackPos = -AS_PTR_SIZE）
              → 隐藏返回值指针（值类型按值返回时，526-527）
              → 各参数（533-576，依 GetSizeOnStackDWords() 递减 stackPos）
offset  +1.. : 局部/临时变量（0 保留，GetVariableOffset 从 1 起算，6596）
```

名为 `"return"` 的伪变量记录返回值槽位（581 行）。函数收尾时 `byteCode.Ret(-stackPos)` 弹掉全部参数（967）。运行时真正的栈帧还要叠加表达式求值用的栈深：`stackNeeded = byteCode.largestStackUsed + variableSpace`（467），前者由 asCByteCode 在 Finalize 时静态模拟得出（第 09 篇）。

一个容易迷惑的点：**变量"偏移"指向其最后一个 DWORD**（GetVariableOffset 注释 6593，多 DWORD 变量返回 `varOffset+size-1`，6609-6616）。这与 VM 用负向增长的栈帧寻址方式配套。

### 3.2 变量分配器（AllocateVariable，as_compiler.cpp:6519）

编译器维护一组平行数组（as_compiler.h:465-483）：`variableAllocations`（每个槽的类型）、`variableIsTemporary`、`variableIsOnHeap`、`freeVariables`（空闲槽）、`tempVariables`（当前活着的临时变量）、`reservedVariables`（本次分配不许碰的偏移）。策略：

- 类型先归一化——1 DWORD 的原始类型一律记作 int，2 DWORD 记作 double（6524-6528），提高槽复用率；
- 原始类型和值类型在栈上，其余（句柄、引用类型）`isOnHeap=true`（6533-6539）——"变量槽"里存的是堆对象指针；
- 优先线性搜索 `freeVariables` 找同类型同温度的空槽复用（6541-6571），否则 push 新槽；偏移是把前面所有槽的尺寸累加出来的（GetVariableOffset，6591）。

所谓 "Reserve"：源码里没有 `ReserveVariable` 函数，对应机制是 `reservedVariables` 数组 + `AllocateVariableNotIn(type, isTemp, forceOnHeap, ctx)`（6510）——先把 `ctx->bc.GetVarsUsed()` 收集到的已用偏移压进 reservedVariables，分配完再弹掉，防止"给右操作数分配的临时变量踩了左操作数还没消费的值"（CompileMathOperator 16190-16200 是典型用法）。

释放的两条路径：`DeallocateVariable(offset)`（6654，纯粹归还槽位）和 `ReleaseTemporaryVariable(type, bc)`（6686/6695，**先生成析构字节码再归还**，只对 isTemporary 生效）。

### 3.3 命名变量的作用域（asCVariableScope）

`variables` 指向当前作用域，构成父链（as_variablescope.h:63-81：`parent`、`isBreakScope`、`isContinueScope`、`variables` 数组）。`AddVariableScope(isBreak, isContinue)`（as_compiler.cpp:6379）/`RemoveVariableScope()`（6391）成对出现；`sVariable` 记录名字、类型、stackOffset、onHeap、isPureConstant（`const int x=5` 直接当编译期常量用，SymbolLookupLocalVar 10949-10952）。查名字就是沿父链线性搜（as_variablescope.cpp:106-119）。**命名变量的槽位在语句块结束时才归还**（CompileStatementBlock 1484-1497：逆序调析构 + DeallocateVariable），临时变量则在表达式内即用即还。

## 4. 表达式编译的层级与一次手工推演

调用层级（函数在 as_compiler.h:298-305 依次声明）：

```
CompileAssignment(10268)     // 最外层：赋值是右结合，特殊处理
 └ CompileCondition(10291)   // 三目 ?:（条件、两分支各自编译，用标签拼接）
    └ CompileExpression(10722)
       ├ ConvertToPostFix(10734)          // Shunting-Yard，把兄弟节点链变成后缀序列
       └ CompilePostFixExpression(10771)  // 显式栈求值：操作数压栈，遇运算符弹两个
          └ CompileExpressionTerm(10872)  // 值 + 后缀op（先，10913）+ 前缀op（后，10922）
             ├ CompileExpressionValue(11966)  // 字面量/变量/函数调用/括号/cast/lambda
             ├ CompileExpressionPostOp(14572) // . [] () ++ --
             └ CompileExpressionPreOp(13540)  // - ! ~ ++ @
```

注意 parser（第 04 篇）不处理优先级，表达式节点是**平铺的兄弟链**；`ConvertToPostFix` 用两个栈按 `GetPrecedence`（15134）现场排序——注释自嘲"写的时候不知道这就是 Shunting-Yard"（10736-10738）。

**推演 `int r = a + b * c;`（a、b、c 均为局部 int）**：

1. 后缀序列：`a b c * +`。
2. `a`、`b`、`c` 各经 `CompileExpressionValue` → `SymbolLookupLocalVar`（10942）→ `type.SetVariable(int, offset, false)`。**没有任何字节码**——值的位置记录在 asCExprValue 里。
3. 弹出 `b`、`c` 编译 `*`：`CompileOperator`（15884）→ 非重载 → `CompileMathOperator`（16181）。两边都是 int 无需转换；非常量路径（16323）：`ConvertToVariableNotIn` 对已是变量的 b、c 是 no-op；`ReleaseTemporaryVariable`（都不是临时，跳过）→ 分配临时槽 a₁ → 发射 `MULi a₁, b, c`（16463-16467 `ctx->bc.InstrW_W_W(instruction, a, b, c)`）→ `ctx->type.SetVariable(int, a₁, true)`。
4. 编译 `+`：同路，左边 a 是普通变量、右边 a₁ 是临时变量。`ReleaseTemporaryVariable(rctx)` 把 a₁ 标记归还——所以 `ADDi` 的结果槽**很可能复用 a₁**。合并字节码后：`MULi a₁,b,c` + `ADDi a₁,a,a₁`。
5. 回到 `CompileDeclaration`（3168）→ 初始化走赋值路径，`PerformAssignment`（6885）发射 `CpyVtoV4 r, a₁`；随后 `bc.OptimizeLocally(tempVariableOffsets)`（第 09 篇的窥孔优化）通常把拷贝合并成直接以 r 为目标。

优化前的概念序列（AS_DEBUG dump 可验证，变量号以实际为准）：

```
MULi  v4, v2, v3    ; a₁ = b*c   （v4 为临时槽）
ADDi  v4, v1, v4    ; a₁ = a+a₁  （结果复用刚释放的 a₁）
CpyVtoV4 v5, v4     ; r = a₁     （OptimizeLocally 后通常直接 ADDi v5,v1,v4）
```

整个过程中**没有任何求值栈操作**——AngelScript 的 VM 虽是栈机，但原始类型运算全部走"寄存器式"的变量槽三地址指令，栈只用于传参和对象地址（第 10 篇）。

若换成字符串拼接 `s = s1 + s2`，第 3 步会在 `CompileOverloadedDualOperator`（15361）命中 `opAdd`，变成一次成员函数调用：参数经 `PrepareArgument`（1773）拷入临时变量，`MakeFunctionCall`（15782）发 `CALLSYS`，返回的新 string 是挂在 ctx 上的临时对象。

## 5. 重载决议与隐式转换

### 5.1 MatchFunctions（as_compiler.cpp:2719）：收集→过滤→逐参数打分→取最小代价

1. **参数个数过滤**（2733-2782）：数目不符者剔除，但要算上默认参数（2759-2766）和变参（2737-2748）。
2. **逐参数打分**（2794-2827）：对第 n 个实参调 `MatchArgument(funcs, tempFuncs, args[n], n)`（15199），返回每个候选的转换代价；与现存候选表**求交集并累加 cost**，转不过去的候选剔除（同时记入 `failedMatches` 供错误消息用）。
3. 命名参数再补一轮匹配（2830-2934）。
4. **取最小总代价**（2937-2950）：可能剩多个 cost 相同的——即歧义。随后 `FilterConst`（2956）按对象常量性再筛一次，变参版本让位给非变参版本（2959-2988）。
5. 仍不是恰好 1 个就报错（2990-3081）：0 个报 "No matching signatures" 并列出候选与失败原因（asSFailedMatch，as_compiler.h:246-261）；多个报 "Multiple matching signatures"。

单参数打分 `MatchArgument`（15225）的本质是：拷贝一份实参的类型描述，**以 `generateCode=false` 跑一遍 ImplicitConversion**（15284），转换后类型与形参不符则 -1，否则返回累计代价。&out 参数还要反向验证"形参类型能转回实参类型"（15254-15268）。

### 5.2 转换代价层级（EConvCost，as_compiler.h:201-220，如实照录）

```cpp
asCC_NO_CONV=0            精确匹配
asCC_CONST_CONV=1         仅加 const
asCC_ENUM_SAME_SIZE_CONV=2 / ENUM_DIFF_SIZE=3
asCC_PRIMITIVE_SIZE_UP_CONV=4 / SIZE_DOWN=5    // 小转大 / 大转小
asCC_SIGNED_TO_UNSIGNED_CONV=6 / UNSIGNED_TO_SIGNED=7
asCC_INT_TO_FLOAT_CONV=8 / FLOAT_TO_INT=9
asCC_REF_CONV=10          派生类→基类、类→接口
asCC_OBJ_TO_PRIMITIVE_CONV=12
asCC_TO_OBJECT_CONV=14    用户定义转换 / 构造转换
asCC_VARIABLE_CONV=16     转 ?（vartype）
```

原始类型间的代价判定在 ImplicitConvPrimitiveToPrimitive 开头的 if-链（7386-7409）。

### 5.3 ImplicitConversion 的分支树（7897）

```
ImplicitConversion(ctx, to, ...)
 ├ to 是 funcdef 且 ctx 是 lambda → ImplicitConvLambdaToFunc(7825)
 ├ to 是 ?                        → 直接改类型，cost=asCC_VARIABLE_CONV(7931-7939)
 ├ to 是原始类型
 │   ├ from 原始 → ImplicitConvPrimitiveToPrimitive(7337)
 │   │             常量走 ImplicitConversionConstant(9220，编译期直接换算数值)
 │   └ from 对象 → ImplicitConvObjectToPrimitive(7959，找 opImplConv/opConv 返回原始类型)
 └ to 是对象类型
     ├ from 原始 → ImplicitConvPrimitiveToObject(9088)：找单参构造/工厂
     │             （explicit 构造只在显式 cast 时候选，9108；cost = asCC_TO_OBJECT_CONV + 内层 MatchFunctions，9139）
     └ from 对象 → ImplicitConvObjectToObject(8576)
         ├ ImplicitConvObjectRef(8185)：派生→基类/接口(8204-8214)、注册的 opCast/opImplCast
         │   经 CompileRefCast(6996)，null→任意句柄(8188-8195)，均为 asCC_REF_CONV
         └ ImplicitConvObjectValue(8299)：用户定义值转换——
             隐式只认 opImplConv(8333)，显式 cast 兼收 opConv/opImplConv(8319)，
             两者都有时显式优先 opConv(8343-8358)；命中即生成成员调用(8384)，cost=asCC_TO_OBJECT_CONV(8389)
```

## 6. 控制流：标签与跳转

标签是编译期递增整数 `nextLabel`（label 0 预留给函数出口，803/937），`bc->Label(id)` 落点、`InstrDWORD(asBC_JMP/JZ/JNZ, id)` 引用，最终由 asCByteCode 回填相对偏移（第 09 篇）。

- **if/else**（CompileIfStatement 4911）：条件 → `CpyVtoR4 + ClrHi + JZ afterLabel`（4946-4948）；有 else 时在 then 尾部 `JMP afterElse`（5015）再落 afterLabel（5019）。条件是常量时直接不发跳转或发无条件 JMP（4955-4961）——分支级常量折叠。
- **while**（5786）：`beforeLabel:` 条件 `JZ afterLabel` … 循环体 … `JMP beforeLabel` `afterLabel:`；循环体内固定插 `asBC_SUSPEND + JitEntry`（5840-5841）保证宿主可以中断脚本。
- **for**（5126）：四个标签（5133-5136），代码排列为 `init; JMP conditionLabel; insideLabel: 体; continueLabel: 增量; conditionLabel: 条件 JNZ insideLabel; afterLabel:`（5216-5239）——条件放尾部使每圈只跳一次。
- **break/continue 栈**：`breakLabels/continueLabels` 两个数组（as_compiler.h:451-452），进入循环/switch 时 push（5795-5796），退出 pop。`CompileBreakStatement`（5944）沿 `variables` 父链向上直到 `isBreakScope`，**逆序为途中每层变量发析构**（5955-5962），再 `JMP breakLabels.back()`。
- **switch**（4513）：表达式转成 32 位变量 → 收集各 case 的**编译期常量**（4588 非常量报错）→ 冒泡排序（4648）→ 按"间隔 ≤5 视为连续"切成 ranges（4669-4680）；每段 >2 个值就生成**跳转表**：`SUBi` 求索引后 `JmpP`（4730）跳入一串 JMP，空洞跳 default（4732-4741）；小段则逐个 `CMPi + JZ`（4748-4755）。default 缺省即 breakLabel（4638-4639）。case 体在比较代码之后统一排布，落 `firstCaseLabel++`（4785），天然支持 fall-through。
- **try/catch**（CompileTryCatch 4869）：没有条件跳转——try 体前放 `bc->TryBlock(beforeCatchLabel)` 伪指令（4881）标记"异常展开到此为止then跳 catch"，try 体尾 `JMP afterCatchLabel`（4888）跳过 catch。TryBlock 标记最终由 ExtractTryCatchInfo（458）收进函数的 try/catch 表。
- **控制流里的 hasReturn 数据流**：每个语句编译函数都回传 `*hasReturn`，if 要两分支都 return 才算（5042 `*hasReturn = hasReturn1 && hasReturn2`），switch 规则更细（4772-4818：break 未 return 即 false、无 default 即 false），最终 CompileFunction 用它诊断"并非所有路径都返回值"（813-817）；顺带做不可达代码告警（1417-1424）。

## 7. 对象生命周期的编译期管理

- **构造/析构**：声明变量时 `CallDefaultConstructor`（1103）或走初始化表达式；作用域結束/break/continue/return 路径各自补 `CallDestructor`（1341）——堆上对象和句柄发 `asBC_FREE`（1356-1363），栈上值类型直接调 destruct 行为（1368-1376）。
- **异常安全信息**：字节码里穿插伪指令 `bc->ObjInfo(offset, asOBJ_INIT/asOBJ_UNINIT)` 标记对象在该点已构造/已析构（如构造后 9196、析构后 1380），`bc->Block(true/false)` 标记清理块（5954/5963）；FinalizeFunction 里 `byteCode.ExtractObjectVariableInfo(outFunc)`（460）把它们提取成 asCScriptFunction 的 objVariableInfo 表，运行时异常展开靠它决定哪些变量需要析构（第 10/11 篇）。try/catch 信息同理（458 `ExtractTryCatchInfo`）。
- **deferred params**：函数调用的引用参数不能在调用后立刻清理——&out 要把结果**回写**到实参 lvalue，返回引用的函数还可能引用着某个参数。`AfterFunctionCall`（12647）把这些参数登记进 `ctx->deferredParams`（12677-12683）；`ProcessDeferredParams`（12702）在表达式安全点统一处理：&in 临时变量直接 Release（12714），&out 构造一个右值 context 对原实参 `DoAssignment` 回写（12734-12756），不可赋值的实参此时才报 "not an lvalue"（12768）。
- **临时对象**：`PrepareTemporaryVariable`（6029）把任意值复制进临时变量以延长生命周期；表达式语句结束时（CompileExpressionStatement 5992）Release 全部残留临时变量，语句块间断言 `tempVariables.GetLength()==0`（1474）。
- **返回引用的安全检查**（CompileReturnStatement 6108）：函数返回引用时编译器静态拒绝一切悬垂可能——不许返回局部变量/临时变量的引用（6163-6169，唯一例外是 this），表达式不许携带 deferredParams（6189-6195，因为回写发生在引用已交出之后），类型必须精确匹配不许经转换（6173-6185，转换会产生新的临时值）。这是"编译期生命周期检查"的一个朴素而有效的样本。
- **构造函数的成员初始化跟踪**：编译构造函数时用 `m_isConstructorCalled`、`m_initializedProperties` 等状态跟踪 super() 是否被调、哪些成员已初始化；if/else 两分支必须行为一致（5056-5061 "both conditions must call constructor"、5066-5086 逐成员比对报错）——控制流敏感的初始化检查。

## 8. 关键代码走读

1. **CompileFunction 主流程**（as_compiler.cpp:766-980）：Reset → SetupParametersAndReturnVariable（773）→ 重新 parse 函数体（797-800）→ 预留出口 label 0（803）→ CompileStatementBlock（808）→ 非 void 检查所有路径有 return（813-817）→ 头部插 `asBC_JitEntry`（823）→ 构造函数成员初始化/基类构造调用（825-911）→ 统计 `variableSpace`（917）→ 逆序析构局部变量（922-933）→ `Label(0)` + 参数析构（937-951）→ `Ret(-stackPos)`（967）→ FinalizeFunction（969）。AS_DEBUG 下把字节码 dump 成 txt（971-977）。
2. **FinalizeFunction**（407-481）：临时变量类型注册给调试器（415-419）→ `byteCode.Finalize`（454，跳转回填+窥孔，见第 09 篇）→ 提取 try/catch 与对象生命周期表（458-460）→ 拷出字节码、计算 `stackNeeded = largestStackUsed + variableSpace`（467）。
3. **CompilePostFixExpression**（10771-10828）：后缀求值循环——`snExprTerm` 压栈（10787-10793），运算符弹 r、l 调 `CompileOperator`（10796-10801），context 对象用 free-list 复用（10789/10800）。整个表达式编译器的"主泵"。
4. **CompileMathOperator**（16181-16667）：先把对象操作数转成最佳数学类型（16188-16201），再做双操作数类型提升（16204-16235，double>float>int64>int32，常量不主导符号性），除以常量零报错（16312-16321）；非常量路径选指令 `ADDi/SUBf/MULi64/...`（16349-16460）发 `InstrW_W_W`（16467）；全常量路径 260 行手写折叠（16478-16666），含 int 除零静默为 0（16494 的 TODO 承认应报错）、`**` 溢出检测（16515-16522）、uint 相减为负自动改 int（16528-16529）。
5. **MatchFunctions 的打分交集**（2793-2827）+ **MatchArgument 试转换**（15270-15286）：重载决议 = "对每个实参用 dry-run 的 ImplicitConversion 算 cost，逐参数求交集累加，最后取最小值"。歧义即"最小 cost 并列"，在 3072-3079 报 multiple matching signatures。
6. **ImplicitConvObjectValue**（8299-8390）：用户定义转换的候选收集——遍历类方法找名为 `opImplConv`/`opConv`、无参、返回目标类型的函数（8314-8338），显式 cast 时 opConv 压过 opImplConv（8343-8358），命中后 `PerformFunctionCall`（8384）。
7. **FindPropertyAccessor**（13995 起）：把 `obj.health` 改写为方法调用——拼出 `"get_"+name`/`"set_"+name`（14007-14008），在成员方法或全局函数里按参数个数筛选（14030/14043），受 `engine->ep.propertyAccessorMode` 管制（14000，mode 3 只认显式 `property` 关键字声明的）。此时**并不立即生成调用**，只把 funcId 存进 `ctx->property_get/property_set`；等上层知道这是读还是写：读走 `ProcessPropertyGetAccessor`（14500，经 MatchFunctions 验证后 MakeFunctionCall，14556），赋值在 `DoAssignment`（9860）里转给 `ProcessPropertySetAccessor`（14296），`p.x += 1` 这类复合赋值则 get+运算+set 三连（ProcessPropertyGetSetAccessor，14358）。
8. **MakeFunctionCall：一次调用的完整装配线**（15782-15882）：先把"对象表达式"的字节码挪到一边（`objBC`，15793-15794），因为参数要先求值；`PrepareFunctionCall`（2310）逐参数调 `PrepareArgument`（1773，按 &in/&out/&inout 和值/引用决定拷贝或取址，&out 会把原表达式存进 `origExpr` 留给回写）；若某个参数临时变量的偏移被 objBC 用到了，就 `ExchangeVar` 换新槽（15809-15832，防求值顺序冲突）；值类型按值返回时先分配返回槽并把地址作为隐藏参数压栈（15866-15873 `DoesReturnOnStack`）；然后拼回 objBC、`MoveArgsToStack`（2355）、`PerformFunctionCall` 发 `asBC_CALL/CALLSYS/CALLINTF`；调用后 `AfterFunctionCall`（12647）登记 deferred params。读懂这一条线，CompileFunctionCall（13183）/CompileConstructCall（12804）都是它的外壳。
9. **CompileStatementBlock**（1393-1507）：语句分发（snDeclaration→3168，其余→CompileStatement 4475 的 switch）、不可达代码告警（1417-1424）、块尾逆序析构+归还槽位（1484-1497）。`bc->Block()` 配对（1403/1500）为异常表划界。每条语句后断言临时变量清零（1474）——整份文件对生命周期纪律的总闸。

### 8.1 速查：本篇引用的函数索引（均为 as_compiler.cpp）

| 主题 | 函数 : 行号 |
|---|---|
| 入口/收尾 | CompileFunction:766 · Reset:126 · SetupParametersAndReturnVariable:484 · FinalizeFunction:407 |
| 语句 | CompileStatementBlock:1393 · CompileStatement:4475 · CompileIfStatement:4911 · CompileSwitchStatement:4513 · CompileForStatement:5126 · CompileWhileStatement:5786 · CompileDoWhileStatement:5864 · CompileTryCatch:4869 · CompileBreakStatement:5944 · CompileContinueStatement:5968 · CompileReturnStatement:6108 · CompileDeclaration:3168 |
| 表达式 | CompileAssignment:10268 · CompileCondition:10291 · CompileExpression:10722 · ConvertToPostFix:10734 · CompilePostFixExpression:10771 · CompileExpressionTerm:10872 · CompileExpressionValue:11966 · CompileExpressionPreOp:13540 · CompileExpressionPostOp:14572 · CompileOperator:15884 · CompileMathOperator:16181 · CompileComparisonOperator:16997 · CompileBooleanOperator:17379 |
| 重载/转换 | MatchFunctions:2719 · MatchArgument:15199/15225 · ImplicitConversion:7897 · ImplicitConvPrimitiveToPrimitive:7337 · ImplicitConvObjectRef:8185 · ImplicitConvObjectValue:8299 · ImplicitConvObjectToObject:8576 · ImplicitConvPrimitiveToObject:9088 · ImplicitConversionConstant:9220 · CompileRefCast:6996 |
| 变量/生命周期 | AllocateVariable:6519 · AllocateVariableNotIn:6510 · GetVariableOffset:6591 · DeallocateVariable:6654 · ReleaseTemporaryVariable:6686 · ConvertToVariable:16030 · CallDefaultConstructor:1103 · CallDestructor:1341 · AfterFunctionCall:12647 · ProcessDeferredParams:12702 |
| 调用/属性 | MakeFunctionCall:15782 · PrepareArgument:1773 · PrepareFunctionCall:2310 · MoveArgsToStack:2355 · CompileFunctionCall:13183 · CompileConstructCall:12804 · FindPropertyAccessor:13995 · ProcessPropertyGetAccessor:14500 · ProcessPropertySetAccessor:14296 · ProcessPropertyGetSetAccessor:14358 · DoAssignment:9860 |

## 9. 如果你要自己实现

最小可行方案（Lua/V8 之外的第三条路，AngelScript 就是这个形态的成熟版）：

1. **类型标注的表达式上下文**：`struct ExprCtx { Type t; Chunk bc; bool isConst, isTemp; int slot; Value constVal; }`。所有 `compileXxx(node, &ctx)` 递归函数只操作它。先支持"常量/栈槽"两种形态，引用形态可以后加。
2. **变量分配器**：一个 `vector<Type> slots` + free-list，按类型复用；命名变量归作用域链管，作用域退出归还。临时变量在每个表达式语句结束时断言清零——这个断言（对应 as_compiler.cpp:1474）能帮你抓 90% 的生命周期 bug。
3. **重载决议先只做精确匹配**：cost 恒 0，不匹配即淘汰。之后按需引入代价表——关键设计是把"隐式转换"写成 `int tryConvert(ctx, to, bool emit)`，dry-run 与真转换共用一份代码（AngelScript 的 `generateCode` 参数就是这个思想），否则两处逻辑必然漂移。
4. **控制流只需要整数标签 + 二遍回填**；break/continue 用两个栈存标签即可，析构展开沿作用域链走。
5. 有析构语义的语言必须从第一天就规划"异常路径清理表"（ObjInfo 的等价物），事后补极其痛苦。

## 10. 动手实验

1. **AS_DEBUG dump**：在 `as_config.h` 或编译选项里定义 `AS_DEBUG`（debug 构建下 as_debug.h:41 生效），CompileFunction 收尾处（as_compiler.cpp:971-977）会对每个函数调 `byteCode.DebugOutput("__<类名>_<函数名>.txt", ...)`，文件写到进程当前目录的 `AS_DEBUG/` 子目录（as_bytecode.cpp:2163-2186）；可用引擎属性 `asEP_NO_DEBUG_OUTPUT`（angelscript.h:186）关掉。编译 `int f(int a,int b,int c){ return a+b*c; }`，对照第 4 节的推演读 `__f.txt`——还能看到优化前后两份列表（DebugOutput 在 Finalize 前后各被利用一次的效果体现在临时变量编号上）。
2. **观察常量折叠**：把函数体改成 `return 2+3*4;`，dump 里应只剩一条设置返回寄存器的指令，没有任何 MULi/ADDi。
3. **观察重载决议代价**：注册 `void g(float)` 和 `void g(uint)`，脚本里 `g(1)`——int→uint 是 cost 6（asCC_SIGNED_TO_UNSIGNED_CONV，反向 uint→int 是 7），int→float 是 cost 8，应选 uint 版；把两个候选改成 `g(int8)`/`g(int16)` 再试，体会 SIZE_DOWN 的并列歧义报错（对照 as_compiler.h:203-219）。
4. **观察 switch 跳转表**：case 值写 1,2,3,100，dump 里能看到 1-3 一段 `JmpP` 表 + 100 单独 `CMPi/JZ`（as_compiler.cpp:4704 的 `>2` 阈值和 4678 的 `+5` 间隔判定）。

5. **观察 deferred out 参数**：注册 `void h(int &out)`，脚本 `int x; h(x);`——dump 里可见调用前为 out 参数分配临时变量、`CALLSYS` 之后才出现把临时变量赋回 x 的代码（ProcessDeferredParams 生成，12747 的 DoAssignment）；再试 `h(void);` 体会 void 表达式匹配 &out（15233-15238）。
6. **观察属性访问器改写**：注册全局 `int get_score() property` / `void set_score(int) property`，脚本 `score += 1;`——dump 里是一次 get 调用、一次 ADDi、一次 set 调用（ProcessPropertyGetSetAccessor，14358），源代码里根本没有的三条调用链。

### 阅读建议（少走弯路）

- 别从文件头顺读——按"CompileFunction → CompileStatementBlock → 挑一个语句 → 跟一个表达式到 CompileMathOperator"的调用链读，一条线走通再横向铺开。
- 碰到 `generateCode` 参数时刻记住它有 dry-run 用法：同一个函数既是"转换执行器"又是"重载决议的代价探针"。
- `ctx->bc` 里的东西**还没进函数**，真正的顺序由 Merge/AddCode 的调用次序决定；调试"求值顺序不对"类问题先看谁先 Merge 谁。
- `OptimizeLocally` 被到处调用（如 4951、5833），dump 出来的字节码和你从源码推的"原始序列"会有出入，属正常。

## 11. 扩展阅读

- Crafting Interpreters · Compiling Expressions：https://craftinginterpreters.com/compiling-expressions.html —— clox 的单遍 Pratt 编译器，与 asCCompiler 同属"边 parse/遍历边发码"流派，但没有类型和重载，读完能体会 AngelScript 多出的复杂度都花在哪。
- Wikipedia · Shunting-yard algorithm：https://en.wikipedia.org/wiki/Shunting-yard_algorithm ——`ConvertToPostFix`（as_compiler.cpp:10738）注释里自己引用的就是这篇。
- Wikipedia · Function overloading / C++ overload resolution：https://en.wikipedia.org/wiki/Function_overloading ——对比 C++ 的 rank 制（exact > promotion > conversion > user-defined）与 AngelScript 的加法代价制：前者按最差参数分级，后者对所有参数求和取最小，语义并不等价（AngelScript 允许"两处小转换"赢过"一处大转换"）。
