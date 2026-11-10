#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>

namespace ffx12
{
// 2026-11-09：native 输入准备原先清空 CS 绑定，破坏游戏后续不重新绑定的 Dispatch。
// 只保存两个准备 pass 确实改写的槽位；两者共用一次快照，不在 draw 钩子增加探针。
class ComputeStateGuard
{
  public:
    explicit ComputeStateGuard(ID3D11DeviceContext *context) : context_(context)
    {
        if (!context_) return;
        context_->CSGetShader(shader_.GetAddressOf(), classes_.data(), &class_count_);
        context_->CSGetConstantBuffers(0, 1, buffer_.GetAddressOf());
        context_->CSGetShaderResources(0, 1, &resource_);
        context_->CSGetUnorderedAccessViews(0, static_cast<UINT>(outputs_.size()), outputs_.data());
        // 旧 UAV 可能是本次输入；必须先解绑，否则 D3D11 会把随后绑定的 SRV 静默置空。
        std::array<ID3D11UnorderedAccessView *, 2> empty {};
        context_->CSSetUnorderedAccessViews(0, static_cast<UINT>(empty.size()), empty.data(), nullptr);
    }

    ~ComputeStateGuard()
    {
        if (!context_) return;
        std::array<ID3D11UnorderedAccessView *, 2> empty {};
        ID3D11ShaderResourceView *empty_resource = nullptr;
        context_->CSSetUnorderedAccessViews(0, static_cast<UINT>(empty.size()), empty.data(), nullptr);
        context_->CSSetShaderResources(0, 1, &empty_resource);
        // 2026-11-09：-1 保留 UAV 当前隐藏计数器；恢复绑定不能重置 append/consume 位置。
        const UINT keep_counts[] = { UINT(-1), UINT(-1) };
        context_->CSSetUnorderedAccessViews(0, static_cast<UINT>(outputs_.size()), outputs_.data(), keep_counts);
        context_->CSSetShaderResources(0, 1, &resource_);
        ID3D11Buffer *buffer = buffer_.Get();
        context_->CSSetConstantBuffers(0, 1, &buffer);
        context_->CSSetShader(shader_.Get(), classes_.data(), class_count_);
        if (resource_) resource_->Release();
        for (auto *output : outputs_)
            if (output) output->Release();
        for (UINT i = 0; i < class_count_; ++i)
            if (classes_[i]) classes_[i]->Release();
    }

    ComputeStateGuard(const ComputeStateGuard &) = delete;
    ComputeStateGuard &operator=(const ComputeStateGuard &) = delete;

  private:
    ID3D11DeviceContext *context_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
    ID3D11ShaderResourceView *resource_ = nullptr;
    std::array<ID3D11UnorderedAccessView *, 2> outputs_ {};
    std::array<ID3D11ClassInstance *, D3D11_SHADER_MAX_INTERFACES> classes_ {};
    UINT class_count_ = static_cast<UINT>(classes_.size());
};
}
