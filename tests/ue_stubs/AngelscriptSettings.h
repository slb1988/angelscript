// ============================================================================
// AngelscriptSettings.h — UE 桩头：引擎集成层配置项
//
// fork 源码中以 FAngelscriptManager::ConfigSettings->bXxx 形式读取。
// 这里全部默认 false（最安静的行为），测试可按需修改后再创建引擎。
// ============================================================================
#pragma once

struct FAngelscriptSettings
{
	bool bWarnOnDivergentComparisonOperatorOverloads = false;
	bool bWarnOnImplicitSignedUnsignedConversion = false;
	bool bWarnOnIncrementDecrementInComplexExpression = false;
	bool bWarnOnUnusedReturnValueForConstMethods = false;
	bool bExposeGlobalFunctionsToOtherScriptFiles = false;
	bool bErrorOnIncorrectEditorOnlyCode = false;
	bool bErrorWhenUsingInvalidWorldContext = false;
};
