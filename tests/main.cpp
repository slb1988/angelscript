// ============================================================================
// AngelScript async/await 前端特性测试（独立 VS2022 控制台工程）
//
// 覆盖范围 = 本仓库 git 历史中 6e6f713b..HEAD 的 10 个提交实现的前端能力：
//   3b649270  async trait 占位
//   32e5e57a  上下文关键字 async/await 解析
//   0045f1d1  非法 async/await 场景诊断
//   8db9830b  await 语句与变量声明的二义性消除
//   d6d378fb+255a3d6f  按值参数放行 / 显式引用参数拒绝（语法层判定）
//   02ea5bcc  WITH_ANGELSCRIPT_ASYNC_AWAIT 宏门控 + asEP_ENABLE_ASYNC_AWAIT
//
// 注意：async 函数体的"状态机 lowering"不在本仓库（在引擎集成层，见提交
// 83f6a668 提到的 P4 CL 137443），所以编译器里埋了熔断器：
// "Async function reached bytecode compilation before state-machine lowering"。
// 因此本套测试断言的是 解析/校验/诊断 行为，而不是 async 函数的真实执行。
//
// 另一个坑：fork 的 asIScriptObject::GetObjectType() 依赖真实 UObject
// （见 tests/ue_stubs/UObject/Object.h 注释），所以本套件不实例化脚本对象，
// 运行时冒烟只执行全局函数。
// ============================================================================

#include <cassert>
#include <crtdbg.h>

// NDEBUG 下 assert 为空操作，注册检查用自定义宏
#define CHECK_REG(r) do { if ((r) < 0) { printf("FATAL: registration failed (r=%d) at %s:%d\n", (r), __FILE__, __LINE__); std::abort(); } } while (0)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "angelscript.h"

// ---------------------------------------------------------------------------
// 一、测试用的应用层类型
//
// async 校验要求返回类型名为 FTask / TTask / TTask<...>（按名字匹配，
// 因为任务类型本来由引擎集成层注册，VM 层只认名字）。这里注册两个
// 极简引用类型顶上。FVector 是值类型，用来验证"按值结构体参数"的行为
// ——这是本 fork 的关键陷阱：GetParsedFunctionDetails 会把按值传递的
// VALUE 类型参数隐式改写为 const 引用（IsReference=true, flag=INOUTREF），
// 语义层无法区分"用户写了 &"和"fork 自动改写"，最终校验只能查语法树。
// ---------------------------------------------------------------------------

struct DummyTask { int refs; };

static DummyTask* Task_Factory()                    { return new DummyTask{ 1 }; }
static void       Task_AddRef(DummyTask* t)         { ++t->refs; }
static void       Task_Release(DummyTask* t)        { if (--t->refs == 0) delete t; }

struct FVectorStub { float x, y, z; };

static void FVector_Construct(void* mem)                              { new (mem) FVectorStub{ 0, 0, 0 }; }
static void FVector_Construct3f(float x, float y, float z, void* mem) { new (mem) FVectorStub{ x, y, z }; }

// ---------------------------------------------------------------------------
// 二、编译消息收集
// ---------------------------------------------------------------------------

struct Messages
{
	std::vector<std::string> errors;
	std::vector<std::string> warnings;
	std::vector<std::string> infos;

	bool HasErrorContaining(const char* sub) const   { return AnyContains(errors, sub); }
	bool HasWarningContaining(const char* sub) const { return AnyContains(warnings, sub); }
	bool AnyAsyncError() const
	{
		// 所有 async 特性专属报错都含这些关键词之一
		static const char* keys[] = {
			"Async", "async functions", "await", "state-machine lowering" };
		for (const std::string& e : errors)
			for (const char* k : keys)
				if (e.find(k) != std::string::npos) return true;
		return false;
	}
	void Dump() const
	{
		for (const std::string& s : errors)   printf("    [err] %s\n", s.c_str());
		for (const std::string& s : warnings) printf("    [wrn] %s\n", s.c_str());
		for (const std::string& s : infos)    printf("    [inf] %s\n", s.c_str());
	}

private:
	static bool AnyContains(const std::vector<std::string>& v, const char* sub)
	{
		for (const std::string& s : v)
			if (s.find(sub) != std::string::npos) return true;
		return false;
	}
};

static void MessageCallback(const asSMessageInfo* msg, void* param)
{
	Messages* msgs = static_cast<Messages*>(param);
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s (%d, %d) : %s",
		msg->section ? msg->section : "", msg->row, msg->col, msg->message);
	if (msg->type == asMSGTYPE_ERROR)        msgs->errors.push_back(buf);
	else if (msg->type == asMSGTYPE_WARNING) msgs->warnings.push_back(buf);
	else                                     msgs->infos.push_back(buf);

	// 同步打印，便于定位引擎初始化期的错误（如注册失败）
	if (msg->type == asMSGTYPE_ERROR)
		printf("    [live-err] %s\n", buf);
}

// ---------------------------------------------------------------------------
// 三、引擎/模块辅助
// ---------------------------------------------------------------------------

static asIScriptEngine* CreateTestEngine(Messages& msgs, bool enableAsync)
{
	asIScriptEngine* engine = asCreateScriptEngine();
	if (engine == nullptr) { printf("FATAL: asCreateScriptEngine failed\n"); std::abort(); }

	int r = engine->SetMessageCallback(asFUNCTION(MessageCallback), &msgs, asCALL_CDECL);
	if (r < 0) { printf("FATAL: SetMessageCallback failed\n"); std::abort(); }

	// 基线提交 6e6f713b 引入的引擎属性（默认 false）
	engine->SetEngineProperty(asEP_ENABLE_ASYNC_AWAIT, enableAsync ? 1 : 0);

	// 本 fork 移除了 '@' token，对象句柄依赖隐式句柄类型，需显式开启
	engine->SetEngineProperty(asEP_ALLOW_IMPLICIT_HANDLE_TYPES, 1);

	// 任务类型（引用类型，引用计数即可）
	for (const char* name : { "FTask", "TTask" })
	{
		r = engine->RegisterObjectType(name, 0, asOBJ_REF | asOBJ_IMPLICIT_HANDLE); CHECK_REG(r);
		// 注意：本 fork 的 token 表移除了 '@'（ttHandle），对象句柄靠
		// asOBJ_IMPLICIT_HANDLE 隐式表达 —— 声明里直接写裸类型名。
		r = engine->RegisterObjectBehaviour(name, asBEHAVE_FACTORY, (std::string(name) + " f()").c_str(),
			asFUNCTION(Task_Factory), asCALL_CDECL);                                      CHECK_REG(r);
		r = engine->RegisterObjectBehaviour(name, asBEHAVE_ADDREF, "void f()",
			asFUNCTION(Task_AddRef), asCALL_CDECL_OBJLAST);                               CHECK_REG(r);
		r = engine->RegisterObjectBehaviour(name, asBEHAVE_RELEASE, "void f()",
			asFUNCTION(Task_Release), asCALL_CDECL_OBJLAST);                              CHECK_REG(r);
	}

	// 值类型（验证按值参数行为）
	r = engine->RegisterObjectType("FVector", sizeof(FVectorStub), asOBJ_VALUE | asOBJ_POD); CHECK_REG(r);
	r = engine->RegisterObjectBehaviour("FVector", asBEHAVE_CONSTRUCT, "void f()",
		asFUNCTION(FVector_Construct), asCALL_CDECL_OBJLAST);                              CHECK_REG(r);
	(void)FVector_Construct3f;

	return engine;
}

// 返回模块，rBuild 带回 Build() 的返回码（<0 表示构建失败）
// classNames：脚本里声明的 class 名。本 fork 要求脚本类在构建前注册
// PreClassData（UE 集成层用来挂接 UObject 影子类型），否则报
// "Structs cannot be base classes for anything"。struct/接口/mixin 不需要。
static asIScriptModule* BuildScript(asIScriptEngine* engine, const char* code, int& rBuild,
	std::initializer_list<const char*> classNames = {})
{
	asIScriptModule* mod = engine->GetModule("test", asGM_ALWAYS_CREATE);
	for (const char* cls : classNames)
		mod->AddPreClassData(cls, asPreClassData{});
	mod->AddScriptSection("test.as", code, std::strlen(code));
	rBuild = mod->Build();
	return mod;
}

// ---------------------------------------------------------------------------
// 四、断言宏
// ---------------------------------------------------------------------------

static int g_passCount = 0;
static int g_failCount = 0;

static void Report(bool ok, const char* name, const Messages& msgs)
{
	if (ok) { ++g_passCount; printf("[PASS] %s\n", name); }
	else
	{
		++g_failCount;
		printf("[FAIL] %s\n", name);
		msgs.Dump();
	}
}

#define CHECK(cond, name) Report((cond), (name), msgs)

// 期望：编译失败，且报错信息中包含 expectedSub
static void ExpectBuildError(const char* name, bool enableAsync, const char* code, const char* expectedSub,
	std::initializer_list<const char*> classNames = {})
{
	Messages msgs;
	asIScriptEngine* engine = CreateTestEngine(msgs, enableAsync);
	int rBuild = 0;
	asIScriptModule* mod = BuildScript(engine, code, rBuild, classNames);

	Report(rBuild < 0 && msgs.HasErrorContaining(expectedSub), name, msgs);

	mod->Discard();
	engine->ShutDownAndRelease();
}

// 期望：编译成功，且没有任何错误消息
static void ExpectBuildOk(const char* name, bool enableAsync, const char* code,
	std::initializer_list<const char*> classNames = {})
{
	Messages msgs;
	asIScriptEngine* engine = CreateTestEngine(msgs, enableAsync);
	int rBuild = 0;
	asIScriptModule* mod = BuildScript(engine, code, rBuild, classNames);

	Report(rBuild >= 0 && msgs.errors.empty(), name, msgs);

	mod->Discard();
	engine->ShutDownAndRelease();
}

// ---------------------------------------------------------------------------
// 五、测试用例
// ---------------------------------------------------------------------------

// --- 引擎属性门控（基线提交 6e6f713b + 提交 02ea5bcc）---
static void TestEngineProperty()
{
	Messages msgs;
	asIScriptEngine* engine = CreateTestEngine(msgs, false);

	CHECK(engine->GetEngineProperty(asEP_ENABLE_ASYNC_AWAIT) == 0,
		"EP: asEP_ENABLE_ASYNC_AWAIT 默认关闭");

	engine->SetEngineProperty(asEP_ENABLE_ASYNC_AWAIT, 1);
	CHECK(engine->GetEngineProperty(asEP_ENABLE_ASYNC_AWAIT) == 1,
		"EP: 运行时可开启 asEP_ENABLE_ASYNC_AWAIT");

	engine->ShutDownAndRelease();
}

// --- 向后兼容：特性开启后 await 仍可作普通标识符（提交 0045f1d1 的两级判定）---
static void TestAwaitAsIdentifier()
{
	Messages msgs;
	asIScriptEngine* engine = CreateTestEngine(msgs, true);

	int rBuild = 0;
	asIScriptModule* mod = BuildScript(engine, R"(		int AddToAwait()
		{
			int await = 1;
			await = await + 2;   // '=' 不在 IsAwaitExpression 的操作数集合里
			return await;
		}
	)", rBuild);
	CHECK(msgs.errors.empty(), "COMPAT: 特性开启时 await 仍可作为变量名");

	// 运行时冒烟：顺带验证模块字节码真的能跑
	if (msgs.errors.empty())
	{
		asIScriptFunction* fn = mod->GetFunctionByDecl("int AddToAwait()");
		asIScriptContext* ctx = engine->CreateContext();
		ctx->Prepare(fn);
		int r = ctx->Execute();
		CHECK(r == asEXECUTION_FINISHED && ctx->GetReturnDWord() == 3,
			"COMPAT: 变量 await 参与运算并正确执行");
		ctx->Release();
	}

	mod->Discard();
	engine->ShutDownAndRelease();
}

// --- async 方法解析与编译（无 await 体可正常编译）（提交 32e5e57a）---
static void TestAsyncMethodParses()
{
	Messages msgs;
	asIScriptEngine* engine = CreateTestEngine(msgs, true);

	int rBuild = 0;
	asIScriptModule* mod = BuildScript(engine, R"(		class Foo
		{
			async FTask M() { return nullptr; }
			async TTask N() { return nullptr; }   // 名字命中 "TTask" 分支
		}
	)", rBuild, { "Foo" });
	CHECK(msgs.errors.empty(), "PARSE: async FTask/TTask 方法正常编译");

	asITypeInfo* foo = mod->GetTypeInfoByName("Foo");
	CHECK(foo != nullptr && foo->GetMethodByDecl("FTask M()") != nullptr,
		"PARSE: async 方法完成注册（声明中不含 async 关键字）");

	mod->Discard();
	engine->ShutDownAndRelease();
}

// --- 诊断：async 必须是实例方法（提交 0045f1d1 ①）---
static void TestAsyncGlobalRejected()
{
	ExpectBuildError("DIAG: 全局 async 函数被拒绝", true,
		"async FTask G() { return nullptr; }",
		"Async functions must be instance class methods");
}

// --- 诊断：async 必须返回任务类型（提交 0045f1d1 ⑦）---
static void TestAsyncMustReturnTask()
{
	ExpectBuildError("DIAG: async 返回 int 被拒绝", true,
		"class Foo { async int M() { return 0; } }",
		"Async functions must return FTask or TTask<T>", { "Foo" });
}

// --- 诊断：async 不能返回引用（提交 0045f1d1 ⑥）---
static void TestAsyncRefReturnRejected()
{
	ExpectBuildError("DIAG: async 返回引用被拒绝", true,
		"class Foo { async FVector& M() { return FVector(); } }",
		"Async functions cannot return references", { "Foo" });
}

// --- 诊断：显式 &out 参数被拒绝（提交 0045f1d1 ⑧）---
static void TestAsyncOutParamRejected()
{
	ExpectBuildError("DIAG: async &out 参数被拒绝", true,
		"class Foo { async FTask M(int &out x) { return nullptr; } }",
		"Async functions cannot have in, out, or reference parameters", { "Foo" });
}

// --- 诊断：显式 &in 参数同样被拒绝（提交 255a3d6f 的关键用例）---
// fork 会把按值结构体参数改写成 const 引用，语义层无法区分；
// 语法树里 const FVector &in 带 ttAmp，必须被拒。
static void TestAsyncInRefParamRejected()
{
	ExpectBuildError("DIAG: async const FVector &in 参数被拒绝（语法层检测 &）", true,
		"class Foo { async FTask M(const FVector &in p) { return nullptr; } }",
		"Async functions cannot have in, out, or reference parameters", { "Foo" });
}

// --- 按值参数放行（提交 255a3d6f 修复的回归用例）---
// 同一个 FVector 按值传递时绝不能被误判为引用参数。
static void TestAsyncByValueParamAllowed()
{
	ExpectBuildOk("DIAG: async 按值参数（FVector/int）正常编译", true,
		"class Foo { async FTask M(FVector p, int x) { return nullptr; } }", { "Foo" });
}

// --- 诊断：非 async 函数里的 await（提交 0045f1d1 编译期环境关）---
// 解析器故意在非 async 函数里也解析 await（为了报出这句人话而不是语法错误）。
static void TestAwaitInNonAsyncRejected()
{
	ExpectBuildError("DIAG: 非 async 函数中的 await 被拒绝", true,
		"class Foo { FTask N() { return nullptr; } void M() { await N(); } }",
		"await expressions are only valid inside async functions", { "Foo" });
}

// --- 诊断：await 的操作数必须是任务（提交 0045f1d1 编译期类型关）---
static void TestAwaitNonTaskRejected()
{
	ExpectBuildError("DIAG: await 非任务类型被拒绝", true,
		"class Foo { async FTask M() { int x = 1; await x; return nullptr; } }",
		"await operand must be FTask or TTask<T>", { "Foo" });
}

// --- 熔断器：await 到达字节码编译说明 lowering 没跑（提交 0045f1d1）---
// 状态机 lowering 在引擎集成层，独立 VM 里任何合法的 await 都必须撞这道墙。
static void TestAwaitHitsLoweringBreaker()
{
	ExpectBuildError("DIAG: 合法 await 撞上 lowering 熔断器", true,
		"class Foo { FTask N() { return nullptr; } async FTask M() { await N(); return nullptr; } }",
		"Async function reached bytecode compilation before state-machine lowering", { "Foo" });
}

// --- 诊断：async 禁止 allow_discard（提交 0045f1d1 ⑤）---
static void TestAsyncAllowDiscardRejected()
{
	ExpectBuildError("DIAG: async + allow_discard 被拒绝", true,
		"class Foo { async FTask M() allow_discard { return nullptr; } }",
		"allow_discard", { "Foo" });
}

// --- async 默认 nodiscard（提交 32e5e57a builder 默认项）---
static void TestAsyncDefaultNoDiscard()
{
	Messages msgs;
	asIScriptEngine* engine = CreateTestEngine(msgs, true);

	int rBuild = 0;
	asIScriptModule* mod = BuildScript(engine, R"(		class Foo
		{
			async FTask M() { return nullptr; }
			void Caller() { M(); }   // 丢弃 nodiscard 返回值 → 警告
		}
	)", rBuild, { "Foo" });
	CHECK(msgs.errors.empty(), "NODISCARD: 丢弃 async 返回值不产生错误（只是警告）");
	CHECK(msgs.HasWarningContaining("nodiscard"),
		"NODISCARD: 丢弃 async 返回值产生 nodiscard 警告");

	mod->Discard();
	engine->ShutDownAndRelease();
}

// --- 诊断：async 接口方法被拒绝（提交 0045f1d1 ③）---
static void TestAsyncInterfaceRejected()
{
	// 注意：接口体内方法走的是独立解析路径（不经过 IsAsyncFunctionModifier），
	// 当前实现里 async 在接口中直接是解析错误。
	// Builder 层的 "Async interface ... not supported" 校验实际不可达，属防御性代码。
	ExpectBuildError("DIAG: async 接口方法被拒绝（当前为解析期错误）", true,
		"interface I { async FTask M(); }",
		"Instead found identifier 'async'");
}

// --- 诊断：async mixin 方法被拒绝（提交 0045f1d1 ③）---
static void TestAsyncMixinRejected()
{
	// 注意：本 fork 的 mixin 语法为 'mixin <函数定义>'（ParseMixin），
	// 不是上游的 'mixin class'。
	ExpectBuildError("DIAG: async mixin 函数被拒绝", true,
		"mixin async FTask F() { return nullptr; }",
		"Async interface, mixin, and override methods are not supported");
}

// --- 诊断：async 属性访问器被拒绝（提交 0045f1d1 ④）---
static void TestAsyncPropertyAccessorRejected()
{
	ExpectBuildError("DIAG: async 属性访问器被拒绝", true,
		"class Foo { async FTask get_P() property { return nullptr; } }",
		"Async property accessors are not supported", { "Foo" });
}

// --- 二义性：await 语句不能被当成变量声明（提交 8db9830b）---
// IsType() 故意把任何标识符当潜在类型，'await Foo();' 曾经被判成
// "声明一个类型为 await、名为 Foo 的变量"。
// 该用例已被 TestAwaitHitsLoweringBreaker 间接覆盖（能走到编译期错误，
// 说明解析期没有误入变量声明分支），这里再补一个最直接的形态。
static void TestAwaitStatementNotVarDecl()
{
	ExpectBuildError("DIAG: 'await t;' 不会被误解析为变量声明", true,
		R"(
		class Foo
		{
			FTask N() { return nullptr; }
			async FTask M()
			{
				FTask t = N();
				await t;            // 最容易被误判成变量声明的写法
				return nullptr;
			}
		})",
		// 能收到这条编译期错误，就证明解析期正确识别了 await 表达式
		"Async function reached bytecode compilation before state-machine lowering", { "Foo" });
}

// --- 门控：特性关闭时 async 语法不生效（提交 02ea5bcc 运行期开关）---
static void TestFeatureOffRejectsAsyncSyntax()
{
	Messages msgs;
	asIScriptEngine* engine = CreateTestEngine(msgs, false);   // 特性关闭

	int rBuild = 0;
	asIScriptModule* mod = BuildScript(engine,
		"class Foo { async FTask M() { return nullptr; } }", rBuild, { "Foo" });

	// 关闭后 async 只是普通标识符：'async FTask M()' 会变成
	// "类型 async + 名字 FTask ..."之类的解析错误，总之必然构建失败，
	// 且绝不能出现 async 特性专属的报错文案。
	CHECK(rBuild < 0 || !msgs.errors.empty(),
		"GATE: 特性关闭时 async 声明无法通过编译");
	CHECK(!msgs.AnyAsyncError(),
		"GATE: 特性关闭时不产生 async 特性专属报错");

	mod->Discard();
	engine->ShutDownAndRelease();
}

// --- 组合场景：一个类里多个 async 方法 + 普通方法共存 ---
static void TestMixedClass()
{
	ExpectBuildOk("PARSE: async 方法与普通方法/属性共存", true,
		R"(
		class Foo
		{
			int Value;
			void Normal() { Value = 1; }
			FTask MakeTask() { return nullptr; }
			async FTask DoAsync(int count, FVector pos) { return nullptr; }
		}
		class Bar
		{
			async TTask Other() { return nullptr; }
		}
		)", { "Foo", "Bar" });
}

// ---------------------------------------------------------------------------

int main()
{
	// 无缓冲输出 + CRT 报告重定向到 stderr，避免调试 CRT 弹窗挂起 CI 式运行
	setvbuf(stdout, nullptr, _IONBF, 0);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
	_set_abort_behavior(0, ~0u);

	printf("=== AngelScript async/await frontend tests ===\n\n");

	TestEngineProperty();
	TestAwaitAsIdentifier();
	TestAsyncMethodParses();
	TestMixedClass();

	TestAsyncGlobalRejected();
	TestAsyncMustReturnTask();
	TestAsyncRefReturnRejected();
	TestAsyncOutParamRejected();
	TestAsyncInRefParamRejected();
	TestAsyncByValueParamAllowed();
	TestAwaitInNonAsyncRejected();
	TestAwaitNonTaskRejected();
	TestAwaitHitsLoweringBreaker();
	TestAsyncAllowDiscardRejected();
	TestAsyncDefaultNoDiscard();
	TestAsyncInterfaceRejected();
	TestAsyncMixinRejected();
	TestAsyncPropertyAccessorRejected();
	TestAwaitStatementNotVarDecl();
	TestFeatureOffRejectsAsyncSyntax();

	printf("\n=== %d passed, %d failed ===\n", g_passCount, g_failCount);
	return g_failCount == 0 ? 0 : 1;
}
