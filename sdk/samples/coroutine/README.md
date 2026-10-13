# coroutine — 脚本内协程（yield / createCoRoutine）

**难度：★★★☆☆　前置：events**

events 示例里"挂起/恢复"由宿主控制；本示例把这个能力**交给脚本自己**：脚本调用 `createCoRoutine()` 创建协程、调用 `yield()` 主动让出执行权。调度工作由 **contextmgr** add_on 完成。

## 运行效果

```
A :9
 B:2      ← 协程 thread2 与 main 交替执行
A :8
 B:1
A :7
 B:0
A :6
A :5 ...  ← thread2 结束后 main 独占
```

## 脚本侧代码（内嵌在 main.cpp:180-205）

```angelscript
void main() {
    for(;;) {
        int count = 10;
        createCoRoutine(thread2,
            dictionary = {{'count', 3}, {'str', ' B'}});   // 创建协程并传参
        while( count-- > 0 ) {
            Print('A :' + count + '\n');
            yield();                                        // 让出执行权
        }
    }
}
void thread2(dictionary @args) {
    int count = int(args['count']);        // 从 dictionary 取参数
    string str = string(args['str']);
    while( count-- > 0 ) {
        Print(str + ':' + count + '\n');
        yield();
    }
}
```

## 知识点拆解

### 1. contextmgr：官方的协程调度器（add_on/contextmgr/）

宿主侧只有三步（main.cpp:88-143）：

```cpp
CContextMgr contextMgr;
contextMgr.RegisterCoRoutineSupport(engine);   // 注册 yield()/createCoRoutine() 给脚本
contextMgr.AddContext(engine, mainFunc);       // 把入口函数加入调度
for(;;) contextMgr.ExecuteScripts();           // 每帧驱动调度器
```

`CContextMgr` 内部维护一组 context 链：
- `yield()` 的实现 = 拿到当前活动 context（`asGetActiveContext()`）→ `Suspend()` → 调度器切换到链上下一个 context 继续 `Execute()`；
- `createCoRoutine(func, args)` = 新建一个 context，`Prepare(func)`，`SetArgObject(0, dict)`，挂到当前协程组；
- 协程函数执行到 FINISHED，调度器把它的 context 回收。

**本质：协程 = context + Suspend + 一个轮转调度器**。引擎核心没有"协程"概念，全部由 add_on 用公开 API 搭出来——这是 AngelScript "机制在引擎、策略在宿主" 设计哲学的典型体现。

### 2. funcdef：函数句柄类型

`createCoRoutine` 的注册声明（见 contextmgr.cpp）是：

```
funcdef void coroutine(dictionary@);
void createCoRoutine(coroutine @func, dictionary @args);
```

`funcdef` 声明一个函数签名类型，脚本里传 `thread2` 时实际传递的是 `asIScriptFunction*` 句柄——脚本层的函数指针。宿主用 `ctx->Prepare(func)` 直接执行它。

### 3. dictionary：异构参数包（add_on/scriptdictionary/）

- `dictionary = {{'count', 3}, {'str', ' B'}}` 是匿名初始化列表语法；
- 值以 (typeId, value) 对存储，取出时 `int(args['count'])` 做显式转换；
- 这解决了"协程入口签名固定，但不同协程需要不同参数"的问题——类似 void* 用户数据，但类型安全且脚本友好。

### 4. 多值观察

- `Sleep(100)`（main.cpp:142）只是宿主减速方便观察，与调度无关；
- `contextMgr.AbortAll()`：按键退出时终止所有协程——注意所有协程的 context 都被统一管理，没有"孤儿"。

## 底层原理

- **为什么 AngelScript 协程如此廉价**：`asCContext` 的调用栈是引擎自己管理的堆内存块（as_context.cpp 中按需增长的栈内存），不是 OS 线程栈。挂起 = 保存几个寄存器成员变量；切换 = 换一个 context 指针调 Execute。没有 setjmp/ucontext/fiber，纯粹的解释器层实现。
- `asGetActiveContext()`（angelscript.h）返回**当前线程**正在执行的 context（TLS 存储）——注册给脚本的宿主函数（如 yield）用它拿到"是谁在调我"。这是宿主函数与执行环境交互的标准入口，asrun/game 里反复出现。
- dictionary 里存对象句柄时参与 GC：dictionary 实现了引用类型的全套 GC 行为（枚举引用、释放引用），后面读 as_gc.cpp 时可以拿它当参照。

## 构建运行

```powershell
& $msbuild projects\msvc2026\coroutine.sln /p:Configuration=Debug /p:Platform=x64 /m
cd bin; .\coroutine.exe    # 按任意键退出
```

## 思考题

1. main 每轮 for 循环都 createCoRoutine 一个新 thread2，为什么同时只看到一个 B 协程？（上一个 thread2 计数 3 次后 FINISHED 被回收，恰好 main 才数完 10。改成 count=20 试试。）
2. `yield()` 时协程的局部变量存在哪？（在该 context 自己的栈内存里，Suspend 不销毁栈。）
3. 对比 events：那里 Suspend 由宿主的行回调强制触发（抢占式切片），这里由脚本 yield 自愿触发（协作式让出）——两种模式可以叠加吗？（可以：contextmgr 也支持给 context 设行回调做超时，asrun 示例就是这么用的。）
