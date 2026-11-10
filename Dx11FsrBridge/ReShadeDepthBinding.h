#pragma once

#include "third_party/reshade/reshade_api.hpp"
#include <vector>

namespace reshade_depth
{
struct DepthBinding
{
    reshade::api::effect_texture_variable variable { 0 };
    reshade::api::resource_view view { 0 }, srgb { 0 };
};

// 2026-11-10：DEPTH 是语义，不是固定变量名；ViewDepth 及自定义 include 可使用不同命名。
// 仅在重载后探测一次：暂绑独有 SRV，以 API 读回找出真正受 DEPTH 影响的变量，随后恢复。
template <typename Runtime>
DepthBinding find_depth_binding(Runtime *runtime, reshade::api::resource_view marker)
{
    std::vector<DepthBinding> bindings;
    runtime->enumerate_texture_variables(nullptr, [&bindings](auto *owner, auto variable)
    {
        DepthBinding binding { variable };
        owner->get_texture_binding(variable, &binding.view, &binding.srgb);
        bindings.push_back(binding);
    });
    if (bindings.empty() || marker == 0)
        return {};
    runtime->update_texture_bindings("DEPTH", marker, marker);
    DepthBinding result;
    for (const auto &binding : bindings)
    {
        reshade::api::resource_view current { 0 };
        runtime->get_texture_binding(binding.variable, &current, nullptr);
        if (current == marker && binding.view != marker)
        {
            result = binding;
            break;
        }
    }
    // 没有消费者时清除未被任何效果引用的探测绑定；重载时 Generic Depth 会重新设置语义。
    runtime->update_texture_bindings("DEPTH", result.view, result.srgb);
    return result;
}
}
