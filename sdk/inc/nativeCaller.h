// Native invocation helpers (compatible with the official SDK).
#pragma once

#include <cstring>
#include <type_traits>

#include "main.h"
#include "types.h"

#define NATIVE_DECL __forceinline

template <typename T>
static inline void nativePush(T val)
{
	UINT64 val64 = 0;
	static_assert(sizeof(T) <= sizeof(UINT64), "native argument too large");
	std::memcpy(&val64, &val, sizeof(T));
	nativePush64(val64);
}

static inline void nativePush(Vector3 v)
{
	nativePush(v.x);
	nativePush(v.y);
	nativePush(v.z);
}

template <typename R, typename... Args>
static inline R invoke(UINT64 hash, Args... args)
{
	nativeInit(hash);
	(nativePush(args), ...);
	if constexpr (std::is_void_v<R>)
		nativeCall();
	else
		return *reinterpret_cast<R*>(nativeCall());
}
