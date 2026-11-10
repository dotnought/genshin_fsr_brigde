#include "ComputeStateGuard.h"

#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
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
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;

constexpr char k_shader[] = R"(
cbuffer Params : register(b0) { uint add_value; uint3 padding; };
StructuredBuffer<uint> source : register(t0);
RWStructuredBuffer<uint> result : register(u0);
AppendStructuredBuffer<uint> records : register(u1);
#if LINKED
interface Operation { uint apply(uint value); };
class Identity : Operation { uint apply(uint value) { return value; } };
Identity identity;
Operation operation;
#endif
[numthreads(1, 1, 1)]
void main()
{
    uint value = source[0] + add_value;
#if LINKED
    value = operation.apply(value);
#endif
    result[0] = value;
    records.Append(value);
}
)";

ComPtr<ID3D11ComputeShader> make_shader(ID3D11ClassLinkage *linkage = nullptr)
{
    const D3D_SHADER_MACRO defines[] = { { "LINKED", linkage ? "1" : "0" }, { nullptr, nullptr } };
    ComPtr<ID3DBlob> bytecode, errors;
    const HRESULT hr = D3DCompile(k_shader, std::strlen(k_shader), nullptr, defines, nullptr,
        "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &errors);
    if (errors) std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
    CHECK(SUCCEEDED(hr));
    ComPtr<ID3D11ComputeShader> shader;
    CHECK(SUCCEEDED(g_device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
        linkage, &shader)));
    return shader;
}

ComPtr<ID3D11Buffer> make_buffer(UINT value)
{
    D3D11_BUFFER_DESC desc {};
    desc.ByteWidth = 64 * sizeof(UINT);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(UINT);
    std::array<UINT, 64> data {};
    data[0] = value;
    D3D11_SUBRESOURCE_DATA initial { data.data(), 0, 0 };
    ComPtr<ID3D11Buffer> buffer;
    CHECK(SUCCEEDED(g_device->CreateBuffer(&desc, &initial, &buffer)));
    return buffer;
}

ComPtr<ID3D11Buffer> make_constants(UINT value)
{
    D3D11_BUFFER_DESC desc {};
    desc.ByteWidth = 16;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const UINT data[4] = { value, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA initial { data, 0, 0 };
    ComPtr<ID3D11Buffer> buffer;
    CHECK(SUCCEEDED(g_device->CreateBuffer(&desc, &initial, &buffer)));
    return buffer;
}

ComPtr<ID3D11ShaderResourceView> make_srv(ID3D11Buffer *buffer)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC desc {};
    desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = 64;
    ComPtr<ID3D11ShaderResourceView> view;
    CHECK(SUCCEEDED(g_device->CreateShaderResourceView(buffer, &desc, &view)));
    return view;
}

ComPtr<ID3D11UnorderedAccessView> make_uav(ID3D11Buffer *buffer, bool append)
{
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc {};
    desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = 64;
    desc.Buffer.Flags = append ? D3D11_BUFFER_UAV_FLAG_APPEND : 0;
    ComPtr<ID3D11UnorderedAccessView> view;
    CHECK(SUCCEEDED(g_device->CreateUnorderedAccessView(buffer, &desc, &view)));
    return view;
}

UINT read_value(ID3D11Buffer *buffer)
{
    D3D11_BUFFER_DESC desc {};
    buffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    CHECK(SUCCEEDED(g_device->CreateBuffer(&desc, nullptr, &staging)));
    g_context->CopyResource(staging.Get(), buffer);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    CHECK(SUCCEEDED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    const UINT value = *static_cast<const UINT *>(mapped.pData);
    g_context->Unmap(staging.Get(), 0);
    return value;
}

UINT read_count(ID3D11UnorderedAccessView *view)
{
    D3D11_BUFFER_DESC desc {};
    desc.ByteWidth = sizeof(UINT);
    desc.Usage = D3D11_USAGE_DEFAULT;
    ComPtr<ID3D11Buffer> buffer;
    CHECK(SUCCEEDED(g_device->CreateBuffer(&desc, nullptr, &buffer)));
    g_context->CopyStructureCount(buffer.Get(), 0, view);
    return read_value(buffer.Get());
}

struct Work
{
    ComPtr<ID3D11ComputeShader> shader = make_shader();
    ComPtr<ID3D11Buffer> source = make_buffer(5), result = make_buffer(0), records = make_buffer(0);
    ComPtr<ID3D11Buffer> constants = make_constants(37);
    ComPtr<ID3D11ShaderResourceView> input = make_srv(source.Get());
    std::array<ComPtr<ID3D11UnorderedAccessView>, 2> outputs {
        make_uav(result.Get(), false), make_uav(records.Get(), true)
    };

    void bind(UINT count)
    {
        g_context->CSSetShader(shader.Get(), nullptr, 0);
        ID3D11Buffer *cb = constants.Get();
        g_context->CSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView *srv = input.Get();
        g_context->CSSetShaderResources(0, 1, &srv);
        ID3D11UnorderedAccessView *uavs[] = { outputs[0].Get(), outputs[1].Get() };
        const UINT counts[] = { UINT(-1), count };
        g_context->CSSetUnorderedAccessViews(0, 2, uavs, counts);
    }
};

void check_inherited_dispatch(bool early_exit)
{
    g_context->ClearState();
    Work game, bridge;
    game.bind(7);
    CHECK(read_count(game.outputs[1].Get()) == 7);
    const auto prepare = [&]()
    {
        ffx12::ComputeStateGuard state(g_context.Get());
        // 2026-11-09：原 UAV1 与准备 pass 的输入别名，验证构造器先解绑输出。
        auto alias = make_srv(game.records.Get());
        ID3D11ShaderResourceView *srv = alias.Get();
        g_context->CSSetShaderResources(0, 1, &srv);
        ComPtr<ID3D11ShaderResourceView> actual;
        g_context->CSGetShaderResources(0, 1, &actual);
        CHECK(actual.Get() == alias.Get());
        bridge.bind(0);
        if (early_exit) return;
        g_context->Dispatch(1, 1, 1);
    };
    prepare();
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11ShaderResourceView> srv;
    std::array<ID3D11UnorderedAccessView *, 2> uavs {};
    g_context->CSGetShader(&shader, nullptr, nullptr);
    g_context->CSGetConstantBuffers(0, 1, &cb);
    g_context->CSGetShaderResources(0, 1, &srv);
    g_context->CSGetUnorderedAccessViews(0, 2, uavs.data());
    CHECK(shader.Get() == game.shader.Get() && cb.Get() == game.constants.Get() && srv.Get() == game.input.Get());
    for (unsigned i = 0; i < uavs.size(); ++i)
    {
        CHECK(uavs[i] == game.outputs[i].Get());
        uavs[i]->Release();
    }
    CHECK(read_count(game.outputs[1].Get()) == 7);
    // 2026-11-09：不重绑任何状态就派发游戏工作，验证 API 状态继承和隐藏计数器。
    g_context->Dispatch(1, 1, 1);
    CHECK(read_value(game.result.Get()) == 42);
    CHECK(read_count(game.outputs[1].Get()) == 8);
    g_context->ClearState();
}

void check_empty_state()
{
    g_context->ClearState();
    Work bridge;
    {
        ffx12::ComputeStateGuard state(g_context.Get());
        bridge.bind(0);
        g_context->Dispatch(1, 1, 1);
    }
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11ShaderResourceView> srv;
    std::array<ID3D11UnorderedAccessView *, 2> uavs {};
    g_context->CSGetShader(&shader, nullptr, nullptr);
    g_context->CSGetConstantBuffers(0, 1, &cb);
    g_context->CSGetShaderResources(0, 1, &srv);
    g_context->CSGetUnorderedAccessViews(0, 2, uavs.data());
    CHECK(!shader && !cb && !srv && !uavs[0] && !uavs[1]);
}

void check_class_instances()
{
    g_context->ClearState();
    ComPtr<ID3D11ClassLinkage> linkage;
    CHECK(SUCCEEDED(g_device->CreateClassLinkage(&linkage)));
    auto shader = make_shader(linkage.Get());
    ComPtr<ID3D11ClassInstance> instance;
    CHECK(SUCCEEDED(linkage->CreateClassInstance("Identity", 0, 0, 0, 0, &instance)));
    Work game, bridge;
    game.bind(7);
    ID3D11ClassInstance *classes[] = { instance.Get() };
    g_context->CSSetShader(shader.Get(), classes, 1);
    {
        ffx12::ComputeStateGuard state(g_context.Get());
        bridge.bind(0);
    }
    ComPtr<ID3D11ComputeShader> actual;
    std::array<ID3D11ClassInstance *, 2> restored {};
    UINT count = static_cast<UINT>(restored.size());
    g_context->CSGetShader(&actual, restored.data(), &count);
    CHECK(actual.Get() == shader.Get() && count == 1 && restored[0] == instance.Get());
    restored[0]->Release();
    g_context->Dispatch(1, 1, 1);
    CHECK(read_value(game.result.Get()) == 42);
    CHECK(read_count(game.outputs[1].Get()) == 8);
    g_context->ClearState();
}
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    UINT flags = D3D11_CREATE_DEVICE_DEBUG;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
    {
        flags = 0;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
            D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
    }
    CHECK(SUCCEEDED(hr));
    std::puts("stage: inherited dispatch");
    check_inherited_dispatch(false);
    std::puts("stage: early exit");
    check_inherited_dispatch(true);
    std::puts("stage: empty state");
    check_empty_state();
    std::puts("stage: class instances");
    check_class_instances();
    std::puts("stage: debug messages");
    ComPtr<ID3D11InfoQueue> messages;
    if (SUCCEEDED(g_device.As(&messages)))
    {
        const UINT64 count = messages->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i)
        {
            SIZE_T bytes = 0;
            CHECK(SUCCEEDED(messages->GetMessage(i, nullptr, &bytes)));
            std::vector<unsigned char> storage(bytes);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(storage.data());
            CHECK(SUCCEEDED(messages->GetMessage(i, message, &bytes)));
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
            {
                std::fprintf(stderr, "%s\n", message->pDescription);
                CHECK(false);
            }
        }
    }
    std::puts("ComputeStateGuardTest: inherited dispatch, early return, empty state, aliases, counters and classes passed");
}
