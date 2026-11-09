#pragma once

#include <cstdint>

namespace hook_compatibility
{
// 2026-11-09：GetProcAddress 允许低 16 位序号，名称指针必须先排除整数资源。
inline bool is_ordinal_export(const char *name)
{
    return reinterpret_cast<std::uintptr_t>(name) <= 0xffffu;
}
}
