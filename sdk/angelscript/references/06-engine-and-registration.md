# 06 — 引擎中枢与注册 API（asCScriptEngine）

> 一句话概括：本篇精读 `as_scriptengine.cpp` 中"引擎作为万物之根"的一面——它如何持有所有类型/函数/属性，以及 `Register*` 系列 API 如何把宿主 C++ 世界的符号翻译成编译器能看懂的 `asCDataType` / `asCScriptFunction`，注入到全局符号表。
>
> 难度：★★★☆☆（代码量大但套路统一；理解"注册 = 向编译器符号表注入外部符号"之后一通百通）
> 前置：05（类型系统 asCDataType / asCTypeInfo / asCObjectType）

---

## 1. 模块定位

`asCScriptEngine` 是整个库唯一的"根对象"。模块（`scriptModules`）、上下文、GC、所有 `asCTypeInfo`、全部函数（`scriptFunctions` 数组，**函数 id 就是数组下标**）、全局属性（`globalProperties`）都直接或间接挂在它身上。它的两大职责：

1. **生命周期根**：一切内部对象用内部引用计数挂靠引擎，引擎析构时按依赖顺序拆除（见第 7 节）；
2. **宿主接口注册**：应用调用 `RegisterObjectType` / `RegisterGlobalFunction` 等，本质是**向编译器的符号表注入外部符号**——编译脚本时 `asCBuilder`/`asCCompiler` 查找类型和函数的范围，就是引擎里这些容器：
   - `allRegisteredTypes`：(namespace, name) → `asCTypeInfo*` 的总映射；
   - `registeredObjTypes` / `registeredTemplateTypes` / `registeredEnums` / `registeredTypeDefs` / `registeredFuncDefs`：分类列表；
   - `registeredGlobalFuncs` / `registeredGlobalProps`：带名字索引的符号表（`asCSymbolTable`）。

注册出来的函数是 `asFUNC_SYSTEM` 类型的 `asCScriptFunction`：没有字节码，只挂一个 `asSSystemFunctionInterface`（原生函数指针 + 调用约定 + 参数封送描述）。对编译器来说它和脚本函数无差别；只有 VM 在调用时才按 `sysFuncIntf` 走原生调用路径（13/14 篇）。

工厂函数 `asCreateScriptEngine(version)` 在 new 引擎之前做三类握手校验（as_scriptengine.cpp:233-273）：

- 版本号兼容：主/次版本必须相等、补丁号"应用 ≤ 库"（236-243），这是 02 篇"只加不改"二进制兼容策略的运行时执法点；
- 基础类型宽度断言：`asBYTE/asWORD/asDWORD/asQWORD/asPWORD` 与 bool（246-254）;
- 字节序断言（257-268）。

构造函数（as_scriptengine.cpp:659-772）依次：

- 初始化整张引擎属性表 `ep`（668-710），每个字段与一个 `asEEngineProp` 枚举值对应；
- `refCount.set(1)`、`currentGroup = &defaultGroup`、默认 accessMask 全 1（716-726）；
- 创建全局命名空间 `""`（732）；
- **保留函数 id 0 表示"无函数"**（745：`scriptFunctions.PushLast(0)`）；
- 把 typeId 序号推进到 `asTYPEID_DOUBLE + 1`，并断言内置原语 typeId 与公开枚举一致（748-762）；
- 注册两个内置行为类型 `RegisterScriptObject` / `RegisterScriptFunction`（766-767）——脚本类与 funcdef 共享的 addref/release 等行为就来源于这两个隐藏类型（`scriptTypeBehaviours` / `functionBehaviours`）。

`SetEngineProperty`（as_scriptengine.cpp:279-506）就是一个 40 个 case 的大 switch，把 `asEEngineProp`（include/angelscript.h:154-193，从 `asEP_ALLOW_UNSAFE_REFERENCES = 1` 到 `asEP_FOREACH_SUPPORT = 40`）写入 `ep` 对应字段，非法值返回 `asINVALID_ARG`（501-502）。有些属性有单位换算（如 `asEP_MAX_STACK_SIZE` 字节转 DWORD，295-306）或副作用（如 `asEP_FOREACH_SUPPORT` 要重建 tokenizer 跳转表，493-499）。`GetEngineProperty`（509 起）是对称的读 switch。

## 2. 注册的通用流程

所有 `Register*` 都是同一个三段式：

```
声明字符串
   → 临时 asCBuilder 借用编译器前端解析（ParseDataType / ParseFunctionDeclaration / VerifyProperty）
   → 构造引擎侧对象（asCScriptFunction / asCGlobalProperty / asCObjectType / asCEnumType…）
   → 存入引擎表 + 当前配置组（currentGroup）
```

关键点是**声明解析完全复用编译器的 parser**：每个 Register 函数体内都有一行 `asCBuilder bld(this, 0);`。宿主写的 `"void print(const string &in)"` 与脚本源码走的是同一套词法/语法/类型解析，所以注册声明的语法天然与脚本语言一致。

以 **RegisterGlobalFunction 完整走读**（as_scriptengine.cpp:3092-3199）为例：

1. `DetectCallingConvention(false, funcPointer, callConv, auxiliary, &internal)`（3100）——把 `asSFuncPtr` + 调用约定翻译成 `asSSystemFunctionInterface`；失败即 `ConfigError`。`AS_MAX_PORTABILITY` 下只接受 `asCALL_GENERIC`（3094-3097）；
2. new 一个 `asFUNC_SYSTEM` 的 `asCScriptFunction`，挂上 `sysFuncIntf`（3105-3116）；
3. `bld.ParseFunctionDeclaration(0, declaration, func, true, &paramAutoHandles, &returnAutoHandle, defaultNamespace)`（3118-3119）——**声明字符串在这里变成 `func->name` / `func->returnType` / `func->parameterTypes[]`（全是 asCDataType）**，顺带解析出 auto-handle（`@+`）标记；第 4 参数 `true` 表示按系统函数规则解析，允许 `&in/&out` 等只对注册接口开放的语法。解析失败返回 `asINVALID_DECLARATION`（3120-3126）；
4. 一串校验：
   - 模板函数（签名带 `class T`）与变参函数只允许 generic 约定（3128-3142）；
   - `bld.CheckNameConflict` 查与已有符号重名（3148-3155，错误码 `asNAME_TAKEN`）；
   - 若是虚属性访问器（`get_x` 带 property trait），`ValidateVirtualProperty` 校验签名（3157-3167）；
   - 与已注册的同名函数逐个比对参数表，完全相同则 `asALREADY_REGISTERED`（3169-3181）；
5. 入表：`func->id = GetNextScriptFunctionId(); AddScriptFunction(func);`（3185-3186），放进 `registeredGlobalFuncs` 与 `currentGroup->scriptFunctions`（3188-3190），最后 `currentGroup->AddReferencesForFunc` 记录跨组类型依赖（3195）。返回函数 id 作为成功值（3198）。

`RegisterGlobalProperty`（as_scriptengine.cpp:2732-2768）是同一套路的属性版：

- `bld.VerifyProperty(0, declaration, name, type, defaultNamespace)` 解析出名字和 `asCDataType`（2742-2744）；
- 拒绝引用类型（2747-2748，全局属性没有安全的生命周期保证）；
- `AllocateGlobalProperty()`（2771-2791）分配 `asCGlobalProperty`——**id 优先复用空闲槽位**（2781-2786），支持属性随配置组/模块反复增删；
- `SetRegisteredAddress(pointer)` 记录宿主变量地址，并把 `地址 → 属性` 存入 `varAddressMap`（2757-2758，字节码序列化时靠它反查指针属于哪个属性）。

`asCGlobalProperty` 本体（as_globalproperty.cpp:40-138）有个精巧细节：脚本访问全局**对象**走的是"指针的指针"——注册的对象类型属性把 `memory = &realAddress`（as_globalproperty.cpp:109-119），原语则直接 `memory = p`；而脚本自己声明的全局变量由 `AllocateMemory()` 独立按需分配（as_globalproperty.cpp:95-107），注释明确说明：每个属性独立分配，"so that variables can be added and removed at any time"。属性还可携带 `initFunc`（脚本全局变量的初始化函数，126-133）。

统一的错误出口是 `ConfigError`（as_scriptengine.cpp:3470-3489）：置 `configFailed = true`、格式化 "Failed in call to func (error)" 消息发给消息回调，并原样返回错误码。**一旦任何注册失败，`PrepareEngine` 直接拒绝（3371），所有后续编译都会失败**——引擎配置被视为一个整体事务。

注册期还有个"懒惰收尾"时机：每次注册都会把 `isPrepared` 置 false（如 1824、2173、3183），等第一次编译前 `PrepareEngine()`（as_scriptengine.cpp:3368-3468）再统一为所有系统函数计算参数封送布局（`PrepareSystemFunction[Generic]`，3374-3385），并做跨项完整性检查（见下节）。

## 3. 对象类型注册的校验逻辑

`RegisterObjectType(name, byteSize, flags)`（as_scriptengine.cpp:1820-2101）第一大段全是 flags 合法性检查：

- `asOBJ_REF` 分支（1828-1847）：只允许附加 GC / NOHANDLE / SCOPED / TEMPLATE / NOCOUNT / IMPLICIT_HANDLE；且 GC、NOHANDLE、SCOPED、NOCOUNT 两两互斥（1835-1842）；IMPLICIT_HANDLE 还要求引擎属性开启（1845-1846）。违规一律 `asINVALID_ARG`；
- `asOBJ_VALUE` 分支（1848-1901）：不得混用引用类 flags（1851-1852）；POD 不得与 ASHANDLE/TEMPLATE 同用（1855-1856）；`asOBJ_APP_CLASS / APP_PRIMITIVE / APP_FLOAT / APP_ARRAY` 四选一互斥，且 `APP_CLASS_CONSTRUCTOR` 等细化标志必须搭配 APP_CLASS（1859-1900）——这些 APP 标志描述 C++ 类型的 ABI 特征（有没有析构、是不是全整数成员等），供原生调用约定决定传参方式；
- 两者都没有 → `asINVALID_ARG`（1902-1903）；值类型必须给出非零 `byteSize`（1914-1918，报 "Value types must have a size"）。

名字校验同样借 parser：先 `bld.ParseDataType(name, ...)` 探测是否撞已有类型（1987，探测期间临时关掉消息回调 1986-1988，避免把"解析失败"这个预期结果报给用户）；新名字则用 tokenizer 确认是单个合法标识符（1996-1998）再 `CheckNameConflict`（2000-2002）。通过后 new 出 `asCObjectType` 填好 name/nameSpace/size/flags/accessMask，插入 `allRegisteredTypes`、`registeredObjTypes` 与 `currentGroup->types`（2008-2025）。

但注意：**ADDREF/RELEASE/构造/析构缺失并不在注册那一刻报错**——注册顺序本来就不定。检查集中在 `PrepareEngine` 第二段（as_scriptengine.cpp:3387-3465）：

| 类型 | 必须的行为 | 检查位置 |
|---|---|---|
| GC 引用类型 | addref + release + 5 个 GC 行为 | 3397-3412 |
| GC 值类型 | ENUMREFS | 3413-3420 |
| `asOBJ_SCOPED` | release | 3423-3430 |
| 普通引用类型（非 SCOPED/NOHANDLE/NOCOUNT） | addref + release | 3432-3443 |
| 非 POD 值类型 | 至少一个构造 + 析构 | 3445-3454 |

缺了就打出 "Type 'X' is missing behaviours" + 具体提示信息，并 `ConfigError(asINVALID_CONFIGURATION)`（3456-3463）。

行为注册走 `RegisterObjectBehaviour`（as_scriptengine.cpp:2104-2131，负责解析类型名并拦下对内置类型/生成的模板实例的修改）→ `RegisterBehaviourToObjectType`（2134-2633）。后者先 `ParseFunctionDeclaration` 解析行为签名（2183），函数名统一格式化为 `$beh<N>`（2190），然后一个巨大的 if-else 按行为逐项校验"该行为允许什么类型 + 签名必须长什么样"。两类典型错误码：

- **类型不够格** → `asILLEGAL_BEHAVIOUR_FOR_TYPE`：CONSTRUCT/DESTRUCT 只许值类型（2227-2231、2277-2281）；FACTORY 只许可有句柄的引用类型（2361-2367）；ADDREF/RELEASE 只许非 NOHANDLE/NOCOUNT（ADDREF 还排除 SCOPED）的引用类型（2468-2478、2493-2502）；GC 五行为要求 `asOBJ_GC`（2544-2552）；TEMPLATE_CALLBACK 要求 `asOBJ_TEMPLATE`（2517-2524）；
- **签名不对** → `asINVALID_DECLARATION`：构造/析构必须返回 void（2209-2210、2286-2288）、析构与 ADDREF/RELEASE 不得带参数（2290-2292、2487-2489、2511-2513）、FACTORY 必须返回本类型句柄（2369-2376）、GETREFCOUNT 必须返回 int（2566-2569）等。

重复注册同一行为返回 `asALREADY_REGISTERED`（如 2251-2252、2480-2481、2526-2527）。校验通过后 `AddBehaviourFunction`（2686-2729）把栈上的临时 `func` 深拷贝成正式系统函数入 `scriptFunctions` 表，id 记入 `objectType->beh` 的对应槽位。拷贝构造/拷贝工厂还会被特别识别：参数为 `const T &in` 且类型等于自身，则同时登记为 `beh->copyconstruct` / `beh->copyfactory`（2258-2271、2458-2465）。

## 4. 接口、funcdef、typedef、enum：四个轻量注册

这四个 API 展示了同一套路在不同复杂度下的样子：

- **RegisterInterface**（as_scriptengine.cpp:1679-1738）：创建一个 `asOBJ_REF | asOBJ_SCRIPT_OBJECT | asOBJ_SHARED`、size 为 0（不可实例化）的 `asCObjectType`（1715-1722），addref/release 直接**借用**内置脚本类的行为函数（1726-1729）。配套的 `RegisterInterfaceMethod`（1741-1792）创建 `asFUNC_INTERFACE` 函数——同样没有实现，`vfTableIdx` 按方法序号分配（1780-1784），供实现该接口的脚本类做虚表派发；
- **RegisterFuncdef**（as_scriptengine.cpp:6124-6180）：`ParseFunctionDeclaration` 解析出 `asFUNC_FUNCDEF` 函数后，包一层 `asCFuncdefType`（类型系统里的"函数指针类型"）入 `funcDefs`/`registeredFuncDefs`/`allRegisteredTypes`（6155-6158）；还支持 `"void Cls::Cb()"` 形式的子 funcdef 挂到父类（6161-6173）。姊妹函数 `FindMatchingFuncdef`（6198-6264）负责在编译器需要时按签名去重复用；
- **RegisterTypedef**（as_scriptengine.cpp:6268-6340）：**不走 parser**，只用 tokenizer 认原语关键字（6284-6306，只接受 bool/int 族/float/double，其他一律 `asINVALID_TYPE`），生成 `asCTypedefType`，`aliasForType` 指向原语（6324-6332）。这是纯别名，编译期即展开；
- **RegisterEnum**（as_scriptengine.cpp:6358-6421）：校验底层类型必须是整型 token（6369-6372），名字查重后创建 `asCEnumType`（flags = `asOBJ_ENUM | asOBJ_SHARED`，6403-6413）。`RegisterEnumValue`（6424-6466）先做**配置组归属检查**——枚举必须在当前组注册过（6427-6428，否则 `asWRONG_CONFIG_GROUP`，保证组卸载不留孤儿值），再查值名合法与重复，最后 push 进 `enumValues`。

注意四者的共同骨架：查重（返回可恢复的 `asALREADY_REGISTERED`）→ tokenizer 验证标识符 → `CheckNameConflict` → new 类型对象 → 三张表（`allRegisteredTypes` + 分类表 + `currentGroup->types`）。

## 5. 模板类型：array<T> 的特殊通道

`RegisterObjectType` 里 `asOBJ_TEMPLATE` 走独立分支（as_scriptengine.cpp:1926-1968）：

- `bld.ParseTemplateDecl("array<class T>")` 拆出模板名与子类型名列表（1929）；
- 子类型通过 `GetTemplateSubTypeByName`（1794 起）获取**全引擎共享的占位类型**（flags 为 `asOBJ_TEMPLATE_SUBTYPE` 的 `asCTypeInfo`，1961-1966）——之后注册 `"T &opIndex(uint)"` 之类的方法时，parser 遇到 `T` 解析出来的就是这个占位符；
- 类型同时进入 `registeredTemplateTypes`（1956）。

该函数还处理**模板特化**：若 `ParseDataType("array<int>")` 成功且指向引擎刚生成的模板实例，就用应用提供的具体类型顶替它并移除生成的实例（2027-2093）；若实例早已生成并被使用则报 "template already generated"（2043-2050）。

给模板注册行为/方法有一条硬规则：一旦已产生过任何具体实例，就不允许再改模板（RegisterBehaviourToObjectType 2156-2171、RegisterMethodToObjectType 2946-2961）。另外 `SetTemplateRestrictions`（2635-2671）根据签名里子类型的用法自动推断限制：按值使用 `T`（非引用非句柄）直接 `asNOT_SUPPORTED`——ABI 上无法对任意类型统一处理（2649-2650、2664-2665）；按句柄用则标记 `acceptValueSubType = false`，按值引用用则标记 `acceptRefSubType = false`。

真正实例化发生在编译器请求 `GetTemplateInstanceType`（as_scriptengine.cpp:3683 起）：

1. 查 `templateInstanceTypes` 缓存，同参实例全引擎共享（~3714）；
2. 按 accept 标记过滤子类型（3721-3728，不合格直接返回 0，编译器报"无法实例化"）；
3. 拷贝模板骨架生成新 `asCObjectType`，归属权交给请求模块或子类型所在模块（3744-3771）；
4. **在填充方法之前调用 `asBEHAVE_TEMPLATE_CALLBACK`**（3773-3803）。回调签名在注册时被强制为 `bool f(int &in, bool &out)`（2529-2540：返回 bool、恰两个参数、第一个 inref 第二个 bool outref）。第一个参数实际收到未完工的 `asITypeInfo*`，返回 false 则实例化失败、该类型作废（3783-3794）；out 参数若被置 true，引擎摘掉实例的 `asOBJ_GC` 标志（3796-3798）——这就是 `array<int>` 不进 GC 而 `array<Foo@>` 进 GC 的机制；
5. 引用类型模板把工厂挪进 construct 槽位（3809-3815），每个签名含 `T` 的方法经 `GenerateFunctionForTemplateObjectInstance`（4270 起）生成替换后的函数副本。

`RegisterDefaultArrayType`（as_scriptengine.cpp:3492-3507）只做一件小事：解析类型名、确认是模板类型（3499-3501）、存进 `defaultArrayObjectType` 并加内部引用——此后脚本里的 `int[]` 语法糖就展开为该模板。`GetDefaultArrayTypeId`（3510-3516）供宿主反查。

## 6. 字符串工厂：字面量的旁路

字符串不是内置类型。引擎通过 `asIStringFactory`（include/angelscript.h:814-822，三个纯虚：`GetStringConstant` / `ReleaseStringConstant` / `GetRawStringData`）与宿主字符串类型解耦。

`RegisterStringFactory(datatype, factory)`（as_scriptengine.cpp:3519-3545）：解析类型名（3529-3533）、拒绝引用和句柄（3536-3537）、把类型置为只读（3540，"All string literals will be treated as const"）后存进 `stringType` + `stringFactory`。就两个成员变量，真正的机制在使用侧。

字面量的数据流：

1. **编译期**：编译器遇到字符串常量，没有工厂就报 "Strings are not recognized by the application"（as_compiler.cpp:12132-12140）；否则 `engine->stringFactory->GetStringConstant(str, length)` 拿到一个**宿主拥有**的对象指针（as_compiler.cpp:12143），直接以 `asBC_PGA`（push 全局地址）指令把裸指针烧进字节码（as_compiler.cpp:12158-12159）。注释点明契约："The string factory already guarantees that the string object is valid throughout the lifetime of the script so no need to add reference count or make local copy"（12155-12157）；
2. **释放**：编译器析构时逐个 `ReleaseStringConstant`（as_compiler.cpp:118）；函数销毁时释放其默认参数里用到的常量（as_scriptfunction.cpp:1419）；
3. **序列化**：保存字节码用 `GetRawStringData` 导出内容，加载时再 `GetStringConstant` 重建（as_restore.cpp:673）——所以工厂的典型实现（add_on/scriptstdstring 的 `CStdStringFactory`）内部是带引用计数的缓存池，同一内容只存一份。

**配置组**（as_configgroup.cpp）解决"动态卸载一批接口"的需求（比如游戏卸载某个 mod 的脚本接口）：

- `BeginConfigGroup(name)`（as_scriptengine.cpp:5876-5897）：查重名后 new 一个 `asCConfigGroup` 并切换 `currentGroup` 指针；**不允许嵌套**（5885-5886，当前必须是默认组）。此后所有 Register 调用都把产物记账到该组；
- `EndConfigGroup`（5901-5910）：切回 `&defaultGroup`；
- `RemoveConfigGroup(name)`（5913-5957）：先清理无主的模板实例（5933-5934），然后两道占用检查——组间引用计数 `group->refCount > 0`（别的组或模块引用了本组的类型/函数）和 `HasLiveObjects()`（本组类型还有活对象，按 `externalRefCount` 判断，as_configgroup.cpp:146-153）——任一不过就返回 `asCONFIG_GROUP_IS_IN_USE`，**宁可拒绝也不做半卸载**（5915-5922 的注释解释了原因：允许运行时移除会让 VM 付出巨大代价）；通过后 `RemoveConfiguration`（as_configgroup.cpp:155-244）按 全局变量 → 全局函数 → 类型成员函数 → 类型本体 的顺序从引擎各表摘除并释放。

组间依赖靠注册时自动记账：`AddReferencesForFunc/AddReferencesForType`（as_configgroup.cpp:91-144）检查签名里用到的每个类型属于哪个组，跨组就 `RefConfigGroup` 给对方加引用（78-89）；还会级联追踪模板实例引用的其他模板实例（106-143）。

## 7. 引擎生命周期

引擎对外是标准引用计数（`AddRef`/`Release`，as_scriptengine.cpp:1105-1126），归零即 `asDELETE`。但模块、上下文、GC 对象都持有引擎引用，直接 Release 往往死锁在"互相等对方先死"。推荐的退出方式是 `ShutDownAndRelease()`（as_scriptengine.cpp:1129-1166），一套精确编排：

1. 先做一轮完整 GC，清掉还攥着引擎引用的对象（1132）；
2. 置 `shuttingDown = true`（1136）；
3. 清空上下文池回调（1142，注释解释：防止清理期间池又造新上下文抬高引擎引用计数）；
4. Discard 全部模块（1146-1149）；
5. 再 GC 一轮，清模块析构塞进 GC 的类型和函数（1153）；
6. `DeleteDiscardedModules()`（1157）；
7. `gc.ReportAndReleaseUndestroyedObjects()`——对没注册全 GC 行为导致的循环引用残留，报告并强拆（1159-1162）；
8. 最后 `Release()`。

析构函数（as_scriptengine.cpp:812-975）是一场按依赖排序的拆除：

- 没 ShutDown 就补一次（821-825）；
- 释放默认数组类型、生成的模板实例函数、列表模式类型（828-847）；
- **配置组三步清场**（859-872）：先 `defaultGroup.RemoveConfiguration(this, true)` 只摘"未被使用"的项，再按逆序删除具名组，最后清默认组剩余——顺序保证组间依赖不悬空；
- 依次清 `registeredGlobalProps`（883-889）、模板子类型占位符（891-899）、`registeredGlobalFuncs`（904-907）、全部 `scriptFunctions`（918-926，顺手把每个函数的 `engine` 指针置零，示意"已脱离引擎"）、funcDefs（934-940）、`globalProperties`（943-951）；
- **userdata 清理回调**在倒数第二步（959-967）：遍历 `userData` 键值对数组（偶数位 type、奇数位 data），对每个非空数据查 `cleanEngineFuncs` 中 type 匹配的回调调用之。回调经 `SetEngineUserDataCleanupCallback` 注册（6670 起）；module/context/function/typeinfo/脚本对象各有对称的一套（6692-6800）——这是宿主给引擎挂扩展数据又不泄漏的标准姿势；
- 最后释放命名空间、`asCThreadManager::Unprepare()`（970-974）。

防御性细节：析构中途允许临时 AddRef（GC 销毁对象时常见），所以 `Release` 里判断 `inDestructor` 避免重入删除（1116-1121）；析构中若发现引用计数不归零会打 "the engine's reference count is not zero" 警告（852-855）。

## 8. 关键代码走读

**(1) 版本握手**（as_scriptengine.cpp:236-243）——三段整除比较实现"主/次版本严格相等、补丁号应用不得高于库"。配合纯虚接口 + 尾部追加的约定，应用可以直接热换 DLL。

**(2) 声明字符串进入类型系统**（as_scriptengine.cpp:3118-3126）——`bld.ParseFunctionDeclaration(0, declaration, func, true, ...)` 一行完成词法+语法+类型解析，产出物直接填充 `asCScriptFunction` 签名字段。整个注册 API 没有第二套声明语法。

**(3) 行为校验的"白名单"风格**（as_scriptengine.cpp:2468-2491，ADDREF 分支）——先查类型资格（REF 且非 NOHANDLE/SCOPED/NOCOUNT，违者 `asILLEGAL_BEHAVIOUR_FOR_TYPE`），再查重复（`asALREADY_REGISTERED`），再查返回 void、零参数（`asINVALID_DECLARATION`），全过才 `AddBehaviourFunction`。每种行为一段，错误码语义可预期。

**(4) 模板回调否决实例化**（as_scriptengine.cpp:3774-3798）——回调在实例方法生成**之前**调用；`CallGlobalFunctionRetBool(ot, &dontGarbageCollect, ...)` 返回 false 则实例胎死腹中；out 参数让宿主按子类型豁免 GC。

**(5) 配置组的卸载安全网**（as_scriptengine.cpp:5936-5942 + as_configgroup.cpp:146-153）——组间引用计数挡"接口被别人依赖"，`HasLiveObjects` 用 `externalRefCount` 挡"还有活对象"。两道都过才真正拆表。

## 9. 如果你要自己实现

最小注册系统只需要两张表：`map<string, TypeDesc>`（符号表，编译器查类型）和 `vector<NativeFn>`（外部函数表，字节码 `CALL_SYS i` 按下标取函数指针 + 签名描述去封送参数）。AngelScript 的增量都在工程化上：

- **声明字符串复用编译器 parser** 是最值得抄的巧思：注册接口不需要发明第二套描述机制（对比 Lua 的手写栈操作、sol2/pybind11 的模板元编程），声明与脚本语言 100% 同构、类型检查规则天然一致，且错误信息直接复用编译器诊断。代价仅是每次注册临时构造一个 `asCBuilder`；
- **校验分两级**：格式/资格错误在注册时立即报（fail fast），跨项完整性（行为齐不齐）推迟到 `PrepareEngine` 统一查——因为注册顺序天然不定；
- **函数 id = 数组下标 + 0 号保留**，字节码里的函数引用是 O(1) 解引用；全局属性 id 带空闲槽复用（2780-2786）支持运行期增删；
- **系统函数与脚本函数同构**（都是 `asCScriptFunction`），编译器完全不用区分符号来自宿主还是脚本，只有 VM 调用时才分叉；
- 若要支持热插拔接口，配置组的"整组记账 + 组间引用计数 + 拒绝式卸载"是复杂度最低的可行方案——比"运行时可变符号表"便宜几个数量级。

## 10. 动手实验

1. **故意注册缺 ADDREF 的引用类型**：`engine->RegisterObjectType("Foo", 0, asOBJ_REF);` 后只注册 RELEASE，然后随便 `module->Build()` 一段脚本。观察消息回调输出 "Type 'Foo' is missing behaviours" + "The reference type must have the addref and release behaviours"（来自 as_scriptengine.cpp:3432-3443 的检查），Build 返回 `asINVALID_CONFIGURATION`。再反过来给值类型注册 ADDREF，会在注册当场收到 `asILLEGAL_BEHAVIOUR_FOR_TYPE`（2471-2478）——体会两级校验的分工。
2. **导出全部注册项**：链接 `sdk/add_on/scripthelper`，调用 `WriteConfigToFile(engine, "config.txt")`（scripthelper.h:33）把引擎完整配置（objtype/beh/func/prop/enum 每行一条）dump 成文本，对照本篇看每类注册项的去向。姊妹函数 `ConfigEngineFromStream`（scripthelper.h:41）能把这份文本重新灌进空引擎——离线编译工具正是这么做的。
3. **模板回调否决**：给自己的模板类型注册 `asBEHAVE_TEMPLATE_CALLBACK`，回调里对特定子类型返回 false，在脚本里写 `MyTmpl<那个类型> x;` 观察编译错误；再试 `*dontGC = true` 并用 `GetGCStatistics` 对比实例是否进 GC。
4. **配置组卸载**：`BeginConfigGroup("g")` 注册一个类型，`EndConfigGroup()` 后先在脚本模块里用它——`RemoveConfigGroup("g")` 返回 `asCONFIG_GROUP_IS_IN_USE`；Discard 模块后再删即成功。

## 11. 扩展阅读

- 官方手册 "Registering the application interface" 总章（对象类型、行为、字符串工厂、配置组各有小节）：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_register_api_topic.html（手册首页 https://www.angelcode.com/angelscript/documentation.html 可达）
- 模板类型注册专章 "Registering a template type"：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_adv_template.html
- 自定义字符串类型 "Registering a custom string type"：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_strings.html
- 引擎属性全表语义 "Custom options"：https://www.angelcode.com/angelscript/sdk/docs/manual/doc_adv_custom_options.html
- 下一篇（07）进入 asCBuilder / asCParser，看本篇反复借用的 `ParseDataType` / `ParseFunctionDeclaration` 内部如何工作。
