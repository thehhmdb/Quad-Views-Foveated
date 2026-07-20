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

#include "compositor_base.h"
#include <memory>

namespace openxr_api_layer {

    // Per-swapchain graphics state for D3D11.
    // Inherits full-FOV lifecycle fields from SwapchainGraphicsStateBase.
    struct D3D11SwapchainGraphicsState : public SwapchainGraphicsStateBase {
        // Cached swapchain images (ComPtr ensures textures stay alive while cached)
        std::vector<ComPtr<ID3D11Texture2D>> images;
        std::vector<ComPtr<ID3D11Texture2D>> fullFovSwapchainImages;

        // Flat images for quad views
        ComPtr<ID3D11Texture2D> flatImage[xr::QuadView::Count];
        // Cached SRVs for flat images
        ComPtr<ID3D11ShaderResourceView> srvFlatImage[xr::QuadView::Count];

        // Sharpened images
        ComPtr<ID3D11Texture2D> sharpenedImage[xr::StereoView::Count];
        // Cached SRV/UAV for sharpened images
        ComPtr<ID3D11ShaderResourceView> srvSharpenedImage[xr::StereoView::Count];
        ComPtr<ID3D11UnorderedAccessView> uavSharpenedImage[xr::StereoView::Count];

        // Cached RTV for destination images (vector indexed by acquiredFullFovImageIndex to avoid dangling thread_local)
        std::vector<ComPtr<ID3D11RenderTargetView>> rtvDestination[xr::StereoView::Count];
    };

    // D3D11 implementation of the compositor interface.
    class D3D11Compositor : public BaseCompositor<D3D11Compositor, D3D11SwapchainGraphicsState> {
    public:
        D3D11Compositor(ID3D11Device* device, OpenXrApi* openXrApi);
        // Call destroy() in the destructor so resources are released even if destroy() was not
        // called explicitly (e.g. due to an exception). destroy() is idempotent.
        ~D3D11Compositor() override {
            destroy();
        }

        bool initialize(int32_t swapchainFormat) override;
        void destroy() override;
        bool isInitialized() const override;
        void waitForGpuIdle() override;

        // compositeView is inherited from BaseCompositor — it calls CRTP hooks below.

        // Called by BaseCompositor::NeedsReallocate via CRTP
        static void GetTextureDesc(const void* texture,
                                    uint32_t& outWidth,
                                    uint32_t& outHeight,
                                    uint32_t& outFormat) {
            auto* d3d11Tex = static_cast<ID3D11Texture2D*>(const_cast<void*>(texture));
            D3D11_TEXTURE2D_DESC desc{};
            d3d11Tex->GetDesc(&desc);
            outWidth = desc.Width;
            outHeight = desc.Height;
            outFormat = static_cast<uint32_t>(desc.Format);
        }

    private:
        // Allow the CRTP base class to access private hooks
        friend BaseCompositor<D3D11Compositor, D3D11SwapchainGraphicsState>;

        // --- CRTP hooks called by BaseCompositor::compositeView ---

        void populateSwapchainImagesCache(D3D11SwapchainGraphicsState& state, XrSwapchain swapchain, bool isFullFov);

        bool acquireAndResolveImages(
            const CompositorParams& params,
            const SwapchainInfo& stereoSwapchain,
            const SwapchainInfo& focusSwapchain,
            D3D11SwapchainGraphicsState& stereoState,
            D3D11SwapchainGraphicsState& focusState,
            void*& outSourceStereo,
            void*& outSourceFocus,
            void*& outDestination);

        void BindDirectSource(D3D11SwapchainGraphicsState& state, uint32_t targetSlot, void* sourceImage, uint32_t format);
        bool NeedsFlatReallocate(D3D11SwapchainGraphicsState& state, uint32_t targetSlot, uint32_t width, uint32_t height, uint32_t format);
        void CreateFlatImage(D3D11SwapchainGraphicsState& state, uint32_t targetSlot, uint32_t width, uint32_t height, uint32_t format);
        void CopySubImage(D3D11SwapchainGraphicsState& state, uint32_t targetSlot, void* sourceImage, const XrCompositionLayerProjectionView& view);

        void sharpenFocusView(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& focusSwapchain,
            D3D11SwapchainGraphicsState& focusState);

        void renderProjection(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& stereoSwapchain,
            const SwapchainInfo& focusSwapchain,
            D3D11SwapchainGraphicsState& stereoState,
            D3D11SwapchainGraphicsState& focusState,
            void* destination);

        void cleanupAndRelease(
            const CompositorParams& params,
            D3D11SwapchainGraphicsState& stereoState);

        // --- View caching helpers ---

        ID3D11ShaderResourceView* getOrCreateFlatSRV(D3D11SwapchainGraphicsState& state, uint32_t slot, DXGI_FORMAT format);
        ID3D11ShaderResourceView* getOrCreateSharpenedSRV(D3D11SwapchainGraphicsState& state, uint32_t viewIndex);
        ID3D11UnorderedAccessView* getOrCreateSharpenedUAV(D3D11SwapchainGraphicsState& state, uint32_t viewIndex);
        ID3D11RenderTargetView* getOrCreateRTV(D3D11SwapchainGraphicsState& state, uint32_t viewIndex, uint32_t acquiredIndex, ID3D11Texture2D* destination, DXGI_FORMAT format);

        ComPtr<ID3D11Device> m_device;
        ComPtr<ID3D11DeviceContext> m_context;

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

        // FIX (Item 2): Query object to track GPU execution completion
        ComPtr<ID3D11Query> m_frameEndQuery;

        // NOTE: m_swapchainStates and m_swapchainStatesMutex are now inherited
        //       from BaseCompositor.
    };

} // namespace openxr_api_layer
