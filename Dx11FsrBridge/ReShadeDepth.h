#pragma once

#include "ReShadeDepthPass.h"
#include <Windows.h>

namespace reshade_depth
{
void configure(bool enable, int jitter_percent);
void initialize(HMODULE module);
void shutdown(bool process_exiting);
void capture(ID3D11DeviceContext *context, ID3D11Texture2D *depth, const Frame &frame);
}
