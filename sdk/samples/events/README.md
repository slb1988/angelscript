# events — 事件驱动的脚本执行（Suspend 时间片调度）

**难度：★★☆☆☆　前置：tutorial**

脚本的 `main()` 是个**死循环**（每 100ms 打印一个字符），但宿主没有被挂死：宿主用行回调把它切成 10ms 的时间片；按键时在**另一个 context** 上执行事件函数 `OnKeyPress()`/`OnQuit()`，两个 context 通过**脚本全局变量**通信。这是理解 AngelScript 调度模型的钥匙。

## 运行效果

```
--------- （main 循环打印 '-'）
A key was pressed
+++++++++ （OnKeyPress 把字符改成 '+'）
（按 ESC → OnQuit 置 doQuit=true → main 退出循环）
```

## 知识点拆解（对照 source/main.cpp）

### 1. Suspend ≠ Abort（本示例的核心）

tutorial 里行回调超时调用 `Abort()`（终止，不可恢复）；本示例调用 **`Suspend()`**（main.cpp:331-336）：

```cpp
void LineCallback(asIScriptContext *ctx, DWORD *timeOut) {
    if( *timeOut < timeGetTime() )
        ctx->Suspend();   // 暂停，不是终止！
}
```

- `Suspend()` 后 `Execute()` 返回 `asEXECUTION_SUSPENDED`，**context 完整保留执行状态**（程序计数器、栈、局部变量）；
- 再次调用 `Execute()` **从暂停点继续**——不需要重新 Prepare；
- 主循环（main.cpp:170-218）每帧给脚本 10ms 时间片：`timeOut = now + 10; mainCtx->Execute();`。

这就是**协作式多任务**：单线程上，宿主与长时脚本轮流执行。coroutine/concurrent 两个示例都建立在这个机制上。

### 2. 一个模块，两个 context（main.cpp:123-135）

```cpp
asIScriptContext *mainCtx  = engine->CreateContext();  // 跑 main() 死循环
asIScriptContext *eventCtx = engine->CreateContext();  // 随时插入执行事件函数
```

- context 只是执行栈，**模块的全局变量存在模块里**，被所有 context 共享——`OnKeyPress` 改了全局变量 `char`，暂停中的 `main` 恢复后立即看到（这是两个"线程"的通信通道）；
- main 被 Suspend 挂起时，eventCtx 可以安全地执行同模块的其他函数——互不干扰，因为**各自有独立的栈**。

### 3. 优雅退出的三级策略（main.cpp:188-217）

按 ESC 后并不粗暴终止：
1. 先在 eventCtx 上执行 `OnQuit()`，它置全局变量 `doQuit = true`；
2. main 脚本的循环条件 `while(!doQuit)` 让它**自己正常退出**（Execute 返回 FINISHED）；
3. 给 1 秒宽限期，超时还没退出才 `mainCtx->Abort()` 强杀。

这是宿主管理脚本生命周期的推荐模式：先协商（事件+共享状态），后强制（Abort）。

### 4. 多段脚本合并编译（main.cpp:288-302）

`scriptMain` 与 `scriptEvents` 是两个 C++ 字符串，通过两次 `AddScriptSection` 加入**同一个模块**——编译时视为一份代码（可互相引用全局变量），错误信息用段名区分来源。对比 concurrent 示例的"两个模块彻底隔离"。

## 底层原理

- **Suspend 的实现**（as_context.cpp）：`asCContext::Suspend()` 只是设置 `m_doSuspend` 标志；`ExecuteNext()` 的解释循环在执行 `asBC_SUSPEND` 字节码（编译器在每个语句边界插入）时检查该标志，成立则保存寄存器状态（`m_regs.programPointer` 等成员）并从解释循环返回。**脚本的"栈"本来就是引擎堆上分配的内存块**（不是 C++ 线程栈），所以挂起/恢复天然廉价——这是与 Lua 协程同理、与 OS 线程截然不同的设计。
- **恢复**：`Execute()` 检查 context 状态是 `asEXECUTION_SUSPENDED` 就直接从保存的 programPointer 继续 `ExecuteNext()`。
- 行回调本身也是 `asBC_SUSPEND`/`asBC_LINE` 字节码驱动的——没有设回调时这些指令几乎零开销。
- 全局变量存储在 `asCModule` 里（as_module.cpp），每个模块一份，context 通过模块指针访问——所以"跨 context 共享、跨模块隔离"。

## 构建运行

```powershell
& $msbuild projects\msvc2026\events.sln /p:Configuration=Debug /p:Platform=x64 /m
cd bin; .\events.exe    # 按任意键触发事件，ESC 退出
```

## 思考题

1. 把时间片从 10ms 改成 1000ms，按键响应会怎样？（事件处理只在主循环检查 kbhit 时发生——时间片越长响应越迟钝，体会时间片大小的权衡。）
2. 事件函数如果也死循环会怎样？（eventCtx 没设行回调——宿主真的会挂死。代码注释 main.cpp:182-186 承认了这一点，可以练习给它也加上超时。）
3. 为什么两个 context 不会破坏全局变量的一致性？（单线程轮流执行，永远只有一个 context 在跑——协作式并发没有数据竞争。）
