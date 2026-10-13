// ============================================================================
// CoreMinimal.h — UE 桩头：TArray / TMap / TPair / TInlineAllocator 最小实现
//
// 只实现 fork 源码实际用到的 API 子集（.Num/.Add/.Find/.FindRef/.FindChecked/
// .FindOrAdd/.Remove/.RemoveSingleSwap/.RemoveAtSwap/.Contains/.IndexOf/
// .Empty/.Reset/.Reserve/operator[]/范围 for 迭代）。
// 语义与 UE 一致的关键点：
//   - TMap 迭代产出带 .Key/.Value 的 TPair（不是 std::pair 的 .first/.second）
//   - TMap::Find 找不到返回 nullptr；FindRef 找不到返回值类型的默认值
//   - TArray::IndexOf 找不到返回 -1；RemoveSingleSwap/RemoveAtSwap 不保序
// ============================================================================
#pragma once

#include "CoreTypes.h"
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

// asCString 没有 operator==，TMap<asCString, ...> 的键比较走这个重载，
// 在 ue_stubs.cpp 中定义（内部调 asCString::Compare）。
class asCString;
bool MapKeyEq(const asCString& a, const asCString& b);

// 通用键相等：默认 operator==（指针/整数/TPair 都走这里）
template <typename A, typename B>
inline bool MapKeyEq(const A& a, const B& b) { return a == b; }

// ---- UE 内存接口（as_typeinfo.cpp / as_variablescope.cpp 等使用）----
struct FMemory
{
	static void* Malloc(size_t size) { return _aligned_malloc(size, 16); }
	static void* Malloc(size_t size, uint32 alignment) { return _aligned_malloc(size, alignment); }
	static void  Free(void* p) { _aligned_free(p); }
	static void* Memcpy(void* dst, const void* src, size_t size) { return std::memcpy(dst, src, size); }
};

// ---- UE RAII 变量守卫 ----
template <typename T>
struct TGuardValue
{
	TGuardValue(T& ref, T newValue) : Ref(ref), OldValue(ref) { Ref = newValue; }
	~TGuardValue() { Ref = OldValue; }
	T& Ref;
	T OldValue;
};

// ---- UE 数学工具 ----
struct FMath
{
	template <typename T> static T Max(T a, T b) { return a > b ? a : b; }
	template <typename T> static T Min3(T a, T b, T c) { T m = a < b ? a : b; return m < c ? m : c; }
	static double Pow(double base, double exp) { return std::pow(base, exp); }
};

// ---- UE 字符串哈希（as_string.h 的 GetTypeHash(asCString) 使用）----
struct FCrc
{
	// FNV-1a，与 UE 实现不同但在本测试场景下只要求自洽
	static uint32 Strihash_DEPRECATED(int32 len, const char* str)
	{
		uint32 hash = 2166136261u;
		for (int32 i = 0; i < len; ++i)
		{
			hash ^= (uint8)str[i];
			hash *= 16777619u;
		}
		return hash;
	}
};

// ---- UE 哈希工具（as_map.h 使用）----
inline uint32 HashCombineFast(uint32 a, uint32 b)
{
	return a ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2));
}

template <typename T>
inline uint32 GetTypeHash(const T* p)
{
	uintptr_t v = reinterpret_cast<uintptr_t>(p);
	return (uint32)(v ^ (v >> 16));
}

// ---- UE 移动语义宏 ----
template <typename T>
inline T&& MoveTemp(T& v) { return static_cast<T&&>(v); }

// ---- TPair：UE 风格 .Key/.Value ----
template <typename K, typename V>
struct TPair
{
	K Key;
	V Value;
	TPair() : Key(), Value() {}
	TPair(const K& k, const V& v) : Key(k), Value(v) {}
};

template <typename K, typename V>
inline bool operator==(const TPair<K, V>& a, const TPair<K, V>& b)
{
	return MapKeyEq(a.Key, b.Key) && MapKeyEq(a.Value, b.Value);
}

// ---- TInlineAllocator：只是占位模板参数 ----
template <int N>
struct TInlineAllocator {};

// ---- TArray：std::vector 薄封装 ----
template <typename T, typename Allocator = void>
class TArray
{
public:
	int  Num() const { return (int)Data.size(); }
	bool IsEmpty() const { return Data.empty(); }

	int  Add(const T& v) { Data.push_back(v); return (int)Data.size() - 1; }
	template <typename... Args>
	int  Emplace(Args&&... args) { Data.emplace_back(std::forward<Args>(args)...); return (int)Data.size() - 1; }
	void AddUninitialized(int count) { Data.resize(Data.size() + (size_t)count); }
	void Reserve(int n) { Data.reserve((size_t)n); }
	void Empty() { Data.clear(); }
	void Reset() { Data.clear(); }

	bool Contains(const T& v) const { return IndexOf(v) != -1; }

	int IndexOf(const T& v) const
	{
		for (size_t i = 0; i < Data.size(); ++i)
			if (MapKeyEq(Data[i], v))
				return (int)i;
		return -1;
	}

	int Remove(const T& v)
	{
		int n = 0;
		for (auto it = Data.begin(); it != Data.end();)
		{
			if (MapKeyEq(*it, v)) { it = Data.erase(it); ++n; }
			else ++it;
		}
		return n;
	}

	bool RemoveSingleSwap(const T& v)
	{
		int i = IndexOf(v);
		if (i < 0) return false;
		RemoveAtSwap(i);
		return true;
	}

	void RemoveAtSwap(int index)
	{
		check(index >= 0 && index < Num());
		Data[(size_t)index] = Data.back();
		Data.pop_back();
	}

	T&       operator[](int i) { return Data[(size_t)i]; }
	const T& operator[](int i) const { return Data[(size_t)i]; }

	T* GetData() { return Data.data(); }

	auto begin() { return Data.begin(); }
	auto end() { return Data.end(); }
	auto begin() const { return Data.begin(); }
	auto end() const { return Data.end(); }

private:
	std::vector<T> Data;
};

// ---- TMap：TPair 线性表（测试场景数据量小，正确性优先于速度）----
// 用线性表而不是 std::map，是为了让范围 for 直接产出带 .Key/.Value 的 TPair，
// 并且键类型不需要 operator<（asCString 就没有）。
template <typename K, typename V>
class TMap
{
public:
	typedef TPair<K, V> PairType;

	void Add(const K& k, const V& v)
	{
		for (auto& p : Pairs)
			if (MapKeyEq(p.Key, k)) { p.Value = v; return; }
		Pairs.push_back(PairType(k, v));
	}

	V* Find(const K& k)
	{
		for (auto& p : Pairs)
			if (MapKeyEq(p.Key, k)) return &p.Value;
		return nullptr;
	}

	const V* Find(const K& k) const
	{
		for (auto& p : Pairs)
			if (MapKeyEq(p.Key, k)) return &p.Value;
		return nullptr;
	}

	// UE 语义：找不到返回 V 的默认值（指针类型即 nullptr）
	V FindRef(const K& k) const
	{
		const V* p = Find(k);
		return p ? *p : V();
	}

	// UE 语义：找不到触发 check（直接崩溃）
	V& FindChecked(const K& k)
	{
		V* p = Find(k);
		check(p != nullptr);
		return *p;
	}

	V& FindOrAdd(const K& k)
	{
		V* p = Find(k);
		if (p) return *p;
		Pairs.push_back(PairType(k, V()));
		return Pairs.back().Value;
	}

	int  Remove(const K& k)
	{
		int n = 0;
		for (auto it = Pairs.begin(); it != Pairs.end();)
		{
			if (MapKeyEq(it->Key, k)) { it = Pairs.erase(it); ++n; }
			else ++it;
		}
		return n;
	}

	void Reset() { Pairs.clear(); }
	int  Num() const { return (int)Pairs.size(); }
	void Reserve(int) {}

	auto begin() { return Pairs.begin(); }
	auto end() { return Pairs.end(); }
	auto begin() const { return Pairs.begin(); }
	auto end() const { return Pairs.end(); }

private:
	std::vector<PairType> Pairs;
};

// ---- TMultiMap：as_map.h 的 asCSymbolMap 使用 ----
// API 子集：AddUnique / Remove(k,v) / FindPair / Empty / Reserve /
// CreateConstKeyIterator（迭代同一键下的所有值，.Value() 取值）
template <typename K, typename V>
class TMultiMap
{
public:
	typedef TPair<K, V> PairType;

	class ConstKeyIterator
	{
	public:
		ConstKeyIterator(const std::vector<PairType>* pairs, const K& key)
			: PairsPtr(pairs), KeyCopy(key), Index(0)
		{
			Advance();
		}

		explicit operator bool() const { return Index < PairsPtr->size(); }
		void operator++() { ++Index; Advance(); }
		const V& Value() const { return (*PairsPtr)[Index].Value; }

	private:
		void Advance()
		{
			while (Index < PairsPtr->size() && !MapKeyEq((*PairsPtr)[Index].Key, KeyCopy))
				++Index;
		}

		const std::vector<PairType>* PairsPtr;
		K KeyCopy;
		size_t Index;
	};

	ConstKeyIterator CreateConstKeyIterator(const K& k) const
	{
		return ConstKeyIterator(&Pairs, k);
	}

	void AddUnique(const K& k, const V& v) { Pairs.push_back(PairType(k, v)); }

	// UE 语义：删除键和值都匹配的项
	void Remove(const K& k, const V& v)
	{
		for (auto it = Pairs.begin(); it != Pairs.end();)
		{
			if (MapKeyEq(it->Key, k) && MapKeyEq(it->Value, v))
				it = Pairs.erase(it);
			else
				++it;
		}
	}

	const PairType* FindPair(const K& k, const V& v) const
	{
		for (auto& p : Pairs)
			if (MapKeyEq(p.Key, k) && MapKeyEq(p.Value, v)) return &p;
		return nullptr;
	}

	void Empty() { Pairs.clear(); }
	void Reserve(int n) { Pairs.reserve((size_t)n); }

	auto begin() { return Pairs.begin(); }
	auto end() { return Pairs.end(); }
	auto begin() const { return Pairs.begin(); }
	auto end() const { return Pairs.end(); }

private:
	std::vector<PairType> Pairs;
};
