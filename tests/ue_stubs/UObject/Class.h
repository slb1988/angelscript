// ============================================================================
// UObject/Class.h — UE 桩头
//
// angelscript.h 的 asIScriptObject::GetObjectType() 默认实现会把 this 强转为
// UObject* 并访问 GetClass()->ScriptTypePtr。在真实 UE 集成里脚本对象由
// UObject 派生类承载；独立测试中不要调用该路径（脚本对象运行时不进入
// GetObjectType，本测试套件刻意只做"编译期"断言，避免走到这里）。
// ============================================================================
#pragma once

class UClass
{
public:
	void* ScriptTypePtr = nullptr;
};
