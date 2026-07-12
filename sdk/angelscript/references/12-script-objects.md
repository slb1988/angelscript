# 12 — 脚本类对象模型（asCScriptObject）

> **概括**：脚本里写 `class Enemy : IEntity { int hp; ... }`，在 C++ 侧每个实例就是一块
> "asCScriptObject 头 + 属性区" 的连续内存。本篇精读 as_scriptobject.cpp/h，回答四个问题：
> 这块内存长什么样、`Enemy()` 一句话背后发生了什么、继承/接口的虚表怎么分发、
> 引用计数与 GC/weakref 如何协作。最后看方法委托（delegate）这个"对象+函数"的胶囊。
>
> **难度**：★★★★☆　**前置**：第 05 篇（类型系统 asCObjectType）、第 10 篇（编译器）、第 11 篇（VM 执行）。

## 1. 模块定位

脚本类是 AngelScript 唯一"运行时才知道布局"的类型：宿主注册类在 C++ 编译期就有固定
sizeof，而脚本类的成员由脚本代码决定。引擎的解法是**一个 C++ 类适配所有脚本类**：
`asCScriptObject`（as_scriptobject.h:73）继承公开接口 `asIScriptObject`，本身只含对象头；
真正的成员数据紧跟在 `this` 之后，靠 `objType`（asCObjectType）里的属性表按字节偏移解释。

samples/game 里 `asIScriptObject *controller = ...` 拿到的就是这个东西；宿主枚举属性
（`GetPropertyCount/GetAddressOfProperty`）、weakref 检查存活，底层全在本篇文件里。

引擎启动时，`RegisterScriptObject()`（as_scriptobject.cpp:284-323）注册一个名为 `"$obj"`
的内部类型 `scriptTypeBehaviours`，flags 为 `asOBJ_SCRIPT_OBJECT | asOBJ_REF | asOBJ_GC`
（as_scriptobject.cpp:290），把 AddRef/Release/opAssign/GC 五件套/weakref 行为挂上去——
所有脚本类共享这套行为，只是各自的 asCObjectType 不同。

## 2. 内存布局

**对象头**就是 `asCScriptObject` 的成员（as_scriptobject.h:131-152）：

| 字段 | 行 | 说明 |
|---|---|---|
| `asCObjectType *objType` | h:134 | 类型指针（public，方便 JIT 用 asOFFSET 直取） |
| `asCAtomic refCount` | h:137 | 原子引用计数 |
| `asBYTE gcFlag:1` | h:138 | GC 标记位（第 14 篇的 SetFlag/GetFlag） |
| `asBYTE hasRefCountReachedZero:1` | h:139 | 防"复活"标记 |
| `bool isDestructCalled` | h:140 | 保证脚本析构器只跑一次 |
| `SExtra *extra` | h:146-152 | 懒分配的 weakRefFlag + userData（多数对象为 0） |

**属性区**从 `this+1` 开始。builder 为每个脚本类初始化
`st->size = sizeof(asCScriptObject)`（as_builder.cpp:2545），之后每声明一个成员就调
`asCObjectType::AddPropertyToClass()`（as_objecttype.cpp:511-586）追加：

- 原始类型和 **POD 值类型**按 `GetSizeInMemoryBytes()` 内联（as_objecttype.cpp:538-539, 554）；
- **非 POD 值类型、引用类型、句柄**一律只占一个指针位——非 POD 值类型还会被
  `prop->type.MakeReference(true)` 改成"按引用存"（as_objecttype.cpp:540-545），因为脚本
  可能在成员初始化前就访问它，指针为 null 可检测，半初始化的内联对象不可检测；
- funcdef 成员必为句柄，占 `AS_PTR_SIZE*4` 字节（as_objecttype.cpp:547-552）；
- 2/4 字节对齐修正后 `prop->byteOffset = size; size += propSize`（as_objecttype.cpp:556-572）。

所以 `asITypeInfo::GetSize()` 返回的就是"头 + 全部属性"的总字节数。读属性时要按存储方式
解引用：`GetAddressOfProperty()`（as_scriptobject.cpp:802-814）对"按引用存"的对象属性会
先 `*(void**)` 取出真实地址。

## 3. 对象诞生记：脚本 `Enemy()` 到底做了什么

脚本类是引用类型，`Enemy e();`/`Enemy@ e = Enemy();` 编译成**调用工厂函数**。工厂不是
宿主 C++ 函数，而是编译器生成的一小段字节码：

1. **builder 兜底生成默认构造器+工厂**：`asCBuilder::AddDefaultConstructor()`
   （as_builder.cpp:4638-4698）——先注册一个签名为 `Enemy@ Enemy()` 的脚本函数设为
   `beh.factory`（as_builder.cpp:4685），随即用 `asCCompiler::CompileFactory()` 编译它。
2. **工厂字节码**：`CompileFactory()`（as_compiler.cpp:331-405）生成
   `PSF 局部变量 → 逐个转推参数 → asBC_ALLOC(objType, 对应构造器id) → LOADOBJ → RET`
   （as_compiler.cpp:354-389）。即"分配 + 调构造器 + 把句柄放到返回寄存器"。
3. **asBC_ALLOC 执行**（as_context.cpp:3138-3179，脚本对象分支）：
   - `m_engine->CallAlloc(objType)` 按 `objType->size` 分配整块内存（3152）；
   - `ScriptObject_Construct()` placement-new 出对象头（3155 → as_scriptobject.cpp:325-328）；
   - 把指针压栈后 `CallScriptFunction(构造器)` 进入脚本构造器字节码（3169）。
4. **对象头构造器** `asCScriptObject::asCScriptObject()`（as_scriptobject.cpp:335-391）：
   refCount 置 1、`objType->AddRef()`、若 `asOBJ_GC` 则登记进 GC（345-346），然后一句
   `memset((void*)(this+1), 0, objType->size - sizeof(asCScriptObject))`（350）把整个属性区清零——
   这就是"所有句柄成员天然为 null"的出处。
5. **构造器字节码**再做成员初始化和用户代码（见第 4 节的编译端）。

**宿主侧**：`asCScriptEngine::CreateScriptObject()`（as_scriptengine.cpp:5457-5567）对脚本类
走 `ScriptObjectFactory()`（as_scriptobject.cpp:41-122）：优先复用当前活动 context 的
`PushState()` 嵌套调用，否则 `RequestContext()`，然后 `Prepare(beh.factory)` + `Execute()`
循环（禁止挂起，86-88），成功后 `GetReturnAddress()` 拿句柄并补一次 `AddRef()`（111-114）。
另有 `CreateUninitializedScriptObject()`（as_scriptengine.cpp:5655-5673）走
`ScriptObject_ConstructUnitialized`，只建头不跑构造器，供反序列化用。

## 4. 继承与多态

### 基类成员布局继承（builder）

`asCBuilder::CompileClasses()` 里，派生类先**原样复制基类的属性表**：
as_builder.cpp:3530-3536 逐个 `AddPropertyToClass(基类属性)`，并断言
`prop->byteOffset == baseType->properties[p]->byteOffset`（3535）——因为追加顺序一致、
初始 size 相同，偏移天然相同。这保证了"基类指针视角"和"派生类视角"访问同名成员
用同一偏移，单继承布局零成本。接口也一并继承（3521-3525）。

### 虚函数表填充

AngelScript 的脚本方法**全部虚化**，没有 C++ 那种非虚方法：

1. 复制基类方法时：若派生类有同签名方法则视为 override，把派生函数推入
   `ot->virtualFunctionTable`（as_builder.cpp:3588-3590；final 检查在 3581-3586，返回值
   协变检查在 3574-3579）；否则复用基类 VFT 里的条目（3600-3601）。
2. 派生类自己的新方法：从 `methods` 移入 VFT，再用 `CreateVirtualFunction()` 造一个
   `asFUNC_VIRTUAL` 占位函数放回 `methods`，其 `vfTableIdx` 记录 VFT 下标
   （as_builder.cpp:3614-3628；占位函数构造在 4472-4501）。
   于是 `methods[]` 里全是"虚方法描述符"，真身都在 `virtualFunctionTable[]`。

### 接口方法映射

每个实现的接口在 VFT 尾部追加一段"接口块"：`ot->interfaceVFTOffsets.PushLast(offset)`
记录该接口块的起始下标，然后按接口方法声明顺序，用 `signatureId` 匹配找到类里的真实
实现推入（as_builder.cpp:3630-3677）。

### 运行时分发

编译器对接口/虚方法调用发出 `asBC_CALLINTF`（处理入口 as_context.cpp:3933-3946），
落到 `asCContext::CallInterfaceMethod()`（as_context.cpp:2171-2231）：

- 从栈上对象取出 `objType`；
- 若被调函数属于接口：线性搜 `objType->interfaces[]` 找到接口序号，取
  `interfaceVFTOffsets[n]` 作基址，`realFunc = objType->virtualFunctionTable[func->vfTableIdx + offset]`
  （2216）；
- 若是普通虚方法：直接 `virtualFunctionTable[func->vfTableIdx]`（2226）。

宿主 API 同样处理这层间接：`GetMethodByDecl/ByName/ByIndex` 默认把 `asFUNC_VIRTUAL`
解析为 VFT 里的真身（as_objecttype.cpp:264-331，`getVirtual=false` 分支）。

### super 调用（compiler）

- 编译器生成的默认构造器：`CompileDefaultConstructor()`（as_compiler.cpp:274-329）——
  先初始化无显式表达式的成员，再 `PSF 0 / RDSPtr / asBC_CALL 基类 beh.construct`
  （300-302），最后初始化带表达式的成员。**基类构造顺序 = 先派生类默认成员、再基类、
  再派生类显式成员**，构造链靠字节码里的普通 CALL 串起来。
- 用户写的构造器：`super(...)` 被识别为基类构造器调用——名字命中 `SUPER_TOKEN` 时
  `funcs = outFunc->objectType->derivedFrom->beh.constructors`（as_compiler.cpp:13291-13295），
  且禁止在循环/switch 里调用（13297-13308）、禁止重复调用（13473-13477 置
  `m_isConstructorCalled`）。没写 super 时编译器自动插入基类默认构造调用
  （as_compiler.cpp:836-850）。

## 5. 析构与 GC 协作

**Release 路径**（as_scriptobject.cpp:613-660）分三步走：

1. refCount==1 且存在 weakRefFlag → 先 `weakRefFlag->Set(true)` 宣告死亡（623-631）；
2. refCount==1 且未调过析构 → `CallDestructor()`（634-638）：借 context 沿
   `ot = ot->derivedFrom` 链**从派生到基类**依次执行每层的脚本 `beh.destruct`
   （as_scriptobject.cpp:676-734），异常被吞掉以尽量释放资源（721-729）；
3. `atomicDec()` 到 0 → 置 `hasRefCountReachedZero` 后 `Destruct()`（649-654）：显式调
   C++ 析构 `~asCScriptObject()` 再 `userFree(this)`（393-406）。

C++ 析构器（as_scriptobject.cpp:408-493）把属性**从后往前**释放：按引用存的对象成员
`FreeObject`（Release 或 destruct+free，1090-1105），funcdef 成员 Release，最后
`objType->Release()`。若析构期间有人 `AddRef()`，`hasRefCountReachedZero` 会触发
"resurrecting object" 错误日志（592-606）。

**GC 行为接口**（呼应第 14 篇）：脚本类恒带 `asOBJ_GC`，出生即登记
（as_scriptobject.cpp:345-346）。GC 探测循环引用靠：

- `EnumReferences()`（816-843）：枚举所有句柄/funcdef 成员，并递归进 GC 值类型成员；
- `SetFlag/GetFlag`（764-772）：操作 1 bit 的 `gcFlag`，AddRef/Release 都会把它清掉
  （609、616）表示"存活期间被触碰过"；
- `ReleaseAllHandles()`（845-888）：GC 判定死环后强制清空全部句柄成员打破循环。

**opAssign 拷贝**：默认注册的 `int &opAssign(int &in)`（as_scriptobject.cpp:296）落到
`CopyFromAs()`（902-1046）——先验证 `other->objType->DerivesFrom(this 的类型)` 否则抛
"value assign mismatch" 异常（906-912）；默认拷贝时先递归拷贝基类部分、再逐属性
按存储类别选择 CopyObject/CopyHandle/memcpy（922-960）；若脚本重写了 opAssign 则
起 context 调脚本版本（964-1041）。

## 6. weakref 与委托

**weakRefFlag**：本质是一个引用计数的共享 bool `asCLockableSharedBool`
（as_scriptobject.h:54-71，实现 as_scriptobject.cpp:1139-1179）。对象与所有 weakref
共同持有它；对象死亡瞬间置 true（Release 里的 623-631）。`GetWeakRefFlag()`
（as_scriptobject.cpp:495-522）懒分配：全局排它锁下建 `SExtra` 和 flag，refCount 已到 0
则直接返回 null。samples/game 的 `CGameObj::GetWeakRefFlag()`
（sdk/samples/game/source/gameobj.cpp:34-39）就是宿主类照抄这套协议，用
`asCreateLockableSharedBool()`（as_scriptobject.cpp:1184-1187）造 flag。

**方法委托**：脚本里 `CB@ cb = CB(obj.method);` 创建的是一个
`asFUNC_DELEGATE` 形态的 asCScriptFunction，靠两个字段捆绑"对象+方法"：
`objForDelegate` / `funcForDelegate`（as_scriptfunction.h:346-348）。

- 工厂：`CreateDelegate()`（as_scriptfunction.cpp:172-187）new 一个无 id、不进
  `engine->scriptFunctions` 的函数对象，`MakeDelegate()`（190-208）对方法 AddRef、对
  对象 AddRefScriptObject，并复制签名；`dontCleanUpOnException = true` 因为参数只是
  转发。编译器通过一个名字非法（`DELEGATE_FACTORY`）、脚本无法直接调用的内建全局
  函数触达它（as_scriptfunction.cpp:151-169，返回类型事后改成 `$func@`）。
- 宿主 API：`asCScriptEngine::CreateDelegate()`（as_scriptengine.cpp:641-656）。
- 调用：`asBC_CALLPTR` 执行时发现 `asFUNC_DELEGATE`，把 `objForDelegate` 压栈当 this，
  然后按方法真身走 `CallSystemFunction` 或 `CallInterfaceMethod(funcForDelegate)`
  （as_context.cpp:4398-4421）——委托保存的是虚方法描述符，调用时仍走虚表，故指向
  基类方法的委托也能分发到派生类实现。

## 7. 关键代码走读

1. **as_scriptobject.cpp:335-391 对象头构造**：注意 350 行 memset 从 `this+1` 开始——
   属性区紧贴 C++ 对象之后，这就是"头+属性区"布局的最直接证据；`doInitialize=false`
   分支（376-390）给非句柄引用成员预分配未初始化对象，供序列化恢复。
2. **as_compiler.cpp:331-405 CompileFactory**：工厂只有五步字节码；384 行
   `byteCode.Alloc(asBC_ALLOC, dt.GetTypeInfo(), constructor, ...)` 把"哪个类型+哪个构造器"
   烧进指令。对照 as_compiler.cpp:17888-17892，值类型堆上构造也复用同一条指令。
3. **as_context.cpp:3138-3179 asBC_ALLOC（脚本对象分支）**：CallAlloc →
   ScriptObject_Construct → CallScriptFunction 三连；3160-3161 先把返回槽写好，
   因为构造器可能挂起/嵌套。
4. **as_builder.cpp:3516-3677 类的"装配车间"**：属性偏移继承（3530-3536）、override
   进 VFT（3588-3590）、asFUNC_VIRTUAL 占位（3614-3628）、接口块与
   `interfaceVFTOffsets`（3630-3677），一个函数看完整个继承模型的编译期准备。
5. **as_context.cpp:2171-2231 CallInterfaceMethod**：运行时分发的全部逻辑；2216 行
   `vfTableIdx + offset` 的加法就是"接口方法槽位 + 该接口在本类 VFT 的基址"。
6. **as_scriptobject.cpp:613-752 Release + CallDestructor**：weakref 宣告 → 析构链
   （676-734 的 `ot = ot->derivedFrom` 循环）→ 计数归零释放，一条龙。

## 8. 如果你要自己实现

最小可行的"动态布局脚本对象"：

1. **一个通用头**：`{ TypeDesc* type; int refCount; }`，实例大小 = `sizeof(头) + Σ成员`。
   TypeDesc 里存 `vector<{name, typeId, byteOffset}>`，注册成员时累加偏移并做对齐。
2. **只允许两类存储**：POD 内联、其余一律存指针。这条向 AngelScript 学
   （as_objecttype.cpp:534-545 的注释解释了为什么非 POD 内联是坑：半初始化不可检测）。
3. **构造 = 分配 + memset + 跑初始化函数**；析构 = 反向遍历成员表释放指针成员。
4. **先做无继承版本**。加继承时关键一步是"复制基类成员表且保持偏移"，让基类代码
   可直接操作派生实例；方法全虚化（方法表 = 数组 + 下标），override 即覆写槽位。
5. **最后加接口**：别做成 C++ 多虚表，学 AngelScript 把接口方法**追加**到同一张表尾部、
   记每个接口的基址偏移——对象里不用存任何额外指针，代价是调用时查一次接口序号。

## 9. 动手实验

1. **枚举并直接改属性**：脚本里写 `class Data { int a = 1; double b = 2.5; string s = "x"; Data@ next; }`，
   宿主 `CreateScriptObject` 后循环 `GetPropertyCount/GetPropertyName/GetAddressOfProperty`，
   打印每个属性的地址相对 `this` 的差值，对照 `asCObjectType::GetProperty` 返回的 offset；
   然后 `*(int*)addr = 42` 直接改值，再从脚本读出来验证。注意 string 成员拿到的地址
   已被 GetAddressOfProperty 解过一层引用（as_scriptobject.cpp:807-813）。
2. **观察 GetSize 变化**：依次声明 `class A{}`、`class B{int8 x;}`、`class C{int8 x; int y;}`、
   `class D{string s;}`，打印 `GetTypeInfo()->GetSize()`。可验证：空类 = sizeof(asCScriptObject)
   （as_builder.cpp:2545）；int8 后跟 int 会看到 4 字节对齐补齐（as_objecttype.cpp:558-559）；
   string（非 POD 值类型）只贡献一个指针大小而非 sizeof(string)。
3. **进阶**：在 `Enemy` 的构造器里打断点（或用 asIScriptContext 的行回调），观察
   `Enemy()` 一次调用的调用栈：factory 字节码 → asBC_ALLOC → 构造器 → 基类构造器，
   与第 3 节的流程图逐帧对上。

## 10. 扩展阅读

- 官方手册《Script classes》与《Inheritance and polymorphism》章节
  （angelcode.com/angelscript/sdk/docs/manual/doc_script_class.html）——脚本作者视角的语义。
- 官方手册《Weak references》与 add-on `weakref` 的实现（sdk/add_on/weakref/），
  是 asCLockableSharedBool 协议的标准消费者。
- Wikipedia: Virtual method table——对比 C++ 的"对象内 vptr"与 AngelScript 的
  "类型对象持表 + 指令查表"两种设计的取舍。
- 下一篇（第 14 篇）GC 会回到 EnumReferences/ReleaseAllHandles，看这些回调如何被
  增量式循环检测驱动。
