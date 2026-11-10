#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

namespace reshade_depth
{
struct Frame
{
    std::uint32_t render_w = 0, render_h = 0;
    std::uint32_t display_w = 0, display_h = 0;
    float jitter_x = 0.0f, jitter_y = 0.0f;
    bool depth_reversed = true;
};

// 2026-11-09：独立快照避免 ReShade 在深度已清除后采样；绝不回写 DLSS 输入。
class DepthPass
{
public:
    bool capture(ID3D11DeviceContext *context, ID3D11Texture2D *depth, const Frame &frame,
                 ID3D11Texture2D *render_color = nullptr, ID3D11Texture2D *display_color = nullptr);
    ID3D11ShaderResourceView *consume(ID3D11DeviceContext *context,
                                    std::uint32_t width, std::uint32_t height, float scale, bool edge_aware = false);
    void discard() { fresh_ = false; }
    bool matches(ID3D11DeviceContext *context, std::uint32_t width, std::uint32_t height) const;
    ID3D11ShaderResourceView *output_view() const { return output_srv_.Get(); }
    bool has_guides() const { return render_guide_srv_ && display_guide_srv_; }
    void reset();
private:
    using Device = Microsoft::WRL::ComPtr<ID3D11Device>;
    template <typename T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Device device_;
    Ptr<ID3D11DeviceContext> context_, native_context_;
    Ptr<ID3D11Texture2D> snapshot_, output_;
    Ptr<ID3D11ShaderResourceView> input_srv_, output_srv_;
    Ptr<ID3D11UnorderedAccessView> output_uav_;
    Ptr<ID3D11ComputeShader> shader_;
    Ptr<ID3D11Buffer> constants_;
    Ptr<ID3D11Texture2D> render_guide_, display_guide_;
    Ptr<ID3D11ShaderResourceView> render_guide_srv_, display_guide_srv_;
    Frame frame_ {};
    DXGI_FORMAT source_format_ = DXGI_FORMAT_UNKNOWN;
    bool fresh_ = false;
};
}
