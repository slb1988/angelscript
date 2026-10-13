// ============================================================================
// AngelscriptCodeModule.h — UE 桩头：代码模块钩子
//
// fork 中以 `FAngelscriptCodeModule::ShouldFunctionEmitReturnInfo && ...`
// 形式使用（先判空再调用），默认 nullptr 即关闭该钩子。
// ============================================================================
#pragma once

class asCScriptFunction;

struct FAngelscriptCodeModule
{
	static bool (*ShouldFunctionEmitReturnInfo)(asCScriptFunction*);
};
