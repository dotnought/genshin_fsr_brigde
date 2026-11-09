#pragma once

#include <cstdint>

namespace hook_compatibility
{
// 2026-11-09：GetProcAddress 允许低 16 位序号，名称指针必须先排除整数资源。
inline bool is_ordinal_export(const char *name)
{
    return reinterpret_cast<std::uintptr_t>(name) <= 0xffffu;
}
// 2026-11-09：不同 IAT 下一跳不能折叠成首次保存的同一个函数（双 provider 回归）。
inline bool can_chain_import(const void *current, const void *replacement, const void *original)
{
    return current == replacement || (current != nullptr && (original == nullptr || current == original));
}

}
