// ============================================================================
// ue_stubs.cpp — UE 桩的全局定义
// ============================================================================
#include "AngelscriptManager.h"
#include "AngelscriptCodeModule.h"
#include "CoreMinimal.h"

// asCString::Compare 需要完整类型
#include "as_config.h"
#include "as_string.h"

static FAngelscriptSettings GDefaultSettings;

FAngelscriptSettings* FAngelscriptManager::ConfigSettings = &GDefaultSettings;
bool FAngelscriptManager::bSimulateCooked = false;

FAngelscriptManager& FAngelscriptManager::Get()
{
	static FAngelscriptManager Instance;
	return Instance;
}

thread_local bool GIsAngelscriptWorldContextAvailable = true;

bool (*FAngelscriptCodeModule::ShouldFunctionEmitReturnInfo)(asCScriptFunction*) = nullptr;

// TMap<asCString, ...> 的键比较
bool MapKeyEq(const asCString& a, const asCString& b)
{
	return a.Compare(b) == 0;
}
