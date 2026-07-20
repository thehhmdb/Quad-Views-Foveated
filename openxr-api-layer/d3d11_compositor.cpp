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

#include "pch.h"
#include "d3d11_compositor.h"

#include "log.h"
#include "util.h"
#include "views.h"
#include "framework/dispatch.gen.h"
#include "logic/compositor_shared.h"

#include <ProjectionVS11.h>
#include <ProjectionPS11.h>
#include <SharpeningCS11.h>

namespace openxr_api_layer {

    using namespace log;
    using namespace xr::math;

    D3D11Compositor::D3D11Compositor(ID3D11Device* device, OpenXrApi* openXrApi)
        : BaseCompositor(openXrApi)
        , m_device(device) {
        // ComPtr's initializing constructor already AddRefs the device; no explicit AddRef needed.
    }

    bool D3D11Compositor::isInitialized() const {
        return m_initialized;
    }

    void D3D11Compositor::populateSwapchainImagesCache(D3D11SwapchainGraphicsState& state, XrSwapchain swapchain, bool isFullFov) {
        auto& images = isFullFov ? state.fullFovSwapchainImages : state.images;
        if (!images.empty()) {
            return;
        }

        uint32_t count;
        CHECK_XRCMD(m_openXrApi->xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr));
        LogDebug("  D3D11: Populating swapchain images cache: swapchain={:x}, count={}\n", (uint64_t)swapchain, count);

        std::vector<XrSwapchainImageD3D11KHR> d3d11Images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        CHECK_XRCMD(m_openXrApi->xrEnumerateSwapchainImages(
            swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(d3d11Images.data())));

        for (uint32_t i = 0; i < count; i++) {
            images.push_back(d3d11Images[i].texture);
        }
    }

    bool D3D11Compositor::initialize(int32_t swapchainFormat) {
        QVF_TRACE("InitializeCompositionResources", TLArg("D3D11", "Api"));
        LogDebug("D3D11 initializeCompositionResources: starting... (format={})\n", swapchainFormat);

        auto device = m_device.Get();

        // Create sampler state
        {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.MaxAnisotropy = 1;
            desc.MinLOD = D3D11_MIP_LOD_BIAS_MIN;
            desc.MaxLOD = D3D11_MIP_LOD_BIAS_MAX;
            CHECK_HRCMD(device->CreateSamplerState(&desc, m_linearClampSampler.ReleaseAndGetAddressOf()));
        }

        // Create rasterizer state
        {
            D3D11_RASTERIZER_DESC desc{};
            desc.FillMode = D3D11_FILL_SOLID;
            desc.CullMode = D3D11_CULL_NONE;
            desc.FrontCounterClockwise = TRUE;
            CHECK_HRCMD(device->CreateRasterizerState(&desc, m_noDepthRasterizer.ReleaseAndGetAddressOf()));
        }

        // Create constant buffers
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = (UINT)std::max((size_t)16, sizeof(ProjectionVSConstants));
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            CHECK_HRCMD(device->CreateBuffer(&desc, nullptr, m_projectionVSConstants.ReleaseAndGetAddressOf()));
        }
        {
            D3D11_BUFFER_DESC desc{};
            // D3D11 requires ByteWidth to be a multiple of 16.
            desc.ByteWidth = (UINT)(((sizeof(ProjectionPSConstants) + 15) / 16) * 16);
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            CHECK_HRCMD(device->CreateBuffer(&desc, nullptr, m_projectionPSConstants.ReleaseAndGetAddressOf()));
        }

        // Cache immediate context for reuse across frames
        device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());

        // Create shaders (SM 5.0 bytecode for D3D11)
        CHECK_HRCMD(device->CreateVertexShader(g_ProjectionVS11, sizeof(g_ProjectionVS11), nullptr, m_projectionVS.ReleaseAndGetAddressOf()));
        CHECK_HRCMD(device->CreatePixelShader(g_ProjectionPS11, sizeof(g_ProjectionPS11), nullptr, m_projectionPS.ReleaseAndGetAddressOf()));

        // Sharpening resources
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = (UINT)std::max((size_t)16, sizeof(SharpeningCSConstants));
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            CHECK_HRCMD(device->CreateBuffer(&desc, nullptr, m_sharpeningCSConstants.ReleaseAndGetAddressOf()));
        }
        CHECK_HRCMD(device->CreateComputeShader(g_SharpeningCS11, sizeof(g_SharpeningCS11), nullptr, m_sharpeningCS.ReleaseAndGetAddressOf()));

        // Blank texture
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.ArraySize = 1;
            desc.Width = desc.Height = 32;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initialData{};
            initialData.SysMemPitch = desc.Width;
            std::vector<uint32_t> blank(desc.Width * desc.Height, 0);
            initialData.pSysMem = blank.data();
            CHECK_HRCMD(device->CreateTexture2D(&desc, &initialData, m_blankTexture.ReleaseAndGetAddressOf()));
        }
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.Texture2D.MipLevels = 1;
            CHECK_HRCMD(device->CreateShaderResourceView(m_blankTexture.Get(), &desc, m_srvBlankTexture.ReleaseAndGetAddressOf()));
        }

        // FIX (Item 2): Create frame end query for accurate GPU idle tracking
        {
            D3D11_QUERY_DESC queryDesc{};
            queryDesc.Query = D3D11_QUERY_EVENT;
            CHECK_HRCMD(device->CreateQuery(&queryDesc, m_frameEndQuery.ReleaseAndGetAddressOf()));
        }

        LogDebug("D3D11: Composition resources initialized successfully!\n");
        m_initialized = true;
        return true;
    }

    //----------------------------------------------------------------------------------------
    // CRTP hooks — called by BaseCompositor::compositeView
    //----------------------------------------------------------------------------------------

    bool D3D11Compositor::acquireAndResolveImages(
            const CompositorParams& params,
            const SwapchainInfo& stereoSwapchain,
            const SwapchainInfo& focusSwapchain,
            D3D11SwapchainGraphicsState& stereoState,
            D3D11SwapchainGraphicsState& focusState,
            void*& outSourceStereo,
            void*& outSourceFocus,
            void*& outDestination) {
        // Resolve source textures from cached swapchain images
        if (params.useQuadViews) {
            outSourceStereo = stereoState.images[stereoSwapchain.lastReleasedIndex].Get();
        }
        outSourceFocus = focusState.images[focusSwapchain.lastReleasedIndex].Get();

        // Acquire full FOV swapchain image (view 0 only for shared array swapchain)
        if (params.viewIndex == 0) {
            const uint32_t idx = acquireFullFovImage(stereoSwapchain.fullFovSwapchain, stereoState);
            if (idx == UINT32_MAX) {
                return false; // acquisition failed — skip this frame
            }
        }

        // Populate full FOV images cache and resolve destination
        populateSwapchainImagesCache(stereoState, stereoSwapchain.fullFovSwapchain, true);
        outDestination = stereoState.fullFovSwapchainImages[stereoState.acquiredFullFovImageIndex].Get();

        return true;
    }

    void D3D11Compositor::BindDirectSource(D3D11SwapchainGraphicsState& state, uint32_t targetSlot, void* sourceImage, uint32_t format) {
        state.flatImage[targetSlot] = static_cast<ID3D11Texture2D*>(sourceImage);
        state.srvFlatImage[targetSlot].Reset();
        getOrCreateFlatSRV(state, targetSlot, (DXGI_FORMAT)format);
    }

    bool D3D11Compositor::NeedsFlatReallocate(D3D11SwapchainGraphicsState& state, uint32_t targetSlot, uint32_t width, uint32_t height, uint32_t format) {
        return NeedsReallocate(state.flatImage[targetSlot].Get(), width, height, format);
    }

    void D3D11Compositor::CreateFlatImage(D3D11SwapchainGraphicsState& state, uint32_t targetSlot, uint32_t width, uint32_t height, uint32_t format) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.ArraySize = 1;
        desc.Width = width;
        desc.Height = height;
        desc.Format = (DXGI_FORMAT)format;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CHECK_HRCMD(m_device->CreateTexture2D(&desc, nullptr, state.flatImage[targetSlot].ReleaseAndGetAddressOf()));
        state.srvFlatImage[targetSlot].Reset();
        getOrCreateFlatSRV(state, targetSlot, (DXGI_FORMAT)format);
    }

    void D3D11Compositor::CopySubImage(D3D11SwapchainGraphicsState& state, uint32_t targetSlot, void* sourceImage, const XrCompositionLayerProjectionView& view) {
        ID3D11Texture2D* src = static_cast<ID3D11Texture2D*>(sourceImage);
        D3D11_BOX box{};
        box.left = view.subImage.imageRect.offset.x;
        box.top = view.subImage.imageRect.offset.y;
        box.right = box.left + view.subImage.imageRect.extent.width;
        box.bottom = box.top + view.subImage.imageRect.extent.height;
        box.back = 1;
        m_context->CopySubresourceRegion(state.flatImage[targetSlot].Get(), 0, 0, 0, 0,
                                         src, view.subImage.imageArrayIndex, &box);
    }

    void D3D11Compositor::sharpenFocusView(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& focusSwapchain,
            D3D11SwapchainGraphicsState& focusState) {
        const uint32_t viewIndex = params.viewIndex;
        auto context = m_context.Get();

        const uint32_t sharpWidth = (uint32_t)focusView.subImage.imageRect.extent.width;
        const uint32_t sharpHeight = (uint32_t)focusView.subImage.imageRect.extent.height;
        const uint32_t sharpFormat = (uint32_t)DXGI_FORMAT_R16G16B16A16_FLOAT;

        if (NeedsReallocate(focusState.sharpenedImage[viewIndex].Get(), sharpWidth, sharpHeight, sharpFormat)) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.ArraySize = 1;
            desc.Width = sharpWidth;
            desc.Height = sharpHeight;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            CHECK_HRCMD(m_device->CreateTexture2D(&desc, nullptr, focusState.sharpenedImage[viewIndex].ReleaseAndGetAddressOf()));
            focusState.srvSharpenedImage[viewIndex].Reset();
            focusState.uavSharpenedImage[viewIndex].Reset();
        }

        ID3D11ShaderResourceView* srv = getOrCreateSharpenedSRV(focusState, viewIndex);
        ID3D11UnorderedAccessView* uav = getOrCreateSharpenedUAV(focusState, viewIndex);

        // Setup constants
        SharpeningPass sharpeningPass;
        SharpeningCSConstants sharpening{};
        sharpeningPass.PrepareConstants(sharpening, params.sharpenFocusView, sharpWidth, sharpHeight);

        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            CHECK_HRCMD(context->Map(m_sharpeningCSConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
            memcpy(mapped.pData, &sharpening, sizeof(sharpening));
            context->Unmap(m_sharpeningCSConstants.Get(), 0);
        }

        // Bind and dispatch
        context->CSSetConstantBuffers(0, 1, m_sharpeningCSConstants.GetAddressOf());
        context->CSSetShaderResources(0, 1, focusState.srvFlatImage[xr::StereoView::Count + viewIndex].GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->CSSetShader(m_sharpeningCS.Get(), nullptr, 0);

        sharpeningPass(params, focusView, focusSwapchain);
        context->Dispatch(sharpeningPass.dispatchX, sharpeningPass.dispatchY, 1);

        // Unbind CS resources so the sharpened texture can be bound as a PS SRV
        // in renderProjection(). D3D11 nullifies an SRV if the same resource is
        // still bound as a UAV, producing black output.
        ID3D11UnorderedAccessView* nullUAV[1] = {nullptr};
        context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

        ID3D11ShaderResourceView* nullCssrv[1] = {nullptr};
        context->CSSetShaderResources(0, 1, nullCssrv);

        context->CSSetShader(nullptr, nullptr, 0);
    }

    void D3D11Compositor::renderProjection(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& stereoSwapchain,
            const SwapchainInfo& focusSwapchain,
            D3D11SwapchainGraphicsState& stereoState,
            D3D11SwapchainGraphicsState& focusState,
            void* destination) {
        const uint32_t viewIndex = params.viewIndex;
        auto context = m_context.Get();
        auto destTex = static_cast<ID3D11Texture2D*>(destination);

        // Select SRVs for stereo and focus views
        ID3D11ShaderResourceView* srvForStereoView =
            params.useQuadViews ? stereoState.srvFlatImage[viewIndex].Get() : m_srvBlankTexture.Get();

        ID3D11ShaderResourceView* srvForFocusView =
            params.sharpenFocusView ? focusState.srvSharpenedImage[viewIndex].Get()
                : focusState.srvFlatImage[xr::StereoView::Count + viewIndex].Get();

        // Get or create cached RTV
        auto rtv = getOrCreateRTV(stereoState, viewIndex, stereoState.acquiredFullFovImageIndex,
                                  destTex, (DXGI_FORMAT)stereoSwapchain.createInfo.format);

        // Compute projection constants
        ProjectionVSConstants projection{};
        ComputeProjectionConstants(projection, params.cachedEyeFov, focusView.fov);
        projection.stereoSubRect = {0, 0, 0, 0};
        projection.focusSubRect = {0, 0, 0, 0};
        projection.stereoSwapchainSize = {0, 0};
        projection.focusSwapchainSize = {0, 0};
        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            CHECK_HRCMD(context->Map(m_projectionVSConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
            memcpy(mapped.pData, &projection, sizeof(projection));
            context->Unmap(m_projectionVSConstants.Get(), 0);
        }

        ProjectionPSConstants drawing{};
        ComputePixelShaderConstants(drawing, params);
        drawing.stereoSubRect = {0, 0, 0, 0};
        drawing.focusSubRect = {0, 0, 0, 0};
        drawing.stereoSwapchainSize = {0, 0};
        drawing.focusSwapchainSize = {0, 0};
        drawing.useDirectStereoSampling = false;
        drawing.useDirectFocusSampling = false;
        {
            D3D11_MAPPED_SUBRESOURCE mapped;
            CHECK_HRCMD(context->Map(m_projectionPSConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
            memcpy(mapped.pData, &drawing, sizeof(drawing));
            context->Unmap(m_projectionPSConstants.Get(), 0);
        }

        // Draw
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->RSSetState(m_noDepthRasterizer.Get());
        D3D11_VIEWPORT viewport{};
        viewport.Width = (float)params.fullFovResolution.width;
        viewport.Height = (float)params.fullFovResolution.height;
        viewport.MaxDepth = 1.f;
        context->RSSetViewports(1, &viewport);
        context->VSSetConstantBuffers(0, 1, m_projectionVSConstants.GetAddressOf());
        context->VSSetShader(m_projectionVS.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, m_projectionPSConstants.GetAddressOf());
        context->PSSetSamplers(0, 1, m_linearClampSampler.GetAddressOf());
        ID3D11ShaderResourceView* srvs[] = {srvForStereoView, srvForFocusView};
        context->PSSetShaderResources(0, 2, srvs);
        context->PSSetShader(m_projectionPS.Get(), nullptr, 0);
        context->Draw(3, 0);

        // Issue end-of-frame query for GPU idle tracking
        context->End(m_frameEndQuery.Get());

        // Debug eye gaze
        if (params.debugEyeGaze) {
            XrOffset2Di eyeGaze;
            eyeGaze.x = (uint32_t)(params.fullFovResolution.width * (params.eyeGaze.x + 1.f) / 2.f);
            eyeGaze.y = (uint32_t)(params.fullFovResolution.height * (1.f - params.eyeGaze.y) / 2.f);

            const float color[] = {0.5f, 0, 0.5f, 1};
            D3D11_RECT rect;
            rect.left = eyeGaze.x - 10;
            rect.right = eyeGaze.x + 10;
            rect.top = eyeGaze.y - 10;
            rect.bottom = eyeGaze.y + 10;
            ComPtr<ID3D11DeviceContext4> context4;
            context->QueryInterface(context4.ReleaseAndGetAddressOf());
            if (context4) {
                context4->ClearView(rtv, color, &rect, 1);
            }
        }
    }

    void D3D11Compositor::cleanupAndRelease(
            const CompositorParams& params,
            D3D11SwapchainGraphicsState& stereoState) {
        auto context = m_context.Get();

        // Unbind only the resources the compositor bound
        ID3D11RenderTargetView* nullRTV[1] = {nullptr};
        context->OMSetRenderTargets(1, nullRTV, nullptr);

        ID3D11ShaderResourceView* nullSRV[2] = {nullptr, nullptr};
        context->PSSetShaderResources(0, 2, nullSRV);

        ID3D11UnorderedAccessView* nullUAV[1] = {nullptr};
        context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

        // Release full FOV swapchain image on view 1 only (shared array swapchain)
        if (params.viewIndex == xr::StereoView::Right) {
            releaseFullFovImage(stereoState);
        }
    }

    //----------------------------------------------------------------------------------------
    // View caching helpers
    //----------------------------------------------------------------------------------------

    ID3D11ShaderResourceView* D3D11Compositor::getOrCreateFlatSRV(
            D3D11SwapchainGraphicsState& state, uint32_t slot, DXGI_FORMAT format) {
        if (!state.srvFlatImage[slot]) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Format = format;
            srvDesc.Texture2D.MipLevels = 1;
            CHECK_HRCMD(m_device->CreateShaderResourceView(
                state.flatImage[slot].Get(), &srvDesc,
                state.srvFlatImage[slot].ReleaseAndGetAddressOf()));
        }
        return state.srvFlatImage[slot].Get();
    }

    ID3D11ShaderResourceView* D3D11Compositor::getOrCreateSharpenedSRV(
            D3D11SwapchainGraphicsState& state, uint32_t viewIndex) {
        if (!state.srvSharpenedImage[viewIndex]) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srvDesc.Texture2D.MipLevels = 1;
            CHECK_HRCMD(m_device->CreateShaderResourceView(
                state.sharpenedImage[viewIndex].Get(), &srvDesc,
                state.srvSharpenedImage[viewIndex].ReleaseAndGetAddressOf()));
        }
        return state.srvSharpenedImage[viewIndex].Get();
    }

    ID3D11UnorderedAccessView* D3D11Compositor::getOrCreateSharpenedUAV(
            D3D11SwapchainGraphicsState& state, uint32_t viewIndex) {
        if (!state.uavSharpenedImage[viewIndex]) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            CHECK_HRCMD(m_device->CreateUnorderedAccessView(
                state.sharpenedImage[viewIndex].Get(), &uavDesc,
                state.uavSharpenedImage[viewIndex].ReleaseAndGetAddressOf()));
        }
        return state.uavSharpenedImage[viewIndex].Get();
    }

    ID3D11RenderTargetView* D3D11Compositor::getOrCreateRTV(
            D3D11SwapchainGraphicsState& state, uint32_t viewIndex, uint32_t acquiredIndex,
            ID3D11Texture2D* destination, DXGI_FORMAT format) {
        if (state.rtvDestination[viewIndex].size() <= acquiredIndex) {
            state.rtvDestination[viewIndex].resize(acquiredIndex + 1);
        }
        if (!state.rtvDestination[viewIndex][acquiredIndex]) {
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            desc.Format = format;
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.FirstArraySlice = viewIndex;
            desc.Texture2DArray.ArraySize = 1;
            CHECK_HRCMD(m_device->CreateRenderTargetView(destination, &desc,
                state.rtvDestination[viewIndex][acquiredIndex].ReleaseAndGetAddressOf()));
        }
        return state.rtvDestination[viewIndex][acquiredIndex].Get();
    }

    void D3D11Compositor::destroy() {
        // FIX: Skip all cleanup during DLL unload.
        if (g_isUnloading) {
            return;
        }

        // Idempotent: safe to call multiple times (ComPtr::Reset() is a no-op on already-null pointers).
        m_swapchainStates.clear();
        m_context.Reset();
        m_linearClampSampler.Reset();
        m_noDepthRasterizer.Reset();
        m_projectionVSConstants.Reset();
        m_projectionPSConstants.Reset();
        m_projectionVS.Reset();
        m_projectionPS.Reset();
        m_sharpeningCSConstants.Reset();
        m_sharpeningCS.Reset();
        m_blankTexture.Reset();
        m_srvBlankTexture.Reset();
        m_frameEndQuery.Reset();
    }

    void D3D11Compositor::waitForGpuIdle() {
        // FIX: Skip GPU sync during DLL unload.
        if (g_isUnloading) {
            return;
        }

        if (!m_context || !m_frameEndQuery) {
            return;
        }

        m_context->Flush();

        // FIX (Item 2): Block CPU until the GPU signals the event query
        BOOL done = FALSE;
        while (m_context->GetData(m_frameEndQuery.Get(), &done, sizeof(done), 0) == S_FALSE) {
            SwitchToThread();
        }
    }

    std::unique_ptr<ICompositor> createD3D11Compositor(ID3D11Device* device, OpenXrApi* openXrApi) {
        auto compositor = std::make_unique<D3D11Compositor>(device, openXrApi);
        if (!compositor) {
            return nullptr;
        }
        return compositor;
    }

} // namespace openxr_api_layer
