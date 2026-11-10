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
// 2026-11-10：平滑表面仍双线性；深度断层用重建颜色选择同一表面，避免 MXAO 读到虚构斜坡。
// 不能恢复源中不存在的几何，也不对深度做跨帧混合；游戏画质及性能尚待实测。
constexpr char k_shader[] = R"(
cbuffer Params : register(b0)
{
    uint2 input_size;
    uint2 output_size;
    float2 jitter_pixels;
    uint use_guides;
    uint depth_reversed;
};
Texture2D<float> source_depth : register(t0);
Texture2D<float4> render_color : register(t1);
Texture2D<float4> display_color : register(t2);
RWTexture2D<float> corrected_depth : register(u0);
int2 clamp_input(int2 p) { return clamp(p, int2(0, 0), int2(input_size) - 1); }
float read_depth(int2 p)
{
    return source_depth.Load(int3(clamp_input(p), 0));
}
float near_depth(float d) { return depth_reversed != 0 ? d : 1.0 - d; }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= output_size)) return;
    float2 p = (float2(id.xy) + 0.5) * float2(input_size) / float2(output_size)
             - 0.5 + jitter_pixels;
    int2 base = int2(floor(p));
    float2 f = frac(p);
    float4 d = float4(read_depth(base), read_depth(base + int2(1, 0)),
                      read_depth(base + int2(0, 1)), read_depth(base + int2(1, 1)));
    float value = lerp(lerp(d.x, d.y, f.x), lerp(d.z, d.w, f.x), f.y);
    float4 z = depth_reversed != 0 ? d : 1.0 - d;
    float lo = min(min(z.x, z.y), min(z.z, z.w));
    float hi = max(max(z.x, z.y), max(z.z, z.w));
    if (use_guides != 0 && hi - lo > max(hi, 0.000001) * 0.2)
    {
        float3 target = saturate(display_color.Load(int3(id.xy, 0)).rgb);
        float depths[9], errors[9], spatial[9];
        float best_cost = 1e20;
        uint best = 0;
        [unroll] for (uint i = 0; i < 9; ++i)
        {
            int2 offset = int2(int(i % 3) - 1, int(i / 3) - 1);
            int2 sample_pos = clamp_input(base + offset);
            float2 delta = float2(sample_pos) - p;
            float3 diff = saturate(render_color.Load(int3(sample_pos, 0)).rgb) - target;
            depths[i] = read_depth(sample_pos);
            errors[i] = dot(diff, diff);
            spatial[i] = exp2(-dot(delta, delta) * 1.5);
            float cost = errors[i] * 64.0 + dot(delta, delta) * 0.02;
            if (cost < best_cost) { best_cost = cost; best = i; }
        }
        float reference = near_depth(depths[best]);
        float sum = 0.0, weight_sum = 0.0;
        [unroll] for (uint j = 0; j < 9; ++j)
        {
            if (abs(near_depth(depths[j]) - reference) <= max(abs(reference), 0.000001) * 0.1)
            {
                float weight = spatial[j] * exp2(-errors[j] * 64.0);
                sum += depths[j] * weight;
                weight_sum += weight;
            }
        }
        value = weight_sum > 0.000001 ? sum / weight_sum : depths[best];
    }
    corrected_depth[id.xy] = value;
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

bool color_view(ID3D11Device *device, ID3D11Texture2D *texture, unsigned width, unsigned height,
                Microsoft::WRL::ComPtr<ID3D11Texture2D> &held,
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> &view)
{
    if (!texture) { held.Reset(); view.Reset(); return false; }
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    if (desc.Width != width || desc.Height != height || desc.SampleDesc.Count != 1 ||
        desc.ArraySize != 1 || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE))
    {
        held.Reset();
        view.Reset();
        return false;
    }
    if (held.Get() == texture && view) return true;
    held.Reset();
    view.Reset();
    Microsoft::WRL::ComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (native_object(owner.Get()).Get() != device)
        return false;
    DXGI_FORMAT format = desc.Format;
    switch (format)
    {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        break;
    default: return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(texture, &srv, &view))) return false;
    held = texture;
    return true;
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
    render_guide_srv_.Reset();
    display_guide_srv_.Reset();
    render_guide_.Reset();
    display_guide_.Reset();
    context_.Reset();
    native_context_.Reset();
    device_.Reset();
    frame_ = {};
    source_format_ = DXGI_FORMAT_UNKNOWN;
}

bool DepthPass::capture(ID3D11DeviceContext *context, ID3D11Texture2D *depth, const Frame &frame,
                        ID3D11Texture2D *render_color, ID3D11Texture2D *display_color)
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
    const bool render_ok = color_view(device_.Get(), render_color, frame.render_w, frame.render_h,
                                      render_guide_, render_guide_srv_);
    const bool display_ok = color_view(device_.Get(), display_color, frame.display_w, frame.display_h,
                                       display_guide_, display_guide_srv_);
    if (!render_ok || !display_ok)
    {
        render_guide_srv_.Reset();
        display_guide_srv_.Reset();
        render_guide_.Reset();
        display_guide_.Reset();
    }
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
                                           std::uint32_t width, std::uint32_t height, float scale, bool edge_aware)
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
        float jx, jy;
        std::uint32_t guides, reversed;
    } params { frame_.render_w, frame_.render_h, width, height,
               frame_.jitter_x * scale, frame_.jitter_y * scale,
               edge_aware && has_guides() ? 1u : 0u, frame_.depth_reversed ? 1u : 0u };
    // 2026-11-10：额外读取 t1/t2 颜色引导；三个 SRV 槽都必须恢复，不能破坏其它 add-on。
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> old_shader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> old_cb;
    std::array<ID3D11ShaderResourceView *, 3> old_srvs {};
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> old_uav;
    std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES> classes {};
    UINT class_count = static_cast<UINT>(classes.size());
    context->CSGetShader(&old_shader, classes.data(), &class_count);
    context->CSGetConstantBuffers(0, 1, &old_cb);
    context->CSGetShaderResources(0, 3, old_srvs.data());
    context->CSGetUnorderedAccessViews(0, 1, &old_uav);
    context->UpdateSubresource(constants_.Get(), 0, nullptr, &params, 0, 0);
    ID3D11Buffer *cb = constants_.Get();
    ID3D11ShaderResourceView *srvs[] = { input_srv_.Get(), render_guide_srv_.Get(), display_guide_srv_.Get() };
    ID3D11UnorderedAccessView *uav = output_uav_.Get();
    ID3D11UnorderedAccessView *null_uav = nullptr;
    // 2026-11-10：旧 UAV 可能正是颜色引导，先解绑再设 SRV，避免运行时静默置空输入。
    context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &cb);
    context->CSSetShaderResources(0, 3, srvs);
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    uav = nullptr;
    ID3D11ShaderResourceView *null_srvs[3] = {};
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->CSSetShaderResources(0, 3, null_srvs);
    uav = old_uav.Get();
    cb = old_cb.Get();
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->CSSetShaderResources(0, 3, old_srvs.data());
    context->CSSetConstantBuffers(0, 1, &cb);
    context->CSSetShader(old_shader.Get(), classes.data(), class_count);
    for (UINT i = 0; i < class_count; ++i)
        if (classes[i]) classes[i]->Release();
    for (auto *srv : old_srvs)
        if (srv) srv->Release();
    return output_srv_.Get();
}
}
