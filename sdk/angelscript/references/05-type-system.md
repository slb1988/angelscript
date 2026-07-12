# 05 — 类型系统（asCDataType / asCTypeInfo / asCObjectType）

> **概括**：本篇精读 AngelScript 的类型系统三件套——描述"某个表达式/变量是什么类型"的值对象 `asCDataType`、引擎持有的类型实体 `asCTypeInfo` 及其最重要的子类 `asCObjectType`、以及暴露给宿主的整数句柄 typeId。同时梳理 `asOBJ_*` 标志如何驱动引擎的内存管理与调用约定决策。
>
> **难度**：★★★☆☆（概念多但代码直白；模板实例化与 GC 判定部分略绕）
>
> **前置**：第 04 篇（引擎与模块生命周期）。建议手边打开 `angelscript.h` 对照枚举注释。

涉及文件：
- `sdk/angelscript/source/as_datatype.h` / `as_datatype.cpp`
- `sdk/angelscript/source/as_typeinfo.h` / `as_typeinfo.cpp`
- `sdk/angelscript/source/as_objecttype.h` / `as_objecttype.cpp`
- `sdk/angelscript/include/angelscript.h`（`asEObjTypeFlags`、`asETypeIdFlags`）
- `sdk/angelscript/source/as_scriptengine.cpp`（typeId 相关）

---

## 1. 模块定位

类型系统是编译器与运行时之间的**共同语言**：

- **类型检查 / 重载决议**：编译器（as_compiler.cpp）对每个表达式维护一个 `asCDataType`，参数匹配、隐式转换、const 正确性全靠它的相等性与修饰位判断（如 `IsEqualExceptRefAndConst`，as_datatype.h:113）。
- **内存布局**：局部变量占几个 DWORD、类属性放在对象的哪个字节偏移，由 `GetSizeOnStackDWords` / `GetSizeInMemoryBytes` 与 `AddPropertyToClass` 决定。
- **GC 判定**：一个类型是否可能形成循环引用（是否要进 GC）、销毁时调用哪个行为函数，由 `asCTypeInfo::flags` 中的 `asOBJ_GC`、`asOBJ_REF` 等位与 `asSTypeBehaviour` 决定。
- **宿主互操作**：宿主拿到的永远是 int 类型的 typeId，通过引擎在 typeId 与内部类型之间双向转换。

一句话：编译期问"这个表达式能不能赋给那个变量"，运行期问"这块内存怎么构造/复制/析构/回收"，答案都存在这三层结构里。

## 2. 三层结构

### 2.1 asCDataType —— 值语义的"类型描述"

`as_datatype.h:60-158` 定义。它不是引擎注册表里的实体，而是一个**可随意拷贝的小值对象**，由两部分组成：

- **基础类型**：`eTokenType tokenType`（as_datatype.h:144）——原始类型直接用词法 token 表示（`ttInt`、`ttFloat`…），对象类型则为 `ttIdentifier` 并配合 `asCTypeInfo *typeInfo`（as_datatype.h:157）指向真正的类型实体。
- **修饰位**：一组位域（as_datatype.h:147-154）——`isReference`、`isReadOnly`、`isObjectHandle`、`isConstHandle`、`isAuto`、`isHandleToAsHandleType`、`ifHandleThenConst`。

也就是说 `const string &`、`string @`、`string` 是**同一个** `asCTypeInfo` 配上不同修饰位的三个 `asCDataType`。构造入口是一组静态工厂：`CreatePrimitive`（as_datatype.cpp:158）、`CreateType`（:87）、`CreateObjectHandle`（:144）、`CreateNullHandle` 等；修饰变换用 `MakeHandle`（:282）、`MakeReference`（:348）、`MakeReadOnly`（:355）、`MakeHandleToConst`（:367）。

注意 as_datatype.h:57 的 TODO：作者自己也认为 `isReference` 不该属于数据类型（应属于表达式值 `asCExprValue`），这是历史包袱。

### 2.2 asCTypeInfo / asCObjectType —— 引擎持有的"类型实体"

`asCTypeInfo`（as_typeinfo.h:63-188）实现公开接口 `asITypeInfo`，是引擎类型注册表中的节点，带引用计数（外部 `externalRefCount` 与内部 `internalRefCount` 分开，as_typeinfo.h:186-187）。核心字段（as_typeinfo.h:163-177）：`name`、`nameSpace`、`size`、`typeId`（惰性分配，见第 5 节）、`flags`（asOBJ_* 位集合）、`accessMask`、所属 `module`。

它有四个子类，用 `CastToXxxType` 自由函数做安全向下转型（as_typeinfo.h:157-160，可传 null）：

| 子类 | 位置 | 表示 |
|---|---|---|
| `asCObjectType` | as_objecttype.h:101 | 类、接口、注册的应用类型、模板实例 |
| `asCEnumType` | as_typeinfo.h:198 | 枚举（`enumValues` 数组） |
| `asCTypedefType` | as_typeinfo.h:216 | 类型别名（`aliasForType`） |
| `asCFuncdefType` | as_typeinfo.h:236 | 函数指针类型（持有一个 `asCScriptFunction *funcdef`） |

shared/external 的处理：`asCTypeInfo::IsShared`（as_typeinfo.cpp:336-343）——脚本可声明的类型（`asOBJ_SCRIPT_OBJECT | asOBJ_ENUM`）必须显式带 `asOBJ_SHARED` 才算共享；应用注册的类型一律视为共享（所有模块可见同一实体）。共享类型可以被多个模块复用而不重复编译，`external` 声明则要求类型必须已存在于其他模块。

### 2.3 typeId —— 暴露给宿主的 int 句柄

宿主 API（`GetTypeIdByDecl`、`asIScriptGeneric::GetArgTypeId` 等）只交付 int。引擎内部用 `mapTypeIdToTypeInfo`（红黑树）维护 typeId → `asCTypeInfo*` 的映射，双向转换函数是 `GetTypeIdFromDataType`（as_scriptengine.cpp:5159）与 `GetDataTypeFromTypeId`（:5226）。

三者关系总结：

```
脚本源码 "const array<int>@ a"
   │ 编译器解析
   ▼
asCDataType { typeInfo=array<int>的asCObjectType*, isObjectHandle=1, isConstHandle=1 }   ← 值对象，随处拷贝
   │ GetTypeIdFromDataType
   ▼
int typeId = 序号 | asTYPEID_TEMPLATE | asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST      ← 交给宿主
   │ GetDataTypeFromTypeId / GetTypeInfoById
   ▼
还原 asCDataType / 拿到 asITypeInfo*
```

## 3. asOBJ_* 标志体系

`angelscript.h:213-261` 的 `asEObjTypeFlags`。按"改变引擎什么行为"分组：

**生命周期模型（注册时二选一）**
- `asOBJ_REF`（1<<0）：引用类型。堆上分配、由 factory 创建、addref/release 管理；只有这类才支持 `@` 句柄（见 `MakeHandle` 的检查，as_datatype.cpp:303-307）。
- `asOBJ_VALUE`（1<<1）：值类型。可内联在栈/对象内存中，由 construct/destruct 行为管理。

**REF 的变体**
- `asOBJ_GC`（1<<2）：可能参与循环引用，实例创建时登记进垃圾回收器，必须注册 5 个 gc 行为（`asSTypeBehaviour` 中 gcGetRefCount 等，as_objecttype.h:85-89）。
- `asOBJ_NOHANDLE`（1<<4）：禁止句柄（典型：单例引擎接口），`MakeHandle` 直接拒绝。
- `asOBJ_SCOPED`（1<<5）：作用域引用类型，无 addref/release，离开作用域即调用 release 行为销毁；只在函数返回值处允许临时句柄（`acceptHandleForScope` 参数，as_datatype.cpp:282）。
- `asOBJ_NOCOUNT`（1<<18）：引擎不调用 addref/release，宿主自己保证生存期。

**VALUE 的变体**
- `asOBJ_POD`（1<<3）：无需构造/析构/赋值行为，可 memcpy；`CanBeCopied` 直接放行（as_datatype.cpp:435），类属性可内联分配（as_objecttype.cpp:538）。
- `asOBJ_ASHANDLE`（1<<7）：值类型但表现得像句柄（如 addon 的 `ref` 类型）。`MakeHandle` 不真正置 `isObjectHandle`，而是置 `isHandleToAsHandleType`（as_datatype.cpp:313-317）；生成 typeId 时也不带 OBJHANDLE 位（as_scriptengine.cpp:5213-5221）。
- `asOBJ_APP_CLASS_*` / `asOBJ_APP_PRIMITIVE` / `asOBJ_APP_FLOAT` / `asOBJ_APP_ARRAY` / `ALLINTS` / `ALLFLOATS` / `ALIGN8`（1<<8 ~ 1<<19）：描述 C++ 侧真实布局，只影响原生调用约定（值传参走寄存器还是内存，见 as_callfunc_*）。

**类型种类与语义**
- `asOBJ_TEMPLATE`（1<<6）：模板类型；实例化时可注册 templateCallback 行为验证子类型。
- `asOBJ_SCRIPT_OBJECT`（1<<21）：脚本声明的类（引擎内部置位）。
- `asOBJ_SHARED`（1<<22）/ `asOBJ_NOINHERIT`（1<<23）/ `asOBJ_ABSTRACT`（1<<29）：跨模块共享 / 禁止继承 / 不可实例化（`CanBeInstantiated` 检查，as_datatype.cpp:409）。
- `asOBJ_FUNCDEF`（1<<24）、`asOBJ_ENUM`（1<<26）、`asOBJ_TYPEDEF`（1<<28）、`asOBJ_LIST_PATTERN`（1<<25）、`asOBJ_TEMPLATE_SUBTYPE`（1<<27）：标记 asCTypeInfo 的具体种类。
- `asOBJ_IMPLICIT_HANDLE`（1<<20）：声明时默认按句柄处理。

## 4. typeId 的位域设计

`angelscript.h:342-363` 的 `asETypeIdFlags`，32 位 int 划分为：

```
bit31 30        29           28  27  26      25 ................. 0
  │  OBJHANDLE  HANDLETOCONST └── MASK_OBJECT ┘   MASK_SEQNBR（序号）
  │  0x40000000 0x20000000    TEMPLATE 0x10000000
  │                           SCRIPTOBJECT 0x08000000
  └─(保留符号位)               APPOBJECT 0x04000000
```

- 0~11 固定分配给 void/bool/int8…double（`asTYPEID_VOID` 到 `asTYPEID_DOUBLE`）。
- 对象类型的序号由 `typeIdSeqNbr++` 惰性分配（as_scriptengine.cpp:5199），并按种类 OR 上 SCRIPTOBJECT / TEMPLATE / APPOBJECT 之一（:5200-5203），存回 `ot->typeId` 并插入 `mapTypeIdToTypeInfo`（:5207）。
- 同一实体的"句柄修饰"不产生新序号，而是在返回时 OR 上 `asTYPEID_OBJHANDLE` / `asTYPEID_HANDLETOCONST`（:5217-5220）。因此宿主判断"是不是句柄"只需 `typeId & asTYPEID_OBJHANDLE`，取实体用 `typeId & (asTYPEID_MASK_OBJECT | asTYPEID_MASK_SEQNBR)`（`GetDataTypeFromTypeId` 正是这样做的，:5231）。

**为什么用 int 而不是指针暴露给宿主？**
1. **稳定与安全**：指针会因类型被销毁（模块卸载、引擎 GC）而悬空；int 句柄查表失败只会得到 null，`GetDataTypeFromTypeId` 查不到就返回无效 `asCDataType`（:5258）。
2. **修饰可编码**：`array<int>@` 和 `array<int>` 是同一个 `asCObjectType`，指针无法区分，而 int 的高位可以叠加句柄/const 修饰。
3. **原始类型无实体**：int/float 根本没有 `asCTypeInfo` 对象（`typeInfo==0`），却也需要一个 id；预留 0~11 恰好覆盖。
4. **序列化友好**：字节码保存/加载、跨进程调试时 int 可直接落盘再重映射。

## 5. 对象类型的内部（asCObjectType）

`as_objecttype.h:101-167`。核心成员：

- **属性表** `asCArray<asCObjectProperty*> properties`（:142）。脚本类添加属性走 `AddPropertyToClass`（as_objecttype.cpp:511）：非 POD 的对象值类型属性**不内联**，改存指针并把类型改成引用（:534-545，避免脚本访问未初始化内存）；然后按 2/4 字节对齐规则补齐（:558-559），`prop->byteOffset = size; size += propSize;`（:571-572）——`size` 既是累计布局游标也是最终对象大小。
- **方法表** `asCArray<int> methods`（:143）：存函数 id（间接指向 `engine->scriptFunctions`），而非指针。
- **behaviours** `asSTypeBehaviour beh`（:155，结构定义在 as_objecttype.h:51-96）：factory/construct/copy/destruct/addref/release/gc 五件套/weakref 等全部是函数 id，0 表示未注册。编译器通过 `asCDataType::GetBehaviour()`（as_datatype.cpp:737）取到它来决定如何生成构造/复制代码。
- **接口列表** `interfaces` + `interfaceVFTOffsets`（:146-147）：实现的每个接口在虚函数表中的起始偏移。
- **虚函数表** `virtualFunctionTable`（:149）与继承链 `derivedFrom`（:148）。构建时机在 as_builder.cpp:3600-3625：派生类先整体继承基类的虚表，覆写的方法替换对应槽位，新虚方法追加到末尾，同时在 `methods` 里放一个 `asFUNC_VIRTUAL` 占位函数记录 `vfTableIdx`。运行时分发在 `asCContext::GetRealFunc`（as_context.cpp:1615）：`asFUNC_VIRTUAL` 直接 `objType->virtualFunctionTable[vfTableIdx]` 一次索引取真函数（:1636-1639）；`asFUNC_INTERFACE` 则按 `signatureId` 在对象实际类型的 methods 中线性查找（:1644-1656）。
- **模板字段** `templateSubTypes` / `acceptValueSubType` / `acceptRefSubType`（:158-160）：每个 `array<int>` 这样的实例是一个**独立的 asCObjectType**，`GetTemplateInstanceType`（as_scriptengine.cpp:3683）先在 `templateInstanceTypes` 里按"模板名 + 子类型数组"查重（:3688-3694），没有才克隆模板类型生成新实体。

## 6. 关键代码走读

**(1) MakeHandle 的准入检查 —— as_datatype.cpp:282-322**
`@` 不是想加就能加：as_datatype.cpp:303-307 要求 typeInfo 存在且带 `asOBJ_REF`（或 TEMPLATE_SUBTYPE / ASHANDLE / FUNCDEF），且不能是 NOHANDLE，SCOPED 仅在返回值场景放行。ASHANDLE 走特殊分支（:313-317）：置 `isHandleToAsHandleType` 而非 `isObjectHandle`——"看起来像句柄的值类型"。这一个函数浓缩了句柄语义的全部规则。

**(2) 栈尺寸 vs 内存尺寸 —— as_datatype.cpp:658-722**
`GetSizeInMemoryBytes`（:658）：句柄恒为指针大小（`4*AS_PTR_SIZE`，:660-661），对象取 `typeInfo->size`，原始类型按 token 硬编码。`GetSizeOnStackDWords`（:709）：引用和对象在栈上都只占一个指针（:714-719）——AngelScript 的求值栈从不内联对象，这就是为何值类型参数也隐式按引用传递；`?`（variant）类型额外 +1 DWORD 存 typeId（:712）。

**(3) typeId 的惰性分配 —— as_scriptengine.cpp:5159-5224**
原始类型直接 switch 返回固定 id（:5166-5184）；对象类型首次请求时在写锁内 `typeIdSeqNbr++` 并按 flags OR 上种类位（:5195-5209），双重检查避免并发重复分配；最后按 `asCDataType` 的修饰临时叠加 OBJHANDLE/HANDLETOCONST 位（:5213-5221）。反向的 `GetDataTypeFromTypeId`（:5226）先掩码取 baseId 查映射表，再用 `MakeHandle`/`MakeHandleToConst` 还原修饰（:5249-5253）。

**(4) 属性偏移计算 —— as_objecttype.cpp:511-586**
注意 :534-545 的注释：非 POD 值类型属性若内联，脚本可能在其构造完成前访问，所以退化为"存引用"（指针），POD 才真正内联 `GetSizeInMemoryBytes()`。这直接决定了脚本类实例的内存布局与 `asCScriptObject` 的构造顺序（第 09 篇脚本对象再展开）。

**(5) 虚方法分发 —— as_context.cpp:1615-1660**
脚本类方法调用编译期绑定到 `asFUNC_VIRTUAL` 占位函数，运行期 `GetRealFunc` 用对象**实际** objType 的虚表一跳定位；接口调用没有全局槽位号，靠 `signatureId` 匹配。对比 C++：AngelScript 的"虚表"挂在类型上而非对象上（对象只存 objType 指针），省内存但接口分发是 O(方法数)。

## 7. 如果你要自己实现

**类型表示的最小方案**：两层就够——
1. 一个引擎持有的 `TypeInfo { name, size, flags, members }` 注册表（interned，指针可当身份用）；
2. 一个值对象 `TypeRef { TypeInfo* base; uint8 modifiers; }`，modifiers 至少要 const 与 handle 两位。
比较类型用 `base 指针相等 + modifiers 相等`；给宿主的 id 用"注册表下标 | 修饰高位"即可复刻 AngelScript 的方案。

**为什么必须区分 handle 与 reference**：两者都是"间接访问"，但语义完全不同——
- **handle（@）**是有所有权的：可存储、可为 null、参与引用计数，生存期由它自己决定；赋值 `@a = b` 改变指向。
- **reference（&）**是无所有权的临时别名：只存在于参数传递/表达式求值期间，不能存进成员，编译器必须保证被引用者在引用存活期内有效（AngelScript 为此甚至会给不安全的引用参数做隐式拷贝）。
若合并成一个概念，要么处处引用计数（性能）、要么处处悬空风险（安全）。AngelScript 把 reference 塞进 `asCDataType` 里连作者都后悔（as_datatype.h:57 的 TODO），你的实现应该一开始就把 reference 放在"表达式值"层而不是"类型"层。

## 8. 动手实验

**(1) 枚举一个类型的方法和属性**（注册 string 与 array addon 后）：

```cpp
void DumpType(asIScriptEngine *engine, const char *decl)
{
    asITypeInfo *ti = engine->GetTypeInfoByDecl(decl);
    if( !ti ) { printf("type not found: %s\n", decl); return; }
    printf("== %s  (typeId=0x%08X, flags=0x%llX, size=%u, ptr=%p)\n",
           ti->GetName(), ti->GetTypeId(), (unsigned long long)ti->GetFlags(),
           ti->GetSize(), (void*)ti);
    for( asUINT n = 0; n < ti->GetMethodCount(); n++ )
        printf("  method: %s\n", ti->GetMethodByIndex(n)->GetDeclaration());
    for( asUINT n = 0; n < ti->GetPropertyCount(); n++ )
        printf("  prop:   %s\n", ti->GetPropertyDeclaration(n));
}
```

**(2) 观察 array<int> 与 array<string> 是两个不同的 asITypeInfo**：

```cpp
DumpType(engine, "array<int>");
DumpType(engine, "array<string>");
```

两次打印的指针与 typeId 序号都不同——因为 `GetTemplateInstanceType`（as_scriptengine.cpp:3683）按子类型查重后为每种参数组合克隆了独立的 `asCObjectType`。再验证修饰不产生新实体：`engine->GetTypeIdByDecl("array<int>@")` 与 `"array<int>"` 的 typeId 只差一个 `asTYPEID_OBJHANDLE`（0x40000000）位，`GetTypeInfoById` 返回同一个指针。

**(3) 进阶**：在 `asCScriptEngine::GetTypeIdFromDataType`（as_scriptengine.cpp:5199）下断点，观察 typeId 是"第一次被谁问到时"才分配的（通常是编译器或宿主首次调 GetTypeIdByDecl）。

## 9. 扩展阅读

- 官方手册 *Data types* 章节，尤其 "Object handles" 一节：https://www.angelcode.com/angelscript/sdk/docs/manual/index.html（搜 object handles；解释了 `@` 与引用计数的脚本侧语义）
- 官方手册 *Registering an object type*：`asOBJ_REF` vs `asOBJ_VALUE` 的选择指南与各 APP_CLASS 标志的含义
- AngelScript 维基 "Type system" 相关页面（https://www.angelcode.com/angelscript/wiki/）
- 本系列第 09 篇（脚本对象 asCScriptObject：属性偏移在运行时如何被使用）、第 12 篇（GC：asOBJ_GC 类型的回收全流程）

---

*系列导航：上一篇 04 — 引擎与模块 | 下一篇 06 — 词法与语法分析*
