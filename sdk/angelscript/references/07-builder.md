# 07 — 编译调度器（asCBuilder：两遍编译）

> **概括**：`asCBuilder`（as_builder.cpp，约 7200 行）是模块编译的总指挥。它不生成一条字节码，却决定了"什么时候解析、什么时候登记声明、什么时候编译函数体"。理解它，就理解了 AngelScript 为什么支持前向引用、跨脚本段引用，以及 `module->Build()` 背后的完整流水线。
>
> **难度**：★★★★☆（文件巨大，但主干清晰；难点在类/接口/mixin 的多轮处理顺序）
>
> **前置**：04（词法/语法分析 asCParser）、05（脚本引擎与模块 asCScriptEngine/asCModule）、06（类型系统 asCDataType/asCObjectType）

---

## 1. 模块定位

- **上游**：`asCModule::AddScriptSection`（as_module.cpp:229）把用户脚本转交给 `asCBuilder::AddCode`（as_builder.cpp:213），builder 用 `asCScriptCode` 对象存下每个脚本段；`asCModule::Build`（as_module.cpp:260）在 `RequestBuild` 拿到编译权后于 as_module.cpp:299 调用 `m_builder->Build()`。
- **下游**：builder 自己只做"声明级"工作（登记类型、函数签名、全局变量），真正的表达式/语句编译全部转发给 `asCCompiler`（第 08 篇）；语法树则来自 `asCParser`（第 04 篇）。
- **横向**：引擎的注册 API（`RegisterGlobalFunction`、`RegisterObjectMethod`、`RegisterObjectProperty`…）也临时构造一个 asCBuilder，只为借用它的声明解析能力（见 §7）。

一句话：**parser 产出语法树，compiler 产出字节码，builder 负责把两者按正确的顺序、正确的次数粘起来**。

### 1.1 核心数据结构（as_builder.h）

builder 的全部状态就是几个"待办清单"，第一遍往里填，第二遍从里取（as_builder.h:259-266）：

| 成员 | 类型 | 内容 |
|---|---|---|
| `scripts` | `asCArray<asCScriptCode*>` | 所有脚本段 |
| `functions` | `asCArray<sFunctionDescription*>` | 待编译函数：脚本段 + 摘下来的函数体 AST + funcId（as_builder.h:61-70） |
| `globVariables` | `asCSymbolTable<sGlobalVariableDescription>` | 待编译全局变量：声明节点 + 初始化表达式节点 + isCompiled 标志（72-86） |
| `classDeclarations` / `interfaceDeclarations` | `asCArray<sClassDeclaration*>` | 类/接口声明：AST 节点 + typeInfo + isExistingShared + 成员初始化式列表 `propInits`（100-117） |
| `funcDefs` / `mixinClasses` | `sFuncDef` / `sMixinClass` | funcdef 与 mixin 的原始 AST（119-135） |
| `namespaceVisibility` | `asCMap<ns, asCArray<ns*>>` | `using namespace` 可见性图（206） |

注意 `sClassDeclaration::propInits`（116）：成员变量的初始化表达式不属于任何函数，builder 把它们攒在类声明上（`AddPropertyToClass` 里 4537 压入），由 asCCompiler 编译构造函数时逐个取出（as_compiler.cpp:598-602）织入构造函数开头。

## 2. 为什么要两遍

脚本语言的常见需求：函数可以调用写在它后面的函数，类 A 的成员可以是类 B，而 B 又持有 A 的句柄——即**前向引用**，不需要 C/C++ 式的先声明后使用。

单遍编译做不到：编译 `A::foo()` 的函数体时如果还不知道 `B` 是什么类型，就无法做类型检查。AngelScript 的解法是经典的两遍（实际是"多阶段"）：

1. **第一遍（声明收集）**：把所有脚本段全部 parse 完，再遍历所有语法树，只登记"名字 → 实体"——类、接口、枚举、funcdef、全局变量、函数**签名**。这一步不看任何函数体。
2. **第二遍（函数体编译）**：此时符号表已完整，任何函数体里出现的名字都能查到，逐个把函数体交给 asCCompiler。

关键实现细节：第一阶段登记函数时，函数体的语法子树被 `node->DisconnectParent()` 从大树上摘下来，挂到 `sFunctionDescription` 里（`functions` 数组）攒着，等第二阶段再编。类声明、全局变量的初始化表达式同理。**跨脚本段引用**天然成立：所有段先统一 parse（ParseScripts 内循环，651-669），声明统一进入引擎级/模块级符号表，第二遍编译时根本不区分符号来自哪个段。

## 3. Build 全流程走读

入口 `asCBuilder::Build()`（as_builder.cpp:303-357），主干只有五个调用，但第一个 `ParseScripts` 内部又分七八个子阶段。按真实执行顺序：

### 3.0 Reset 与模板延迟验证（303-314）

`Reset()`（199）清零错误计数；置 `engine->deferValidationOfTemplateTypes = true`（313）——因为编译中途实例化 `array<A>` 时 A 可能只有个空壳，模板回调（如"该子类型能否实例化"）必须推迟到所有类结构确定后再跑；同时记下当前模板实例数 `numTempl`（314），之后只验证本次 build 新增的实例。

### 3.1 ParseScripts（316；函数体 651-826）——"第一遍"的全部

1. **全部脚本段 parse 成语法树**（659-669）：为每个 `asCScriptCode` 建一个 `asCParser`，`ParseScript` 只做语法分析（函数体在 parser 里也是延迟解析的超级块）。任何段有语法错误则整体中止。
2. **登记命名空间可见性**（674-678 → `RegisterNamespaceVisibility` 886）：递归发现 `namespace X { ... }` 节点、调用 `engine->AddNameSpace`（902）创建命名空间对象，并把 `using namespace` 指令登记进 `namespaceVisibility` 映射（`RegisterUsingNamespace` 2277）。
3. **登记所有类型声明**（681-685 → `RegisterTypesFromScript` 828-884）：遍历顶层节点，按节点类型分发——`snClass`→`RegisterClass`(2375)、`snInterface`→`RegisterInterface`(2601)、`snEnum`→`RegisterEnum`(4700)、`snTypedef`→`RegisterTypedef`(4922)、`snFuncDef`→`RegisterFuncDef`(2053)、`snMixin`→`RegisterMixinClass`(2308)；遇到 `snNamespace` 则拼接名字后递归（837-847）。每个 Register 函数的套路一致：读修饰符（final/shared/abstract/external，2384-2443）→ 取名字 → `CheckNameConflict`（1690，与引擎注册类型、模块内已有实体、funcdef、mixin 逐一比对）→ new 一个 `sClassDeclaration` 压入清单。此时每个类只是一个**空壳 asCObjectType**（只有名字和标志），成员一概未知——这正是允许前向引用的根基。
4. **确定类型关系**（689 → `DetermineTypeRelations` 3235-3459）：现在所有类型名都已可查，回头解析每个接口/类声明头上的继承列表：接口继承接口（3238-3322，含"不能继承自己"环检测 3294-3311）；类的基类与接口列表（3325-3458）——区分基类（`objType->size != 0`，设置 `derivedFrom`，3443）、接口（3452）、mixin（3377），并检查单继承（3395）、final 不可继承（3385）、shared 只能继承 shared（3419-3425）。
5. **补全 funcdef 签名**（692-693 → `CompleteFuncDef` 2114）：funcdef 的返回值/参数类型可能引用刚登记的类型，所以放在类型关系确定之后。
6. **登记非类型全局实体**（696-701 → `RegisterNonTypesFromScript` 960-1003）：全局函数签名（`RegisterScriptFunctionFromNode` 982）、全局变量声明（`RegisterGlobalVar` 984，初始化表达式节点存进 `sGlobalVariableDescription`，不编译）、虚属性（986）、`import` 函数（988）。
7. **登记接口方法**（704-729）与**类方法/属性方法**（732-757）：遍历每个接口/类声明体内的 `snFunction`/`snVirtualProperty` 节点，调 `RegisterScriptFunctionFromNode`(5196)/`RegisterVirtualProperty`(5739) 建立 `asCScriptFunction` 签名对象。签名提取由 `GetParsedFunctionDetails`（4978）完成：返回类型、参数类型/名字/inout 修饰、默认参数表达式（存为字符串，`GetCleanExpressionString` 5167 顺带剥掉注释）、const/final/override 等 traits。随后校验默认参数必须靠右连续（`ValidateDefaultArgs` 438-455）并警告因默认参数导致的重载歧义（`CheckForConflictsDueToDefaultArgs` 460-514）。函数体子树在这里被摘下、存入 `functions` 数组。
8. **补默认成员函数**（759-818）：若类没有显式构造/拷贝构造/opAssign，按引擎属性决定是否生成默认版本（`AddDefaultConstructor` 787、`AddDefaultCopyConstructor` 802）；这些"自动函数"没有语法树节点（`sFunctionDescription::node == 0`），第二遍会走专门的编译路径。

### 3.2 CompileInterfaces（321；函数体 3143-3233)

先把有继承关系的接口**拓扑排序**（被继承者在前，3149-3170），再把基接口的方法递归拷入派生接口（3199-3231，签名重复的不拷 3210-3222），同时用 `vfTableIdx` 给每个接口方法编好它在虚表块中的索引（3189-3195）。

### 3.3 CompileClasses（322；函数体 3462-4241）——类的"结构定型"

见 §4 详述。完成后每个 asCObjectType 拥有完整的属性布局、方法表、虚函数表。

### 3.4 EvaluateTemplateInstances（326；函数体 255-301）

类结构已定，对本次 build 期间新产生的每个模板实例逐个调用注册时提供的 template callback（269-285），失败则报 `TXT_INSTANCING_INVLD_TMPL_TYPE`；回调声明"不会形成环"的实例摘掉 asOBJ_GC 标志（289-290）。随后 `deferValidationOfTemplateTypes = false`（327）。

### 3.5 CompileGlobalVariables（333；函数体 2721-2996）

编译全局变量初始化表达式（见 §5）。放在函数体之前，是因为 `auto` 声明的全局变量要先推导出类型，函数里才能正确引用（331-332 的注释）。

### 3.6 CompileFunctions（336；函数体 1005-1095）——"第二遍"

遍历攒下的 `functions` 数组：跳过复用的 shared 函数（1013-1014）和无函数体的（接口方法，1017-1018）；对每个函数 new 一个 `asCCompiler`，调 `compiler.CompileFunction(this, script, paramNames, node, func, classDecl)`（1049）。若是构造函数，先找到所属 `sClassDeclaration` 传进去（1023-1037）——**成员初始化表达式是在构造函数体的编译过程中一起编的**。无节点的自动函数走 `CompileDefaultConstructor`（1069）/`CompileDefaultCopyConstructor`（1085）。

### 3.7 收尾（343-356）

`compilerWarnings == 2` 时警告升级为错误（343-344）；模块一无所有则报 `TXT_NOTHING_WAS_BUILT`（350-353）；否则 asSUCCESS。

### 3.8 全流程速查图

```
Build() [303]
 ├─ ParseScripts() [651]                       ←—— 第一遍：只登记，不编译
 │   ├─ 每段 parser->ParseScript      [659-669]  语法树（函数体延迟）
 │   ├─ RegisterNamespaceVisibility   [674-678]  namespace / using namespace
 │   ├─ RegisterTypesFromScript       [681-685]  类/接口/枚举/typedef/funcdef/mixin 空壳
 │   ├─ DetermineTypeRelations        [689]      继承链、接口列表、mixin 挂接
 │   ├─ CompleteFuncDef               [692-693]  funcdef 签名补全
 │   ├─ RegisterNonTypesFromScript    [696-701]  全局函数签名/全局变量/虚属性/import
 │   ├─ 接口方法 / 类方法签名          [704-757]  函数体 AST 摘下存入 functions[]
 │   └─ 默认构造/拷贝构造/opAssign 补齐 [759-818]
 ├─ CompileInterfaces()  [321→3143]              接口方法继承展开 + vfTableIdx
 ├─ CompileClasses()     [322→3462]              属性布局、虚表、接口实现检查、GC 标记
 ├─ EvaluateTemplateInstances [326→255]          模板实例回调验证
 ├─ CompileGlobalVariables()  [333→2721]         不动点迭代编初始化式
 └─ CompileFunctions()        [336→1005]        ←—— 第二遍：逐函数体 → asCCompiler
```

## 4. 类的处理难点（CompileClasses 内部顺序）

`CompileClasses`（3462）是 builder 里最烧脑的函数，内部又是一串小阶段，顺序不能乱：

1. **基类在前排序**（3467-3491）：冒泡式地把派生类挪到基类之后，保证继承拷贝时基类已定型。
2. **mixin 方法并入**（3513 → `IncludeMethodsFromMixins` 4243）：注释明确"mixin 方法优先于继承来的方法"（3512）。
3. **继承拷贝**（3515-3611）：派生类继承基类的全部接口（3520-3527）、属性（保持相同偏移，3529-3536）、方法（3538-3611）——若派生类有同签名方法即为 override，把基类版本替换掉；否则把基类函数压入虚表（3599）。
4. **构建虚函数表**（3613-3628）：本类的每个方法被移入 `virtualFunctionTable`，方法表中的原位置换成 `CreateVirtualFunction`（4472）生成的 asFUNC_VIRTUAL 桩（3625）——脚本类的**所有**方法调用都走虚表。随后为每个直接实现的接口建立虚表块（3630-3676），按接口方法的 `vfTableIdx` 排列真实实现。
5. **枚举声明的属性**（3680-3760）：此时才解析成员变量的类型（`CreateDataTypeFromNode`）并 `AddPropertyToClass`（4503）确定偏移；成员初始化表达式节点被跳过留给构造函数（3752）。再并入 mixin 属性（3763 → `IncludePropertiesFromMixins` 4317）。
6. **接口实现检查**（3778-3825）：继承完成后，验证类实现了每个接口显式声明的方法，缺失则报错。
7. **结构合法性验证**（3826-3916）：检测"类直接或间接按值包含自身"的非法递归结构。
8. **自动拷贝构造可行性复核**（3918-4038）：若成员或基类不可拷贝，撤销之前自动生成的拷贝构造，免得第二遍编译报出莫名错误。
9. **GC 标记收窄**（4040-4241，开关在 4053）：分析哪些类**真正**可能形成循环引用，只给它们保留 asOBJ_GC，其余摘除——直接影响运行期 GC 压力。

## 5. 全局变量的编译与初始化顺序

`CompileGlobalVariables`（2721-2996）没有做真正的依赖图拓扑排序，而是一个**不动点迭代 + 两档分类**：

- **先原始类型、后复杂类型**（2743-2749 注释与 2774）：先反复尝试编译所有 primitive 全局变量，这样复杂类型的构造函数至少能用上已初始化的 primitive 常量。
- **不动点循环**（2752-2975）：每一轮遍历所有未编译成功的变量逐个试编（枚举值走特殊路径 2787-2850：无初始化式则取前一枚举值 +1，2814-2841）；整轮期间错误消息被重定向到临时缓冲（2735-2736），只要本轮至少有一个成功（`compileSucceeded`）就再来一轮——因为它的成功可能解锁依赖它的变量（如枚举常量传播）。整轮无进展且已在编复杂类型时才停下，把攒下的错误一次性吐出（2965-2973）。
- 每个非枚举变量的初始化式被编成独立的 init 小函数（2854-2916），字节码只剩 SUSPEND+RET 的直接丢弃（2896、2917-2923）；**成功编译的先后顺序**记录在 `initOrder`，最后与模块符号表交换（2985-2993），即运行期 `ResetGlobalVars` 的执行顺序 = 编译收敛顺序。Build() 里 338-341 的 TODO 坦承：涉及函数调用的跨变量依赖并未分析，顺序不保证。

## 6. 增量编译：CompileFunction / CompileGlobalVar

这是控制台示例（如 asrun 里输入一行代码立即执行）的入口，走 `asCModule::CompileFunction`（as_module.cpp:1740）/`CompileGlobalVar`（1682）：

- **`asCBuilder::CompileFunction`**（516-649）：单独 parse 一段只允许含**一个函数**的代码（540-547），手工填好 `asCScriptFunction` 签名（558-564），若带 `asCOMP_ADD_TO_MODULE` 则查名字冲突后挂进模块（575-587），然后循环编译（609-616，循环是因为 lambda 会追加新函数）。出错时把已挂上的函数全部摘除回滚（623-643）。
- **`asCBuilder::CompileGlobalVar`**（359-435）：同样只许一个变量（381-388），`RegisterGlobalVar` 后直接复用 `CompileGlobalVariables()`（394），失败则从模块移除该变量（427-429）。

与完整 Build 的本质差异：**跳过了所有类型阶段**（无 RegisterTypesFromScript/CompileClasses），新代码只能引用模块中已存在的类型和函数，不能声明类；且它们不 `RequestBuild` 整个两遍流程，代价是 O(一个函数) 而非 O(整个模块)。

## 7. 声明解析的复用：ParseDataTypeFromDecl 一族

引擎的注册 API 大量借用 builder：

- **`ParseDataType`**（1099-1122）：把 `"array<int>@"` 这类字符串包成临时 `asCScriptCode`（1103-1104），用 parser 的 `ParseDataType` 入口得到子树，再 `CreateDataTypeFromNode`（6440）转成 asCDataType。`RegisterObjectProperty`、`GetTypeIdByDecl` 等都调它（如 as_scriptengine.cpp:1613、2111）。
- **`ParseFunctionDeclaration`**（1316-1545）：解析 `"void f(int &out, MyObj@+)"` 完整签名填充 asCScriptFunction，顺带识别 `@+` auto-handle（1371-1381）、模板子类型（1354-1368）、系统函数的按值返回引用类型限制（1383-1389）。`RegisterGlobalFunction`（as_scriptengine.cpp:1502）、`RegisterObjectMethod`（2183）等的幕后主力。
- 另有 `ParseVariableDeclaration`（1547）、`ParseTemplateDecl`（1124）。

这就是为什么注册 API 的声明字符串语法与脚本语法完全一致——**同一个 parser、同一个 builder 在解析**。

此外，builder 还是第二遍编译期间 asCCompiler 的**符号查询服务台**：

- `GetFunctionDescriptions`（5977）：按名字+命名空间收集全局函数候选（模块函数、引擎注册函数、import 函数），供重载决议；
- `GetObjectMethodDescriptions`（6014）：按名字收集类方法候选，处理 const 对象过滤与显式作用域（`Base::foo` 式调用）；
- `GetGlobalProperty`（1283）：查全局变量，顺带返回"是否已编译/是否纯常量"——CompileGlobalVariables 的不动点迭代正是靠这个判断依赖是否就绪；
- `GetEnumValue`（7174）/`GetEnumValueFromType`（7156）：枚举常量查找；
- `GetObjectType`（7012）、`GetFuncDef`（7111）、`GetType`（6982）：类型名解析的底层入口，全部同时搜索模块类型表和引擎注册类型表——脚本能无差别使用两边的类型，就是在这一层合流的。

## 8. 命名空间、shared 复用、mixin

- **命名空间**：不是作用域树而是**扁平的全名字符串**——`namespace A { namespace B {} }` 生成名为 `"A::B"` 的 asSNameSpace（846-847 拼接、engine->AddNameSpace）。查找时从最内层全名逐级剥离外层再试；`using namespace` 的可见性图由 `AddVisibleNamespaces`（915）/`FindNextVisibleNamespace`（935）遍历。
- **shared 类型复用**：`RegisterClass` 里若声明带 shared，先在 `engine->sharedScriptTypes` 中按名字+命名空间找现成类型（2481-2501），找到则 `decl->isExistingShared = true` 直接挂用，后续各阶段对该 decl 只做"与原声明一致性校验"而不重编（如 CompileClasses 3500-3507、CompileFunctions 1013-1014）。shared **函数**同理，在 RegisterScriptFunction 中查找签名相同的既有 shared 函数（5458-5471）。这是多模块共享类型不重复编译的机制。
- **mixin**：`RegisterMixinClass`（2308-2373）根本不创建 asCObjectType，只把语法树存进 `sMixinClass`（2342-2353）——mixin 是**编译期文本级构件**，在 DetermineTypeRelations 中把其接口并入宿主（3377 → `AddInterfaceFromMixinToClass` 3052），在 CompileClasses 中把方法/属性节点复制到每个包含它的类里（3513、3763），各类分别编译一份。

## 9. 错误报告：asCScriptCode 与行列定位

`asCScriptCode`（as_scriptcode.cpp，仅 152 行）是脚本段的物理载体：

- `SetCode`（67-99）：按 `copyScriptSections` 引擎属性决定复制还是借用用户缓冲（76-90）；关键是**顺手扫一遍全文，把每个 `'\n'` 的下一位置压入 `linePositions` 数组**（92-96）——一次 O(n) 预处理换来之后任意次 O(log n) 定位。
- `ConvertPosToRowCol`（101-142）：语法树节点只存字节偏移 `tokenPos`；报错时在 `linePositions` 上**二分查找**（110-138）出所在行，行号 = 下标+1+`lineOffset`（140，lineOffset 支持"这段代码其实从原文件第 N 行开始"），列号 = 偏移差+1（141）。
- builder 的 `WriteError`/`WriteWarning`/`WriteInfo`（6105-6150）统一走这条路：`node->tokenPos` → ConvertPosToRowCol → `engine->WriteMessage(section, row, col, ...)`。函数的声明位置也被压缩成 `(row & 0xFFFFF)|(col<<20)` 存进 `declaredAt`（如 563）。
- `TokenEquals`（144-150）供各 Register 函数比对关键字（shared/final/abstract 都是上下文关键字而非保留字，只能按文本比）。
- 值得注意的消息缓冲技巧：CompileGlobalVariables（2735-2736）和 EvaluateTemplateInstances（263-265）都会**临时劫持引擎的消息回调**到 `asCOutputBuffer`——因为迭代式编译中"这一轮的失败"未必是真失败，只有确定无法收敛时才把攒下的消息一次性回放给用户回调（2971-2972），成功那一轮则只回放警告（2887-2888）。写重试型编译器时这招必学，否则用户会看到一堆随后又成功了的假错误。

## 10. 如果你要自己实现

两遍编译的最小框架，抄 asCBuilder 的作业：

1. **数据结构**：一个 `PendingFunction { 名字, 签名, 函数体AST, 所属类 }` 列表 + 一个符号表。第一遍只往两者里填东西，绝不递归进函数体。
2. **阶段拆分口诀**——"名字 → 关系 → 签名 → 实体"：先登记所有类型**名**（空壳即可）；再解析继承/成员**类型**（此时任何类型名都能查到）；再登记函数**签名**；最后一遍编函数体。每个阶段只依赖上一阶段的产物，就永远不会有"用到时还不存在"的问题。
3. **AST 所有权转移**是省内存的关键技巧：像 `DisconnectParent()` 那样把函数体子树摘下挂到 pending 表，编完即毁，语法树不必整棵活到编译结束。
4. **全局变量初始化**没有依赖图时，可用 asCBuilder 的懒办法：不动点迭代——反复试编、有进展就再来一轮，最后一轮的错误才是真错误。简单、正确性可接受，但要向用户说明顺序不完全保证。
5. **错误定位**：token 只存字节偏移，报错时二分行号表。不要在每个 AST 节点里存 (row, col) 两个 int——偏移一个 size_t 就够。
6. **阶段间的顺序约束**要显式写成注释甚至断言，asCBuilder 里到处是这种隐含依赖：funcdef 补全必须在类型关系之后（其签名可能引用子类型）、类结构定型必须在模板回调之前、`auto` 全局变量必须在函数体之前。乱序的 bug 往往只在特定声明顺序的脚本上爆发，极难复现。
7. 若要支持 shared/热重载类语义，学 `isExistingShared` 的做法：复用旧实体时**不跳过声明遍历**，而是把每一步降级为"一致性校验"——这样既不重复构建，又能报出"新声明与原共享声明不符"这类高质量错误（CompileClasses 3504-3507 的注释解释了只查新增、不查缺失的取舍）。

## 11. 动手实验

1. **前向引用互指**：
   ```angelscript
   class A { B@ b; void hi() { print("A"); } }
   class B { A@ a; void hi() { print("B"); } }
   ```
   放进单个脚本段 Build——顺利通过。把两个类拆进两个 `AddScriptSection` 再 Build，依然通过（ParseScripts 651-669 统一先 parse 所有段）。改成 `class A { B b; }` `class B { A a; }`（按值互含）则触发 3826 起的结构合法性检查报错。
2. **观察两遍先后**：在 `RegisterClass`（2375）、`RegisterScriptFunction`（5260）、`asCCompiler::CompileFunction` 各下断点，Build 上述脚本：会看到两个类、四个函数签名**全部登记完毕**之后，第一个函数体才开始编译。再在 `CompileClasses` 的 3625（虚表桩生成）停一下，观察 `ot->methods` 与 `ot->virtualFunctionTable` 的对应。
3. **收敛式全局变量**：写 `int x = y + 1; int y = 2;`，在 `CompileGlobalVariables` 的 while 循环（2752）观察第一轮 x 编译失败、y 成功，第二轮 x 成功——以及模块符号表最终顺序变成 y、x（2991-2992）。
4. **增量编译**：用 asrun 示例（`ExecuteString`/`CompileFunction` 路径），输入一个引用已有全局函数的新函数，在 `asCBuilder::CompileFunction`（516）断点确认它完全绕过了 RegisterTypesFromScript。

## 12. 扩展阅读

- 官方手册 "Understanding AngelScript / Script modules" 与 "Shared script entities"（sdk/docs）——shared/external/mixin 的语义定义。
- Crafting Interpreters 第 11 章 *Resolving and Binding*（https://craftinginterpreters.com/resolving-and-binding.html）——用单独的 resolve pass 解决前向引用与闭包捕获，是本篇"先声明后解析"思想的教科书版。
- 本系列 04（asCParser 的延迟函数体解析恰好为两遍编译铺路）、08（asCCompiler：函数体如何变成字节码）。
