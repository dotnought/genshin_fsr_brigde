#include "ReShadeDepthPass.h"
#include "NativeGraphicsObject.h"

#include <d3dcompiler.h>
#include <array>
#include <cmath>

namespace reshade_depth
{
namespace
{
// 2026-11-09：FFX ComputeHrPosFromLrPos 使用 inputCenter - Jitter；反向采样因此加 Jitter。
// 输出保留原始设备深度，ReShade 原有 reversed/linearization 配置继续生效。
// 双线性重采样仅校正空间相位；不能恢复低分辨率未采到的几何，游戏画质尚待实测。
constexpr char k_shader[] = R"(
cbuffer Params : register(b0)
{
    uint2 input_size;
    uint2 output_size;
    float2 jitter_pixels;
    float2 padding;
};
Texture2D<float> source_depth : register(t0);
RWTexture2D<float> corrected_depth : register(u0);
float read_depth(int2 p)
{
    return source_depth.Load(int3(clamp(p, int2(0, 0), int2(input_size) - 1), 0));
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= output_size)) return;
    float2 p = (float2(id.xy) + 0.5) * float2(input_size) / float2(output_size)
             - 0.5 + jitter_pixels;
    int2 base = int2(floor(p));
    float2 f = frac(p);
    corrected_depth[id.xy] = lerp(
        lerp(read_depth(base), read_depth(base + int2(1, 0)), f.x),
        lerp(read_depth(base + int2(0, 1)), read_depth(base + int2(1, 1)), f.x), f.y);
}
)";

bool depth_formats(DXGI_FORMAT source, DXGI_FORMAT &texture, DXGI_FORMAT &view)
{
    switch (source)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        texture = DXGI_FORMAT_R32G8X24_TYPELESS;
        view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        texture = DXGI_FORMAT_R24G8_TYPELESS;
        view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        texture = DXGI_FORMAT_R32_TYPELESS;
        view = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
        texture = DXGI_FORMAT_R16_TYPELESS;
        view = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}
}

void DepthPass::reset()
{
    fresh_ = false;
    output_uav_.Reset();
    output_srv_.Reset();
    input_srv_.Reset();
    output_.Reset();
    snapshot_.Reset();
    constants_.Reset();
    shader_.Reset();
    context_.Reset();
    native_context_.Reset();
    device_.Reset();
    frame_ = {};
    source_format_ = DXGI_FORMAT_UNKNOWN;
}

bool DepthPass::capture(ID3D11DeviceContext *context, ID3D11Texture2D *depth, const Frame &frame)
{
    fresh_ = false;
    if (!context || !depth || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        frame.render_w == 0 || frame.render_h == 0 || frame.display_w == 0 || frame.display_h == 0 ||
        !std::isfinite(frame.jitter_x) || !std::isfinite(frame.jitter_y) ||
        std::abs(frame.jitter_x) > 2.0f || std::abs(frame.jitter_y) > 2.0f)
        return false;
    D3D11_TEXTURE2D_DESC source {};
    depth->GetDesc(&source);
    DXGI_FORMAT copy_format, view_format;
    if (source.Width != frame.render_w || source.Height != frame.render_h ||
        source.SampleDesc.Count != 1 || source.MipLevels != 1 || source.ArraySize != 1 ||
        !depth_formats(source.Format, copy_format, view_format))
        return false;
    auto native_context = context_.Get() == context ? native_context_ : native_object(context);
    if (!native_context) return false;
    Microsoft::WRL::ComPtr<ID3D11Device> device, source_device;
    native_context->GetDevice(&device);
    depth->GetDevice(&source_device);
    device = native_object(device.Get());
    source_device = native_object(source_device.Get());
    if (device.Get() != source_device.Get())
        return false;
    if (native_context_.Get() != native_context.Get())
    {
        reset();
        native_context_ = native_context;
        device_ = device;
    }
    context_ = context;
    if (!snapshot_ || source_format_ != source.Format || frame_.render_w != frame.render_w ||
        frame_.render_h != frame.render_h || frame_.display_w != frame.display_w ||
        frame_.display_h != frame.display_h)
    {
        snapshot_.Reset();
        input_srv_.Reset();
        output_.Reset();
        output_srv_.Reset();
        output_uav_.Reset();
        D3D11_TEXTURE2D_DESC desc = source;
        desc.Format = copy_format;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = desc.MiscFlags = 0;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &snapshot_)))
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = view_format;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(device_->CreateShaderResourceView(snapshot_.Get(), &srv, &input_srv_)))
        {
            snapshot_.Reset();
            return false;
        }
        desc.Width = frame.display_w;
        desc.Height = frame.display_h;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &output_)) ||
            FAILED(device_->CreateShaderResourceView(output_.Get(), nullptr, &output_srv_)) ||
            FAILED(device_->CreateUnorderedAccessView(output_.Get(), nullptr, &output_uav_)))
        {
            snapshot_.Reset();
            return false;
        }
    }
    frame_ = frame;
    source_format_ = source.Format;
    context->CopyResource(snapshot_.Get(), depth);
    fresh_ = true;
    return true;
}

bool DepthPass::matches(ID3D11DeviceContext *context, std::uint32_t width, std::uint32_t height) const
{
    if (!context || !native_context_ || frame_.display_w != width || frame_.display_h != height)
        return false;
    return context == context_.Get() || context == native_context_.Get() ||
           native_object(context).Get() == native_context_.Get();
}

ID3D11ShaderResourceView *DepthPass::consume(ID3D11DeviceContext *context,
                                           std::uint32_t width, std::uint32_t height, float scale)
{
    const bool ready = fresh_;
    fresh_ = false;
    if (!ready || !matches(context, width, height) ||
        !std::isfinite(scale) || std::abs(scale) > 2.0f || !input_srv_ || !output_uav_)
        return nullptr;
    if (!shader_)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(k_shader, sizeof(k_shader) - 1, "ReShadeDepthCorrection", nullptr, nullptr,
                             "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
            FAILED(device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_)))
            return nullptr;
    }
    if (!constants_)
    {
        D3D11_BUFFER_DESC desc {};
        desc.ByteWidth = 32;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device_->CreateBuffer(&desc, nullptr, &constants_)))
            return nullptr;
    }
    struct Constants
    {
        std::uint32_t rw, rh, dw, dh;
        float jx, jy, pad0, pad1;
    } params { frame_.render_w, frame_.render_h, width, height,
               frame_.jitter_x * scale, frame_.jitter_y * scale, 0, 0 };
    // 2026-11-09：ReShade 回调内只触碰 CS 的这四项；恢复原状态，不能破坏其它 add-on。
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> old_shader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> old_cb;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> old_srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> old_uav;
    std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES> classes {};
    UINT class_count = static_cast<UINT>(classes.size());
    context->CSGetShader(&old_shader, classes.data(), &class_count);
    context->CSGetConstantBuffers(0, 1, &old_cb);
    context->CSGetShaderResources(0, 1, &old_srv);
    context->CSGetUnorderedAccessViews(0, 1, &old_uav);
    context->UpdateSubresource(constants_.Get(), 0, nullptr, &params, 0, 0);
    ID3D11Buffer *cb = constants_.Get();
    ID3D11ShaderResourceView *srv = input_srv_.Get();
    ID3D11UnorderedAccessView *uav = output_uav_.Get();
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &cb);
    context->CSSetShaderResources(0, 1, &srv);
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    uav = nullptr;
    srv = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->CSSetShaderResources(0, 1, &srv);
    uav = old_uav.Get();
    srv = old_srv.Get();
    cb = old_cb.Get();
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->CSSetShaderResources(0, 1, &srv);
    context->CSSetConstantBuffers(0, 1, &cb);
    context->CSSetShader(old_shader.Get(), classes.data(), class_count);
    for (UINT i = 0; i < class_count; ++i)
        if (classes[i]) classes[i]->Release();
    return output_srv_.Get();
}
}
