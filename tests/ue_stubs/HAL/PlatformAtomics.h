// ============================================================================
// HAL/PlatformAtomics.h — UE 桩头：原子计数（as_atomic.cpp 使用）
// ============================================================================
#pragma once

#include <intrin.h>

struct FPlatformAtomics
{
	static int InterlockedIncrement(volatile int* v)
	{
		return (int)_InterlockedIncrement((volatile long*)v);
	}
	static int InterlockedDecrement(volatile int* v)
	{
		return (int)_InterlockedDecrement((volatile long*)v);
	}
};
