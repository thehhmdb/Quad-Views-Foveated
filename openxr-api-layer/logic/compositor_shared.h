// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "pch.h"
#include "compositor.h"
#include <utils/graphics.h>

#define A_CPU
#include <ffx_a.h>
#include <ffx_cas.h>

namespace openxr_api_layer {

    // ---------------------------------------------------------------------------
    // Shared Constant Buffer Structs
    // ---------------------------------------------------------------------------
    // These structs must byte-match the HLSL cbuffer layouts in:
    //   - ProjectionVS.hlsl / ProjectionVS11.hlsl
    //   - ProjectionPS.hlsl  / ProjectionPS11.hlsl
    //   - SharpeningCS.hlsl  / SharpeningCS11.hlsl
    // ---------------------------------------------------------------------------

    struct ProjectionVSConstants {
        alignas(16) DirectX::XMFLOAT4X4 focusProjection;
        // Direct sampling fields (must match ProjectionVS.hlsl cbuffer)
        alignas(16) DirectX::XMFLOAT4 stereoSubRect;
        alignas(16) DirectX::XMFLOAT4 focusSubRect;
        alignas(8)  DirectX::XMFLOAT2 stereoSwapchainSize;
        alignas(8)  DirectX::XMFLOAT2 focusSwapchainSize;
    };

    struct ProjectionPSConstants {
        alignas(4) float smoothingArea;
        alignas(4) uint32_t ignoreAlpha;
        alignas(4) uint32_t isUnpremultipliedAlpha;
        alignas(4) uint32_t debugFocusView;
        alignas(4) float sharpenFocusView;
        alignas(4) float ditheringAmount;
        alignas(4) uint32_t frameCount;
        // Direct sampling fields (must match ProjectionPS.hlsl cbuffer)
        alignas(16) DirectX::XMFLOAT4 stereoSubRect;
        alignas(16) DirectX::XMFLOAT4 focusSubRect;
        alignas(8)  DirectX::XMFLOAT2 stereoSwapchainSize;
        alignas(8)  DirectX::XMFLOAT2 focusSwapchainSize;
        alignas(4) bool useDirectStereoSampling;
        alignas(4) bool useDirectFocusSampling;
    };

    struct SharpeningCSConstants {
        alignas(4) uint32_t Const0[4];
        alignas(4) uint32_t Const1[4];
    };

    // ---------------------------------------------------------------------------
    // Layout Locks — compile-time guarantees for struct sizes and key offsets
    // ---------------------------------------------------------------------------
    static_assert(sizeof(ProjectionVSConstants) == 112, "ProjectionVSConstants size must be 112 bytes");
    static_assert(sizeof(ProjectionPSConstants) == 96, "ProjectionPSConstants size must be 96 bytes");
    static_assert(sizeof(SharpeningCSConstants) == 32, "SharpeningCSConstants size must be 32 bytes");

    // ---------------------------------------------------------------------------
    // Shared Computation Helpers
    // ---------------------------------------------------------------------------
    // Inline functions guarantee byte-identical constant buffer contents for
    // both D3D11 and D3D12 compositors.
    // ---------------------------------------------------------------------------

    inline void ComputeProjectionConstants(ProjectionVSConstants& out, const XrFovf& cachedEyeFov, const XrFovf& focusViewFov) {
        const DirectX::XMMATRIX baseLayerViewProjection =
            xr::math::ComposeProjectionMatrix(cachedEyeFov, xr::math::NearFar{0.1f, 20.f});
        const DirectX::XMMATRIX layerViewProjection =
            xr::math::ComposeProjectionMatrix(focusViewFov, xr::math::NearFar{0.1f, 20.f});

        DirectX::XMStoreFloat4x4(
            &out.focusProjection,
            DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, baseLayerViewProjection) *
                                       layerViewProjection));
    }

    inline void ComputePixelShaderConstants(ProjectionPSConstants& out, const CompositorParams& params) {
        out.smoothingArea = params.useQuadViews ? params.smoothenFocusViewEdges : 0;
        out.ignoreAlpha = ~(params.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) ? 1 : 0;
        out.isUnpremultipliedAlpha = (params.layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? 1 : 0;
        out.debugFocusView = params.debugFocusView ? 1 : 0;
        out.sharpenFocusView = params.sharpenFocusView;
        out.ditheringAmount = params.ditheringAmount;
        out.frameCount = params.frameCount;
    }

    inline void ComputeCasConstants(SharpeningCSConstants& out, float sharpness, uint32_t width, uint32_t height) {
        CasSetup(out.Const0,
                 out.Const1,
                 std::clamp(sharpness, 0.f, 1.f),
                 (AF1)width,
                 (AF1)height,
                 (AF1)width,
                 (AF1)height);
    }

    // ---------------------------------------------------------------------------
    // Utility Helpers
    // ---------------------------------------------------------------------------

    /// Returns true when the sub-image region already covers the full swapchain
    /// at array index 0, so no flatten copy is required.
    inline bool NeedsFlattening(const XrCompositionLayerProjectionView& view,
                                const SwapchainInfo& swapchainInfo) {
        return !(view.subImage.imageRect.offset.x == 0 &&
                 view.subImage.imageRect.offset.y == 0 &&
                 view.subImage.imageRect.extent.width == swapchainInfo.createInfo.width &&
                 view.subImage.imageRect.extent.height == swapchainInfo.createInfo.height &&
                 view.subImage.imageArrayIndex == 0);
    }

    /// Sharpening pass configuration and dispatch helpers.
    struct SharpeningPass {
        static constexpr int ThreadGroupWorkRegionDim = 16;

        uint32_t dispatchX;
        uint32_t dispatchY;

        /// Returns true when sharpening should run for this view.
        bool operator()(const CompositorParams& params,
                        const XrCompositionLayerProjectionView& focusView,
                        const SwapchainInfo& focusSwapchainInfo) {
            // Only sharpen when the compositor flag is set AND the focus view
            // requires a flatten copy (sharpening operates on the flat image).
            if (!params.sharpenFocusView)
                return false;

            // Compute dispatch dimensions
            dispatchX = (focusView.subImage.imageRect.extent.width + (ThreadGroupWorkRegionDim - 1)) / ThreadGroupWorkRegionDim;
            dispatchY = (focusView.subImage.imageRect.extent.height + (ThreadGroupWorkRegionDim - 1)) / ThreadGroupWorkRegionDim;

            return true;
        }

        /// Prepare CAS constants for the sharpening compute shader.
        void PrepareConstants(SharpeningCSConstants& out,
                              float sharpness,
                              uint32_t width,
                              uint32_t height) const {
            ComputeCasConstants(out, sharpness, width, height);
        }
    };

} // namespace openxr_api_layer
