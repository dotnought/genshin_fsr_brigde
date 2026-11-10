#include "ReShadeDepthBinding.h"
#include "NativeGraphicsObject.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

using namespace reshade::api;
#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            std::exit(1); \
        } \
    } while (0)

namespace
{
// 2026-11-10：只提供语义 API，没有 find_texture_variable，证明探测不依赖任何变量命名。
struct Runtime
{
    std::map<std::uint64_t, std::string> variables;
    std::map<std::string, std::pair<resource_view, resource_view>> bindings;
    template <typename F> void enumerate_texture_variables(const char *, F callback)
    {
        for (const auto &[variable, semantic] : variables)
            callback(this, effect_texture_variable { variable });
    }
    void get_texture_binding(effect_texture_variable variable, resource_view *view, resource_view *srgb)
    {
        const auto found = bindings.find(variables.at(variable.handle));
        const auto value = found == bindings.end() ? std::pair<resource_view, resource_view> {} : found->second;
        if (view) *view = value.first;
        if (srgb) *srgb = value.second;
    }
    void update_texture_bindings(const char *semantic, resource_view view, resource_view srgb)
    {
        if (view == 0) bindings.erase(semantic);
        else bindings[semantic] = { view, srgb == 0 ? view : srgb };
    }
};

class Alias : public IUnknown
{
public:
    explicit Alias(IUnknown *next = nullptr) : next_(next) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == reshade_depth::k_unwrapped_object && next_)
        {
            next_->AddRef();
            *out = next_;
            return S_OK;
        }
        if (iid == __uuidof(IUnknown))
        {
            AddRef();
            *out = static_cast<IUnknown *>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    ULONG references = 1;
    void loop() { next_ = this; }
private:
    IUnknown *next_;
};
}

int main()
{
    Runtime runtime;
    runtime.variables = { { 1, "COLOR" }, { 2, "DEPTH" }, { 3, "DEPTH" }, { 4, "CUSTOM" } };
    runtime.bindings = { { "COLOR", { { 10 }, { 11 } } }, { "DEPTH", { { 42 }, { 43 } } },
                         { "CUSTOM", { { 55 }, { 56 } } } };
    const auto before = runtime.bindings;
    auto depth = reshade_depth::find_depth_binding(&runtime, { 99 });
    CHECK(depth.variable == 2 && depth.view == 42 && depth.srgb == 43);
    CHECK(runtime.bindings == before);
    runtime.update_texture_bindings("DEPTH", { 99 }, { 99 });
    resource_view a { 0 }, b { 0 };
    runtime.get_texture_binding({ 3 }, &a, &b);
    CHECK(a == 99 && b == 99);
    runtime.update_texture_bindings("DEPTH", depth.view, depth.srgb);
    CHECK(runtime.bindings == before);

    runtime.bindings.erase("DEPTH");
    const auto absent = runtime.bindings;
    depth = reshade_depth::find_depth_binding(&runtime, { 99 });
    CHECK(depth.variable == 2 && depth.view == 0);
    CHECK(runtime.bindings == absent);
    runtime.variables.erase(2);
    runtime.variables.erase(3);
    CHECK(reshade_depth::find_depth_binding(&runtime, { 99 }).variable == 0);
    CHECK(runtime.bindings == absent);
    runtime.variables.clear();
    CHECK(reshade_depth::find_depth_binding(&runtime, { 99 }).variable == 0);

    Alias native;
    Alias proxy1(&native), proxy2(&proxy1), different;
    {
        const auto first = reshade_depth::native_object<IUnknown>(&proxy1);
        const auto second = reshade_depth::native_object<IUnknown>(&proxy2);
        CHECK(first.Get() == &native && second.Get() == &native);
        CHECK(first.Get() != reshade_depth::native_object<IUnknown>(&different).Get());
    }
    CHECK(native.references == 1 && proxy1.references == 1 && proxy2.references == 1);
    Alias self;
    self.loop();
    { CHECK(reshade_depth::native_object<IUnknown>(&self).Get() == &self); }
    CHECK(self.references == 1);
    CHECK(!reshade_depth::native_object<IUnknown>(nullptr));
    std::puts("ReShadeDepthBindingTest: arbitrary DEPTH names, shared consumers, restore, missing binding,");
    std::puts("nested native aliases, distinct native objects, self-loop and COM references passed");
}
