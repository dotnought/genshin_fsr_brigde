#include "ReShadeDepthPass.h"

#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <algorithm>
#include <utility>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
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

std::vector<float> read_texture(ID3D11Texture2D *texture)
{
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    CHECK(desc.Format == DXGI_FORMAT_R32_FLOAT);
    const unsigned width = desc.Width, height = desc.Height;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    CHECK(SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &staging)));
    g_context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    CHECK(SUCCEEDED(g_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    std::vector<float> result(width * height);
    for (unsigned y = 0; y < height; ++y)
    {
        const auto *row = reinterpret_cast<const float *>(static_cast<const char *>(mapped.pData) + y * mapped.RowPitch);
        for (unsigned x = 0; x < width; ++x) result[y * width + x] = row[x];
    }
    g_context->Unmap(staging.Get(), 0);
    return result;
}

std::vector<float> read_view(ID3D11ShaderResourceView *view)
{
    CHECK(view != nullptr);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    CHECK(SUCCEEDED(resource.As(&texture)));
    return read_texture(texture.Get());
}

ComPtr<ID3D11Texture2D> make_source(unsigned width, unsigned height, const std::vector<float> &values)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SUBRESOURCE_DATA data { values.data(), width * static_cast<UINT>(sizeof(float)), 0 };
    ComPtr<ID3D11Texture2D> texture;
    CHECK(SUCCEEDED(g_device->CreateTexture2D(&desc, &data, &texture)));
    return texture;
}

void check_ramp(reshade_depth::DepthPass &pass, unsigned width, unsigned height)
{
    constexpr unsigned rw = 13, rh = 9;
    // 2026-11-09：源 ramp 随 jitter 平移；正确反采样应在内部像素精确还原同一稳定平面。
    for (const auto &jitter : { std::pair<float, float> { -0.4f, 0.3f },
                               { 0.45f, -0.35f }, { 0.0f, 0.0f } })
    {
        std::vector<float> input(rw * rh);
        for (unsigned y = 0; y < rh; ++y)
            for (unsigned x = 0; x < rw; ++x)
                input[y * rw + x] = 0.2f + 0.01f * (x - jitter.first) + 0.015f * (y - jitter.second);
        auto source = make_source(rw, rh, input);
        const reshade_depth::Frame frame { rw, rh, width, height, jitter.first, jitter.second };
        CHECK(pass.capture(g_context.Get(), source.Get(), frame));
        // 复制后清源模拟游戏重用深度；输出仍必须来自快照，源不应被校正 pass 回写。
        std::vector<float> cleared(rw * rh, 0.0f);
        g_context->UpdateSubresource(source.Get(), 0, nullptr, cleared.data(), rw * sizeof(float), 0);
        const auto actual = read_view(pass.consume(g_context.Get(), width, height, 1.0f));
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
            {
                const float px = (x + 0.5f) * rw / width - 0.5f;
                const float py = (y + 0.5f) * rh / height - 0.5f;
                const float sx = std::clamp(px + jitter.first, 0.0f, static_cast<float>(rw - 1));
                const float sy = std::clamp(py + jitter.second, 0.0f, static_cast<float>(rh - 1));
                const float expected = 0.2f + 0.01f * (sx - jitter.first) + 0.015f * (sy - jitter.second);
                CHECK(std::abs(actual[y * width + x] - expected) < 0.00001f);
                if (px > 1 && py > 1 && px < rw - 2 && py < rh - 2)
                    CHECK(std::abs(actual[y * width + x] - (0.2f + 0.01f * px + 0.015f * py)) < 0.00001f);
            }
        CHECK(read_texture(source.Get()) == cleared);
        CHECK(pass.consume(g_context.Get(), width, height, 1.0f) == nullptr);
    }
}

void check_depth_format(DXGI_FORMAT format, DXGI_FORMAT view_format)
{
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 13;
    desc.Height = 9;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> source;
    CHECK(SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &source)));
    D3D11_DEPTH_STENCIL_VIEW_DESC view {};
    view.Format = view_format;
    view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;
    CHECK(SUCCEEDED(g_device->CreateDepthStencilView(source.Get(), &view, &dsv)));
    g_context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.375f, 0);
    g_context->OMSetRenderTargets(0, nullptr, dsv.Get());
    reshade_depth::DepthPass pass;
    CHECK(pass.capture(g_context.Get(), source.Get(), { 13, 9, 29, 19, -0.4f, 0.3f }));
    g_context->OMSetRenderTargets(0, nullptr, nullptr);
    const auto output = read_view(pass.consume(g_context.Get(), 29, 19, 1.0f));
    for (float value : output) CHECK(std::abs(value - 0.375f) < 0.0001f);
}

void check_state_restore()
{
    ComPtr<ID3DBlob> code;
    constexpr char shader[] = "RWTexture2D<float> x:register(u0); [numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID){x[id.xy]=1;}";
    CHECK(SUCCEEDED(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, nullptr)));
    ComPtr<ID3D11ComputeShader> sentinel;
    CHECK(SUCCEEDED(g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &sentinel)));
    D3D11_BUFFER_DESC cbdesc {};
    cbdesc.ByteWidth = 32;
    cbdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;
    CHECK(SUCCEEDED(g_device->CreateBuffer(&cbdesc, nullptr, &cb)));
    auto source = make_source(13, 9, std::vector<float>(117, 0.5f));
    auto dest = make_source(13, 9, std::vector<float>(117, 0.0f));
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    CHECK(SUCCEEDED(g_device->CreateShaderResourceView(source.Get(), nullptr, &srv)));
    CHECK(SUCCEEDED(g_device->CreateUnorderedAccessView(dest.Get(), nullptr, &uav)));
    g_context->CSSetShader(sentinel.Get(), nullptr, 0);
    ID3D11Buffer *cbp = cb.Get();
    ID3D11ShaderResourceView *sp = srv.Get();
    ID3D11UnorderedAccessView *up = uav.Get();
    g_context->CSSetConstantBuffers(0, 1, &cbp);
    g_context->CSSetShaderResources(0, 1, &sp);
    g_context->CSSetUnorderedAccessViews(0, 1, &up, nullptr);
    reshade_depth::DepthPass pass;
    CHECK(pass.capture(g_context.Get(), source.Get(), { 13, 9, 29, 19, 0.4f, -0.2f }));
    CHECK(pass.consume(g_context.Get(), 29, 19, 1.0f));
    ComPtr<ID3D11ComputeShader> actual;
    ComPtr<ID3D11Buffer> acb;
    ComPtr<ID3D11ShaderResourceView> asrv;
    ComPtr<ID3D11UnorderedAccessView> auav;
    g_context->CSGetShader(&actual, nullptr, nullptr);
    g_context->CSGetConstantBuffers(0, 1, &acb);
    g_context->CSGetShaderResources(0, 1, &asrv);
    g_context->CSGetUnorderedAccessViews(0, 1, &auav);
    CHECK(actual.Get() == sentinel.Get() && acb.Get() == cb.Get());
    CHECK(asrv.Get() == srv.Get() && auav.Get() == uav.Get());
    g_context->ClearState();
}
}

int main()
{
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
                                 nullptr, 0, D3D11_SDK_VERSION, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                              nullptr, 0, D3D11_SDK_VERSION, &g_device, &level, &g_context);
    CHECK(SUCCEEDED(hr) && level >= D3D_FEATURE_LEVEL_11_0);
    reshade_depth::DepthPass pass;
    check_ramp(pass, 26, 18);
    check_ramp(pass, 29, 19);
    check_depth_format(DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    check_depth_format(DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT);
    check_depth_format(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT);
    check_depth_format(DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM);
    check_state_restore();
    auto source = make_source(13, 9, std::vector<float>(117, 0.5f));
    const reshade_depth::Frame valid { 13, 9, 29, 19, 0.1f, -0.2f };
    CHECK(pass.capture(g_context.Get(), source.Get(), valid));
    CHECK(pass.consume(g_context.Get(), 28, 19, 1.0f) == nullptr);
    CHECK(pass.consume(g_context.Get(), 29, 19, 1.0f) == nullptr);
    CHECK(pass.capture(g_context.Get(), source.Get(), valid));
    pass.discard();
    CHECK(pass.consume(g_context.Get(), 29, 19, 1.0f) == nullptr);
    auto invalid = valid;
    invalid.jitter_x = std::numeric_limits<float>::quiet_NaN();
    CHECK(!pass.capture(g_context.Get(), source.Get(), invalid));
    CHECK(!pass.capture(nullptr, source.Get(), valid));
    CHECK(!pass.capture(g_context.Get(), nullptr, valid));
    ComPtr<ID3D11InfoQueue> queue;
    if (SUCCEEDED(g_device.As(&queue)))
    {
        for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            queue->GetMessage(i, nullptr, &size);
            std::vector<char> bytes(size);
            auto *message = reinterpret_cast<D3D11_MESSAGE *>(bytes.data());
            CHECK(SUCCEEDED(queue->GetMessage(i, message, &size)));
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR)
            {
                std::fprintf(stderr, "%s\n", message->pDescription);
                CHECK(false);
            }
        }
    }
    std::puts("ReShadeDepthPassTest: WARP jitter, boundaries, snapshot isolation, formats, resize, freshness and state passed");
}
