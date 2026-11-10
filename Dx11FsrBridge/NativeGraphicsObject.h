#pragma once

#include <Windows.h>
#include <Unknwn.h>
#include <wrl/client.h>

namespace reshade_depth
{
// 2026-11-10：ReShade 的 get_native 返回代理底层对象，不能与游戏收到的代理地址直接比较。
// 此 IID 来自 ReShade 6.8 source/com_utils.hpp（18deaa52）；GIMI 对未知 IID 向下转发。
inline constexpr GUID k_unwrapped_object =
    { 0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };

template <typename T>
Microsoft::WRL::ComPtr<T> native_object(T *object)
{
    Microsoft::WRL::ComPtr<T> current = object;
    // 上限防御异常代理循环；只使用 COM 契约，不读取任何代理的私有内存布局。
    for (unsigned level = 0; current && level < 8; ++level)
    {
        Microsoft::WRL::ComPtr<T> next;
        // IID 的契约是返回原接口的底层对象；再次普通 QI 会被 GIMI 封装回代理。
        if (FAILED(current->QueryInterface(k_unwrapped_object,
                                          reinterpret_cast<void **>(next.GetAddressOf()))) || !next)
            break;
        if (next.Get() == current.Get())
            break;
        current = next;
    }
    return current;
}
}
