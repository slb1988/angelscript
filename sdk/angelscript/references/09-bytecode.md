# 09 — 字节码指令集与优化器（asCByteCode）

> **概括**：本篇精读 `include/angelscript.h` 中的指令集定义（`asEBCInstr`/`asBCTYPE_*`/`asBCInfo`，约 300 行）与 `source/as_bytecode.h/.cpp`（约 3200 行）。`asCByteCode` 是编译器（第 08 篇）与虚拟机（第 10 篇）之间的桥：编译期它是一条可以随意增删改的**指令双向链表**，上面跑着一个相当激进的窥孔优化器；`Finalize()` 之后它被压成一个扁平的 `asDWORD` 数组，成为 VM 执行和字节码序列化的最终格式。
>
> **难度**：★★★☆☆（指令语义繁多但套路一致；窥孔优化的模式匹配代码量大，挑重点读即可）
> **前置**：第 08 篇（表达式编译器 asCCompiler）。知道 `asCCompiler` 一边遍历 AST 一边往 `asCByteCode` 对象里发射指令即可。

---

## 1. 模块定位

- **编译器的输出格式**：`asCCompiler` 的每个表达式/语句编译函数都持有局部的 `asCByteCode` 对象，通过 `Instr*` 函数族发射指令，再用 `AddCode()`（as_bytecode.cpp:1630-1651，O(1) 链表拼接）把子表达式的代码串起来。
- **VM 的输入格式**：`FinalizeFunction()`（as_compiler.cpp:407-467）最后调用 `byteCode.Output(outFunc->scriptData->byteCode.AddressOf())`（第 465 行），把链表写成扁平 `asDWORD` 数组存进 `asCScriptFunction::scriptData->byteCode`——`asCContext::ExecuteNext` 的大 switch 直接在这个数组上取指。
- **序列化的对象**：`asIBinaryStream` 保存/加载模块（as_restore.cpp）读写的正是这个数组；`asBCInfo` 表提供逐指令的格式与尺寸信息，使序列化器无需硬编码每条指令的布局。
- 顺带一提：`source/as_texts.h` 集中了整个库的用户可见文本——全部是 `TXT_*` 宏形式的 printf 格式串，按字母序分组，后缀（`_s`/`_i`/`_d`）编码占位符类型（如 as_texts.h:44 `TXT_s_ALREADY_DECLARED "'%s' is already declared"`）；字节码模块本身不产生用户错误，出错只靠 `asASSERT`。

## 2. 指令集设计

### 2.1 指令枚举与分类浏览

`asEBCInstr`（angelscript.h:1463-1675）定义了 **201 条真实指令**（0 到 `asBC_MAXBYTECODE=201`，行 1666），外加 6 个**临时标记**（250-255：`asBC_TryBlock/VarDecl/Block/ObjInfo/LINE/LABEL`，行 1668-1674，注释明言 "Temporary tokens. Can't be output to the final program"）。粗略分类：

| 类别 | 代表指令（编号） |
|---|---|
| 栈操作 | `PopPtr`(0)、`PshC4`(2)、`PshV4`(3)、`PSF`(4 压变量地址)、`SwapPtr`(5)、`PshC8`(47)、`PshVPtr`(48)、`RDSPtr`(49 栈顶解引用)、`PshNull`(73) |
| 算术 | `ADDi/SUBi/MULi/DIVi/MODi`(115-119)、浮点(120-129)、立即数变体 `ADDIi/SUBIi/MULIi...`(130-135)、64 位(160-170)、`POWi` 族(193-199)、自增自减 `IncVi/DecVi`(37-38) |
| 比较/测试 | `CMPd/CMPu/CMPf/CMPi`(50-53)、立即数比较 `CMPIi/CMPIf/CMPIu`(54-56)、`CmpPtr`(99)、测试指令 `TZ/TNZ/TS/TNS/TP/TNP`(18-23，把值寄存器转成 bool) |
| 跳转 | `JMP`(11)、条件跳 `JZ/JNZ/JS/JNS/JP/JNP`(12-17)、switch 跳表 `JMPP`(57)、`JLowZ/JLowNZ`(187-188) |
| 调用 | `CALL`(9 脚本函数)、`CALLSYS`(61 注册的宿主函数)、`CALLBND`(62 绑定导入)、`CALLINTF`(139 接口/虚方法)、`CallPtr`(176 函数指针)、`Thiscall1`(200)、`RET`(10) |
| 对象生命周期 | `ALLOC`(64)、`FREE`(65)、`LOADOBJ/STOREOBJ`(66-67 对象寄存器搬运)、`REFCPY`(69)、`CHKREF`(70 空指针检查)、`RefCpyV`(186)、`Cast`(144) |
| 类型转换 | `iTOf/fTOi/uTOf/fTOu...`(101-114)、窄化 `iTOb/iTOw`(140-141)、64 位互转(145-155) |
| 全局变量/寄存器搬运 | `CpyVtoV4/CpyVtoR4/CpyVtoG4/CpyRtoV4/CpyGtoV4`(80-87)、`LDG/LDV`(96-97 取地址进寄存器)、经寄存器读写 `WRTV1..8/RDR1..8`(88-95) |
| 杂项 | `SUSPEND`(63)、`JitEntry`(175)、初始化列表 `AllocMem/SetListSize/...`(189-192)、`LINE`(254 编译期专用) |

### 2.2 编码格式族 asBCTYPE_*

`asEBCType`（angelscript.h:1678-1704）用一套命名约定描述每条指令的参数布局：

- 小写前缀是**语义**：`rW` = 读一个局部变量（16 位栈帧偏移）、`wW` = 写一个局部变量；无前缀的 `W` = 16 位字面量；`DW`/`QW` = 32/64 位常量。
- 组合即格式：`asBCTYPE_NO_ARG` 无参；`asBCTYPE_wW_rW_rW_ARG` 是"三地址指令"（写一个变量、读两个变量）；`asBCTYPE_rW_DW_ARG` 是"一个变量 + 一个 32 位立即数"；`asBCTYPE_W_DW_ARG` 是"16 位字面量 + 32 位常量"（如 `CALLSYS`）。
- `PTR` 不是独立格式，而是按目标平台指针宽度映射到 DW 或 QW 的宏（angelscript.h:1744-1762），`AS_PTR_SIZE` 相应为 1 或 2（以 DWORD 计）——同一份编译器代码就能生成 32/64 位两种布局。

每种格式的总尺寸（以 DWORD 为单位，1~4）由 `asBCTypeSize[24]` 给出（angelscript.h:1707-1733）。最终内存布局由 `Output()`（as_bytecode.cpp:1968-2045）决定：

```
DWORD 0:  | opcode(byte0) | 0(byte1) | wArg[0](byte2-3，无则清零) |
DWORD 1+: | DW/QW 大参数（memcpy 进来）...                        |
特例 wW_rW_rW：| opcode | 0 | wArg[0] | wArg[1] | wArg[2] |  共 2 个 DWORD
```

`as_bytecode.h:51-53` 的 `BYTECODE_SIZE 4 / MAX_DATA_SIZE 8 / MAX_INSTR_SIZE 12` 就是"4 字节指令头 + 最多 8 字节数据"。opcode 只占一个字节且第二字节恒零（Output 第 1981-1982 行）——解释器取指只需读一个 byte。

### 2.3 栈机模型：操作数栈 + 变量帧 + 值寄存器

AngelScript 的 VM 不是纯栈机。每个函数帧内有三块状态：

1. **局部变量区**：以 word 偏移寻址（`rW/wW` 参数）。算术指令是**三地址式**直接读写变量——`int a = b + c` 编译成一条 `ADDi va, vb, vc`，而纯栈机需要 `push b; push c; add; store a` 四条。
2. **操作数栈**：只用于传参、对象指针和少数中转（`PshC4`/`PopPtr`/`PSF` 等）。`asBCInfo` 的 `stackInc` 字段静态记录每条指令对栈深的影响（如 `PopPtr` 是 `-AS_PTR_SIZE`，angelscript.h:1769）。
3. **值寄存器（value register）**：一个隐式累加器。`CMPi` 把比较结果（-1/0/+1）放进寄存器，`JZ/JS/JP` 等直接测试寄存器，`TZ` 等把寄存器内容转成 bool——避免了纯栈机"结果压栈再弹出判断"的往返。另有**对象寄存器**配合 `LOADOBJ/STOREOBJ` 传递对象返回值。

### 2.4 asBCInfo 表与代表性指令详解

`asBCInfo[256]`（angelscript.h:1767 起）是**唯一权威的指令元数据表**：`{opcode, 编码格式, stackInc, 名字字符串}`（结构体定义行 1736-1742，条目由宏 `asBCINFO(b,t,s)` 生成，行 1764）。发射函数、优化器、序列化器、调试清单全都查这张表。`stackInc == 0xFFFF` 表示栈变化量编译期不定（CALL/RET/ALLOC），必须走专用发射函数。挑几条细看：

- **`asBC_ADDi`**（asBCInfo 行 1884）：`wW_rW_rW_ARG` / stackInc 0。`v[a] = v[b] + v[c]`，完全不碰操作数栈；整数/浮点/64 位共 15 条同构指令（115-129）。
- **`asBC_CALL`**（行 1778）：`DW_ARG` / 0xFFFF。DW 存内部函数 id。经 `Call()`（as_bytecode.cpp:1699-1714）发射：`stackInc = -pop`（注释说明参数由被调方返回时清理），`wArg[0]` 存实参个数供变长参数的序列化与异常处理用，并自动尾随一条 `JitEntry` 让 JIT 能在调用后恢复执行（1712-1713）。
- **`asBC_JZ`**（行 1781）：`DW_ARG` / 0。值寄存器 == 0 则相对跳转。编译期 DW 里放的是 label id，`ResolveJumpAddresses()` 才换算成相对偏移（见 §3）。
- **`asBC_SUSPEND`**（行 1832）：`NO_ARG` / 0。挂起点：VM 在此检查行回调与挂起请求。由 `asBC_LINE` 变换而来（§6），因此挂起粒度 = 语句粒度。
- **`asBC_ALLOC`**（行 1833）：`W_PTR_DW_ARG` / 0xFFFF。堆上分配对象：PTR = objtype 指针，DW = 构造函数 id，W = 实参数。经 `Alloc()`（as_bytecode.cpp:1733-1749）发射。
- **`asBC_RefCpyV`**（行 1955）：`wW_PTR_ARG` / 0。把栈顶句柄拷入局部变量并维护引用计数——枚举里有它，但编译器从不直接发射，它是窥孔优化把 `PSF x + REFCPY` 合成的产物（§4 模式 5）。
- **`asBC_PshC4`**（行 1771）：`DW_ARG` / +1，压 32 位常量入栈；同样常由优化器从 `SetV4 + PshV4` 合成（as_bytecode.cpp:523-533）。
- **`asBC_CMPIi`**（行 1823）：`rW_DW_ARG` / 0，变量与**立即数**比较，结果进值寄存器——也是合成指令。

## 3. 编译期的字节码构造

`asCByteInstruction`（as_bytecode.h:176-200）是链表节点：

```cpp
asCByteInstruction *next, *prev;   // 双向链表
asEBCInstr op;
asQWORD arg[2];                    // 大参数（DW/QW/PTR）
short   wArg[3];                   // 最多三个 word 参数
int     size;                      // 最终尺寸（DWORD 数），来自 asBCTypeSize
int     stackInc;                  // 栈深变化，来自 asBCInfo（或专用发射函数覆写）
bool    marked; int stackSize;     // 供 PostProcess 路径分析用
```

节点从引擎的内存池分配（`AddInstruction()`，as_bytecode.cpp:1653-1674，`engine->memoryMgr.AllocByteInstruction()` + placement new）。

发射侧是一族按参数形状命名的函数：`Instr()`（无参，2550-2563）、`InstrW_W_W()`（2565-2581）、`InstrSHORT_DW()`（2709）、`InstrPTR()`（2828）……每个都以 `asASSERT(asBCInfo[bc].type == ...)` 校验 opcode 与所用函数匹配，`size/stackInc` 一律从表里抄——想传错参数形状在 Debug 版第一时间断言。栈变化量不定或需要附带元数据的指令有专用入口：`Call()/CallPtr()/Alloc()/Ret()/JmpP()`（1699-1778）。`JmpP()` 还把 switch 的最大 case 值存进 arg（1776-1777），这是 §5 路径分析能枚举跳表所有出口的前提。

**为什么先链表后 Finalize 成扁平数组**：

- 窥孔优化需要在任意位置 O(1) 地删除、改写、交换指令（`DeleteInstruction`/`InsertBefore`/`RemoveInstruction`，295-318、1955-1966）；
- 编译器需要回退试探性发射的指令（`GetLastInstr()/RemoveLastInstr()`，2915-2942）；
- 跳转目标在链表阶段用 `asBC_LABEL`（size=0 的伪指令，`Label()` 1780-1789）符号化表示，指令随便增删都不用重算偏移。

直到 `Finalize()`（as_bytecode.cpp:68-83）才按固定顺序收尾：

```cpp
PostProcess();            // 验证 + 死代码删除 + 栈深计算（§5）
Optimize();               // 全局窥孔（§4）
ResolveJumpAddresses();   // label id -> 相对偏移（1910-1952）
ExtractLineNumbers();     // LINE -> SUSPEND / 行号表（§6）
```

`ResolveJumpAddresses()` 对每条跳转调 `FindLabel()`（1858-1908）：先从跳转处向前线性扫描累加 `GetSize()`，找不到再向后扫——得到的直接就是相对字节偏移；`TryBlock` 则存**绝对**地址方便异常处理器（1940-1941）。之后 `Output()` 一次遍历按 §2.2 的布局写出扁平数组。

注意局部窥孔 `OptimizeLocally()` 不在 Finalize 里——它被 `asCCompiler` 在每个语句/表达式编译完后就地调用（as_compiler.cpp 中有 24 处调用点），让模式在被后续代码"稀释"之前就地匹配掉。

## 4. 窥孔优化走读

两个入口都受引擎属性 `asEP_OPTIMIZE_BYTECODE` 控制（as_bytecode.cpp:618、1115）。`OptimizeLocally()`（613-1107）**从尾往头**倒序扫描，头部注释解释了原因：一条指令被删掉后，为它准备操作数的前驱指令可能随之变成死代码，倒序天然支持这种级联。六个真实模式：

**模式 1：push+pop 消除**（as_bytecode.cpp:802-818）

```
PshVPtr y / PSF y / VAR y / PshNull / PshRPtr
PopPtr                                          -> （两条全删）
```
指针刚压栈就被弹掉——子表达式求值结果被丢弃时的典型残留。同族还有 `RDSPtr, PopPtr -> PopPtr`（788-792）。

**模式 2：常量折叠进指令**（446-510，位于 `RemoveUnusedValue()`）

```
SetV4 t, C          ->    CMPIi a, C        （t 为之后不再读的临时变量，456-459）
CMPi  a, t
SetV4 t, C          ->    ADDIi d, a, C     （477-484）
ADDi  d, a, t
```
覆盖 `CMPi/CMPf/CMPu` 与 `ADD/SUB/MUL` 的整型浮点共 9 种；当常量在左操作数时还会交换操作数（489-510，`wArg[1] = wArg[2]`——利用加法/乘法交换律）。

**模式 3：死值删除**（`RemoveUnusedValue`，424-444）：任何写临时变量的指令（`wW_*` 格式族），若该临时变量之后从未被读（`IsTempVarRead()`，1274 起——沿控制流向后扫描，遇跳转分叉两条路径都查），整条删除。判定"临时"靠编译器传入的 `tempVariableOffsets` 表（`IsTemporary()`，604-611）。

**模式 4：测试+跳转合并**（969-990）

```
TZ            ->    JNZ +x        （972-974，共 12 种组合）
JZ +x
```
"比较结果转 bool 再判断 bool"折成一次条件跳转。类似地 `ClrHi + JZ -> JLowZ`（697-713），而 `T??, ClrHi -> T??`（683-695）删掉冗余高位清零。

**模式 5：超级指令合成**

```
PshVPtr 0, ADDSi, PopRPtr  ->  LoadThisR      （917-925，this.member 地址一条搞定）
PshVPtr x, ADDSi, PopRPtr  ->  LoadRObjR      （928-941）
LDV x, INCi                ->  IncVi x        （717-723，i++ 专用）
PSF x, REFCPY              ->  RefCpyV x      （959-967）
PGA, RDSPtr                ->  PshGPtr        （884-890）
PshNull, RefCpyV, PopPtr   ->  FREE           （793-801，句柄赋 null 即释放）
```

**模式 6：全局 `Optimize()`**（1109-1205）负责需要整个函数视角的模式：`SUSPEND, SUSPEND -> SUSPEND`、`SUSPEND, JitEntry, SUSPEND -> SUSPEND`（1150-1171）；`LINE, LINE -> LINE`（1187-1192，语句无实际代码时）；`PopPtr, RET -> RET`（1141-1149，注释解释 RET 本身会恢复栈指针，PopPtr 多余）；**跳向紧邻 label 的 JMP 删除**（"JMP +0"，1201-1202：`curr->arg == instr->wArg[0]` 即目标就是下一条 label）；以及在引擎未开启 JIT 时整体剥除所有 `JitEntry`（1131-1135）。

改写用三个原语：`DeleteInstruction`/`ChangeFirstDeleteNext`/`DeleteFirstChangeNext`（266-293），每次改写后 `GoBack/GoForward` 把游标回退最多两步，让新暴露的相邻组合有机会再次匹配。辅助谓词 `IsTempRegUsed()`（1364，向后扫描确认值寄存器在被覆写前无人再读）保证合并不破坏寄存器语义。函数末尾还有一段专门优化"返回临时句柄"的 `LOADOBJ` 模式（1050-1106），消除 return 语句多余的 RefCpy+Free 对。

## 5. 栈大小与路径分析

`PostProcess()`（as_bytecode.cpp:2047-2159）在优化前做一次**静态控制流遍历**，注释明言三个目标：删除永不执行的代码、算出每条字节码位置的栈深、求最大栈深。

算法是经典 worklist：

1. 所有指令清 `marked/stackSize`（2060-2066），把第一条指令以栈深 0 入队（2070）。
2. `AddPath()`（250-264）是入队原语：若指令已标记，**断言两条路径到达时栈深一致**（第 255 行 `asASSERT(instr->stackSize == stackSize)`）——这是字节码平衡性的核心校验，编译器发射了不平衡的 push/pop 会在这里当场死掉。
3. 主循环（2073-2136）沿直线执行流累加每条指令的 `stackInc`，随手更新 `largestStackUsed`；遇 `JMP` 用 `FindLabel` 找目标入队后终止本路径；条件跳转把**两个后继**都入队（2107-2110）；`JMPP`（switch 跳表）把全部 max+1 个后继入队（2114-2128）——max 正是 `JmpP()` 发射时存进 arg 的最大 case 值。
4. 收尾扫描删除所有未标记（不可达）的指令（2138-2159，`asBC_Block` 除外，以保持作用域块配对），并断言可达指令的 `stackSize >= 0`（2155）。

最终 `outFunc->scriptData->stackNeeded = byteCode.largestStackUsed + variableSpace`（as_compiler.cpp:467）——VM 在 `Prepare` 时据此一次性预留栈空间，运行期不再逐指令检查栈溢出。`ExtractTryCatchInfo()`（1594-1614）也顺手把每个 try 块入口的 `stackSize` 记进异常表（1607），异常处理器展开时用它恢复栈指针。

## 6. 调试信息：行号如何嵌入

编译器在每个语句开头调用 `Line(line, column, scriptIdx)`（as_bytecode.cpp:1791-1809）发射伪指令 `asBC_LINE`：行号与列号打包进一个 DWORD——`(line & 0xFFFFF) | (column << 20)`（第 1804 行，行号 20 位、列号 12 位），第二个 DWORD 存脚本段索引（同一函数可能横跨多个 `AddScriptSection`）。它的 `size` 被预设为 `SUSPEND` 的尺寸或 0（取决于 `asEP_BUILD_WITHOUT_LINE_CUES`，1799-1802），使后续所有位置计算不受最终变换影响。

`ExtractLineNumbers()`（1472-1528）在 Finalize 末尾把 LINE 变现：

- 每条 LINE 往 `lineNumbers` 数组追加 `(字节码位置, 打包行列)` 对、往 `sectionIdxs` 追加段索引（1496-1499）；同一位置连续多条 LINE 只留最后一条（1489-1494）。
- 然后 LINE 本体**变身为 `asBC_SUSPEND`**（1514-1517）——所以行回调/挂起的粒度恰好是语句粒度；若引擎配置了 buildWithoutLineCues 则直接删除（1519-1523）。
- 函数第一条 LINE 总是被删（1501-1511），注释解释：`PrepareScriptFunction` 反正会先调一次行回调。

`asBC_Block/ObjInfo/VarDecl` 伪指令则由 `ExtractObjectVariableInfo()`（1530-1592）转成 `objVariableInfo` 表——异常处理器靠它知道抛异常那一刻哪些对象变量是活的、调试器靠 `declaredAtProgramPos`（1572）知道变量从何处可见。

## 7. 如果你要自己实现

- **栈机 vs 寄存器机**：AngelScript 给出务实的中间答案——传参走栈（天然变长、调用约定简单），算术走"栈帧偏移即寄存器"的三地址指令（省掉大量 push/pop 的解释器 dispatch 开销），比较结果走隐式值寄存器（省掉 bool 中转）。纯栈机指令更短但**条数更多**，而解释器的主要开销在每条指令的分发上，条数少更划算——Lua 5.0 从栈机转寄存器机是同一判断。
- **定长 vs 变长编码**：这里选择"以 DWORD 为单位的变长"（1-4 个 DWORD），opcode 固定在首字节，参数天然 32 位对齐——解码只是指针加法，没有字节级拆包。代价是无法随机定位第 N 条指令，所以跳转全部用相对偏移、行号表存的是预先算好的字节位置。
- **元数据表驱动**：`asBCInfo` 一张表同时服务发射断言、优化器的读写集分析（`GetVarsUsed`/`IsTempVarReadByInstr` 都按格式族分类处理，as_bytecode.cpp:110-152、1207-1235）、序列化和反汇编清单。自己实现时最先写的就应该是这张表，否则每加一条指令要同步改五个地方。
- **解释器友好性优先**：源码里大量 TODO 写着 "runtime optimize"（如 623-637），所有优化的度量标准都是运行期解释开销而非字节码尺寸；`JitEntry` 这种纯为 JIT 服务的空指令在不需要时被整体剥除。窥孔优化本质上是在**为解释器造 CISC 指令**——`LoadThisR`、`IncVi`、`CMPIi` 都是把高频序列固化成一次 dispatch。

## 8. 动手实验

`AS_DEBUG` 宏（文档见 as_config.h:65-67）**不会随 Debug 构建自动定义**（全库没有任何 `#define AS_DEBUG`），需要手动开启：

1. 在 `as_config.h` 加一行 `#define AS_DEBUG`（或加进编译器预处理定义），重新编译 angelscript 库；
2. 编译并运行 `sdk/samples/tutorial`；
3. 在**运行目录**下会出现 `AS_DEBUG/` 文件夹（`DebugOutput()` 开头 `_mkdir("AS_DEBUG")`，as_bytecode.cpp:2170），每个编译出的函数一个 `.txt`：全局函数如 `__main.txt`、类方法为 `__ClassName_method.txt`（命名逻辑在调用点 as_compiler.cpp:971-977）、全局变量初始化为 `___init_变量名.txt`（as_compiler.cpp:1606）。

清单内容（`DebugOutput()`，as_bytecode.cpp:2163 起）依次是：函数声明（2199）、临时变量偏移表（2201-2208）、变量表含 `(heap)` 标注（2210-2217），然后是逐指令清单——每行为 `位置 | 该点栈深(stackSize+variableSpace) | 可达标记* | 助记符 参数`（2236-2246），变量参数显示为 `v偏移`，语句边界插有 `- 行,列 -` 分隔线（2227-2232）。这是验证 §4 窥孔模式最直接的显微镜：对比 `engine->SetEngineProperty(asEP_OPTIMIZE_BYTECODE, false/true)` 前后同一函数的清单即可看到 `SetV4+ADDi` 如何变成 `ADDIi`。

补充两点：引擎属性 `asEP_NO_DEBUG_OUTPUT`（angelscript.h:186）可在保留 AS_DEBUG 编译的同时按引擎实例关闭文件输出（as_bytecode.cpp:2165）；`as_debug.h` 里还藏着一个 MSVC 专用的层级性能剖析器（`AS_PROFILE` + `TimeIt` 宏，as_debug.h:85-263，输出 `AS_DEBUG/profiling_summary.txt`），字节码各阶段遍布的 `TimeIt("asCByteCode::...")` 调用点就是为它准备的。

## 9. 扩展阅读

- Crafting Interpreters — Chunks of Bytecode：https://craftinginterpreters.com/chunks-of-bytecode.html （从零设计字节码格式、常量池与反汇编器，与本篇 §2/§3 直接对照）
- Wikipedia — Peephole optimization：https://en.wikipedia.org/wiki/Peephole_optimization
- Wikipedia — Stack machine：https://en.wikipedia.org/wiki/Stack_machine （与 register machine 的对比一节可对照 §7 的混合设计）
- 下一篇（10）：虚拟机 `asCContext::ExecuteNext`——本篇每条指令的运行期另一半。
