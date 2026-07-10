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

#include "compositor.h"

namespace openxr_api_layer {

    // Per-swapchain graphics state for D3D11.
    struct D3D11SwapchainGraphicsState {
        // Cached swapchain images
        std::vector<ID3D11Texture2D*> images;
        std::vector<ID3D11Texture2D*> fullFovSwapchainImages;
        uint32_t acquiredFullFovImageIndex{0};

        // Flat images for quad views
        ComPtr<ID3D11Texture2D> flatImage[xr::QuadView::Count];
        // Cached SRVs for flat images (Performance: avoid per-frame CreateShaderResourceView)
        ComPtr<ID3D11ShaderResourceView> srvFlatImage[xr::QuadView::Count];

        // Sharpened images
        ComPtr<ID3D11Texture2D> sharpenedImage[xr::StereoView::Count];
        // Cached SRV/UAV for sharpened images
        ComPtr<ID3D11ShaderResourceView> srvSharpenedImage[xr::StereoView::Count];
        ComPtr<ID3D11UnorderedAccessView> uavSharpenedImage[xr::StereoView::Count];

        // History textures for temporal stability (TAA-lite)
        ComPtr<ID3D11Texture2D> historyImage[xr::StereoView::Count];
        ComPtr<ID3D11ShaderResourceView> srvHistoryImage[xr::StereoView::Count];
        ComPtr<ID3D11RenderTargetView> rtvHistoryImage[xr::StereoView::Count];

        // Cached RTVs for full FOV destination images (one per array slice)
        ComPtr<ID3D11RenderTargetView> rtvDestination[xr::StereoView::Count];
    };

    // D3D11 implementation of the compositor interface.
    class D3D11Compositor : public ICompositor {
    public:
        D3D11Compositor(ID3D11Device* device, OpenXrApi* openXrApi);
        // Call destroy() in the destructor so resources are released even if destroy() was not
        // called explicitly (e.g. due to an exception). destroy() is idempotent.
        ~D3D11Compositor() override {
            destroy();
        }

        bool initialize(int32_t swapchainFormat) override;
        void* compositeView(const CompositorParams& params,
                            const SwapchainInfo& stereoSwapchain,
                            const XrCompositionLayerProjectionView& stereoView,
                            const SwapchainInfo& focusSwapchain,
                            const XrCompositionLayerProjectionView& focusView) override;
        void destroy() override;
        bool isInitialized() const override;

        // Evict the cached graphics state for a swapchain. Must be called by the layer when a
        // swapchain is destroyed, to avoid holding dangling raw texture pointers.
        void evictSwapchainState(XrSwapchain handle) {
            m_swapchainStates.erase(handle);
        }

        // FIX: Wait for GPU to finish all composition work.
        void waitForGpuIdle() override;

    private:
        void populateSwapchainImagesCache(D3D11SwapchainGraphicsState& state, XrSwapchain swapchain, bool isFullFov);

        ComPtr<ID3D11Device> m_device;
        ComPtr<ID3D11DeviceContext> m_context;
        OpenXrApi* m_openXrApi{nullptr};

        // Composition resources
        ComPtr<ID3D11SamplerState> m_linearClampSampler;
        ComPtr<ID3D11RasterizerState> m_noDepthRasterizer;
        ComPtr<ID3D11Buffer> m_projectionVSConstants;
        ComPtr<ID3D11Buffer> m_projectionPSConstants;
        ComPtr<ID3D11VertexShader> m_projectionVS;
        ComPtr<ID3D11PixelShader> m_projectionPS;
        ComPtr<ID3D11Buffer> m_sharpeningCSConstants;
        ComPtr<ID3D11ComputeShader> m_sharpeningCS;
        ComPtr<ID3D11Texture2D> m_blankTexture;
        ComPtr<ID3D11ShaderResourceView> m_srvBlankTexture;

        // Per-swapchain graphics state
        std::unordered_map<XrSwapchain, D3D11SwapchainGraphicsState> m_swapchainStates;
    };

} // namespace openxr_api_layer
