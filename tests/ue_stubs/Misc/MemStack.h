// ============================================================================
// Misc/MemStack.h — UE 桩头：线性内存栈（解析器/构建器用它分配 AST 节点）
//
// 语义：Alloc 出来的内存随 MemStack 析构整体释放，不支持单独 Free。
// fork 源码只用到了 Alloc(size, align)。
// ============================================================================
#pragma once

#include <cstdlib>
#include <vector>

class FMemStackBase
{
public:
	FMemStackBase() = default;
	~FMemStackBase()
	{
		for (void* b : Blocks)
			_aligned_free(b);
	}

	FMemStackBase(const FMemStackBase&) = delete;
	FMemStackBase& operator=(const FMemStackBase&) = delete;

	void* Alloc(size_t size, size_t align)
	{
		void* p = _aligned_malloc(size, align);
		Blocks.push_back(p);
		return p;
	}

private:
	std::vector<void*> Blocks;
};
