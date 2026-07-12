# 01 — 内存管理与基础容器

一句话概括：AngelScript 从不直接调用 `new`/`malloc`，所有堆内存都经过一对可替换的全局函数指针 `userAlloc`/`userFree`，并在其上自建了一整套无异常、可嵌入的容器（动态数组、SSO 字符串、红黑树、符号表）与线程原语。

- **难度**：★★☆☆☆（模板与红黑树需要一点耐心，但没有黑魔法）
- **前置篇目**：无（本篇是整个系列的地基）

## 1. 模块定位

这一层位于引擎最底部：词法分析器、编译器、字节码、上下文、GC 等所有上层模块的内存分配和数据结构都落在这里。理解它之后，读任何其他模块时看到 `asCArray<...>`、`asCString`、`asCMap<...>`、`asNEW(...)` 都不再需要停下来查。

**为什么不用 STL？** 从代码里能找到四条直接证据：

1. **自定义分配器必须贯穿一切**。公开 API `asSetGlobalMemoryFunctions()` 承诺"应用程序决定如何分配内存"（as_memory.cpp:35-36 的文件头注释）。std 容器要做到这点需要给每个容器传 allocator 模板参数，侵入性极大；自制容器则统一走 `asNEW`/`asNEWARRAY` 宏。
2. **不依赖 C++ 异常**。整个库的 OOM 策略是"返回而不做任何事"：`asCArray::Allocate` 中 `// Out of memory. Return without doing anything`（as_array.h:217）、`asCMap::Insert` 分配节点失败时 `return -1`（as_map.h:198-202）。std 容器在 OOM 时抛 `std::bad_alloc`，与很多游戏机/嵌入式编译配置（禁用异常）冲突。
3. **老编译器与老平台兼容**。as_symboltable.h:58 明确写着 `// Interface to avoid nested templates which is not well supported by older compilers, e.g. MSVC6`；as_criticalsection.h:178-180 也有 `maxReaders` 因 MSVC6 编译错误而放在类外的注释；as_criticalsection.h:159-163 解释为兼容 Windows XP 而不用 Vista 才有的 SRWLOCK；as_atomic.cpp:97 还有 XBox360（AS_XENON）分支。
4. **跨 DLL/ABI 安全**。不重载全局 `operator new`（as_memory.h:70：`// We don't overload the new operator as that would affect the application as well`），也不让 std 容器对象跨越库边界，避免不同运行时库间的 ABI 问题；as_thread.cpp:175-180 的注释还专门处理了多 DLL 共享线程管理器的场景。

## 2. 核心数据结构

### 2.1 userAlloc / userFree 与 asNEW 宏族（as_memory.h / as_memory.cpp）

- 默认值就是 `malloc`/`free`（as_memory.cpp:119-120），MSVC 下用 `#pragma init_seg(lib)` 保证这两个全局变量先于用户全局对象初始化（as_memory.cpp:115-118），否则"把引擎放在全局变量里"的程序会崩（as_memory.cpp:111-113 注释）。
- `asNEW(x)` = **placement new**：先 `userAlloc(sizeof(x))` 拿裸内存，再在其上构造（as_memory.h:74）。`asDELETE(ptr,x)` 手动调析构再 `userFree`（as_memory.h:75）——所以宏需要你把类型名再写一遍。
- Debug 版把 `userAlloc` 强转成带 `__FILE__, __LINE__` 的三参函数指针（as_memory.h:94-96），靠"多传的参数被 cdecl 调用方清栈"这个技巧兼容原有两参分配器。

### 2.2 asCMemoryMgr —— 小对象池（as_memory.h:118-138）

只为两种高频小对象服务：解析树节点 `asCScriptNode` 和编译期字节码指令 `asCByteInstruction`。释放的对象不还给系统，而是压进 `asCArray<void*>` 空闲链（首次释放时预留 100 个槽位，as_memory.cpp:240-241）。脚本节点池加了临界区（多个方法可并发触发解析器，as_memory.cpp:195-197），字节码池不加（引擎保证同一时刻只有一个编译，as_memory.cpp:257）。Debug 下用 `0xCD` 填充已释放内存帮助发现 use-after-free（as_memory.cpp:247）。

### 2.3 asCArray<T> —— 动态数组（as_array.h）

- 关键字段：`T *array; asUINT length; asUINT maxLength;` **故意 public**，让 JIT 编译器可以 `asOFFSET` 直接访问（as_array.h:87-91）；另有内联小缓冲 `char buf[2*4*AS_PTR_SIZE]`（as_array.h:94，即两个指针大小，"Avoid dynamically allocated memory for tiny arrays"）——相当于 std::vector 没有的 **small buffer optimization**。
- 增长策略：`PushLast` 满了就 `Allocate(2*maxLength, true)` 翻倍（as_array.h:171-176）。
- 与 `std::vector` 差异：长度用 32 位 `asUINT`；OOM 不抛异常而是静默返回或返回 `false`；元素搬移用赋值/`memcpy`（`AllocateNoConstruct` 变体，as_array.h:281）而非移动语义。

### 2.4 asCString —— 带 SSO 的字符串（as_string.h / as_string.cpp）

- 布局只有 16 字节：`unsigned int length` + `union { char *dynamic; char local[12]; }`（as_string.h:88-93）。
- **SSO 阈值是 11 个字符**：`length <= 11` 时数据存在 `local`（11 字符 + 结尾 `\0` 恰好 12 字节），超过才用堆上 `dynamic`。判定逻辑见 `AddressOf()`（as_string.cpp:112-118）和 `Allocate()`（as_string.cpp:142、165）。
- **不存 capacity**：作者在 as_string.cpp:135-140 的注释里说明实测过"记录容量以省 realloc"与"对象更大"的权衡，当前方案在分配次数与内存体积间最平衡。代价是缩短再变长会重新分配。
- 与 `std::string` 差异：无引用计数、无 COW（as_string.h:31-33 头注释自述"designed to be easy to use, but not necessarily efficiency"）；C++11 下补了移动构造/移动赋值（as_string.cpp:60-77）。

### 2.5 asCMap<KEY,VAL> —— 手写红黑树（as_map.h）

- 节点 `asSMapNode` 含 `parent/left/right/isRed/key/value`（as_map.h:105-117），新节点默认红色。
- 三条不变量写在 as_map.h:95-100 注释中：根为黑、根到叶黑高相等、红节点无红父。`BalanceInsert`（as_map.h:254）和 `BalanceErase`（as_map.h:471，注释里逐条列出 case 1/2a/2b/3/4）是教科书式实现，甚至自带 `CheckIntegrity` 调试函数（as_map.h:340）。
- 与 `std::map` 差异：没有 STL 风格迭代器，用"游标"API `MoveTo/MoveFirst/MoveNext`（as_map.h:369、714、738）；`Insert` 可直接接收外部构造好的节点（as_map.h:211），配合对象池复用；空游标取值返回 `dummy` 节点而非未定义行为（as_map.h:687-693）。

### 2.6 asCSymbolTable<T> —— 索引数组 + 有序映射的复合结构（as_symboltable.h）

- 两套存储（as_symboltable.h:152-154）：`asCArray<T*> m_entries` 保证**索引稳定**（字节码里用索引引用符号），`asCMap<asSNameSpaceNamePair, asCArray<asUINT>> m_map` 提供**按"命名空间+名字"的快速查找**，同名符号（函数重载）映射到一个索引数组（as_symboltable.h:101-103 注释）。
- 注意：查找底层是**红黑树而非哈希表**——key `asSNameSpaceNamePair` 的 `operator<` 先比命名空间指针再比名字（as_namespace.h:68-71），O(log n)。
- `Put` 时若 key 已存在就往索引数组追加（as_symboltable.h:409-417）；`Erase` 用"末尾元素换位填坑"保持数组紧凑，并回写 map 中被移动元素的新索引（as_symboltable.h:375-393）。
- 重载筛选用 `asIFilter` 抽象接口而非嵌套模板，原因即上文的 MSVC6 兼容（as_symboltable.h:58-63）。

### 2.7 asCAtomic —— 引用计数原语（as_atomic.h / as_atomic.cpp）

- 就是一个 `asDWORD value` 加 `atomicInc/atomicDec`（as_atomic.h:49-65）。每个操作前都有 `asASSERT(value < 1000000)` ——超高引用计数几乎必然是野指针/写坏内存（as_atomic.cpp:48-50 注释），一个便宜实用的自检。
- 平台分派（as_atomic.cpp:85-191）：`AS_NO_THREADS` 时退化为普通 `++`/`--`；Windows 用 `InterlockedIncrement`（as_atomic.cpp:120-123）；Linux/BSD/Android 用 GCC `__sync_add_and_fetch`（as_atomic.cpp:144-147）；macOS/iOS 现已改用 `std::atomic_fetch_add`（as_atomic.cpp:176-179）。配置不匹配时用 `int ERROR_PleaseFixTheConfig[-1];` 制造编译错误（as_atomic.cpp:189）。

### 2.8 临界区与 TLS（as_criticalsection.h / as_thread.cpp）

- 全部锁操作都是宏：`AS_NO_THREADS` 时 `ENTERCRITICALSECTION(x)` 直接展开为空（as_criticalsection.h:46-59），单线程构建**零开销**。
- 双实现：POSIX `pthread_mutex_t`/`pthread_rwlock_t` 与 Windows `CRITICAL_SECTION`。Windows 读写锁用"信号量 × maxReaders(=10) 个读者槽"手工搭建（as_thread.cpp:419-424），写者逐个占满槽位以免饿死（as_thread.cpp:448-452 注释），思路来自 Qt 的公平读写锁文章（as_criticalsection.h:166-170）。
- TLS：单例 `threadManager`（as_thread.cpp:48）持有 `mtlsKey`，POSIX 走 `pthread_key_create`（as_thread.cpp:140-142），Windows 视配置走 `FlsAlloc`（纤程安全，带自动回收回调）或 `TlsAlloc`（as_thread.cpp:116-126）。`GetLocalData()` 懒创建每线程的 `asCThreadLocalData`（as_thread.cpp:305-339）。首次创建线程管理器存在竞态，所以 API 要求应用在主线程先调 `asPrepareMultithread()`（as_thread.cpp:161-171 注释）。这也解释了 `asSetGlobalMemoryFunctions` 为何先 `asThreadCleanup()`：避免用新分配器去释放旧分配器分的 TLS 内存（as_memory.cpp:145-148）。

## 3. 设计决策与权衡

| 决策 | 得到 | 付出 |
|---|---|---|
| 函数指针 + 宏，而非重载 operator new | 不污染宿主程序；换分配器一行搞定 | `asDELETE(ptr, 类型)` 要手写类型，忘了就是没调析构 |
| OOM 返回错误码而非异常 | 可在禁异常环境编译；行为可预测 | 每个调用点要自查（库内很多地方其实默默吞掉） |
| 容器内联小缓冲（asCArray 两指针、asCString 11 字符） | 词法单元、短标识符等海量小对象零堆分配 | 对象本体变大；SwapWith 需要特判 buf 自引用（as_array.h:434-438） |
| 符号表"数组 + 树"双索引 | 索引永久稳定（序列化字节码可用）且按名查找 O(log n) | 插入/删除要同步维护两套结构 |
| 平台原语全走 #ifdef 而非 std::thread/std::atomic | 支持 2003 年至今的所有平台，包括游戏主机 | 每支持一个新平台都要加一个分支 |

## 4. 如果你要自己实现一个脚本引擎

这一层的最小可行方案（现代 C++ 视角）：

1. **一对全局分配函数指针 + asNEW/asDELETE 式包装**——这是唯一真正"必须自制"的东西，宿主（游戏引擎）一定会要求接管内存。
2. 容器可以直接用 `std::vector/std::string/std::map` + 自定义 allocator，或干脆 pmr；除非你也要支持禁异常/老编译器，否则不必手写红黑树。
3. 符号表照抄"稳定索引数组 + (namespace,name)→索引列表 的映射"这个结构，它同时满足编译期查找和字节码序列化两种需求；映射可以换 `unordered_map`。
4. 引用计数用 `std::atomic<int>`，但保留那个 `assert(count < 1000000)` 的野指针探测，很值。
5. 单线程构建把锁编译成空宏（或用 if constexpr），别让不需要多线程的用户付锁的开销。

## 5. 动手实验

1. **挂统计分配器**：在任意 tutorial 的 `asCreateScriptEngine()` 之前调用 `asSetGlobalMemoryFunctions(myAlloc, myFree)`，在 `myAlloc` 里累加次数和字节数并记录峰值。编译一段脚本后打印，直观看到解析/编译阶段的分配热度；再对比调用 `engine->GetEngineProperty()` 前后差异。
2. **验证 SSO 阈值**：写个小程序 `#include "as_string.h"`，打印 `sizeof(asCString)`（应为 16），再配合实验 1 的统计分配器分别构造 11 字符和 12 字符的 `asCString`，确认前者零分配、后者触发一次 `userAlloc(13)`。
3. **触碰红黑树自检**：在 Debug 构建下对一个 `asCMap<int,int>` 随机插入/删除几千次，每步后调用 `CheckIntegrity(0)`（as_map.h:340），返回 -1 即违反不变量——然后故意把 `BalanceInsert` 里某次变色注释掉，看它多快被抓到。

## 6. 扩展阅读

- AngelScript 官方手册（内存管理、多线程章节可从目录进入）：https://www.angelcode.com/angelscript/sdk/docs/manual/index.html （站内搜索关键词：`asSetGlobalMemoryFunctions`、`Multithreading`）
- 红黑树：https://en.wikipedia.org/wiki/Red%E2%80%93black_tree
- 小字符串优化（SSO 属于其一般化）：https://en.wikipedia.org/wiki/Small_object_optimization 以及 https://en.wikipedia.org/wiki/String_(computer_science)
- 读写锁与读者-写者问题：https://en.wikipedia.org/wiki/Readers%E2%80%93writer_lock
- 线程局部存储（TLS/FLS 背景）：https://en.wikipedia.org/wiki/Thread-local_storage
- 对象池模式（asCMemoryMgr 的理论出处）：https://en.wikipedia.org/wiki/Object_pool_pattern
