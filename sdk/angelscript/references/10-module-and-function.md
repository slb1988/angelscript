# 10 — 模块与函数对象（asCModule / asCScriptFunction）

> **概括**：本篇精读 `as_module.h/.cpp`（约 2100 行）、`as_scriptfunction.h/.cpp`（约 2400 行）与 `as_globalproperty.cpp`（140 行）。`asCModule` 是编译产物的容器——脚本函数、全局变量、类/枚举/typedef/funcdef 都归它收纳并管理生命周期；`asCScriptFunction` 则是引擎中"可调用物"的统一表示，一个类同时扮演脚本函数、注册的 C++ 函数、接口占位、虚方法占位、委托、imported 函数等多种角色。读懂这两个类，就掌握了"编译器产出什么、VM 消费什么"的中间格式。
>
> **难度**：★★★☆☆
> **前置**：第 07 篇（编译器与字节码生成，理解 `scriptData->byteCode` 从何而来）、第 09 篇（asCContext 执行模型，理解函数对象如何被 `Prepare/Execute` 消费）。

---

## 1. 模块定位

- **模块 = 命名空间 + 生命周期单元**。宿主通过 `engine->GetModule("name", asGM_ALWAYS_CREATE)` 拿到 `asCModule`，往里 `AddScriptSection` 再 `Build`；编译产生的所有实体（函数、类、全局变量）都记录在这个模块的表里，模块 `Discard()` 时统一走向销毁。不同模块可以有同名函数/类而互不干扰——跨模块协作要么走 `import` 绑定（第 5 节），要么用 `shared` 实体，要么由宿主拿着 `asIScriptFunction*` 手工桥接。
- **函数对象 = 字节码 + 元数据的载体**。`asCScriptFunction` 持有签名（名字、返回类型、参数表、traits）、所属模块/对象类型，以及仅脚本函数才有的 `scriptData`（字节码、局部变量表、行号表、try/catch 表）。VM 执行、调试器取行号、反射 API `GetDeclaration()`，读的都是同一个对象。
- **所有权倒挂的一点提醒**：函数虽然"属于"模块，但同时注册在引擎的全局数组 `engine->scriptFunctions[id]` 里（`as_module.cpp:1285` 的 `m_engine->AddScriptFunction(func)`），字节码里的调用指令存的是函数 id，运行时经引擎数组间接寻址——模块只是引用计数意义上的属主，寻址权在引擎。

## 2. asCModule 内部：一堆表

`as_module.h:215-253` 列出了全部成员，注释明确标注了每张表是否增加引用计数：

| 成员 | 内容 | 引用计数 |
|---|---|---|
| `m_scriptFunctions`（as_module.h:224） | 模块编译出的**所有**函数：全局函数、类方法、工厂、全局变量初始化函数… | 增加（internal ref） |
| `m_globalFunctions`（as_module.h:227） | 仅全局函数，`asCSymbolTable` 按 (命名空间, 名字) 索引，服务 `GetFunctionByName/Decl` | 不增加（指针同时在上表中） |
| `m_bindInformations`（as_module.h:229） | imported 函数的 `sBindInfo*`（签名 + 来源模块名 + 已绑定函数 id） | 增加 |
| `m_templateInstances`（as_module.h:231） | 本模块触发实例化的模板类型（如 `array<MyClass>`） | 增加 |
| `m_scriptGlobals`（as_module.h:234） | 脚本声明的全局变量 `asCGlobalProperty`，符号表 | 增加 |
| `m_classTypes / m_enumTypes / m_typeDefs / m_funcDefs`（as_module.h:238-244） | 类与接口 / 枚举 / typedef / funcdef | 增加 |
| `m_typeLookup`（as_module.h:248） | 上述所有类型的 (ns, name) → `asCTypeInfo*` 快查 map | 不增加 |
| `m_externalTypes / m_externalFunctions`（as_module.h:251-253） | 脚本里 `external` 显式声明引入的共享实体 | 不增加 |

类型进表统一走 `AddClassType/AddEnumType/AddTypeDef/AddFuncDef`（as_module.cpp:1514-1541），每个都同时往 `m_typeLookup` 里插一份；头文件注释（as_module.h:204-211）强调这些方法"接管引用而不增加计数"。

### 2.1 Build 与 AddScriptSection 的模块侧

- `AddScriptSection`（as_module.cpp:229-247）只做一件事：惰性创建 `asCBuilder` 并把源码交给 `builder->AddCode` 暂存（是否拷贝源码由引擎属性 `copyScriptSections` 决定）。真正的工作全在 `Build` 里。
- `Build()`（as_module.cpp:260-334）流程：
  1. `HasExternalReferences(false)` 检查——外部还攥着上一版函数的引用就拒绝重编，返回 `asMODULE_IS_IN_USE`（as_module.cpp:270-274）；
  2. `RequestBuild()` 独占编译锁（同一时刻只允许一个线程 Build，as_module.cpp:278）；
  3. `InternalReset()` 清空旧产物，然后 `m_builder->Build()` 干活，**编译完立即删掉 builder**（as_module.cpp:299-301）——编译器是一次性工具，不常驻；
  4. 成功后 `JITCompile()`（as_module.cpp:250-257，遍历每个函数调 `func->JITCompile()`）；
  5. 若引擎属性 `initGlobalVarsAfterBuild` 为真则顺手 `ResetGlobalVars(0)`（as_module.cpp:329-330）。
- 增量入口 `CompileFunction`（as_module.cpp:1740-1802）与 `CompileGlobalVar`（as_module.cpp:1682-1737）允许运行期往已 Build 的模块里塞单个函数/变量：临时 builder 编译单元，`asCOMP_ADD_TO_MODULE` 标志决定函数是否入模块表；不入表的函数由返回给宿主的外部引用独活（`Release()` 里对应 `module == 0` 的断言路径，as_scriptfunction.cpp:526-533）。反向的 `RemoveFunction`（as_module.cpp:1805-1819）从两张函数表里摘除并 `ReleaseInternal`。

### 2.2 全局变量的存储与初始化

`asCGlobalProperty`（as_globalproperty.cpp）自带存储：

- 小于等于 2 个 DWORD 的值直接放在成员 `storage` 里（构造函数里 `memory = &storage`，as_globalproperty.cpp:44-45）；更大的在 `AllocateMemory()` 里单独 `asNEWARRAY`（as_globalproperty.cpp:95-107）。每个变量独立分配，因此可以随时增删单个变量而不搬迁其它变量（源码注释原话，as_globalproperty.cpp:91-94）。
- 宿主注册的全局属性走 `SetRegisteredAddress`（as_globalproperty.cpp:109-119）：对象类型时 `memory` 是"指向指针的指针"。
- 每个带初始化表达式的脚本全局变量，编译器会为它生成一个**独立的初始化函数**，通过 `SetInitFunc` 挂上（as_globalproperty.cpp:126-133，持 internal ref）；`~asCGlobalProperty` 与 `DestroyInternal` 负责释放它（as_globalproperty.cpp:54-84）。

初始化的执行侧在模块：

- `ResetGlobalVars`（as_module.cpp:337-343）= 已初始化则先 `CallExit()` 反初始化，再 `CallInit(ctx)`。
- `CallInit`（as_module.cpp:352-403）分两趟：第一趟遍历符号表把每个变量的内存 `memset` 清零（as_module.cpp:358-364）；第二趟对有 init 函数的变量调用 `InitGlobalProp`——后者就是普通的 `ctx->Prepare(initFunc); ctx->Execute()`（as_module.cpp:421-424），宿主没传 ctx 就向引擎 `RequestContext()` 借一个（as_module.cpp:376-381）。
- 任何一个初始化抛异常，整个流程停止并返回 `asINIT_GLOBAL_VARS_FAILED`，异常函数与行号经 `WriteMessage` 报给宿主（as_module.cpp:427-452）。
- 反向的 `UninitializeGlobalProp`（as_module.cpp:476-515）按类型收尾：引用类型调 release 行为、值类型调析构再 `CallFree`、funcdef 变量 `Release()` 函数指针，并把槽位清零防悬空访问（as_module.cpp:502-503）。`CallExit`（as_module.cpp:518-530）逐个调用它并复位 `m_isGlobalVarInitialized`。

### 2.3 Discard 的延迟销毁语义

`Discard()`（as_module.cpp:100-135）**不是 delete**：

1. 先 `CallExit()` 反初始化全局变量——否则全局变量里的对象会永远拖住模块（as_module.cpp:102-105 的注释同时警告：清理期间还有活对象访问这些全局变量会得到脚本异常）；
2. 把自己从 `engine->scriptModules` 挪进 `engine->discardedModules`（as_module.cpp:115-120），从此对宿主 API 不可见，但内存还在；
3. 触发一次 `GarbageCollect()` 或 `DeleteDiscardedModules()` 尝试立即清理（as_module.cpp:124-134）。

真正的删除发生在 `asCScriptEngine::DeleteDiscardedModules`（as_scriptengine.cpp:774-810）：逐个检查 `HasExternalReferences(shuttingDown)`（as_module.cpp:533-654）。该函数扫描全局变量 init 函数、脚本函数、类、funcdef、模板实例的 **externalRefCount**——只要宿主或 GC 管理的对象还持有任何一个实体的外部引用，模块就继续留在弃置堆里；shared 实体若能经 `FindNewOwnerForSharedFunc/Type` 移交给别的模块则不算数（as_module.cpp:567-568）。引擎关闭时（`shuttingDown=true`）不再等待，而是打印"There is an external reference to..."警告后强拆。这就是"Discard 之后要等 GC 转完才真正释放"的根源。

析构函数 `~asCModule`（as_module.cpp:65-97）调用 `InternalReset()`（as_module.cpp:657-837）按固定顺序拆表：

1. 全局变量先 `DestroyInternal` 但**暂不释放**——函数还需要经引擎的 `varAddressMap` 找到它们，提前移除会导致引用计数无法归零而泄漏（源码注释，as_module.cpp:666-668）；
2. `UnbindAllImportedFunctions()` 并释放 `sBindInfo`（as_module.cpp:676-688）;
3. 释放模板实例 → 类 → 枚举 → typedef → funcdef（shared 的先尝试移交所有权，as_module.cpp:691-798）；
4. 释放函数（as_module.cpp:801-819）；
5. 最后才从引擎移除并 `Release` 全局变量（as_module.cpp:821-830），收尾 `asASSERT( IsEmpty() )`。

## 3. asCScriptFunction 的多面性

`asEFuncType`（angelscript.h:390-400）共 8 个值，同一个类按 `funcType` 分饰多角，各角色只用自己那几个成员：

- **asFUNC_SCRIPT**：唯一持有 `scriptData` 的角色（构造函数 as_scriptfunction.cpp:379-380 按类型分配，`AllocateScriptFunctionData` 在 as_scriptfunction.cpp:387-398），字节码在 `scriptData->byteCode`。
- **asFUNC_SYSTEM**：宿主注册的 C++ 函数，**没有字节码**，靠 `sysFuncIntf`（as_scriptfunction.h:396，`asSSystemFunctionInterface*`，记录原生函数地址、调用约定、参数封送信息，第 12 篇细讲）。系统函数天然 shared（as_scriptfunction.cpp:1838）。
- **asFUNC_INTERFACE**：接口方法声明，无字节码。调用时靠 **signatureId** 在实际对象的方法表里线性匹配同签名方法（as_context.cpp:1643-1656）。
- **asFUNC_VIRTUAL**：类虚方法的**占位符**，只有 `vfTableIdx`（as_scriptfunction.h:393）有意义；运行时用对象的 `objType->virtualFunctionTable[vfTableIdx]` 查出真正的 `asFUNC_SCRIPT` 实现（as_context.cpp:1634-1640）。也就是说类的"方法列表"里放的是占位，真实现另有其人。
- **asFUNC_FUNCDEF**：函数签名类型（函数指针的"类型"），本体挂在 `asCFuncdefType` 上（`funcdefType` 反向指针在 as_scriptfunction.h:344，不计数）。
- **asFUNC_IMPORTED**：`import` 声明产生的签名壳，见第 5 节。
- **asFUNC_DELEGATE**：**持有对象**的可调用体，`funcForDelegate` + `objForDelegate` 二元组（as_scriptfunction.h:346-348）。`CreateDelegate`（as_scriptfunction.cpp:172-187）创建它——不分配函数 id、不进 `engine->scriptFunctions`；`MakeDelegate`（as_scriptfunction.cpp:190-208）对函数和对象各加一个引用，把签名从被委托方法浅拷过来，并设 `dontCleanUpOnException = true`（委托不拥有参数，异常清理时不能替真函数擦屁股，as_scriptfunction.cpp:205-207）。
- **asFUNC_DUMMY**：栈上临时对象专用（如 `GetFunctionByDecl` 里解析声明串时，as_module.cpp:930），不参与引用计数，析构有专门断言豁免（as_scriptfunction.cpp:416-417）。

另有 **asFUNC_TEMPLATE**（模板函数本体，`templateSubTypes` 成员），实例化后生成普通 `asFUNC_SCRIPT`。

按角色速查各成员的有效性：

| funcType | 有字节码 | 关键专属成员 | 进 engine->scriptFunctions | 进 GC |
|---|---|---|---|---|
| SCRIPT | 有 | `scriptData` | 是 | 否 |
| SYSTEM | 无 | `sysFuncIntf` | 是 | 否 |
| INTERFACE | 无 | `signatureId`（匹配用） | 是 | 否 |
| VIRTUAL | 无 | `vfTableIdx` | 是 | 否 |
| FUNCDEF | 无 | `funcdefType` | 是 | 否 |
| IMPORTED | 无 | id 带 `FUNC_IMPORTED` 位 | 否（进 `importedFunctions`） | 否 |
| DELEGATE | 无 | `objForDelegate/funcForDelegate` | 否 | 是 |
| DUMMY | 无 | —（栈上临时） | 否 | 否 |

签名之外的修饰信息集中在 `asSFunctionTraits`（as_scriptfunction.h:103-127）：一个 asDWORD 位集，涵盖 `asTRAIT_CONSTRUCTOR/DESTRUCTOR/CONST/PRIVATE/PROTECTED/FINAL/OVERRIDE/SHARED/EXTERNAL/EXPLICIT/PROPERTY/DELETED/VARIADIC` 十三个标志位，`IsReadOnly()/IsFinal()/IsShared()` 等公开查询都是对它的一行封装（如 as_scriptfunction.cpp:1852-1879）。

一个隐蔽细节：模块加入一个**已编译的 shared 脚本函数**时（`AddScriptFunction(asCScriptFunction*)` 重载，as_module.cpp:1299-1333），会顺带扫描其字节码里的 `asBC_FuncPtr` 指令，把名字以 `$` 开头的**匿名函数**（lambda）一并收编进本模块——匿名函数是 shared 函数的隐式附属物，不收编就会在原模块销毁时被误删。

### 3.1 双引用计数：external 与 internal

`asCScriptFunction` 有**两个**计数器（as_scriptfunction.h:314-315）：

- `externalRefCount`：宿主与脚本世界的引用（`AddRef/Release`，as_scriptfunction.cpp:510-537）；
- `internalRefCount`：引擎内部结构的引用——模块的表、其它函数字节码里的交叉引用等（`AddRefInternal/ReleaseInternal`，as_scriptfunction.cpp:540-561）。

两个都归零才 `asDELETE`。分开的意义正是 `HasExternalReferences` 那套判断：模块能区分"我自己内部的循环引用"（可以整体拆除）和"外面还有人在用"（必须等待）。`Release()` 里有一条断言：外部归零且内部为零时 `module` 必为 0（as_scriptfunction.cpp:526-530）——即只有脱离模块管辖的函数（动态编译且未入模块）才会由外部 Release 直接触发删除。

函数还通过 `AddReferences/ReleaseReferences`（as_scriptfunction.cpp:1159-1321 / 1324 起）主动持有它用到的一切资源：扫描自己的字节码，对 `asBC_OBJTYPE/FREE/REFCPY/ALLOC` 引用的类型、`asBC_PGA/LDG/SetG4` 等指令引用的全局变量（经 `GetPropertyByGlobalVarPtr` 反查，as_scriptfunction.cpp:1780-1789）、`asBC_CALL/CALLINTF` 引用的函数逐一加内部引用；字符串常量还会向 string factory 重新申请一份以维持正确的资源归属（as_scriptfunction.cpp:1250-1264）。`DestroyInternal`（as_scriptfunction.cpp:453-501）先 `ReleaseReferences()` 切断出边再清空自身——这是引用计数系统能拆掉环的关键次序。

### 3.2 为什么函数对象要参与 GC

脚本里的函数指针/委托可以构成循环：funcdef 变量指着一个委托，委托的 `objForDelegate` 是个脚本对象，对象的成员又存着这个 funcdef 变量。引擎启动时把内部类型 `$func` 注册为 `asOBJ_REF | asOBJ_GC` 并挂上全套 GC 行为（`RegisterScriptFunction`，as_scriptfunction.cpp:122-137）；同一个函数里还注册了名为 `DELEGATE_FACTORY`（`"$dlgte"`，故意取非法标识符防脚本直呼其名）的委托工厂（as_scriptfunction.cpp:151-169）。

但只有**委托**真的进 GC：构造函数末尾 `if( funcType == asFUNC_DELEGATE ) engine->gc.AddScriptObjectToGC(this, &engine->functionBehaviours)`（as_scriptfunction.cpp:382-384）；GC 回调 `GetRefCount/SetFlag/GetFlag/EnumReferences/ReleaseAllHandles`（as_scriptfunction.cpp:1792-1832）里都有 `asASSERT( funcType == asFUNC_DELEGATE )`，枚举的唯一出边就是 `objForDelegate`。委托的计数初值也反着来：`externalRefCount=1, internalRefCount=0`（as_scriptfunction.cpp:349-354）——它像对象实例，不像模块拥有的代码。

### 3.3 signatureId 与 GetDeclarationStr

- `ComputeSignatureId`（as_scriptfunction.cpp:1090-1110）在引擎的 `signatureIds` 表里找签名相同的第一个函数并共享其 id；比较用 `IsSignatureEqual`（名字 + 返回类型 + 参数，**刻意不含所属类型**，as_scriptfunction.cpp:1092-1095 注释），这样类方法与它实现的接口方法 `signatureId` 相等，`asBC_CALLINTF` 只需比较一个 int。
- `GetDeclarationStr`（as_scriptfunction.cpp:683-841）是反射 API 的核心：拼出 `retType ns::Class::name(params...) const` 形式的声明串。细节包括：构造/析构（内部名 `$beh0/$beh2`）不打返回类型（as_scriptfunction.cpp:690-693）、`$behN` 还原成 `Type(...)`/`~Type(...)`（as_scriptfunction.cpp:716-726）、模板函数打 `<subtypes>`、引用参数附 `in/out/inout`、可选带参数名与默认值、末尾附加列表模式 `{ repeat T }`（as_scriptfunction.cpp:803-838）。它同时也是 import 绑定时跨模块查找用的"签名序列化格式"。

## 4. 调试信息：变量表与行号表

都在 `ScriptFunctionData`（as_scriptfunction.h:354-386）里，`SaveByteCode(out, stripDebugInfo=true)` 剥掉的就是这些：

- **变量表 `variables`**：`asSScriptVariable`（as_scriptfunction.h:59-67）= 名字 + 类型 + 栈偏移 + 是否在堆上 + 声明处字节码位置。编译器经 `AddVariable`（as_scriptfunction.cpp:1058-1073）填入，显式变量和临时变量都收录（as_scriptfunction.h:374-375 注释：服务异常处理、字节码序列化、调试、上下文序列化四类客户）。调试器的 `ctx->GetVarCount/GetVar` 与函数级 `GetVar/GetVarDecl`（as_scriptfunction.cpp:1029-1055）都读它。
- **objVariableInfo**：`asSObjectVariableInfo`（as_scriptfunction.h:129-134）= (程序位置, 变量偏移, 事件)，事件枚举 `asOBJ_UNINIT/asOBJ_INIT/asBLOCK_BEGIN/asBLOCK_END/asOBJ_VARDECL`（as_scriptfunction.h:94-101）——记录每个对象变量在哪段字节码区间内"活着"，异常展开时据此决定要不要析构。
- **行号表 `lineNumbers`**：扁平 int 数组，**成对**存放 `(字节码位置, 行列打包值)`，行占低 20 位、列占高 12 位（as_scriptfunction.h:379-382）。它由 `asCByteCode::ExtractLineNumbers`（as_bytecode.cpp:1472-1499）在编译收尾时从 `asBC_LINE` 伪指令中提取——LINE 指令随后转成 SUSPEND 或直接删除，同一位置连续两条 LINE 只留最后一条（as_bytecode.cpp:1489-1494）。
- **行号查询**：`GetLineNumber(programPosition)`（as_scriptfunction.cpp:963-1012）对成对数组做手写二分，返回小于等于指定位置的最近记录——`ctx->GetLineNumber()`、异常报告、asrun 调试器断点判定的行号都出自这里。函数由多个 script section 拼成时，`sectionIdxs` 的 (位置, 节索引) 对参与换算（as_scriptfunction.cpp:971-980）。公开接口 `GetLineEntryCount/GetLineEntry`（as_scriptfunction.cpp:897-945）把整张表暴露给宿主工具，还能顺带取到对应字节码地址（给 JIT/反汇编器用）。
- **tryCatchInfo**：`asSTryCatchInfo`（as_scriptfunction.h:136-141）= try 起点 / catch 入口 / 栈大小，异常处理用。
- **声明位置**：`declaredAt` 同样是行 20 位 + 列 12 位打包（as_scriptfunction.h:382-383），`GetDeclaredAt`（as_scriptfunction.cpp:948-961）解包。

## 5. import 机制：跨模块调用的另一条路

脚本里写 `import void Foo(int) from "OtherModule";`，编译器为它调用 `AddImportedFunction`（as_module.cpp:1336-1381）：创建一个 `asFUNC_IMPORTED` 的签名壳函数，包进 `sBindInfo`（as_module.h:65-71：签名 + 来源模块名 + `boundFunctionId`，初值 -1），同时登记到引擎的 `importedFunctions` 数组。imported 函数的 id 带 `FUNC_IMPORTED = 0x40000000` 标志位（as_module.h:54）以区别于普通函数 id（`GetNextImportedFunctionId`，as_module.cpp:1225-1232）。

绑定有两条 API：

- `BindImportedFunction(index, func)`（as_module.cpp:1391-1421）：手工把签名匹配的函数填进 `boundFunctionId`。只允许 `asFUNC_SCRIPT/asFUNC_SYSTEM`（类方法、委托、别的 imported 函数都不行，as_module.cpp:1404-1407），用 `IsSignatureExceptNameEqual` 校验签名，绑定成功对目标 `AddRefInternal`。
- `BindAllImportedFunctions()`（as_module.cpp:1466-1501）：自动版——对每个 import 用 `GetDeclarationStr(false, true)` 生成声明串，到 `import from` 指定的模块里 `GetFunctionByDecl` 查找并绑定；有任何一个没绑上就返回 `asCANT_BIND_ALL_FUNCTIONS`。

运行时字节码用专门的 `asBC_CALLBND` 指令调用（as_context.cpp:3049-3078）：**每次调用**都实时读 `importedFunctions[i & ~FUNC_IMPORTED]->boundFunctionId`，为 -1 就抛 "Unbound function called" 脚本异常。这个间接层意味着可以随时 `UnbindImportedFunction`（as_module.cpp:1424-1441）换绑——事件分发/热替换场景（丢弃旧模块 → 编译新模块 → 重新 Bind）就靠这条路。头文件里两处 TODO（as_module.h:53-54、80-81）表明作者视其为终将被 funcdef 函数指针取代的历史机制，但它至今仍是官方支持的跨模块晚绑定手段。

## 6. 关键代码走读

1. **as_module.cpp:352-403 `CallInit`**——先统一 memset 清零再逐个跑 init 函数的两趟设计，保证"变量 A 的初始化表达式引用变量 B"时 B 至少是零值而非垃圾。失败也要置 `m_isGlobalVarInitialized = true`（as_module.cpp:393-397 注释）：否则已成功初始化的那部分变量将无人反初始化而泄漏。
2. **as_module.cpp:100-135 `Discard`**——注意第 107-110 行先把 `m_engine` 拷到局部变量：模块一旦进入 discarded 堆，别的线程可能抢先删掉它，之后不能再碰任何成员。
3. **as_module.cpp:801-819 `InternalReset` 释放函数段**——shared 函数先 `FindNewOwnerForSharedFunc` 尝试移交所有权，移交成功只 `ReleaseInternal`；否则 `DestroyInternal()` 切断它对其它资源的引用（打破环）再释放。"先拆边、再放点"是引用计数系统能拆掉模块内循环引用的关键次序。
4. **as_scriptfunction.cpp:346-385 构造函数**——按 `funcType` 决定初始计数方向（委托 external=1，其余 internal=1）、是否分配 `scriptData`、是否注册进 GC。一个函数看清"多面性"如何落地。
5. **as_scriptfunction.cpp:963-1012 `GetLineNumber`**——对 (位置, 行号) 成对数组的手写二分，返回"小于等于 programPosition 的最近一条记录"的行号；配合 as_bytecode.cpp:1487-1499 的提取与去重逻辑一起读。
6. **as_context.cpp:1634-1656 虚方法/接口方法解析**——`asFUNC_VIRTUAL` 走 `virtualFunctionTable[vfTableIdx]` O(1) 查表；`asFUNC_INTERFACE` 线性扫对象方法表比对 `signatureId`。两种占位函数在这里兑现成真正的脚本函数；同文件 `CallInterfaceMethod`（as_context.cpp:2171）与 `asBC_CALLBND`（as_context.cpp:3049）是三条"晚绑定调用"路径的汇合处。

## 7. 如果你要自己实现

- **模块作为编译单元的边界**：把"编译产物容器"和"编译器"分开（asCModule 持有 asCBuilder 只到 Build 结束就删，as_module.cpp:299-301），容器只剩表和生命周期逻辑，编译器可整体裁掉（`AS_NO_COMPILER` 下模块照常工作、只能 LoadByteCode）。重编译 = 拒绝（还有外部引用时）或全量 reset 后重来；AngelScript 没做增量替换（as_module.h:90-94 的 TODO 承认了这点），你若要做，"调用指令存函数 id 而非指针、经引擎数组间接寻址"是现成的钩子。
- **延迟销毁优于立即销毁**：Discard → discarded 堆 → 无外部引用时才删，把"宿主还攥着函数指针"从崩溃问题降级为内存暂留问题。代价是需要内/外两套引用计数来区分环内引用和真实使用者。
- **函数元数据最小集合**：名字 + 返回类型 + 参数类型/inout 标志 + 所属对象类型 + funcType 判别字段。这五样是调用检查与签名匹配的底线；字节码、变量表、行号表都应设计成可剥离的附件（AngelScript 用可空的 `scriptData` 指针实现，系统函数干脆没有；stripDebugInfo 序列化选项则剥掉调试三件套）。
- **用一个类分饰多角还是类层次**？AngelScript 选了前者（union 风格的按需成员），省掉虚表且方便统一放进 `engine->scriptFunctions[id]` 数组，代价是满代码的 `if (funcType == ...)` 和一堆"仅某角色有效"的成员。如果角色超过这里的 8 种，类层次可能更干净。

## 8. 动手实验

1. **跨模块手工桥接 + import**：建两个模块 A、B 各自 `Build` 一段脚本；宿主用 `modA->GetFunctionByDecl("int add(int,int)")`（as_module.cpp:923-986，注意它会沿命名空间逐层向上找、遇到多个匹配返回 0）拿到函数，`ctx->Prepare(func); ctx->Execute()` 直接调用——体会"函数对象一旦到手，与它属于哪个模块无关"。再在 B 里写 `import int add(int,int) from "A";`，`modB->BindAllImportedFunctions()` 后从 B 的脚本里调它；然后 `UnbindImportedFunction(0)` 再调一次，观察 "Unbound function called" 异常。
2. **观察 Discard 的延迟释放**：编译一个模块，宿主对某个函数 `AddRef()` 留住外部引用，然后 `mod->Discard()`。断点设在 `~asCModule`（as_module.cpp:65），确认析构没有发生；再 `func->Release()` 并调用 `engine->GarbageCollect()`，这次 `DeleteDiscardedModules`（as_scriptengine.cpp:774）里 `HasExternalReferences` 返回 false，模块才真正析构。
3. **委托环**：脚本里声明 `funcdef void CB(); class T { CB@ cb; void f(){} }`，让 `t.cb = CB(t.f);` 形成 T → delegate → T 的环，Release 掉所有句柄后调用 `engine->GarbageCollect()`，用 `GetGCStatistics` 观察委托对象（`$func` 类型）被环检测回收；断点 `asCScriptFunction::EnumReferences`（as_scriptfunction.cpp:1812）看 GC 如何枚举 `objForDelegate`。

## 9. 扩展阅读

- 官方手册 · 模块使用：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_module.html
- 官方手册 · import 与动态绑定：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_adv_import.html
- 官方手册 · `asIScriptModule` 接口参考：https://www.angelcode.com/angelscript/sdk/docs/manual/classas_i_script_module.html
- 官方手册 · `asIScriptFunction` 接口参考：https://www.angelcode.com/angelscript/sdk/docs/manual/classas_i_script_function.html
- 官方手册 · 函数指针与委托：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_datatypes_funcptr.html
- 官方手册 · 调试脚本（行号/变量检查 API 的使用方）：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_debug.html
