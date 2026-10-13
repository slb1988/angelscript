// ============================================================================
// CoreTypes.h — UE 桩头（仅用于独立测试，勿用于真实 UE 构建）
//
// 背景：本仓库是 ProjectLungfish 的 AngelScript UE fork 的 vendor VM 源码，
// 公共头 angelscript.h 直接 #include "CoreMinimal.h" 等 UE 头文件，
// 因此无法脱离 UE 编译。tests/ue_stubs/ 下的这些桩头提供了刚好够用的
// UE 类型/宏子集，使 VM 能在纯 VS2022 控制台工程中编译，便于调试
// async/await 前端（解析器/校验器）行为。
// ============================================================================
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>

// ---- 与 angelscript.h 中 MSVC 分支完全一致的基础类型定义 ----
// （相同的 typedef 重复定义在 C++ 中是合法的）
#ifdef _MSC_VER
typedef signed   __int8  int8;
typedef signed   __int16 int16;
typedef signed   __int32 int32;
typedef signed   __int64 int64;
typedef unsigned __int8  uint8;
typedef unsigned __int16 uint16;
typedef unsigned __int32 uint32;
typedef unsigned __int64 uint64;
#else
typedef std::int8_t   int8;
typedef std::int16_t  int16;
typedef std::int32_t  int32;
typedef std::int64_t  int64;
typedef std::uint8_t  uint8;
typedef std::uint16_t uint16;
typedef std::uint32_t uint32;
typedef std::uint64_t uint64;
#endif

// ---- 字符串/内联宏 ----
#ifndef TEXT
#define TEXT(x) x
#endif
#ifndef ANSI_TO_TCHAR
#define ANSI_TO_TCHAR(x) (x)
#endif
#ifndef FORCEINLINE
#define FORCEINLINE __forceinline
#endif

// ---- 编辑器开关：独立测试始终为非编辑器环境 ----
#ifndef WITH_EDITOR
#define WITH_EDITOR 0
#endif

// ---- 断言 ----
// UE 的 check() 失败即崩溃；ensure() 记录后返回 false 并继续。
#ifndef check
#define check(expr) \
	do { if (!(expr)) { \
		std::fprintf(stderr, "check failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
		std::abort(); \
	} } while (0)
#endif

#ifndef ensure
#define ensure(expr) \
	((expr) ? true : (std::fprintf(stderr, "ensure failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__), false))
#endif

#ifndef ensureMsgf
#define ensureMsgf(expr, fmt, ...) \
	((expr) ? true : (std::fprintf(stderr, "ensure failed: %s (%s:%d): ", #expr, __FILE__, __LINE__), \
		std::fprintf(stderr, fmt, ##__VA_ARGS__), std::fprintf(stderr, "\n"), false))
#endif

#ifndef checkSlow
#define checkSlow(expr) check(expr)
#endif

// ---- 调用约定宏（上游来自 windows.h，UE 构建来自 UE 头，这里直接定义）----
#ifndef STDCALL
#define STDCALL __stdcall
#endif
#ifndef CDECL
#define CDECL __cdecl
#endif
#ifndef THISCALL
#define THISCALL __thiscall
#endif

// ---- UE 整数界限常量 ----
constexpr int8   MIN_int8   = (int8)0x80;
constexpr int8   MAX_int8   = 0x7f;
constexpr int16  MIN_int16  = (int16)0x8000;
constexpr int16  MAX_int16  = 0x7fff;
constexpr int32  MIN_int32  = (int32)0x80000000;
constexpr int32  MAX_int32  = 0x7fffffff;
constexpr int64  MIN_int64  = (int64)0x8000000000000000ll;
constexpr int64  MAX_int64  = 0x7fffffffffffffffll;
constexpr uint8  MIN_uint8  = 0x00;
constexpr uint8  MAX_uint8  = 0xff;
constexpr uint16 MIN_uint16 = 0x0000;
constexpr uint16 MAX_uint16 = 0xffff;
constexpr uint32 MIN_uint32 = 0x00000000;
constexpr uint32 MAX_uint32 = 0xffffffffu;
constexpr uint64 MIN_uint64 = 0x0000000000000000ull;
constexpr uint64 MAX_uint64 = 0xffffffffffffffffull;

// ---- UE 全局状态（编辑器相关，独立测试恒为 false）----
inline bool GIsEditor = false;
