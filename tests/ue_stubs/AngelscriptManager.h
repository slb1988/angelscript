// ============================================================================
// AngelscriptManager.h — UE 桩头：引擎集成层管理器单例
// ============================================================================
#pragma once

#include "AngelscriptSettings.h"

class FAngelscriptManager
{
public:
	static FAngelscriptManager& Get();

	// fork 源码中既有 FAngelscriptManager::ConfigSettings 静态访问，
	// 也有 FAngelscriptManager::Get().ConfigSettings 实例式访问，
	// 因此必须是 static 成员。
	static FAngelscriptSettings* ConfigSettings;
	static bool bSimulateCooked;
};

// as_context.cpp 中 extern 声明，用于"无世界上下文"报错路径。
extern thread_local bool GIsAngelscriptWorldContextAvailable;
