#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <unordered_map>

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

// 2026-11-09：原生与 GIMI/ReShade 的 this 不可互换；按对象保留不可变的原始槽位。
// 快照由调用方保留到进程结束；重装会递增代次，线程缓存不能沿用被复用地址的旧入口。
class OriginalMethods
{
  public:
    void publish(const void *instance, void **methods, std::size_t count)
    {
        std::lock_guard lock(mutex_);
        entries_[instance] = { methods, count };
        revision_.fetch_add(1, std::memory_order_release);
    }

    void *find(const void *instance, std::size_t index) const
    {
        struct Cached
        {
            const void *instance = nullptr;
            void **methods = nullptr;
            std::size_t count = 0;
        };
        struct Cache
        {
            const OriginalMethods *owner = nullptr;
            std::uint64_t revision = 0;
            std::array<Cached, 4> entries {};
            std::size_t cursor = 0;
        };
        thread_local Cache cache;
        const auto revision = revision_.load(std::memory_order_acquire);
        if (cache.owner != this || cache.revision != revision)
        {
            cache = {};
            cache.owner = this;
            cache.revision = revision;
        }
        for (const auto &entry : cache.entries)
        {
            if (entry.instance == instance && entry.methods != nullptr)
                return index < entry.count ? entry.methods[index] : nullptr;
        }
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(instance);
        if (it == entries_.end())
            return nullptr;
        auto &entry = cache.entries[cache.cursor++ % cache.entries.size()];
        entry = { instance, it->second.methods, it->second.count };
        return index < entry.count ? entry.methods[index] : nullptr;
    }

  private:
    struct Entry
    {
        void **methods;
        std::size_t count;
    };
    mutable std::mutex mutex_;
    std::unordered_map<const void *, Entry> entries_;
    std::atomic<std::uint64_t> revision_ { 0 };
};

}
