# AngelScript async/await 前端改造：从 `6e6f713b` 到 HEAD 的逐提交逐行解析

> 分析范围：`6e6f713b9866e46889a1b57a6e270900a7e3299b`（不含）→ `83f6a668`（HEAD，不含）
> 共 10 个提交。仓库：`E:/angelscript-merge`（ProjectLungfish 的 AngelScript 2.33 UE5.8.2 fork）
> 作者：sunlaibing；时间跨度：2026-09-15 22:11 ~ 2026-10-12 18:31

## 0. 总览：这批提交在做什么

基线提交 `6e6f713b` 刚刚加入了一个**默认关闭**的引擎属性 `asEP_ENABLE_ASYNC_AWAIT`。
这 10 个提交在其之上，为 AngelScript 脚本语言实现了 **async/await 的前端部分**：
词法/语法识别（Parser）→ 声明校验（Builder）→ 编译期诊断（Compiler），
外加编译期宏开关、一次失败的 GC 注册修复（后回退）、以及一个热重载越界 bug 的修复。

| # | 提交 | 日期 | 一句话 |
|---|------|------|--------|
| 1 | `3b649270` | 09-15 22:11 | 给函数特性位掩码加 `asTRAIT_ASYNC_RESUME_ENTRY` |
| 2 | `32e5e57a` | 09-16 05:31 | 解析上下文关键字 `async` / `await` 语法 |
| 3 | `0045f1d1` | 09-16 05:53 | 校验非法的 async/await 使用场景 |
| 4 | `8db9830b` | 09-16 06:08 | 消除 await 语句与变量声明的二义性 |
| 5 | `d6d378fb` | 09-16 06:10 | （试图）允许 async 方法的按值参数 |
| 6 | `255a3d6f` | 09-16 06:14 | 改为从语法树检测显式引用参数（真正解决问题） |
| 7 | `02ea5bcc` | 09-22 18:19 | 用默认开启的宏 `WITH_ANGELSCRIPT_ASYNC_AWAIT` 门控全部改动 |
| 8 | `3c30219d` | 09-27 18:21 | 修复 UE fork 中无效的内建函数 GC 行为注册 |
| 9 | `779b0201` | 09-27 20:36 | 回退 #8 |
| 10 | `83f6a668` | 10-12 18:31 | 生成的脚本不再放进 `scripts` 数组，修复索引配对越界 |

注意提交 2~6 的时间戳：05:31、05:53、06:08、06:10、06:14 —— 45 分钟内连续 5 个提交，
明显是一次"写代码 → 编译/测试 → 发现问题 → 立即修正"的现场调试过程，第 4~6 个提交尤其能体现这一点。

---

## 1. 预备知识（读懂后续内容所需的最小背景）

### 1.1 AngelScript 的三段式编译管线

```
脚本源码
  │  asCParser::ParseScript()         ← 词法+语法分析，产出 AST（asCScriptNode 树）
  ▼
asCBuilder                            ← 注册类型/函数声明（RegisterScriptFunction），
  │                                      把 AST 节点翻译成语义对象（asCScriptFunction）
  ▼
asCCompiler::CompileFunction()        ← 对函数体再次调用 ParseStatementBlock() 解析语句块，
                                         然后遍历 AST 生成字节码
```

关键点：**函数体会被解析两次的不同阶段处理**。函数签名在 Builder 阶段解析；
函数体（statement block）则推迟到 Compiler 编译该函数时，才用一个新的 `asCParser`
调用 `ParseStatementBlock(script, blockBegin)` 单独解析。这个设计后面会反复出现。

### 1.2 上下文关键字（contextual keyword）

`async` / `await` **不是**保留字。老脚本里可能有人把变量命名为 `await`。
所以做法不是加 token 类型，而是：

1. 词法层它们仍然是普通的 `ttIdentifier`；
2. 语法层用 `IdentifierIs(token, "async")` 这样的辅助函数，在特定位置做**向前看（lookahead）**；
3. 确认模式后用 `RewindTo(&token)` 回退读取位置，再正式按新语义解析。

`IsAsyncFunctionModifier()`、`IsAwaitExpression()` 都是这种"试探-回退"模式。

### 1.3 本 fork（UE-AS）的三个特殊点

后面很多"费解"的改动，根源都在这三个 fork 定制上：

1. **`//@CYANCOOK(...) BEGIN/END` 标记**：本 fork 需要持续合并上游 AngelScript 新版本。
   所有 fork 私有改动都用成对注释框起来，合并上游时便于识别哪些代码不是上游的。
2. **按值结构体参数会被隐式改写为 const 引用**（存在于基线，来自更早的 `18d6156b`）：

   ```cpp
   // as_builder.cpp, GetParsedFunctionDetails() 内（基线已有代码）
   if (!type.IsReference())
   {
       // Script functions that take a value type by value should always take a const ref instead
       auto* typeInfo = type.GetTypeInfo();
       if (typeInfo != nullptr && typeInfo->GetFlags() & asOBJ_VALUE)
       {
           type.MakeReference(true);          // ← FVector x 被改写成 const FVector&
           type.MakeReadOnly(true);
           inOutFlag = asETypeModifiers::asTM_INOUTREF;  // ← 连 in/out 标志也一起改写
       }
       ...
   }
   ```
   也就是说，脚本里写 `void F(FVector pos)`，到了语义层 `parameterTypes` 里
   `IsReference()==true`、`inOutFlags==asTM_INOUTREF`。**语义层已经无法区分
   "用户写了 `&`"和"fork 自动改写"** —— 这是提交 5、6 故事的根源。
3. **`asOBJ_GC = 0`**（`include/angelscript.h:190`，注释 `// Unused by UE-AS`）。
   这个 fork 把上游的 GC 类型标志位置 0 了 —— 这是提交 8 故事的根源。

### 1.4 基线提交 `6e6f713b` 回顾

加入了引擎属性 `asEP_ENABLE_ASYNC_AWAIT = 48`（36~47 注释说明保留给上游 2.38 的新属性，
避免将来合并时编号冲突），配套 `SetEngineProperty`/`GetEngineProperty` 分支和
`ep.enableAsyncAwait` 字段，**默认 false**。也就是说整个特性运行时默认关闭，需要宿主显式开启。

---

## 2. 提交 1：`3b649270` — Add async resume function trait

```diff
 enum asEFuncTrait
 {
 	...
 	asTRAIT_DEFAULTS_ONLY = 0x4000000,
+	//@CYANCOOK(AngelScript Async Await) BEGIN
+	// 0x08000000-0x20000000 are reserved for official 2.38 method traits.
+	asTRAIT_ASYNC_RESUME_ENTRY = 0x40000000,
+	//@CYANCOOK(AngelScript Async Await) END
 };
```

**逐行解释：**

- `asEFuncTrait` 是函数特性位掩码（每个函数一个 bit），存在 `asSFunctionTraits` 里，
  随函数签名参与比较、缓存、持久化。
- 注释 `0x08000000-0x20000000 are reserved for official 2.38 method traits`：
  **主动让出一段位空间给上游未来的版本**。这是维护 fork 的经典手法——上游 2.38 若新增 trait，
  会从低位开始用，fork 直接从 `0x40000000` 起跳，合并时不会撞车。
- `asTRAIT_ASYNC_RESUME_ENTRY = 0x40000000`：标记"这个函数是 async 状态机的**恢复入口函数**"。
  async 函数最终会被改写（lower）成状态机：原函数体变成 `Resume(...)` 之类的入口，
  每次 await 挂起后从这里继续。这个 trait 就是给将来生成的那个函数打标签用的。
  本提交只是**占位**，还没有任何代码读它——先抢段位号，避免后续提交之间产生位冲突。

**设计要点**：第一步不占逻辑、只占位，是典型的"协议先行"——位掩码是跨模块协议，
先定协议再写实现，后续提交可以并行推进。

---

## 3. 提交 2：`32e5e57a` — Parse contextual async and await syntax

本特性最核心的提交：让解析器认识 `async FTask Foo()` 声明和 `await expr` 表达式。
涉及 7 个文件。按文件逐一拆解。

### 3.1 `source/as_tokendef.h`

```diff
+//@CYANCOOK(AngelScript Async Await) BEGIN
+const char * const ASYNC_TOKEN = "async";
+const char * const AWAIT_TOKEN = "await";
+//@CYANCOOK(AngelScript Async Await) END
```

只是两个字符串常量，供 `IdentifierIs(t, ASYNC_TOKEN)` 这样的调用做标识符文本比较。
与既有 `DEPRECATED_TOKEN = "deprecated"` 等上下文关键字常量风格完全一致。

### 3.2 `source/as_scriptnode.h`

```diff
 	snFallthrough,
 	snForRange,
+	//@CYANCOOK(AngelScript Async Await) BEGIN
+	snAsync,
+	snAwait,
+	//@CYANCOOK(AngelScript Async Await) END
 };
```

AST 节点类型枚举加两个值。`snAsync` 会挂在函数声明节点下作为子节点（标记这是个 async 函数）；
`snAwait` 会挂在表达式项（`snExprTerm`）节点下，地位等同于 `-`、`!` 这类**前置运算符**。
追加在枚举末尾而不是插入中间，避免打乱既有数值（减小对序列化/合并的影响）。

### 3.3 `source/as_scriptfunction.h`

```diff
-enum asEFuncTrait
+//@CYANCOOK(AngelScript Async Await) BEGIN
+enum asEFuncTrait : asDWORD
+//@CYANCOOK(AngelScript Async Await) END
 {
 	...
 	asTRAIT_ASYNC_RESUME_ENTRY = 0x40000000,
+	asTRAIT_ASYNC = 0x80000000u,
 	//@CYANCOOK(AngelScript Async Await) END
 };
```

两行改动都大有讲究：

1. **`enum asEFuncTrait : asDWORD`** —— 显式指定枚举底层类型为 `asDWORD`（即 `unsigned int`）。
   原因：新增的 `asTRAIT_ASYNC = 0x80000000u` 超过了 **有符号** 32 位整数的最大值
   （`0x7FFFFFFF`）。C++ 不指定底层类型时，枚举的底层类型由编译器选定，
   能装下所有枚举值即可——塞一个 `0x80000000u` 进去，在 MSVC/Clang 上行为不一致，
   且旧代码里若有 `int traits = ...` 之类的隐式转换会溢出（实现定义行为）。
   显式 `: asDWORD` 后，最高位合法、行为跨平台确定。
2. **`asTRAIT_ASYNC = 0x80000000u`**：标记"这是一个 async 函数"。占最高位（提交 1 已把
   `0x40000000` 占了），`u` 后缀保证字面量本身是 unsigned，避免编译告警。

### 3.4 `source/as_parser.h`

```diff
-	int ParseStatementBlock(asCScriptCode *script, asCScriptNode *block);
+	int ParseStatementBlock(asCScriptCode *script, asCScriptNode *block, bool isAsyncFunction = false);
```

`ParseStatementBlock` 增加第三个参数：**正在解析的函数体是否属于 async 函数**。
带默认值 `false`，因此其它既有调用点（如 Lambda、独立语句解析）不用改。

```diff
+	bool IsAsyncFunctionModifier();
```

```diff
+	bool isParsingAsyncFunction;
```

两个新成员的声明，实现在 .cpp（见下）。

### 3.5 `source/as_parser.cpp` —— 重头戏

**(a) 构造函数与 `Reset()`**

```diff
+	isParsingAsyncFunction = false;
```

两处各加一行初始化。`Reset()` 在每次 `ParseScript`/`ParseStatementBlock` 入口都会调用，
保证状态不跨解析任务泄漏——这个新标志也必须遵守这个规矩。

**(b) `ParseExprTerm()`：把 `await` 解析成前置运算符**

原代码结构（基线已有）：

```cpp
for(;;)
{
    GetToken(&t);
    RewindTo(&t);
    if( !IsPreOperator(t.type) )   // 不是 - ! ~ ++ -- 等前置运算符就停
        break;
    node->AddChildLast(ParseExprPreOp());
}
node->AddChildLast(ParseExprValue());   // 然后是表达式本体
```

插入的代码：

```diff
 		GetToken(&t);
 		RewindTo(&t);
+		//@CYANCOOK(AngelScript Async Await) BEGIN
+		if (engine->ep.enableAsyncAwait && isParsingAsyncFunction
+			&& IdentifierIs(t, AWAIT_TOKEN))
+		{
+			asCScriptNode* awaitNode = ParseIdentifier();
+			awaitNode->nodeType = snAwait;
+			node->AddChildLast(awaitNode);
+			continue;
+		}
+		//@CYANCOOK(AngelScript Async Await) END
 		if( !IsPreOperator(t.type) )
 			break;
```

逐行：

- `engine->ep.enableAsyncAwait`：双重门控第一层——引擎属性（基线提交加的，默认关）。
  没开特性时 `await` 就是普通标识符，老脚本零影响。
- `isParsingAsyncFunction`：第二层——只有 async 函数体内才把 `await` 当运算符。
  （下一个提交会放宽这一点，见 §4.3。）
- `IdentifierIs(t, AWAIT_TOKEN)`：当前标识符文本等于 `"await"`。
- `ParseIdentifier()`：正式消费这个标识符，生成一个普通标识符节点；
- `awaitNode->nodeType = snAwait;`：**改牌**——把节点类型改成 `snAwait`。
  这是上下文关键字的典型实现：先按通用规则解析，再原地改类型。
- `node->AddChildLast(awaitNode); continue;`：挂到 `snExprTerm` 子节点列表，
  并继续循环——所以 `await await x`、`-await x` 这种前置运算符叠加也能解析。

为什么放在 `IsPreOperator` 判断**之前**？因为 `await` 不是 `IsPreOperator` 认识的
token 类型（它是 `ttIdentifier`），不拦截就会直接 `break` 掉，把 `await` 留在原地，
后面的 `ParseExprValue()` 会把它当成普通变量名。

**(c) `ParseScript()`：脚本层识别 async 函数声明**

```diff
 			if( t1.type == ttFuncDef )
 				node->AddChildLast(ParseFuncDef());
 			else if( t1.type == ttLocal )
 				node->AddChildLast(ParseLocal());
+			//@CYANCOOK(AngelScript Async Await) BEGIN
+			else if (IsAsyncFunctionModifier())
+				node->AddChildLast(ParseFunction());
+			//@CYANCOOK(AngelScript Async Await) END
 			else if( t1.type == ttConst || t1.type == ttScope || t1.type == ttAuto || IsDataType(t1) )
```

脚本顶层遇到 `async FTask Foo() { ... }` 时，第一个 token 是标识符 `async`，
原有的分支都不匹配（它不是数据类型声明的开头）。新增分支：如果是 async 修饰符模式，
就走 `ParseFunction()`。位置必须在 `IsDataType(t1)` 分支**之前**——否则 `async`
可能被当成类型名而误入变量声明分支。

**(d) `ParseStatementBlock()`：接收并记录 async 上下文**

```diff
-int asCParser::ParseStatementBlock(asCScriptCode *in_script, asCScriptNode *in_block)
+int asCParser::ParseStatementBlock(
+	asCScriptCode *in_script, asCScriptNode *in_block, bool in_isAsyncFunction)
 {
 	Reset();
+	isParsingAsyncFunction = in_isAsyncFunction;
```

回忆 §1.1：函数体是在编译阶段单独解析的，解析器是一个**新建的** `asCParser`，
它不知道自己在编哪个函数。所以必须由调用方（编译器）把"这个函数是 async"传进来，
(b) 中的 `await` 识别才有上下文。这就是这个参数存在的全部意义。

**(e) 新增 `IsAsyncFunctionModifier()`：四步向前看**

```cpp
bool asCParser::IsAsyncFunctionModifier()
{
	if (!engine->ep.enableAsyncAwait)
		return false;                          // ① 特性没开：一切照旧

	sToken asyncToken;
	GetToken(&asyncToken);
	if (!IdentifierIs(asyncToken, ASYNC_TOKEN))
	{
		RewindTo(&asyncToken);                 // ② 不是 async：回退，不是我们的菜
		return false;
	}

	sToken afterType;
	if (!IsType(afterType))                    // ③ async 后面必须跟一个类型（返回类型）
	{
		RewindTo(&asyncToken);
		return false;
	}

	RewindTo(&afterType);                      // ④ 跳过类型后，必须是 "标识符 + ("
	sToken functionName;
	GetToken(&functionName);
	sToken openParenthesis;
	GetToken(&openParenthesis);
	const bool isAsyncFunction = functionName.type == ttIdentifier
		&& openParenthesis.type == ttOpenParanthesis;
	RewindTo(&asyncToken);                     // ⑤ 无论结果如何，读位置复原
	return isAsyncFunction;
}
```

这是教科书式的**推测解析（speculative parsing）**：读 → 不匹配就 `RewindTo` 回退 →
匹配也要 `RewindTo`，因为这个函数只回答"是不是"，消费工作留给 `ParseFunction()`。

为什么需要这么多步？因为要区分：

| 脚本代码 | 判定 | 理由 |
|---|---|---|
| `async FTask Foo() {}` | ✔ 是 | 类型+名字+`(` 齐全 |
| `async int x;` | ✘ 否 | 用户可能有名为 `async` 的类型，声明变量 `x`；`x` 后面是 `;` 不是 `(` |
| `async = 5;` | ✘ 否 | 变量 `async` 的赋值；`=` 不是类型 |

注意 ③ 用了 `IsType()`，而 `IsType` **故意把任何标识符都当作潜在类型**
（代码里有注释：此处不验证类型是否真实存在，留给 ParseDeclaration 报更友好的错）。
所以 ④ 的 `(` 才是区分"函数声明"与"变量声明"的决定性证据。

**(f) `IsFuncDecl()`：两处消化 async 前缀**

`IsFuncDecl` 是另一处向前看（判断当前位置是不是函数声明，用于类体/脚本层的分流）。

```diff
 		else if( t1.type != ttPrivate && t1.type != ttProtected )
 			RewindTo(&t1);
 
+		//@CYANCOOK(AngelScript Async Await) BEGIN
+		if (IsAsyncFunctionModifier())
+		{
+			GetToken(&t1);
+		}
+		//@CYANCOOK(AngelScript Async Await) END
+
 		// A class constructor starts with identifier followed by parenthesis
 		// A class destructor starts with the ~ token
 		GetToken(&t1);
```

第一处（`isMethod` 分支内）：消化掉访问修饰符（`private`/`protected`/`access`）之后、
判断构造函数（`Name(`）/析构函数（`~Name(`）之前，先把可能存在的 `async` 跳过，
否则 `async` 会被误当成构造函数名。

```diff
+	//@CYANCOOK(AngelScript Async Await) BEGIN
+	if (IsAsyncFunctionModifier())
+	{
+		sToken asyncToken;
+		GetToken(&asyncToken);
+	}
+	//@CYANCOOK(AngelScript Async Await) END
+
 	// A function decl starts with a type
 	sToken t1;
 	if (!IsType(t1))
```

第二处（公共路径）：跳过 `async` 后再验证"类型开头"，全局函数和类方法都覆盖。

**(g) `ParseFunction()`：正式解析时消费 async**

```diff
+	//@CYANCOOK(AngelScript Async Await) BEGIN
+	RewindTo(&t1);
+	if (IsAsyncFunctionModifier())
+	{
+		asCScriptNode* asyncNode = ParseIdentifier();
+		asyncNode->nodeType = snAsync;
+		node->AddChildLast(asyncNode);
+		GetToken(&t1);
+	}
+	else
+	{
+		GetToken(&t1);
+	}
+	//@CYANCOOK(AngelScript Async Await) END
+
 	if( isSyntaxError ) return node;
 
 	// If it is a global function, or a method, except constructor and destructor, then the return type is parsed
 	sToken t2;
 	GetToken(&t2);
 	RewindTo(&t1);
```

上下文：执行到这里时，`t1` 是访问修饰符之后的第一个 token（可能是 `async`、
返回类型的开头、`~` 或构造函数名），并且 token 流已经读过了它。

逐行：

- `RewindTo(&t1);`：退回到 `t1` 的位置，以便 `IsAsyncFunctionModifier()` 从头试探。
- 若是 async：`ParseIdentifier()` 正式消费 `async`，节点改牌为 `snAsync` 挂为函数节点的
  子节点（**这就是 Builder 阶段识别 async 函数的标记**）；然后 `GetToken(&t1)` 让
  `t1` 变成 `async` 之后的 token（即返回类型开头）。
- 若不是：直接 `GetToken(&t1)`——因为刚才 Rewind 回去了，要重新读出来，保持
  `t1` 的含义与改动前完全一致。
- 关键不变式：**无论走哪个分支，代码继续往下时 `t1` 都指向"返回类型/ctor/dtor 的起点"**。
  下面一行 `GetToken(&t2); RewindTo(&t1);` 依赖这个不变式来判断构造函数
  （`t1`=名字、`t2`=`(`）。

### 3.6 `source/as_builder.cpp`：把语法标记变成语义 trait

**(a) `GetParsedFunctionDetails()`：读取 snAsync 节点**

```diff
 	//@CYANCOOK(AngelScript Async Await) BEGIN
+	funcTraits.SetTrait(asTRAIT_ASYNC, false);
+	if (node->nodeType == snAsync)
+	{
+		funcTraits.SetTrait(asTRAIT_ASYNC, true);
+		node = node->next;
+	}
 	//@CYANCOOK(AngelScript Async Await) END
 
 	// Find the name
```

此函数负责把函数声明 AST 节点翻译成名称/返回类型/参数/trait 等语义信息。
执行到这里时 `node` 已经跳过访问修饰符。新增逻辑：先默认非 async；若当前子节点是
`snAsync`（Parser 在 3.5(g) 里挂上的），则置 trait 并**推进到下一个子节点**，
让后续"找名字/返回类型"的逻辑看到的是 `async` 之后的内容。

**(b) async 函数默认 nodiscard**

```diff
-	funcTraits.SetTrait(asTRAIT_NODISCARD, false);
+	//@CYANCOOK(AngelScript Async Await) BEGIN
+	funcTraits.SetTrait(asTRAIT_NODISCARD, funcTraits.GetTrait(asTRAIT_ASYNC));
+	//@CYANCOOK(AngelScript Async Await) END
```

把 NODISCARD 的默认值从无条件 `false` 改为"等于 ASYNC trait"。
原因：async 函数返回的是 `FTask`/`TTask<T>`（一个任务句柄），**丢弃返回值 ≈ 静默丢失
一个异步任务**，几乎肯定是 bug。所以 async 函数默认不可丢弃返回值。
（下一提交会禁止用 `allow_discard` 显式关闭它，见 §4.1，等于强制 nodiscard。）

### 3.7 `source/as_compiler.cpp`：把 trait 传进函数体解析

```diff
 		asCParser parser(builder);
-		int r = parser.ParseStatementBlock(script, blockBegin);
+		int r = parser.ParseStatementBlock(
+			script, blockBegin, in_outFunc->traits.GetTrait(asTRAIT_ASYNC));
```

呼应 §1.1：编译器编译函数时新建 parser 解析函数体，这里把注册阶段算出的
ASYNC trait 传进去，打通"声明是 async → 函数体内认识 `await`"的信息链。

**本提交小结**：建立了完整链路
`async 关键字 → snAsync 节点 → asTRAIT_ASYNC trait → ParseStatementBlock(isAsync) → await 识别`。
但此时还没有任何合法性校验——乱写也能过解析。那是下一个提交的事。

---

## 4. 提交 3：`0045f1d1` — Diagnose invalid async and await contexts

原则：**语法能解析 ≠ 语义合法**。async 状态机改写有很多限制（参数要被拷进状态结构体、
要由任务系统调度……），与其将来崩溃，不如在注册/编译时给出明确报错。

### 4.1 `as_builder.cpp`：`RegisterScriptFunction()` 入口的七条禁令

```diff
+	if (funcTraits.GetTrait(asTRAIT_ASYNC))
+	{
+		if (objType == nullptr || isGlobalFunction)
+			WriteError("Async functions must be instance class methods", file, node);
```

① **必须是类的实例方法**。全局函数不允许。设计决策：async 任务挂在对象生命期上，
`this` 也是状态机要捕获的上下文之一；全局函数没有天然的宿主。

```diff
+		if (funcTraits.GetTrait(asTRAIT_CONSTRUCTOR) || funcTraits.GetTrait(asTRAIT_DESTRUCTOR))
+			WriteError("Async constructors and destructors are not supported", file, node);
```

② **构造/析构不行**。构造函数的"返回值"是被构造对象本身，无法换成 Task；
析构在 GC/作用域结束时同步触发，无法挂起。

```diff
+		if (isInterface || isMixin || funcTraits.GetTrait(asTRAIT_MIXIN)
+			|| funcTraits.GetTrait(asTRAIT_OVERRIDE))
+			WriteError("Async interface, mixin, and override methods are not supported", file, node);
```

③ **接口方法 / mixin / override 不行**。前两者没有具体函数体可改写；
override 不行是因为基类不知道这个方法是 async 状态机，虚函数签名（返回 Task）
与多态调用约定对不上。

```diff
+		if (funcTraits.GetTrait(asTRAIT_PROPERTY))
+			WriteError("Async property accessors are not supported", file, node);
```

④ **属性访问器（get/set）不行**。属性访问器模拟字段读写语法，无处安放 await。

```diff
+		if (funcTraits.GetTrait(asTRAIT_ALLOWDISCARD))
+			WriteError("Async task results cannot be marked allow_discard; use Forget() explicitly", file, node);
```

⑤ **禁止 `allow_discard`**。与 §3.6(b) 呼应：async 默认 nodiscard，这里把唯一的
"逃生门"也焊死，并给出替代方案——显式调 `Forget()` 表明"我故意不要结果"。
好的报错信息应该给出出路，这条是范例。

```diff
+		if (returnType.IsReference())
+			WriteError("Async functions cannot return references", file, node);
```

⑥ **不能返回引用**。引用指向调用者的栈/临时对象，而 async 函数恢复执行时
当初的栈帧早已不存在——返回引用必然悬空。

```diff
+		asCTypeInfo* returnTypeInfo = returnType.GetTypeInfo();
+		const bool returnsTask = returnTypeInfo != nullptr
+			&& (returnTypeInfo->name == "FTask"
+				|| returnTypeInfo->name == "TTask"
+				|| returnTypeInfo->name.StartsWith("TTask<"));
+		if (!returnsTask)
+			WriteError("Async functions must return FTask or TTask<T>", file, node);
```

⑦ **必须返回任务类型**：无参 `FTask`、或模板 `TTask<T>`。
这是 UE 任务系统的"可等待货币"。名字按字符串匹配（含 `TTask<` 前缀匹配模板实例），
因为任务类型由引擎集成层注册，VM 层不认识具体类型，只能认名字。

```diff
+		for (asUINT parameterIndex = 0; parameterIndex < parameterTypes.GetLength(); ++parameterIndex)
+		{
+			if (parameterTypes[parameterIndex].IsReference()
+				|| (parameterIndex < inOutFlags.GetLength() && inOutFlags[parameterIndex] != asTM_NONE))
+			{
+				WriteError("Async functions cannot have in, out, or reference parameters", file, node);
+				break;
+			}
+		}
```

⑧ **参数检查 v1**：拒绝一切引用语义——`IsReference()`（类型是引用）或
`inOutFlags` 非 `asTM_NONE`（声明了 `&in/&out/&inout`）。
原因：async 函数挂起后恢复时，引用参数指向的调用方栈变量早已销毁；
所有参数必须在挂起点被**拷贝**进状态机结构体，引用无法安全拷贝。

> ⚠️ 注意 `inOutFlags` 下标保护 `parameterIndex < inOutFlags.GetLength()`——
> 两个数组长度不总是一致（某些注册路径如属性 set 访问器是手工拼的参数列表），
> 直接下标访问有越界风险。

这个条件在接下来两个提交里被连续改了两次，专题见 §9。

### 4.2 `as_compiler.cpp`：编译期的 await 检查与"熔断器"

`CompileExpressionTerm()` 中，前置运算符是从表达式值节点**倒着**往前遍历的：

```diff
 	pnode = vnode->prev;
 	while( pnode )
 	{
+		//@CYANCOOK(AngelScript Async Await) BEGIN
+		if (pnode->nodeType == snAwait)
+		{
+			if (!outFunc->traits.GetTrait(asTRAIT_ASYNC))
+			{
+				Error("await expressions are only valid inside async functions", pnode);
+				return -1;
+			}
+
+			asCTypeInfo* awaitedType = v.type.dataType.GetTypeInfo();
+			const bool isTask = awaitedType != nullptr
+				&& (awaitedType->name == "FTask"
+					|| awaitedType->name == "TTask"
+					|| awaitedType->name.StartsWith("TTask<"));
+			if (!isTask)
+			{
+				Error("await operand must be FTask or TTask<T>", pnode);
+				return -1;
+			}
+
+			Error("Async function reached bytecode compilation before state-machine lowering", pnode);
+			return -1;
+		}
+		//@CYANCOOK(AngelScript Async Await) END
 		r = CompileExpressionPreOp(pnode, &v, bIsStatementRoot); if( r < 0 ) return r;
```

检查插在 `CompileExpressionPreOp` **之前**拦截 `snAwait`，三道关卡：

1. **环境关**：非 async 函数里出现 await → 报错。（配合 §4.3，此时非 async 函数
   也能把 await **解析**出来，就是为了能报出这句人话，而不是莫名其妙的语法错误。）
2. **类型关**：`v` 是 await 后面那个表达式值已编译出的结果；它的类型必须是
   `FTask`/`TTask<T>`——只能 await 任务。
3. **熔断器**：过了前两关**依然报错**——"到达字节码编译阶段说明状态机 lowering 没跑"。
   这一行揭示了整个特性在此时的状态：**前端（解析+校验）已完成，但把 async 函数
   改写成状态机的 lowering  pass 不在本仓库范围内**（在引擎集成层，见提交 10 提到的
   P4 CL 137443）。正常流程下 lowering 会把含 await 的函数体重写掉，`snAwait` 节点
   根本不应该活着走到字节码编译；真走到了，说明流程异常。这是防御性的"不可能路径"断言，
   比静默生成错误字节码好得多。

### 4.3 `as_parser.cpp`：抽出 `IsAwaitExpression()` 并放宽非 async 场景

```diff
-		if (engine->ep.enableAsyncAwait && isParsingAsyncFunction
-			&& IdentifierIs(t, AWAIT_TOKEN))
+		if (IsAwaitExpression())
```

把 §3.5(b) 的内联条件抽成函数，逻辑升级为两层：

```cpp
bool asCParser::IsAwaitExpression()
{
	if (!engine->ep.enableAsyncAwait)
		return false;

	sToken awaitToken;
	GetToken(&awaitToken);
	if (!IdentifierIs(awaitToken, AWAIT_TOKEN))
	{
		RewindTo(&awaitToken);
		return false;
	}

	if (isParsingAsyncFunction)
	{
		RewindTo(&awaitToken);
		return true;                     // async 函数里：await 一律是 await
	}

	// 非 async 函数：要求下一个 token "长得像表达式起点"
	sToken operandToken;
	GetToken(&operandToken);
	const bool looksLikeAwait = operandToken.type == ttIdentifier
		|| operandToken.type == ttScope        // await ::ns::Foo()
		|| operandToken.type == ttCast         // await cast<T>(x)
		|| IsRealType(operandToken.type)       // await int(...) 之类
		|| IsConstant(operandToken.type);      // await 42（类型关会拦，先解析出来）
	RewindTo(&awaitToken);
	return looksLikeAwait;
}
```

为什么要放宽到非 async 函数？——为了**报错质量**。若非 async 函数里的 `await task;`
根本不按 await 解析，`await` 会被当变量名，报出一串莫名其妙的错（`'await' 未声明`之类）。
让它解析成 `snAwait`，编译器就能报出 §4.2 里那句精确的
"await expressions are only valid inside async functions"。

为什么非 async 路径要多看一个 token？——**向后兼容**。老脚本里 `await` 可能是变量名：

```cpp
int await;
await = 5;        // `=` 不在 looksLikeAwait 集合里 → 不当 await，正常赋值
```

而 `await task;`、`await Foo();` 的下一个 token 是标识符/作用域符，会被识别为 await
表达式——然后在编译期收到那条清晰的报错。完美的渐进式升级路径。

---

## 5. 提交 4：`8db9830b` — Disambiguate await statements and value parameters

修两个"昨天写的代码今天测出问题"的 bug。

### 5.1 `IsVarDecl()` 的开头加 await 拦截（核心修复）

**问题**：AngelScript 的 `IsType()` 故意把任何标识符当潜在类型（§3.5(e) 提过）。
于是一条最普通的 await 语句：

```cpp
await MyTask;
```

走 `IsVarDecl()` 的判定流程是：`await` 是标识符→**算个类型** ✔；下一个 token `MyTask`
是标识符→**算变量名** ✔；再下一个 `;` → "这是变量声明！" 于是 `await MyTask;`
被解析成"声明一个类型为 `await`、名为 `MyTask` 的变量"，然后报"类型 await 不存在"。
更糟的是 `await Foo();` 也会命中 `IsVarDecl` 的"带构造初始化的声明"分支。
**最常见的 await 写法全军覆没。**

修复——在 `IsVarDecl()` 所有判定之前拦截：

```diff
 bool asCParser::IsVarDecl()
 {
 	sToken t;
 	GetToken(&t);
 	RewindTo(&t);
 
+	//@CYANCOOK(AngelScript Async Await) BEGIN
+	if (isParsingFunctionBody && IsAwaitExpression())
+	{
+		RewindTo(&t);
+		return false;
+	}
+	//@CYANCOOK(AngelScript Async Await) END
```

"以 await 表达式开头 ⇒ 一定不是变量声明"，直接否掉，语句就会走表达式解析路径，
命中 §3.5(b) 的 await 处理。

**配套的 `isParsingFunctionBody` 新状态**（构造函数/`Reset()`/`ParseStatementBlock` 各加一行）：

```diff
 	Reset();
 	isParsingAsyncFunction = in_isAsyncFunction;
+	isParsingFunctionBody = true;
```

为什么不直接删掉这个条件、在 `IsVarDecl` 里无条件查 await？因为 `IsVarDecl`
也用于**类体/脚本顶层**（解析成员变量），那里：

- 不可能出现 await 语句（语句只存在于函数体内）；
- 但**可能**有人定义了名为 `await` 的类型并写 `await myMember;`——
  若不加 `isParsingFunctionBody` 限制，这种合法代码会被新逻辑误伤。

`ParseStatementBlock()` 是函数体的专属入口（§1.1），在这里置 `true`，
类体/脚本层走 `ParseScript()` 路径保持 `false`，新老行为泾渭分明。
**状态最小化原则：新解析行为只作用于它该出现的作用域。**

### 5.2 参数检查 v2

```diff
-			if (parameterTypes[parameterIndex].IsReference()
-				|| (parameterIndex < inOutFlags.GetLength() && inOutFlags[parameterIndex] != asTM_NONE))
+			if (parameterIndex < inOutFlags.GetLength()
+				&& inOutFlags[parameterIndex] != asTM_NONE)
```

去掉了 `IsReference()` 这一半。结合 §1.3(2) 的 fork 隐式改写来看，这个条件
仍然会把"按值传结构体"误判为引用（改写后 `inOutFlag = asTM_INOUTREF`），
所以问题并没解决——下一提交继续改。见 §9 专题。

---

## 6. 提交 5：`d6d378fb` — Allow by-value parameters on async methods（2 分钟后）

```diff
-			if (parameterIndex < inOutFlags.GetLength()
-				&& inOutFlags[parameterIndex] != asTM_NONE)
+			if (parameterTypes[parameterIndex].IsReference())
```

提交信息说"允许 async 方法的按值参数"。意图显然是：按值参数（如 `FVector pos`）
不该被拒，因为状态机可以把它们拷进状态结构体。

**但严格按代码推演，这个版本并没有达成目的**：回忆 §1.3(2)，fork 在
`GetParsedFunctionDetails` 里把按值的 VALUE 类型参数改写成了
`MakeReference(true) + asTM_INOUTREF`，所以 `IsReference()` 对它们照样返回 `true`，
照样被拒之门外。事实上对照 `ModifyDataTypeFromNode` 的逻辑（`&` 必然同时置
IsReference 和非 NONE 的 flag），v2 与 v3 拒绝的参数集合几乎完全相等。

这是凌晨 6 点连续调试中的一步试探（4 分钟后就被下一提交取代）。它真正的价值是
明确了**意图**：拒绝的应该是"用户显式写的引用"，而不是"fork 内部改写出来的引用"。
——而语义层数据（`parameterTypes`/`inOutFlags`）经过改写已经丢失了这个区别，
答案只能去**语法层**找。

---

## 7. 提交 6：`255a3d6f` — Detect explicit async reference parameters from syntax（4 分钟后）

最终方案：不看语义数据，直接翻 AST 找用户手写的 `&`：

```diff
-		for (asUINT parameterIndex = 0; parameterIndex < parameterTypes.GetLength(); ++parameterIndex)
-		{
-			if (parameterTypes[parameterIndex].IsReference())
-			{
-				WriteError("Async functions cannot have in, out, or reference parameters", file, node);
-				break;
+		asCScriptNode* parameterListNode = node->firstChild;
+		while (parameterListNode != nullptr && parameterListNode->nodeType != snParameterList)
+			parameterListNode = parameterListNode->next;
+		bool hasExplicitReferenceParameter = false;
+		if (parameterListNode != nullptr)
+		{
+			for (asCScriptNode* parameterNode = parameterListNode->firstChild;
+				parameterNode != nullptr; parameterNode = parameterNode->next)
+			{
+				if (parameterNode->nodeType == snDataType
+					&& parameterNode->firstChild != nullptr
+					&& parameterNode->firstChild->tokenType == ttAmp)
+				{
+					hasExplicitReferenceParameter = true;
+					break;
+				}
 			}
 		}
+		if (hasExplicitReferenceParameter)
+			WriteError("Async functions cannot have in, out, or reference parameters", file, node);
```

逐段：

1. **找参数列表节点**：函数声明节点的直接子节点依次是
   [访问修饰符?] [snAsync?] [返回类型] [名字] [snParameterList] [函数体]……
   循环沿 `next` 链找到 `snParameterList`。找不到（异常 AST）就跳过检查——防御式写法。
2. **遍历每个参数的 snDataType 节点**：参数列表的子节点按
   `snDataType, (identifier), (默认表达式), snDataType, ...` 排列。
3. **判据 `parameterNode->firstChild->tokenType == ttAmp`**：
   `&` 在语法树里的位置正是数据类型节点的第一个子节点
   （对照 `ModifyDataTypeFromNode`：`n = node->firstChild; if( n && n->tokenType == ttAmp )`）。
   用户写了 `&in`/`&out`/`&inout`/裸 `&`，这里一定是 `ttAmp`；
   而 `FVector pos` 这种按值写法，语法树里**没有** `&`——fork 的隐式改写发生在
   语义层，不碰语法树。**完美区分"用户写的"和"fork 改的"。**
4. 报错信息沿用了旧的措辞，但现在它精确地只在用户写了 `&` 时触发；
   `async FTask Tick(FVector Location, float Dt)` 终于能过校验。

**这一组三连提交（4→5→6）的教学价值**：

> 当语义层的检查怎么改都不对时，先问一句：**我要区分的两种东西，在语义层还长得不一样吗？**
> fork 的规范化（normalization）把"按值"和"显式引用"抹成了同一个样子，
> 答案只能回溯到更原始的信息源——语法树。

---

## 8. 提交 7：`02ea5bcc` — Gate async compiler extensions with a default-on shared macro

前面 6 个提交把功能写完了，这个提交做**工程化收尾**：给所有改动套上编译期宏开关。

### 8.1 新文件 `include/as_async_feature.h`

```cpp
#pragma once

// Shared by the vendor VM, engine integration and project consumers.
// Development defaults to ON. Override consistently for the entire build, never per translation unit.
#ifndef WITH_ANGELSCRIPT_ASYNC_AWAIT
#define WITH_ANGELSCRIPT_ASYNC_AWAIT 1
#endif
```

- **默认开（1）**：开发期默认启用，需要裁剪的发行配置可以整体关掉。
- 放在 `include/`（公共头目录）：注释明说"vendor VM、引擎集成层、项目使用方三方共享"。
- 注释第二行是**严肃的 ABI 警告**："必须全工程统一覆盖，绝不能按翻译单元分别定义"。
  原因下面逐条看——这个宏会改变结构体布局和函数签名。

### 8.2 为什么这个宏如此"敏感"——逐处分析

**(a) `include/angelscript.h`**：头部 include 该宏定义文件（保证任何包含 angelscript.h
的 TU 都看到同一个宏）；`asEP_ENABLE_ASYNC_AWAIT = 48` 枚举值被套进 `#if`。
宏关闭时，宿主代码若引用此枚举会直接编译失败——这正是想要的效果：
**裁剪配置下，任何残留的特性使用都应在编译期暴露，而不是运行期静默出错。**

**(b) `source/as_scriptengine.h/.cpp`**：

```diff
+#if WITH_ANGELSCRIPT_ASYNC_AWAIT
 		bool   enableAsyncAwait;
+#endif
 	} ep;
```

⚠️ **这改变了 `ep` 结构体的内存布局！** 如果两个 TU 对宏的取值不一致，
一个看到的 `ep` 比另一个小 1 字节（加上对齐可能更多），跨 TU 传指针就是内存损坏。
这就是头文件里那句 ABI 警告的具体原因。

`SetEngineProperty`/`GetEngineProperty`/构造函数初始化三处同样套 `#if`，
与枚举的存废保持一致。

**(c) `source/as_scriptfunction.h`**：

```diff
+#if WITH_ANGELSCRIPT_ASYNC_AWAIT
 enum asEFuncTrait : asDWORD
+#else
+enum asEFuncTrait
+#endif
```

宏关闭时**连 `: asDWORD` 都拿掉**，完全恢复上游原貌；两个新 trait 枚举值同理。
这体现了明确的裁剪哲学：**关闭配置 = 与上游逐字节一致**，合并上游时零冲突。
（trait 位掩码同样影响 ABI：`asSFunctionTraits` 的位布局不能两套并存。）

**(d) `as_parser.h/.cpp` 的函数签名分叉**：

```diff
+#if WITH_ANGELSCRIPT_ASYNC_AWAIT
 	int ParseStatementBlock(asCScriptCode *script, asCScriptNode *block, bool isAsyncFunction = false);
+#else
+	int ParseStatementBlock(asCScriptCode *script, asCScriptNode *block);
+#endif
```

函数签名都变了——这也是 ABI 敏感点。`isParsingAsyncFunction`、`isParsingFunctionBody`
成员、`IsAsyncFunctionModifier()`/`IsAwaitExpression()` 方法、
`snAsync`/`snAwait` 枚举、`ASYNC_TOKEN`/`AWAIT_TOKEN` 常量、parser 里的每个插入块、
builder/compiler 里的每个校验块——全部套 `#if`。

**(e) 调用点的双份写法**（`as_compiler.cpp`）：

```diff
+#if WITH_ANGELSCRIPT_ASYNC_AWAIT
 		int r = parser.ParseStatementBlock(
 			script, blockBegin, in_outFunc->traits.GetTrait(asTRAIT_ASYNC));
+#else
+		int r = parser.ParseStatementBlock(script, blockBegin);
+#endif
```

**(f) 一个细节**（`as_builder.cpp`）：

```diff
+#if WITH_ANGELSCRIPT_ASYNC_AWAIT
 	funcTraits.SetTrait(asTRAIT_NODISCARD, funcTraits.GetTrait(asTRAIT_ASYNC));
+#else
+	funcTraits.SetTrait(asTRAIT_NODISCARD, false);
+#endif
```

`#else` 分支恢复的是基线原句。可以对比验证：本提交所有 `#else` 分支拼起来，
恰好等于把提交 1~6 全部撤掉——宏关闭配置没有夹带任何行为差异。

### 8.3 双开关架构

至此特性有了两层开关，职责分明：

| 开关 | 层级 | 默认 | 作用 |
|---|---|---|---|
| `WITH_ANGELSCRIPT_ASYNC_AWAIT` | 编译期 | 开 | 代码在不在二进制里（裁剪/合并上游） |
| `asEP_ENABLE_ASYNC_AWAIT` (ep.enableAsyncAwait) | 运行期 | 关 | 每个引擎实例认不认 async/await 语法 |

编译期宏管"有没有"，引擎属性管"用不用"。parser 里每个入口都是两个条件串联
（`engine->ep.enableAsyncAwait` 在 `IsAsyncFunctionModifier`/`IsAwaitExpression` 内部第一行）。

---

## 9. 专题：参数检查策略的 45 分钟四连变

把提交 3→4→5→6 里同一处检查的变化排在一起，是理解"fork 维护之难"的最佳案例：

| 版本 | 提交 | 条件 | `FVector pos`（按值结构体） | `FVector &in p` | `int x` |
|---|---|---|---|---|---|
| v1 | `0045f1d1` 05:53 | `IsReference() \|\| flag != NONE` | ❌ 误杀 | ❌ | ✔ |
| v2 | `8db9830b` 06:08 | `flag != NONE` | ❌ 误杀（fork 改写成 INOUTREF） | ❌ | ✔ |
| v3 | `d6d378fb` 06:10 | `IsReference()` | ❌ 误杀（fork 改写成引用） | ❌ | ✔ |
| v4 | `255a3d6f` 06:14 | 语法树里找 `ttAmp` | ✔ | ❌ | ✔ |

根因再强调一次：本 fork 在 `GetParsedFunctionDetails()` 里把所有按值传递的
VALUE 类型参数隐式改写为 `const T&` + `asTM_INOUTREF`（§1.3(2)）。
这个改写对调用约定是优化，但**信息不可逆地丢失了**——之后任何基于
`parameterTypes`/`inOutFlags` 的判断都无法区分两种写法。v4 跳出语义层、
回到语法层，一击命中。

---

## 10. 提交 8 与 9：`3c30219d` + `779b0201` — 一次当天回退的 GC 注册修复

### 10.1 修复想解决什么

`RegisterScriptFunction()` 给内建 `$func` 类型（脚本函数句柄/委托的内部表示）
注册行为。基线代码：

```cpp
engine->functionBehaviours.flags = asOBJ_REF | asOBJ_GC;
...
r = engine->RegisterBehaviourToObjectType(..., asBEHAVE_GETREFCOUNT, "int f()", ...); asASSERT( r >= 0 );
r = engine->RegisterBehaviourToObjectType(..., asBEHAVE_SETGCFLAG, ...); asASSERT( r >= 0 );
r = engine->RegisterBehaviourToObjectType(..., asBEHAVE_GETGCFLAG, ...); asASSERT( r >= 0 );
r = engine->RegisterBehaviourToObjectType(..., asBEHAVE_ENUMREFS, ...); asASSERT( r >= 0 );
r = engine->RegisterBehaviourToObjectType(..., asBEHAVE_RELEASEREFS, ...); asASSERT( r >= 0 );
```

问题在于 §1.3(3)：**本 fork 把 `asOBJ_GC` 定义为 0**。两个连锁后果：

1. `flags = asOBJ_REF | asOBJ_GC` 实际等于 `asOBJ_REF`——`$func` 不是 GC 类型；
2. `RegisterBehaviourToObjectType` 内部对 GC 类行为有校验：

   ```cpp
   else if( behaviour >= asBEHAVE_FIRST_GC && behaviour <= asBEHAVE_LAST_GC )
   {
       // Only allow GC behaviours for types registered to be garbage collected
       if( !(func.objectType->flags & asOBJ_GC) )   // asOBJ_GC == 0，永远为真
       {
           WriteMessage("", 0, 0, asMSGTYPE_ERROR, TXT_ILLEGAL_BEHAVIOUR_FOR_TYPE);
           return ConfigError(asILLEGAL_BEHAVIOUR_FOR_TYPE, ...);
       }
   ```

   `flags & 0` 恒为 0 → **每一次创建引擎**，这 5 个注册全部失败、
   全部向消息回调写错误（"poison every new engine's configuration"），
   Debug 下 `asASSERT(r >= 0)` 还会连环触发断言。

### 10.2 修复内容

- `flags` 改为纯 `asOBJ_REF`（去掉那个本来就等于没写的 `| asOBJ_GC`）；
- 删掉 5 个 GC 行为的注册语句；
- 删掉只被这些注册使用的 5 个 generic 包装函数
  （`ScriptFunction_GetRefCount_Generic` 等）；
- 顺带删掉一段 `#if 0` 包围的死代码（非 generic 调用约定版本，多年未启用）；
- 保留 `AddRef`/`Release`（引用计数与 GC 无关，引用类型必须有）。

修复后 `$func` 只剩引用计数语义——函数元数据靠 refcount 管理生命期，
不参与 GC 循环检测，与 fork"不用 AS 的 GC"的大方向一致。

### 10.3 为什么 2 小时 15 分后又回退了？

`779b0201` 是 `git revert`，无额外说明，净效果为零。提交信息里没有留下原因，
以下为基于代码事实的合理推断（供参考，非定论）：

1. **可能有下游依赖这些"失败注册"的副作用**：5 个注册虽然返回错误码，
   但注册流程中的某些副作用（如 `isPrepared = false`、函数名解析等）已经执行；
2. **错误消息可能是已知噪音**：这套基线已在生产环境跑了很久，消息回调里的
   这几条错误可能被上层静默吞掉，修复的收益没有想象中大，反而有未知风险；
3. **可能决定换更上游的修法**：比如在引擎集成层处理，而不是改 vendor VM 源码——
   vendor 目录改动越少，合并上游越轻松（对照 CYANCOOK 标记哲学）。

**教学要点**：这个"修复→回退"对本身也是信息——它说明 vendor 代码里
"看起来明显是 bug 的东西"也可能有维持现状的理由；对 fork 基线做"清理"前，
要先确认没有隐性依赖，并做好随时回退的准备（作者也确实这么干了）。

---

## 11. 提交 10：`83f6a668` — 生成脚本的存放位置引发的热重载越界

这是唯一一个直接附带完整事故报告的提交（提交信息即 postmortem）。

### 11.1 事故机理

`asCBuilder` 里有两个**按下标配对**的数组：

```cpp
asCArray<asCScriptCode *>  scripts;   // 第 i 个脚本源码
asCArray<asCParser*>       parsers;   // 第 i 个脚本对应的解析器
```

配对关系在 `BuildParallelParseScripts()` 建立（每 Parse 一个 scripts[n]，
push 一个 parser），并被后续阶段按下标联合使用：

```cpp
// BuildGenerateTypes():
for (n = 0; n < scripts.GetLength(); n++)          // ← 按 scripts 长度循环
{
    asCScriptNode *node = parsers[n]->GetScriptNode();   // ← 却按下标取 parsers
    RegisterTypesFromScript(node, scripts[n], ...);
}
// RegisterNonTypesFromScript 同理
```

async lowering（状态机改写，在引擎集成层，P4 CL 137443）会在**解析完成之后**
合成新的脚本源码（状态结构体、包装函数等），之前它们被直接 append 进了
`builder->scripts`——**但 `parsers` 没有对应的新条目**（这些脚本不是解析出来的，
没有 parser）。于是 `scripts.GetLength() > parsers.GetLength()`。

平时不炸，是因为 `BuildGenerateTypes` 在 lowering 之前就跑完了。
但**热重载依赖批处理**会对已 lowering 过的模块**重入** `BuildGenerateTypes`，
循环按膨胀后的 `scripts` 长度走，`parsers[n]` 越界读 → 崩溃/未定义行为。

### 11.2 修复：所有权不变，存放位置分开

`as_builder.h`：

```diff
 	asCArray<asCScriptCode *>                scripts;
 	asCArray<class asCParser*>               parsers;
 
+	//@CYANCOOK(Async Generated Script Ownership) BEGIN
+#if WITH_ANGELSCRIPT_ASYNC_AWAIT
+	// Scripts synthesized after parsing (async lowering). Owned by the builder, but kept out of
+	// `scripts`, which must stay index-aligned with `parsers` for BuildGenerateTypes re-entry.
+	asCArray<asCScriptCode *>                  generatedScripts;
+#endif
```

`as_builder.cpp` 析构函数：

```diff
+	//@CYANCOOK(Async Generated Script Ownership) BEGIN
+#if WITH_ANGELSCRIPT_ASYNC_AWAIT
+	for( n = 0; n < generatedScripts.GetLength(); n++ )
+	{
+		if( generatedScripts[n] )
+			asDELETE(generatedScripts[n],asCScriptCode);
+		generatedScripts[n] = 0;
+	}
+#endif
+	//@CYANCOOK(Async Generated Script Ownership) END
```

要点：

- 合成脚本仍然**由 builder 持有并负责释放**（引擎集成层只管生成，不管生命周期，
  避免泄漏）；析构循环照抄上面 `scripts` 的释放模式（含置零习惯）。
- 但它们**不再进入 `scripts`**，`scripts`↔`parsers` 的下标配对恢复完整，
  `BuildGenerateTypes`/`RegisterNonTypes` 重入安全。
- 注释里直接写明了配对不变式（"must stay index-aligned with `parsers`"），
  防止后来者再把生成脚本塞回去。
-  lowering 代码本体不在这个 git 仓库里（在引擎集成层，提交信息注明 P4 CL 137443），
  本提交只提供存储与清理的基础设施。

**教学要点**：
> "两个数组按下标配对"是一种**隐性契约**——编译器不会帮你检查。
> 任何第三方（这里是 lowering pass）往其中一个数组塞东西都会破坏契约。
> 修复的方向不是"让循环更聪明"，而是**把不属于契约的数据请出契约数组**，
> 并用注释把契约显式化。

---

## 12. 收尾全景：最终架构与设计经验

### 12.1 特性最终形态

```
用户脚本:  async FTask UMyClass::Foo(FVector Pos) { await Bar(); }
                    │
Parser  ┌─ IsAsyncFunctionModifier(): lookahead 识别 "async 类型 名字("
        ├─ ParseFunction(): 生成 snAsync 子节点
        ├─ IsAwaitExpression(): 函数体内识别 await（含非 async 函数的容错解析）
        └─ IsVarDecl(): 用 isParsingFunctionBody + IsAwaitExpression 排除误判
                    │
Builder ├─ GetParsedFunctionDetails(): snAsync → asTRAIT_ASYNC; 默认 nodiscard
        └─ RegisterScriptFunction(): 7 条合法性校验（实例方法/返回 Task/
           无显式 & 参数(语法层判定)/...）
                    │
Compiler└─ CompileExpressionTerm(): snAwait → 环境校验、Task 类型校验、
           lowering 未执行的熔断报错
                    │
        (状态机 lowering 在引擎集成层，生成脚本存 builder->generatedScripts)
```

双开关：`WITH_ANGELSCRIPT_ASYNC_AWAIT`（编译期，默认开）+
`asEP_ENABLE_ASYNC_AWAIT`（运行期，默认关）。

### 12.2 可迁移的经验

1. **上下文关键字三件套**：按普通标识符解析 → 特定位置 lookahead 试探 →
   `RewindTo` 回退 + 命中后节点改牌（`nodeType = snAwait`）。不污染词法层，向后兼容。
2. **报错质量是功能的一部分**：宁可让非法代码先解析出来再在编译期报精准错误
   （非 async 函数里的 await），也不要让它死在莫名其妙的解析路径上；
   报错信息要给出替代方案（"use Forget() explicitly"）。
3. **语义层被规范化抹平的信息，去语法层找**（参数检查四连变）。
4. **fork 维护的纪律**：CYANCOOK 标记框住一切改动；枚举位/属性编号给上游预留区间；
   裁剪宏的 `#else` 分支保持与上游逐字节一致；跨 TU 影响布局/签名的宏必须全工程统一。
5. **熔断器优于静默错误**：lowering 未接入时，编译路径上放一条
   "不可能到达"的硬报错，把集成 bug 变成明确信息。
6. **隐性契约显性化**：`scripts`↔`parsers` 下标配对这类契约，一旦破坏就是
   难查的越界——修复 + 注释固化。
7. ** Vendor 代码的"明显 bug"也可能是负载均衡的结果**：清理前先验证无隐性依赖，
   并保留回退能力（`3c30219d`→`779b0201` 的当天回退是负责任的做法）。
