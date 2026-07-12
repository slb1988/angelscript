# concurrent — 多脚本协作式并发（sleep 调度与模块隔离）

**难度：★★★☆☆　前置：events、coroutine**

两个**互相独立**的脚本"同时"运行：script1 每 333ms 打印一次，script2 每 1000ms 打印一次。没有 OS 线程——还是单线程上的 context 调度，但调度依据从 coroutine 的"轮流"升级为"按时间唤醒"（`sleep()`）。

## 运行效果

```
A :0
 B:0
A :1
A :2
 B:1     ← A 打印 3 次，B 打印 1 次（333ms vs 1000ms）
A :3
...
```

## 知识点拆解（对照 source/main.cpp）

### 1. 模块 = 命名空间隔离（main.cpp:228-259）

两个脚本被编译进**两个模块**：

```cpp
asIScriptModule *mod = engine->GetModule("script1", asGM_ALWAYS_CREATE);
// ... AddScriptSection + Build
mod = engine->GetModule("script2", asGM_ALWAYS_CREATE);
// ... AddScriptSection + Build
```

两个脚本都有 `int count` 和 `void main()`——**同名不冲突**，因为每个模块有独立的全局变量存储和函数表。对比 events 示例（两段源码进同一模块共享全局变量），这是刻意展示的另一极：

| | events | concurrent |
|--|--------|-----------|
| 源码组织 | 两段 → 一个模块 | 两段 → 两个模块 |
| 全局变量 | 共享（用作通信） | 隔离（互不可见） |
| 适用场景 | 一个脚本系统的多个入口 | 互不相关的独立脚本（如不同 NPC、不同插件） |

### 2. sleep()：时间驱动的调度（contextmgr）

```cpp
contextManager.SetGetTimeCallback((TIMEFUNC_t)&timeGetTime);  // 告诉调度器怎么读时钟
contextManager.RegisterThreadSupport(engine);                  // 注册 sleep() 给脚本
contextManager.AddContext(engine, script1_main);
contextManager.AddContext(engine, script2_main);
for(;;) contextManager.ExecuteScripts();
```

`sleep(333)` 的实现（add_on/contextmgr/contextmgr.cpp）：给当前 context 记录唤醒时间戳 → `Suspend()`。`ExecuteScripts()` 每次只挑**唤醒时间已到**的 context 执行。脚本视角是阻塞式 sleep，宿主视角没有任何线程阻塞——这就是游戏脚本里最常用的"绿色线程"模型（比如 NPC 脚本 `walkTo(x,y); sleep(2000); say("hi");` 的写法）。

### 3. RegisterThreadSupport vs RegisterCoRoutineSupport

contextmgr 提供两组能力，本示例与 coroutine 示例各用一组：
- **Thread 支持**：`sleep()` + 每个 AddContext 是一个独立"线程"；
- **CoRoutine 支持**：`yield()`/`createCoRoutine()` + 协程组内轮转。

可以同时注册（asrun 就是），"线程"之间按时间调度，线程内部还能开协程。

## 底层原理

- 与真正的多线程对比：AngelScript 引擎本身**支持**真 OS 多线程（每个线程跑自己的 context，引擎内部用原子引用计数 as_atomic.cpp 和临界区 as_criticalsection.h 保护共享状态，见 tests/test_multithread），但**同一个 context 永远只能被一个线程执行**。本示例的"并发"全部在单线程内，因此脚本之间没有数据竞争，不需要锁——对游戏逻辑来说这通常是更好的模型。
- `sleep` 挂起后 context 的状态依然是 `asEXECUTION_SUSPENDED`，与 events 的行回调挂起、coroutine 的 yield 挂起在引擎层是**同一个机制**（as_context.cpp 的 Suspend），差别只在"谁在什么时机调用 Suspend、调度器按什么规则 Resume"。三个示例读完应能总结出：**引擎只提供可挂起的 VM，一切调度策略都是宿主/add_on 层的自由发挥**。
- 模块隔离的实现：`asCScriptEngine` 持有模块表（as_scriptengine.cpp），每个 `asCModule`（as_module.cpp）有自己的 `scriptFunctions`、`scriptGlobals`、类型表；编译时符号查找只在本模块 + 引擎注册项范围内进行。

## 构建运行

```powershell
& $msbuild projects\msvc2026\concurrent.sln /p:Configuration=Debug /p:Platform=x64 /m
cd bin; .\concurrent.exe    # 按任意键退出
```

## 思考题

1. 若把两个脚本编进同一个模块会发生什么？（Build 报错：`int count` 和 `void main()` 重复定义。）
2. `sleep(0)` 是什么语义？（让出一次调度机会，等价于 yield 到其他到期的 context。）
3. 主循环没有 Sleep，会空转烧 CPU——怎么改进？（让 ExecuteScripts 返回最近的唤醒时间，宿主据此 Sleep；读 contextmgr 源码想想接口怎么设计。）
