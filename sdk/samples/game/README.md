# game — 完整的游戏脚本集成范例（双向绑定大成）

**难度：★★★★★　前置：前面全部示例**

一个 10×10 字符网格游戏：玩家 `p` 躲避僵尸 `z`，石头 `0` 挡路。**所有对象行为都由脚本控制**——`player.as`/`zombie.as` 里的脚本类实现 AI，宿主 C++ 只提供世界规则（移动碰撞、渲染、生命周期）。这是"C++ 引擎 + 脚本玩法"架构的教科书实现，前面所有示例的技术在此合体。

```
源码结构：
  source/main.cpp       入口：创建两个管理器，跑主循环
  source/gamemgr.cpp    游戏世界：对象表、渲染、输入、carnage 清理
  source/gameobj.cpp    游戏对象：引用计数、移动、消息转发
  source/scriptmgr.cpp  脚本层：全部 AngelScript 交互都封装在这里（精读重点）
  bin/player.as         玩家控制器（响应输入、处理被攻击消息）
  bin/zombie.as         僵尸 AI（追踪玩家、撞墙转向、攻击）
  bin/shared.as         跨模块共享的消息类
```

## 知识点拆解

### 1. 注册引用类型：把 C++ 对象交给脚本（scriptmgr.cpp:51-54）

```cpp
engine->RegisterObjectType("CGameObj", 0, asOBJ_REF);
engine->RegisterObjectBehaviour("CGameObj", asBEHAVE_ADDREF,  "void f()", asMETHOD(CGameObj, AddRef),  asCALL_THISCALL);
engine->RegisterObjectBehaviour("CGameObj", asBEHAVE_RELEASE, "void f()", asMETHOD(CGameObj, Release), asCALL_THISCALL);
```

- `asOBJ_REF`：引用类型，脚本以句柄 `CGameObj@` 持有，生命周期由**侵入式引用计数**管理（gameobj.cpp:42-55 自己实现 AddRef/Release）；
- **故意不注册工厂行为**：脚本无法 `CGameObj()` 创建对象——只有宿主能创建，脚本只能引用。权限控制靠"不注册"实现；
- 对比值类型（`asOBJ_VALUE`，如 string）：值类型按值拷贝、栈上构造，需要注册构造/析构/赋值行为，见 add_on/scriptstdstring 的实现。

### 2. weakref：脚本安全引用可能死亡的对象（scriptmgr.cpp:54 + zombie.as:19-26）

```cpp
engine->RegisterObjectBehaviour("CGameObj", asBEHAVE_GET_WEAKREF_FLAG, "int &f()", asMETHOD(CGameObj, GetWeakRefFlag), ...);
```

宿主对象持有一个 `asILockableSharedBool`（gameobj.cpp:34-40），析构时置 true（gameobj.cpp:23-28）。脚本侧（zombie.as）：

```angelscript
const_weakref<CGameObj> playerRef;       // 弱引用不增加计数、不阻止对象死亡
const CGameObj @player = playerRef;      // 每次使用前"升格"为强引用，死了得 null
if( player is null ) { @player = game.FindObjByName('player'); @playerRef = player; }
```

僵尸缓存玩家引用又不阻止玩家对象销毁——游戏里"记住另一个对象"的标准安全模式。

### 3. 声明字符串的高级修饰

- **属性访问器**：`"int get_x() const property"`（scriptmgr.cpp:57）→ 脚本写 `obj.x` 像字段，实际调 GetX()。只注册 get 即只读；
- **自动句柄 `@+`**：`"const CGameObj @+ FindObjByName(...)"`（scriptmgr.cpp:97）→ 引擎自动帮返回值 AddRef，C++ 实现（gamemgr.cpp:186）无需关心脚本引用计数；`"void Send(ref msg, const CGameObj @+ to)"` 同理自动管理入参；
- **const 句柄**：FindObjByName 返回 `const CGameObj@`——脚本只能读别人的对象（x/y），只能改自己的 `self`（非 const）。**用类型系统实施游戏规则**；
- **单例**：`RegisterObjectType("CGameMgr", 0, asOBJ_REF | asOBJ_NOHANDLE)` + `RegisterGlobalProperty("CGameMgr game", gameMgr)`（scriptmgr.cpp:82-85）——NOHANDLE 禁止脚本存句柄，只能通过全局名 `game` 即用即调，不涉引用计数。

### 4. 脚本类实现宿主接口（本示例的架构核心）

```cpp
engine->RegisterInterface("IController");                  // 宿主声明空接口（scriptmgr.cpp:77）
```

```angelscript
class CZombie : IController { CZombie(CGameObj @obj) {...} void OnThink() {...} }
```

宿主发现并实例化控制器类（`GetControllerScript`，scriptmgr.cpp:117-215）：

1. 每个对象类型一个模块：`builder.StartNewModule(engine, "zombie")` 加载 zombie.as——**模块隔离让不同 AI 脚本互不干扰**；
2. 遍历模块类型找实现了 IController 的类：`mod->GetObjectTypeByIndex(n)` → `type->GetInterfaceCount()/GetInterface(i)`——反射发现，脚本不用注册自己；
3. 拿工厂函数：`type->GetFactoryByDecl("CZombie@ CZombie(CGameObj @)")`——**脚本类的构造器在宿主侧叫工厂**，签名约定构造时注入 owner；
4. 缓存 `OnThink`/`OnMessage` 方法指针（可选实现——`GetMethodByDecl` 返回 null 就不调，接口不强制方法，见 scriptmgr.cpp:76 注释的性能考量）；
5. **缓存塞进类型的 UserData**：`type->SetUserData(ctrl)`（scriptmgr.cpp:212）→ 运行时 `object->GetObjectType()->GetUserData()` O(1) 取回（scriptmgr.cpp:253）——避免每帧按名字查找的关键优化。

实例化与调用（scriptmgr.cpp:217-280）：

```cpp
ctx->Prepare(factoryFunc); ctx->SetArgObject(0, gameObj); ctx->Execute();
asIScriptObject *obj = *(asIScriptObject**)ctx->GetAddressOfReturnValue(); obj->AddRef();
// 调方法：Prepare(onThinkMethod); ctx->SetObject(obj); Execute();
```

`asIScriptObject` 是脚本类实例在宿主侧的通用句柄；调方法比调全局函数多一步 `SetObject`（绑定 this）。

### 5. 跨模块通信：shared 类 + ref 泛型句柄

player 和 zombie 在**不同模块**，模块间类型默认互不相认。两件武器打通它（shared.as + scriptmgr.cpp:45,68）：

- **`shared class CMessage`**：shared 关键字让所有模块引用**同一个类型实体**（zombie 构造的 CMessage，player 能 cast 成功）；
- **`ref` 泛型句柄**（add_on/scripthandle）：`Send(ref msg, ...)` 用 ref 参数让宿主转发任意脚本对象而**无需知道其类型**（gameobj.cpp:98 的 C++ 只是把 CScriptHandle 递给对方）；接收方 `cast<CMessage>(m)`（player.as:37）动态还原类型。

消息流全链路：`zombie.as: self.Send(CMessage('Attack'), player)` → C++ `CGameObj::Send` → `scriptMgr->CallOnMessage` → `player.as: OnMessage` 里 cast 判断 → `self.Kill(); game.EndGame(false)`。

### 6. 生命周期与循环引用的工程处理

对象死亡不直接 delete（gamemgr.cpp:81-93 + gameobj.cpp:57-69）：`isDead` 标记 → 帧末 `DestroyAndRelease()`——先主动放掉 controller 引用**打破循环**（C++ 对象持有脚本控制器，脚本控制器持有 `CGameObj@ self`——典型的跨语言循环引用），再 Release。宿主打破循环是比依赖 GC 更确定性的做法；GC 是兜底（脚本对象参与引擎 GC，宿主 CGameObj 不参与）。

## 底层原理

- **脚本类实例的内存布局**：`asCScriptObject`（as_scriptobject.cpp）——头部 + 按声明顺序排列的属性；工厂函数是编译器自动生成的字节码函数（分配对象 + 调构造器）。`GetFactoryByDecl` 查的就是这些合成函数。
- **接口与虚方法**：脚本类方法调用走 `asCObjectType` 的虚函数表（as_objecttype.cpp）；`RegisterInterface` 创建一个无方法的接口类型，实现检查发生在 Build 期。
- **shared 类型**：存放在引擎级共享作用域（as_typeinfo.cpp 中 sharedness 处理），各模块 Build 时先查找已有同名 shared 实体做签名校验，一致则复用——所以修改 shared 类后所有用到它的模块都要重编。
- **属性访问器**：纯编译期糖——`obj.x` 被 asCCompiler 改写为 `obj.get_x()` 调用（搜 as_compiler.cpp 的 property accessor 处理）。
- **auto handle @+**：编译器在调用点自动插入 AddRef/Release 字节码，as_callfunc.cpp 按声明标志处理。

## 构建运行

```powershell
& $msbuild projects\msvc2026\game.sln /p:Configuration=Debug /p:Platform=x64 /m
cd bin; .\game.exe    # u/d/l/r 移动，q 退出；工作目录必须是 bin（要读 player.as）
```

## 思考题

1. 新建 `stone.as` 给石头写个控制器（比如随机漂移）——体会"有 .as 就有行为，没有就是静物"的数据驱动设计（scriptmgr.cpp:142-146 对文件不存在的处理）。
2. 把 zombie.as 的 `const_weakref` 换成普通 `CGameObj@` 缓存，玩家死后会发生什么？（对象被引用锁住不释放；内存不泄漏但"幽灵对象"永存——这正是 weakref 要解决的问题。）
3. 为什么 `OnThink`/`OnMessage` 用缓存的 `asIScriptFunction*` 而工厂每次通过 SController 结构取？都缓存在哪一层？（都在 SController，挂在 asITypeInfo 的 UserData 上——类型级缓存，实例无关。）
4. 挑战：给游戏加"血量"——需要动 CGameObj 注册、player.as/zombie.as、消息协议三处，正好走一遍完整的双向绑定开发流。
