#include "ReShadeDepth.h"
#include "BridgeLogger.h"
#include "third_party/reshade/reshade.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

namespace reshade_depth
{
namespace
{
using namespace reshade::api;
std::atomic_bool g_registered { false };
bool g_enabled = true;
float g_scale = 1.0f;
HMODULE g_module = nullptr;
HMODULE g_reshade_module = nullptr;

struct State
{
    std::mutex mutex;
    DepthPass pass;
    effect_runtime *runtime = nullptr;
    effect_texture_variable anchor { 0 };
    resource_view original { 0 }, original_srgb { 0 };
    bool bound = false;
    bool wants_depth = true;
    bool pending = false;
    unsigned warnings = 0;
    std::uint32_t logged_w = 0, logged_h = 0;
    Frame frame {};
};

// 2026-11-09：与现有 FFX 后端相同，进程退出由 OS 回收，避免 loader lock 下释放 D3D。
State *g_state = nullptr;

void warn_once(State &state, unsigned bit, const char *message)
{
    if ((state.warnings & bit) != 0)
        return;
    state.warnings |= bit;
    LOG_WARN(blog::cat::coexist, std::string("reshade_depth_fallback reason=") + message);
}

void on_begin(effect_runtime *runtime, command_list *cmd_list, resource_view, resource_view)
{
    if (!g_registered.load(std::memory_order_acquire) || !runtime || !cmd_list ||
        runtime->get_device()->get_api() != device_api::d3d11)
        return;
    State &state = *g_state;
    std::lock_guard lock(state.mutex);
    if (state.bound)
        return;
    std::uint32_t width = 0, height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);
    auto *context = reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
    // 2026-11-09：其它交换链/小视图不能消耗主场景快照。
    if (!state.pass.matches(context, width, height))
        return;
    if (state.runtime != runtime)
    {
        state.runtime = runtime;
        state.anchor = { 0 };
    }
    // 2026-11-09：读取标准 ReShade.fxh 的 DEPTH 绑定，结束时原样恢复，防止缺帧沿用旧输出。
    // namespace 及编译器 unique_name 均兼容；自定义 DEPTH 名称未找到时安全放行。
    if (state.anchor == 0)
    {
        for (const char *name : { "ReShade::DepthBufferTex", "DepthBufferTex", "ReShade__DepthBufferTex" })
        {
            state.anchor = runtime->find_texture_variable(nullptr, name);
            if (state.anchor != 0) break;
        }
    }
    state.wants_depth = state.anchor != 0;
    if (!state.wants_depth)
    {
        state.pass.discard();
        return;
    }
    state.original = state.original_srgb = { 0 };
    runtime->get_texture_binding(state.anchor, &state.original, &state.original_srgb);
    if (state.original == 0)
    {
        state.pass.discard();
        warn_once(state, 1, "no_generic_depth_binding");
        return;
    }
    const bool pending = state.pending;
    state.pending = false;
    ID3D11ShaderResourceView *view = state.pass.consume(context, width, height, g_scale);
    if (!view)
    {
        if (pending) warn_once(state, 4, "depth_pass_or_runtime_context_mismatch");
        return;
    }
    const resource_view corrected { reinterpret_cast<std::uint64_t>(view) };
    runtime->update_texture_bindings("DEPTH", corrected, corrected);
    state.bound = true;
    if (state.logged_w != width || state.logged_h != height)
    {
        state.logged_w = width;
        state.logged_h = height;
        LOG_INFO(blog::cat::coexist, "reshade_depth_active input=" +
            std::to_string(state.frame.render_w) + "x" + std::to_string(state.frame.render_h) +
            " output=" + std::to_string(width) + "x" + std::to_string(height) +
            " jitter_correction=1 source_unchanged=1");
    }
}

void on_finish(effect_runtime *runtime, command_list *, resource_view, resource_view)
{
    if (!g_registered.load(std::memory_order_acquire))
        return;
    State &state = *g_state;
    std::lock_guard lock(state.mutex);
    if (state.bound && state.runtime == runtime)
    {
        runtime->update_texture_bindings("DEPTH", state.original, state.original_srgb);
        state.bound = false;
    }
}

void on_present(effect_runtime *runtime)
{
    if (!g_registered.load(std::memory_order_acquire)) return;
    std::lock_guard lock(g_state->mutex);
    std::uint32_t width = 0, height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);
    auto *context = reinterpret_cast<ID3D11DeviceContext *>(
        runtime->get_command_queue()->get_immediate_command_list()->get_native());
    // 2026-11-09：即使全部效果关闭/正在编译，Present 后也不能留下跨帧快照。
    if (g_state->pass.matches(context, width, height))
    {
        g_state->pass.discard();
        g_state->pending = false;
    }
}

void on_reload(effect_runtime *runtime)
{
    // 2026-11-09：Generic Depth 在设备创建时才注册；桥先注入时需把 begin 回调移到其后。
    // 修改的是另一事件的列表，不在 begin 回调迭代过程中改写该列表。
    reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin);
    if (!g_state) return;
    std::lock_guard lock(g_state->mutex);
    if (g_state->runtime == runtime)
    {
        g_state->anchor = { 0 };
        g_state->wants_depth = true;
        g_state->pass.discard();
        g_state->pending = false;
    }
}

void on_destroy(effect_runtime *runtime)
{
    if (!g_state) return;
    std::lock_guard lock(g_state->mutex);
    if (g_state->runtime == runtime)
    {
        g_state->bound = false;
        g_state->runtime = nullptr;
        g_state->anchor = { 0 };
        g_state->wants_depth = true;
        g_state->pass.reset();
        g_state->pending = false;
    }
}
}

void configure(bool enable, int jitter_percent)
{
    g_enabled = enable;
    g_scale = static_cast<float>(std::clamp(jitter_percent, -200, 200)) / 100.0f;
}

void initialize(HMODULE module)
{
    if (!g_enabled)
        return;
    if (!reshade::register_addon(module))
    {
        LOG_INFO(blog::cat::coexist, "reshade_depth_unavailable reason=reshade_addon_api_missing");
        return;
    }
    g_state = new State;
    g_module = module;
    g_reshade_module = reshade::internal::get_reshade_module_handle();
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin);
    reshade::register_event<reshade::addon_event::reshade_finish_effects>(on_finish);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reload);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_reload);
    reshade::register_event<reshade::addon_event::reshade_present>(on_present);
    g_registered.store(true, std::memory_order_release);
    LOG_INFO(blog::cat::coexist, "reshade_depth_registered correction=1 jitter_percent=" +
        std::to_string(static_cast<int>(g_scale * 100.0f)));
}

void shutdown(bool process_exiting)
{
    (void)process_exiting;
    if (!g_registered.exchange(false, std::memory_order_acq_rel))
        return;
    // 2026-11-09：ReShade 可能先卸载；显式卸载桥时仅向仍存在的模块撤销回调，不等待渲染锁。
    HMODULE found = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(g_reshade_module), &found) || found != g_reshade_module)
        return;
    reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_begin);
    reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(on_finish);
    reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reload);
    reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy);
    reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_reload);
    reshade::unregister_event<reshade::addon_event::reshade_present>(on_present);
    reshade::unregister_addon(g_module);
}

void capture(ID3D11DeviceContext *context, ID3D11Texture2D *depth, const Frame &frame)
{
    if (!g_registered.load(std::memory_order_acquire))
        return;
    State &state = *g_state;
    std::lock_guard lock(state.mutex);
    if (!state.wants_depth || state.bound)
        return;
    state.pending = false;
    if (!state.pass.capture(context, depth, frame))
    {
        warn_once(state, 2, "unsupported_depth_snapshot");
        return;
    }
    state.frame = frame;
    state.pending = true;
}
}
