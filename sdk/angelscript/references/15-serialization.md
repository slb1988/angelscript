# 15 — 字节码序列化（asCWriter / asCReader）

> **概括**：as_restore.cpp（约 6000 行）实现模块的字节码保存/加载。`asCWriter` 把编译好的模块（类型、全局变量、函数、字节码）写成平台无关的二进制流；`asCReader` 在另一个（可能完全不同的）平台/进程里重建模块，并把流里的"表索引"重新绑定到当前引擎的真实指针上。它本质上是一个**链接器**：保存时做符号化（指针→名字/索引），加载时做重定位（名字/索引→指针）。
>
> **难度**：★★★★☆（单条路径都不难，难在字节码逐指令重写与平台差异调整两套"镜像逻辑"必须严格互逆）
>
> **前置**：09（字节码指令集与 asBCTYPE 指令格式）、10（asCScriptFunction 与调用栈布局）

---

## 1. 模块定位

- **入口**：`asIScriptModule::SaveByteCode(out, stripDebugInfo)` → `asCModule::SaveByteCode`（as_module.cpp:1619）构造 `asCWriter` 并调 `Write()`；`LoadByteCode` → `asCModule::LoadByteCode`（as_module.cpp:1638）先检查模块没有外部引用（否则返回 asMODULE_IS_IN_USE）、`RequestBuild` 拿编译权，然后 `asCReader::Read()`，成功后还会触发 `JITCompile()`（as_module.cpp:1664）。
- **典型用户**：sdk/samples/asbuild —— 一个离线编译器，把 .as 编成 .abc 字节码文件；配套的 asrun 在运行期加载执行。
- **发布模式的三个动机**：
  1. 目标环境可以定义 `AS_NO_COMPILER` 编译引擎，砍掉 parser/builder/compiler 的代码体积。注意 as_restore.h:159：`asCWriter` 整个类在 AS_NO_COMPILER 下不编译，`asCReader` 保留——只读不写；
  2. 不发行脚本源码（保护知识产权）；
  3. 跳过词法/语法/编译全流程，加速加载。
- 流接口极简：`asIBinaryStream` 只有 `Read/Write` 两个方法，用户自己决定落盘、压缩还是走网络。
- 类结构（as_restore.h:48-260）：两个类各自持有 module/stream/engine 指针、error 标志和 bytesRead/bytesWritten 计数器（报错时告诉你失败位置），加上 §2 的那些表。

## 2. 核心问题：内存里的字节码怎么落盘

编译产物 `asCScriptFunction::scriptData->byteCode` 是一串 asDWORD，其中大量操作数是**运行期指针或进程内 id**：

- `asBC_CALLSYS` 带引擎函数表里的函数 id；
- `asBC_OBJTYPE` / `asBC_REFCPY` 直接嵌一个 `asCObjectType*` 裸指针；
- `asBC_PGA` 嵌全局变量地址或字符串常量对象地址；
- `asBC_TYPEID` 带本进程分配的 typeId；
- `asBC_ADDSi` 带成员属性的字节偏移（随平台指针大小/对齐而变）。

这些值换个进程、换个平台全部作废。通用解法是几张"使用表"（as_restore.h:211-215 / 103-121）：

| 表 | 内容 | 写出形式 |
|---|---|---|
| `usedTypes` | `asCTypeInfo*` | 类型名+命名空间（模板实例递归展开子类型） |
| `usedTypeIds` | int typeId | 完整 asCDataType |
| `usedFunctions` | `asCScriptFunction*` | 来源标记+完整签名 |
| `usedGlobalProperties` | 变量地址 `void*` | 名字+命名空间+类型+模块/注册标记 |
| `usedStringConstants` | 字符串常量地址 | 原始字节 |
| `usedObjectProperties` | 类型+属性 | 所属类型+属性名 |

写出字节码时遇到指针就调 `FindXxxIdx`——查表没有就追加（如 `FindFunctionIndex`，as_restore.cpp:6000-6010；`FindTypeInfoIdx`，6025-6036；`FindStringConstantIndex` 用 map 加速，4192-4202）——把操作数替换成表下标；全部函数写完后，再把这些表以**名字+签名**的形式写到流尾部。加载方按签名在当前引擎里查回真实对象，最后一趟 `TranslateFunction` 把下标换回指针。查不到即报错——这就是"与宿主注册接口一致性"的校验点。

另一个维度是**平台无关性**，三件事：

- **字节序**：`WriteData`（3997-4012）在小端机上把字节倒序逐字节写出（`#if defined(AS_BIG_ENDIAN)` 则顺序写），`ReadData`（56-71）做镜像还原——即流内多字节标量统一为大端序。位标志用 `SAVE_TO_BIT/LOAD_FROM_BIT` 宏（47-49）做端序无关的位序列化。注：本代码库中**没有** SneakyWrite 之类的函数，端序处理就靠这两个函数加下面的变长整数编码。
- **变长整数**：几乎所有数值走 `WriteEncodedInt64`（4681-4748）：首字节最高位是符号位（负数取绝对值存），随后用前缀标记总长——`<2^6` 占 1 字节，`0x40+` 前缀 2 字节，`0x60+` 3 字节，`0x70+` 4 字节，`0x78+` 5 字节，`0x7C+` 6 字节，`0x7E+` 7 字节，`0x7F` 9 字节。`ReadEncodedUInt64`（2102-2174）按前缀还原；`ReadEncodedUInt`（2086-2095）还校验高 32 位必须为 0 或全 1，否则报无效字节码。
- **指针大小**：字节码在流里一律按"指针占 1 个 dword"的规范布局保存，与 `AS_PTR_SIZE`（32 位平台=1，64 位=2）解耦；加载时按当前平台重算所有栈偏移（§6）。dword 级的"程序位置"（跳转目标、行号表、objVariableInfo）也全部换算成**指令序号**存储，因为同一条指令的 dword 长度在不同指针宽度下不同。

## 3. 写出流程走读：asCWriter::Write（as_restore.cpp:4014-4190）

代码注释明说"Store everything in the same order that the builder parses scripts"。真实段落顺序：

| # | 段 | 行 | 内容 |
|---|---|---|---|
| 0 | stripDebugInfo 标志 | 4027 | 1 字节编码整数，注释强调"即使平台 bool 是 4 字节也只写 1 字节" |
| 1 | enums | 4030-4040 | 每个枚举 WriteTypeDeclaration phase1+phase2 |
| 2 | classTypes（仅名字） | 4043-4053 | 所有类/接口只写 phase1——先立空壳，供后续任何类型引用（前向引用在二进制层的对应物） |
| 3 | funcDefs | 4056-4063 | 每个 funcdef 走 WriteFunction |
| 4 | 接口方法 | 4066-4075 | 接口类型 phase2 |
| 5 | 类方法与行为 | 4078-4086 | 非接口类型 phase2 |
| 6 | 类属性 | 4089-4097 | 非接口类型 phase3 |
| 7 | typedefs | 4100-4110 | phase1+phase2 |
| 8 | scriptGlobals | 4113-4121 | WriteGlobalProperty：名字+命名空间+类型+初始化函数（4777-4787） |
| 9 | scriptFunctions | 4124-4135 | 所有 `objectType==0` 的模块函数（成员函数已在段 5 随类写出） |
| 10 | globalFunctions | 4138-4149 | 全局函数符号表——全是对段 9 的引用（'r'+index，见下） |
| 11 | bindInformations | 4152-4162 | import 函数签名 + 来源模块名 |
| 12 | usedTypes | 4165-4172 | WriteTypeInfo 逐个（4833-4904） |
| 13 | usedTypeIds | 4175 | WriteUsedTypeIds（5849-5860）：每个 typeId 存完整 asCDataType |
| 14 | usedFunctions | 4178 | WriteUsedFunctions（4222-4255） |
| 15 | usedGlobalProperties | 4181 | WriteUsedGlobalProps（5871-5903） |
| 16 | usedStringConstants | 4184 | WriteUsedStringConstants（4204-4220，经 stringFactory 取回原始字节） |
| 17 | usedObjectProperties | 4187 | WriteUsedObjectProps（5905-5917） |

**关键设计**：段 12-17 的表内容是在写段 1-11 时由 `WriteByteCode` 的 `FindXxxIdx` 惰性填充的——即文件布局上"引用先于定义"。读取方因此必须两阶段：先把字节码原样（带下标）读进内存，读完表后再统一翻译（§5 的 TranslateFunction）。

几个横切的去重机制：

- **字符串**：`WriteString`（4750-4775）第一次出现写 `len*2` + 原文并登记 stringToIdMap，之后写 `idx*2+1`——最低位区分新串/引用。`ReadString`（2202-2225）靠 savedStrings 数组还原。
- **函数对象**：`WriteFunction`（4358-4386）null 写 `'\0'`，重复引用写 `'r'`+savedFunctions 下标，新函数写 `'f'`+内容；`ReadFunction`（1205-1242）镜像处理。段 10 的 globalFunctions 因此全是 1 字节 'r' + 索引。
- **数据类型**：asCDataType 也有 savedDataTypes 去重表（WriteDataType，4800 起）。

`WriteFunction` 对脚本函数（4392-4526）依次写：bits 标志字节（1=shared、2=dontCleanUpOnException、4=external、8=有 objVariableInfo、16=有 tryCatch、32=explicit，4398-4408；external 时后续全免）→ `CalculateAdjustmentByPos`（4415）→ `WriteByteCode`（4417）→ variableSpace（4419-4420）→ objVariableInfo（4422-4432）→ tryCatchInfo（4434-4446）→ 未 strip 时的行号表与脚本段名表（4450-4480）→ **变量表**（4487-4503：即使 strip 也要存栈位置+类型，代码注释说明这是上下文序列化所需；strip 只省名字和声明位置；onHeap 标志挤进 stackOffset 的最低位）→ 脚本段名+declaredAt、参数名（均仅未 strip，4506-4525）。虚函数/接口方法只存 `vfTableIdx`（4527-4531），funcdef 只存 shared/external 位（4532-4539）。

`WriteFunctionSignature`（4257-4356）存：名字、返回类型、参数类型数组、inout 标志、funcType（模板函数 +128）、默认参数、objectType、只读/私有/保护/final/override/explicit/property/variadic 位标志字节、命名空间（或 funcdef 的父类）、模板子类型。这是加载时重绑定的全部依据。

`WriteTypeDeclaration` 的三个 phase（4542-4679）：phase1 = 名字+flags（8 字节 WriteData）+size+命名空间+external 标记（脚本类的 size 只写 1，"平台相关，读方重算"，4554-4559）；phase2 = 枚举值列表 / typedef 底层类型 / 类的基类、接口表+VFT 偏移、析构+构造/工厂对、methods[]、virtualFunctionTable[]；phase3 = properties[]（名字+类型+private/protected/inherited 位）。

## 4. 字节码指令的重写：WriteByteCode（as_restore.cpp:5210-5696）

先写指令**条数**而非 dword 长度（5215-5218，注释："长度不能存，因为它平台相关"）。逐条循环：把指令拷进 `tmpBC[4]` 临时缓冲（5227，最大指令 4 dword），先按**具体操作码**替换操作数，再按 **asBCTYPE 格式**调偏移、落盘。具体转换例子：

- **asBC_CALLSYS / CALL / CALLINTF / Thiscall1**（5301-5308）：DW_ARG 里的函数 id → `FindFunctionIndex(engine->scriptFunctions[id])`。加载侧 TranslateFunction 3025-3040 用 `FindFunction(idx)->id` 换回，索引越界或查不到 → `Error(TXT_INVALID_BYTECODE_d)`。
- **asBC_PGA / PshGPtr / LDG / PshG4 / LdGRdR4 / CpyGtoV4 / CpyVtoG4 / SetG4**（5327-5353）：PTR_ARG 里是裸地址。先查 `engine->varAddressMap` 判断指向全局变量还是字符串常量：全局变量存 `(index<<1)|1`，字符串常量存 `(index<<1)|0`（断言只有 PGA/PshGPtr 可能是字符串）。加载侧 3094-3128 按最低位分流查 `usedGlobalProperties` / `usedStringConstants`。
- **asBC_ALLOC**（5229-5241）：W_PTR_DW_ARG，`asCObjectType*` → `FindTypeInfoIdx`；构造函数 id 若非 0 则存 `1+FindFunctionIndex(...)`——0 保留表示"无构造函数"，读回时减 1（3047-3067）。
- **asBC_JMP/JZ/JNZ/JLowZ/JLowNZ/JS/JNS/JP/JNP**（5354-5374）：dword 偏移随平台变，借 `bytecodeNbrByPos` 换算成**指令条数差**存储；读回时按本平台各指令尺寸正向/反向累加还原 dword 偏移（3129-3155）。
- 其余散例：TYPEID/Cast 的 typeId → `FindTypeIdIdx`（5254-5259）；ADDSi/LoadThisR 的属性字节偏移 → `FindObjectPropIndex` + typeId 索引（5260-5268；FindObjectPropIndex 5919-5998 还要窥视后续指令区分复合属性）；JitEntry 的 JIT 入口指针直接清零（5249-5253）；COPY/RET 的字数清零留待加载方按本平台重算（5288-5300）；GETOBJ/GETOBJREF/GETREF/ChkNullS 走 AdjustGetOffset（5375-5382）；AllocMem/FREE/SetListSize/PshListElmnt/SetListType 五条列表初始化指令由 SListAdjuster 辅助类换算列表缓冲区内的元素偏移（5383-5434，读侧镜像 3156-3218）。

然后是与操作码无关的两个 switch，**这就是 asBCTYPE 与序列化的关系**——序列化器不必认识全部 200+ 条指令，只要认识 24 种操作数格式：

1. **变量偏移归一化**（5436-5478）：按 `asBCInfo[c].type` 判断哪些 WORD 字段是栈变量偏移（wW_ARG、rW_DW_ARG、wW_rW_rW_ARG、W_rW_ARG 等 13 种格式各自声明了 0-3 个变量操作数），逐个 `AdjustStackPosition` 压成"指针=1 dword"布局。
2. **落盘编码**（5484-5690）：同样按格式分派——操作码永远 1 字节；WORD 参数 `WriteEncodedInt64(short)`；DWORD 参数 `WriteEncodedInt64(int)`；QWORD 参数 `WriteEncodedInt64(qw)`。例如 wW_rW_DW_ARG = 1 字节操作码 + 两个编码 short + 一个编码 int。

数据表都在 angelscript.h：asBCTYPE 枚举（1682 附近）、`asBCTypeSize[24]`（1707）、`asSBCInfo asBCInfo[256]`（1767）。5480 的 TODO 承认非 IEEE754 平台的浮点尚未特殊处理（浮点按位模式经整数编码往返）。

`ReadByteCode`（2514-2779）是逐格式的镜像：读 1 字节操作码 → 按 `asBCTypeSize[asBCInfo[b].type]` 扩容（用平均指令长度预估总量，2537-2540）→ 按格式读回各参数放进 dword 流。此时操作数还是表下标，"活"的字节码要等 TranslateFunction。

## 5. 读入与重建：asCReader

`Read()`（73-123）：调 `ReadInner()`；失败则清空所有已加载函数的字节码并 `module->InternalReset()`（85-103，注释：防止半成品函数析构时释放从未增加过的引用）；成功则 `engine->PrepareEngine()`、按 `ep.initGlobalVarsAfterBuild` 决定是否立即 `ResetGlobalVars`、回传 `wasDebugInfoStripped`（106-115）。

`ReadInner()`（139-652）严格按 §3 的段落顺序读。要点：

- **shared 类型合并**：读到 shared 枚举/类时先在 `engine->sharedScriptTypes` 里按名字+命名空间+种类找原本，找到就丢弃新建对象、复用原本并登记进 `existingShared` / `dontTranslate`（170-216、243-295）；shared 脚本函数同理——按签名找到已有实现后把新函数替换掉并放进 dontTranslate（449-483，之后 TranslateFunction 2919 会跳过它们）。声明为 `external shared` 的实体若引擎里不存在原本，报 `TXT_EXTERNAL_SHARED_s_NOT_FOUND`（as_texts.h:126）并整体失败（191-199、486-493）。
- **防御**：所有 count 都过 `SanityCheck`（2176-2200）设上限（多为 1000000），对抗损坏/恶意流；`ep.disallowGlobalVars` 时读到全局变量段直接报错（424-428）。
- **usedTypes 重建**：`ReadTypeInfo`（2335-2512）按标记字节分派——'o' 普通类型先查模块再查引擎注册表，查不到报 `TXT_OBJECT_TYPE_s_DOESNT_EXIST`（2457）；'a' 模板实例现场调 `engine->GetTemplateInstanceType` **重新实例化**（2384），模板本体不存在报 `TXT_TEMPLATE_TYPE_s_DOESNT_EXIST`（2353）；模板回调验证推迟到全部读完后统一跑（583-616，失败报 `TXT_INSTANCING_INVLD_TMPL_TYPE_s_s`）；'l' 列表模式、's' 模板子类型、'c' 子 funcdef、`$obj`/`$func` 内建类型各有分支。
- **usedFunctions 重绑定**：`ReadUsedFunctions`（677-1047）对每项读来源字节：'n'=空、'm'=模块函数、's'=shared、'a'=应用注册。用读回的签名构造 dummy `asCScriptFunction`，在对应集合里做 `IsSignatureEqual` 匹配；应用侧还细分 `$beh0` 构造（模板类型要透过工厂桩里的 CALLSYS 找真构造，845-869）、`$fact`/`$beh3` 工厂、`$list` 列表工厂、`$beh2` 析构、`$dlgte` 委托工厂等隐藏名（835-939），以及模板函数的现场实例化（957-985、1004-1033）。**任何一项匹配不到 → `Error(TXT_INVALID_BYTECODE_d)`**（1040-1044）。
- **usedGlobalProps**：按名字+命名空间匹配且**类型必须完全相等**（2795-2833，2824 行 `globProp->type == type`）；**usedObjectProps** 按类型+属性名（2836-2874）；**字符串常量**要求引擎已注册 stringFactory，否则报 `TXT_STRINGS_NOT_RECOGNIZED`（663-667）。

`ReadFunction`（1205-1400+）读单个函数的细节也值得一看：'r' 引用直接查 savedFunctions（越界报错，1220-1231）；'f' 新函数先 `ReadFunctionSignature`，脚本函数若属于 external shared 类型则在原类型的方法里按签名找现成实现并加入 dontTranslate（1259-1285）；否则读 bits 字节 → `ReadByteCode` → variableSpace → objVariableInfo（**枚举值合法性校验**：option 必须是 asOBJ_INIT/asOBJ_UNINIT/asBLOCK_BEGIN/asBLOCK_END/asOBJ_VARDECL 之一，否则整体失败，1325-1334）→ tryCatchInfo → 调试信息。处处可见"不信任流"的姿态。

加载失败时的错误消息一览（均定义于 as_texts.h）：

| 消息 | 触发点 |
|---|---|
| `TXT_INVALID_BYTECODE_d`（363："LoadByteCode failed. The bytecode is invalid. Number of bytes read from stream: %d"） | 兜底：签名匹配失败、索引越界、SanityCheck 超限等 |
| `TXT_UNEXPECTED_END_OF_FILE`（300） | 流提前结束（ReadData 返回负值，68） |
| `TXT_OBJECT_TYPE_s_DOESNT_EXIST` | usedTypes 里的类型在当前引擎/模块找不到（2457、2498） |
| `TXT_TEMPLATE_TYPE_s_DOESNT_EXIST` / `TXT_TEMPLATE_SUBTYPE_s_DOESNT_EXIST` | 模板本体/子类型未注册（2353、2433） |
| `TXT_INSTANCING_INVLD_TMPL_TYPE_s_s` | 模板实例化被回调拒绝（605、2397） |
| `TXT_EXTERNAL_SHARED_s_NOT_FOUND`（126） | external shared 实体无原本（194、268、368、489） |
| `TXT_STRINGS_NOT_RECOGNIZED` | 有字符串常量但引擎没注册 stringFactory（665） |
| `TXT_GLOBAL_VARS_NOT_ALLOWED` | 引擎配置禁用全局变量（426） |

最终错误通常成对出现：一条具体原因 + 一条带 `bytesRead` 的兜底消息，后者能帮你定位失败在流的哪一段。

**TranslateFunction（2916-3333）——链接阶段**，对每个脚本函数与全局变量初始化函数各跑一遍（623-634）：

1. 预扫一遍算出每条指令的尺寸和"指令号→dword 位置"映射（2923-2941）；
2. 第一遍按操作码把所有下标换回指针/id/偏移：OBJTYPE 类换回 `asCObjectType*`（2947-2954）、CALL 类换回函数 id、PGA 类换回地址、跳转换回 dword 偏移、RET 按当前签名重算弹栈字数（3017-3024）、COPY 按当前 typeId 重算拷贝字数（2992-3016）；
3. `CalculateAdjustmentByPos`（3224）+ 第二遍按 asBCTYPE 展开所有变量偏移（3228-3276）；
4. 第三遍单独处理 GETOBJ/GETREF/GETOBJREF/ChkNullS（3296-3309，依赖第 2 步已把函数 id 换回）；
5. 调整 variables/objVariableInfo/tryCatchInfo/行号表的程序位置（指令号→dword）与栈偏移（3282-3330）；
6. `CalculateStackNeeded`（3332→3526-3684）：模拟执行流重算 `stackNeeded`——这个值根本不存流里。

## 6. 平台差异调整：CalculateAdjustmentByPos 详解

写出侧 `asCWriter::CalculateAdjustmentByPos`（4906-5018）建三张表：

- `adjustNegativeStackByPos`（4941-4950）：参数区（栈偏移为负）。this 指针、返回值指针（DoesReturnOnStack）、每个引用/对象参数在真实栈上占 AS_PTR_SIZE，规范布局按 1 算，逐位置累加 `1-AS_PTR_SIZE` 的差值；原始类型参数按 `GetSizeOnStackDWords` 原样。
- `adjustStackByPos`（4952-4998）：局部变量区（正偏移）。栈上分配的值类型对象保持 `GetSizeInMemoryDWords`，句柄和堆对象压成 1（4966-4976），做成前缀和表。
- `bytecodeNbrByPos`（5000-5017）：dword 位置→指令序号，供跳转/行号/objVariableInfo 换算；末位存指令总数（5013-5017 注释：函数可能不以 RET 结尾，如死循环）。

`AdjustStackPosition`（5020-5038）查表平移。读入侧（3687-3804）做**完全相反**的展开：参数区按当前 AS_PTR_SIZE 从 1 撑开，局部变量区依据函数**变量表**（这就是 strip 模式也必须存变量类型的原因之一）把值类型撑回内存尺寸。

**列表初始化缓冲区是另一处平台差异重灾区**：`{1, "a", obj}` 这类初始化列表被编译成一块由 `asBC_AllocMem` 分配的缓冲区，元素偏移取决于各元素类型的本平台尺寸与对齐。写侧 `asCWriter::SListAdjuster`（5698-5847）依照类型的列表模式（asSListPatternNode 链）把缓冲区内偏移换算成"规范偏移"；读侧 `asCReader::SListAdjuster`（3335-3524）按当前平台重算真实偏移，且缓冲区总大小干脆不存（写侧 AllocMem 的 DWORDARG 清零，5383-5386），等读侧遇到配对的 `asBC_FREE` 时由 `AdjustAllocMem`（3514）回填。

最微妙的是 `AdjustGetOffset`（读侧 3806-3952，写侧 5040 起镜像）：GETOBJ 类指令的偏移是相对"已为即将发生的调用压栈的参数"的，而参数里每个指针的宽度都在变。必须从当前指令向后扫描到 CALL/CALLSYS/ALLOC/CallPtr 等调用指令（3820-3852），拿到被调函数签名，数出目标偏移之上压了几个指针才能修正（3860 起）；途中靠 `asBCInfo[bc].stackInc` 累计栈变化。遇到 REFCPY/COPY 则简化处理（3839-3845）。

## 7. 版本兼容

**流里没有任何版本号或魔数**——`Write()` 的第一个字节就是 stripDebugInfo 标志（4027），整个 as_restore.cpp 中 "VERSION" 出现 0 次，读取侧也没有任何版本检查逻辑。官方政策（手册 "Pre-compiled byte code" 章）：字节码格式随库版本演进、**不保证跨版本兼容**，保存与加载必须用同一版本、同一套注册接口的引擎；需要版本管理的应用应自己在流外包一层版本头（存 `asGetLibraryVersion()`/`asGetLibraryOptions()` 是常见做法）。版本不匹配时旧字节码通常会因段结构错位触发 `TXT_INVALID_BYTECODE_d` 或 SanityCheck 失败，但这是概率性防御而非可靠检测。同理需要一致的还有引擎配置：注册的类型/函数全集、字符串工厂、`asEP_DISALLOW_GLOBAL_VARS` 等选项。

## 8. 关键代码走读

1. **as_restore.cpp:4014-4190 `asCWriter::Write`** —— 全部段落布局（§3 的表）；对照 139-652 `asCReader::ReadInner` 逐段镜像，尾部 621-650 是翻译与 AddReferences 收尾。
2. **as_restore.cpp:5210-5696 `WriteByteCode`** —— 指针符号化核心：按操作码换索引（5229-5434）→ 按 asBCTYPE 归一化偏移（5436-5478）→ 按格式编码落盘（5484-5690）。
3. **as_restore.cpp:2916-3333 `TranslateFunction`** —— 加载侧"链接器"：索引→指针、跳转/RET/COPY 重算、三遍偏移调整、CalculateStackNeeded。
4. **as_restore.cpp:677-1047 `ReadUsedFunctions`** —— 按签名重绑定与错配报错的确切位置（1040-1044）；配合 4257-4356 `WriteFunctionSignature` 看签名里到底存了什么。
5. **as_restore.cpp:4906-5018 与 3687-3804 两个 `CalculateAdjustmentByPos`** —— "指针=1 dword"规范化及其逆变换；3806 `AdjustGetOffset` 是其中最烧脑的一段。
6. **as_restore.cpp:3997-4012 `WriteData` / 56-71 `ReadData` / 4681-4748 `WriteEncodedInt64`** —— 端序统一（流内大端）与变长整数，整个格式的地基。

## 9. 如果你要自己实现

最小方案：

1. 流头写魔数 + 格式版本 + 库版本/配置指纹（AngelScript 没写，你应该写——这是它最值得商榷的设计决定）；
2. 所有引擎对象引用一律经"使用表"符号化，表项存**可再解析的名字+完整签名**而不是进程内 id；
3. 数值走变长编码并固定端序；操作码只存 1 字节；
4. 平台相关量（栈偏移、跳转 dword 距离、结构体成员偏移）要么存规范形式加载时重算（AngelScript 的做法），要么存足够类型信息现场重排。

两个陷阱：

- **自包含 vs 依赖宿主接口一致性**。AngelScript 选了后者：流里只有签名，语义完全依赖"加载方注册了同一套接口"。文件小、加载快，但兼容责任全在宿主——注册接口改一个参数类型，旧字节码全部作废，报错还很笼统。若想更自包含，可存接口清单哈希，加载时给出 diff 式的精确报错。
- **两套互逆逻辑的漂移**。WriteByteCode ↔ ReadByteCode+TranslateFunction、两个 CalculateAdjustmentByPos，是手工维护的镜像；新增一条带指针操作数的指令必须同时改两边。asBCTYPE 格式表兜得住"格式"层面，兜不住"语义"层面（哪个 DW_ARG 是函数 id，只有那串 if-else 知道）。表驱动（每个操作码声明各操作数语义）可以消除这类漂移。

## 10. 动手实验

1. **辨认段落**：编译 sdk/samples/asbuild，对一个含枚举、类、全局变量、字符串常量的脚本生成 .abc，hexdump 之。首字节是 stripDebugInfo（0x00/0x01），随后按 §3 顺序找：枚举名/类名以"长度*2 编码 + 明文"形式出现（WriteString）；文件尾部集中出现的明文是 usedFunctions 签名表和 usedStringConstants。对比 strip 与不 strip 两个版本的体积差，差值就是行号表+变量名+参数名。
2. **观察错配报错**：用注册了 `void print(const string &in)` 的宿主保存字节码，把注册签名改成 `void print(string)` 再加载——消息回调收到 "LoadByteCode failed. The bytecode is invalid. Number of bytes read from stream: N"；再试删掉一个被脚本用到的注册类型，会先收到 "Object type 'X' doesn't exist"。观察 N 与失败段落的对应关系。
3. **跨指针宽度往返**：32 位构建保存、64 位构建加载（或反之），验证正常运行；在 `asCReader::AdjustStackPosition`（3788）下断点，观察参数区偏移从规范值 1 撑开成 AS_PTR_SIZE 的过程。

4. **验证 strip 的代价**：对同一脚本分别以 `SaveByteCode(out, false)` 和 `SaveByteCode(out, true)` 保存，加载 strip 版后故意让脚本抛异常——`asIScriptContext::GetExceptionLineNumber` 将拿不到有效行号（行号表没存），但变量检视（GetVarCount/GetAddressOfVar）依然可用，因为变量的类型与栈位置即使 strip 也被保留（4483-4486 的注释解释了原因）。
5. **观察去重效果**：写一个把同一字符串字面量用 20 次的脚本，hexdump 里该字符串明文只出现一次（usedStringConstants 去重 + WriteString 的引用编码），后 19 处都是 1-2 字节的索引。

## 11. 扩展阅读

- 官方手册（angelcode.com/angelscript/sdk/docs/manual/）："Pre-compiled byte code"（doc_adv_precompile）——保存/加载须知与版本政策；asIBinaryStream 接口文档；"Porting the library" 中关于端序/指针尺寸的章节。
- sdk/samples/asbuild/source/main.cpp —— 离线编译器完整示例（含二进制流实现）。
- 维基百科 "Java class file format" 词条 —— 对比一个**自包含**的字节码格式：魔数 0xCAFEBABE、major/minor 版本号硬校验、常量池（AngelScript 的 used* 表正是常量池的亲戚，差别在 Java 把版本与验证做成了硬性规范，而 AngelScript 把一致性责任交给宿主）。
