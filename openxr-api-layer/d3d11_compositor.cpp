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
#include <utils/graphics.h>

#define A_CPU
#include <ffx_a.h>
#include <ffx_cas.h>

#include <ProjectionVS11.h>
#include <ProjectionPS11.h>
#include <SharpeningCS11.h>

namespace openxr_api_layer {

    using namespace log;
    using namespace xr::math;

    // Anonymous namespace to avoid ODR violations with FFX-CAS inline functions
    namespace {
        // Constant buffer structs
        struct ProjectionVSConstants {
            alignas(16) DirectX::XMFLOAT4X4 focusProjection;
        };

        struct ProjectionPSConstants {
            alignas(4) float smoothingArea;
            alignas(4) bool ignoreAlpha;
            alignas(4) bool isUnpremultipliedAlpha;
            alignas(4) bool debugFocusView;
            alignas(4) float sharpenFocusView;
            alignas(4) float chromaticAberrationCorrection;
            alignas(4) uint32_t frameCount;
            alignas(4) float _padding[1]; // Pad to 32 bytes (multiple of 16 for D3D11 constant buffer)
        };

        struct SharpeningCSConstants {
            alignas(4) uint32_t Const0[4];
            alignas(4) uint32_t Const1[4];
        };
    }

    D3D11Compositor::D3D11Compositor(ID3D11Device* device, OpenXrApi* openXrApi)
        : m_device(device)
        , m_openXrApi(openXrApi) {
        // ComPtr's initializing constructor already AddRefs the device; no explicit AddRef needed.
    }

    bool D3D11Compositor::isInitialized() const {
        return m_projectionPS != nullptr;
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
        TraceLoggingWrite(g_traceProvider, "InitializeCompositionResources", TLArg("D3D11", "Api"));
        LogDebug("D3D11 initializeCompositionResources: starting... (format={})\n", swapchainFormat);

        auto device = m_device.Get();

        // Cache the immediate context for the lifetime of this compositor
        // (Performance: avoid per-frame GetImmediateContext calls)
        device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());

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

        LogDebug("D3D11: Composition resources initialized successfully!\n");
        return true;
    }

    void* D3D11Compositor::compositeView(const CompositorParams& params,
                                          const SwapchainInfo& stereoSwapchain,
                                          const XrCompositionLayerProjectionView& stereoView,
                                          const SwapchainInfo& focusSwapchain,
                                          const XrCompositionLayerProjectionView& focusView) {
        auto device = m_device.Get();
        const uint32_t viewIndex = params.viewIndex;

        LogDebug("D3D11 compositeView: viewIndex={}\n", viewIndex);

        // Get or create swapchain graphics state
        auto& stereoState = m_swapchainStates[stereoSwapchain.handle];
        auto& focusState = m_swapchainStates[focusSwapchain.handle];

        // Populate swapchain images cache
        populateSwapchainImagesCache(stereoState, stereoSwapchain.handle, false);
        populateSwapchainImagesCache(focusState, focusSwapchain.handle, false);

        // Use cached immediate context (Performance: avoid per-frame GetImmediateContext)
        auto context = m_context.Get();

        // Grab input/output textures
        ID3D11Texture2D* sourceImage = nullptr;
        ID3D11Texture2D* sourceFocusImage = nullptr;
        ID3D11Texture2D* destinationImage = nullptr;
        uint32_t acquiredImageIndex = 0;
        {
            if (params.useQuadViews) {
                sourceImage = stereoState.images[stereoSwapchain.lastReleasedIndex];
            }
            sourceFocusImage = focusState.images[focusSwapchain.lastReleasedIndex];

            // Acquire/release full FOV swapchain image
            // Call base class virtual method directly to bypass layer's deferred release quirk,
            // because the full FOV swapchain is not tracked in m_swapchains.
            // With a shared array swapchain (arraySize=2), acquire once per frame on view 0, release on view 1.
            if (params.viewIndex == 0) {
                // Release previous swapchain image first (if any) to avoid call order errors.
                // The previous image may not have been acquired yet (first frame), in which case
                // the runtime returns an error that we can safely ignore.
                {
                    const XrResult releaseResult =
                        m_openXrApi->OpenXrApi::xrReleaseSwapchainImage(stereoSwapchain.fullFovSwapchain, nullptr);
                    if (XR_FAILED(releaseResult)) {
                        LogDebug("D3D11: xrReleaseSwapchainImage (full FOV, pre-acquire) failed: {}\n",
                                 xr::ToCString(releaseResult));
                    }
                }

                CHECK_XRCMD(m_openXrApi->OpenXrApi::xrAcquireSwapchainImage(stereoSwapchain.fullFovSwapchain, nullptr, &acquiredImageIndex));
                XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                waitInfo.timeout = 10000000000;
                CHECK_XRCMD(m_openXrApi->OpenXrApi::xrWaitSwapchainImage(stereoSwapchain.fullFovSwapchain, &waitInfo));

                stereoState.acquiredFullFovImageIndex = acquiredImageIndex;
            }

            populateSwapchainImagesCache(stereoState, stereoSwapchain.fullFovSwapchain, true);
            destinationImage = stereoState.fullFovSwapchainImages[stereoState.acquiredFullFovImageIndex];

            // Create history textures for temporal stability if not already created
            D3D11_TEXTURE2D_DESC destDesc{};
            destinationImage->GetDesc(&destDesc);
            for (uint32_t v = 0; v < xr::StereoView::Count; v++) {
                if (!stereoState.historyImage[v]) {
                    D3D11_TEXTURE2D_DESC histDesc = destDesc;
                    histDesc.Format = (DXGI_FORMAT)stereoSwapchain.createInfo.format; // Force fully-typed format
                    histDesc.ArraySize = 1;
                    histDesc.MipLevels = 1;
                    histDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    histDesc.CPUAccessFlags = 0;
                    histDesc.MiscFlags = 0;
                    CHECK_HRCMD(device->CreateTexture2D(&histDesc, nullptr, stereoState.historyImage[v].ReleaseAndGetAddressOf()));

                    // Create SRV for history
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Format = histDesc.Format;
                    srvDesc.Texture2D.MipLevels = 1;
                    CHECK_HRCMD(device->CreateShaderResourceView(stereoState.historyImage[v].Get(),
                                                                  &srvDesc,
                                                                  stereoState.srvHistoryImage[v].ReleaseAndGetAddressOf()));
                }
            }
        }

        // Timer (optional profiling - disabled in refactored compositor)

        // Flatten source images
        {
            const auto flattenSourceImage = [&](ID3D11Texture2D* image,
                                                const XrCompositionLayerProjectionView& view,
                                                const SwapchainInfo& swapchainInfo,
                                                D3D11SwapchainGraphicsState& state,
                                                uint32_t startSlot) {
                D3D11_TEXTURE2D_DESC desc{};
                if (state.flatImage[startSlot + viewIndex]) {
                    state.flatImage[startSlot + viewIndex]->GetDesc(&desc);
                }
                if (!state.flatImage[startSlot + viewIndex] ||
                    desc.Width != (UINT)view.subImage.imageRect.extent.width ||
                    desc.Height != (UINT)view.subImage.imageRect.extent.height) {
                    desc = {};
                    desc.ArraySize = 1;
                    desc.Width = view.subImage.imageRect.extent.width;
                    desc.Height = view.subImage.imageRect.extent.height;
                    desc.Format = (DXGI_FORMAT)swapchainInfo.createInfo.format;
                    desc.MipLevels = 1;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.MiscFlags = 0;
                    desc.SampleDesc.Count = 1;
                    CHECK_HRCMD(device->CreateTexture2D(&desc, nullptr, state.flatImage[startSlot + viewIndex].ReleaseAndGetAddressOf()));
                    
                    // Invalidate cached SRV since the texture was recreated
                    state.srvFlatImage[startSlot + viewIndex].Reset();
                }
                
                // Create cached SRV if it doesn't exist
                if (!state.srvFlatImage[startSlot + viewIndex]) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Format = (DXGI_FORMAT)swapchainInfo.createInfo.format;
                    srvDesc.Texture2D.MipLevels = 1;
                    CHECK_HRCMD(device->CreateShaderResourceView(
                        state.flatImage[startSlot + viewIndex].Get(),
                        &srvDesc,
                        state.srvFlatImage[startSlot + viewIndex].ReleaseAndGetAddressOf()));
                }

                D3D11_BOX box{};
                box.left = view.subImage.imageRect.offset.x;
                box.top = view.subImage.imageRect.offset.y;
                box.right = box.left + view.subImage.imageRect.extent.width;
                box.bottom = box.top + view.subImage.imageRect.extent.height;
                box.back = 1;
                context->CopySubresourceRegion(state.flatImage[startSlot + viewIndex].Get(),
                                               0, 0, 0, 0,
                                               image,
                                               view.subImage.imageArrayIndex,
                                               &box);
            };

            if (params.useQuadViews) {
                flattenSourceImage(sourceImage, stereoView, stereoSwapchain, stereoState, 0);
            }
            flattenSourceImage(sourceFocusImage, focusView, focusSwapchain, focusState, xr::StereoView::Count);
        }

        // Sharpen if needed
        if (params.sharpenFocusView) {
            D3D11_TEXTURE2D_DESC desc{};
            if (focusState.sharpenedImage[viewIndex]) {
                focusState.sharpenedImage[viewIndex]->GetDesc(&desc);
            }
            if (!focusState.sharpenedImage[viewIndex] ||
                desc.Width != (UINT)focusView.subImage.imageRect.extent.width ||
                desc.Height != (UINT)focusView.subImage.imageRect.extent.height) {
                desc = {};
                desc.ArraySize = 1;
                desc.Width = focusView.subImage.imageRect.extent.width;
                desc.Height = focusView.subImage.imageRect.extent.height;
                desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.MipLevels = 1;
                desc.SampleDesc.Count = 1;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                CHECK_HRCMD(device->CreateTexture2D(&desc, nullptr, focusState.sharpenedImage[viewIndex].ReleaseAndGetAddressOf()));
            }

            // Cache SRV for sharpened image
            if (!focusState.srvSharpenedImage[viewIndex]) {
                D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.Texture2D.MipLevels = 1;
                CHECK_HRCMD(device->CreateShaderResourceView(
                    focusState.sharpenedImage[viewIndex].Get(),
                    &desc,
                    focusState.srvSharpenedImage[viewIndex].ReleaseAndGetAddressOf()));
            }
            // Cache UAV for sharpened image
            if (!focusState.uavSharpenedImage[viewIndex]) {
                D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                CHECK_HRCMD(device->CreateUnorderedAccessView(
                    focusState.sharpenedImage[viewIndex].Get(), &uavDesc,
                    focusState.uavSharpenedImage[viewIndex].ReleaseAndGetAddressOf()));
            }

            // Set up shader
            SharpeningCSConstants sharpening{};
            CasSetup(sharpening.Const0,
                     sharpening.Const1,
                     std::clamp(params.sharpenFocusView, 0.f, 1.f),
                     (AF1)focusView.subImage.imageRect.extent.width,
                     (AF1)focusView.subImage.imageRect.extent.height,
                     (AF1)focusView.subImage.imageRect.extent.width,
                     (AF1)focusView.subImage.imageRect.extent.height);
            {
                D3D11_MAPPED_SUBRESOURCE mappedResources;
                CHECK_HRCMD(context->Map(m_sharpeningCSConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                memcpy(mappedResources.pData, &sharpening, sizeof(sharpening));
                context->Unmap(m_sharpeningCSConstants.Get(), 0);
            }

            context->CSSetConstantBuffers(0, 1, m_sharpeningCSConstants.GetAddressOf());
            context->CSSetShaderResources(0, 1, focusState.srvFlatImage[xr::StereoView::Count + viewIndex].GetAddressOf());
            context->CSSetUnorderedAccessViews(0, 1, focusState.uavSharpenedImage[viewIndex].GetAddressOf(), nullptr);
            context->CSSetShader(m_sharpeningCS.Get(), nullptr, 0);

            static const int threadGroupWorkRegionDim = 16;
            int dispatchX = (focusView.subImage.imageRect.extent.width + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            int dispatchY = (focusView.subImage.imageRect.extent.height + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            context->Dispatch((UINT)dispatchX, (UINT)dispatchY, 1);

            // Unbind
            ID3D11UnorderedAccessView* nullUAV[] = {nullptr};
            context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
        }

        // Composite
        {
            // Use cached SRVs for flat images
            ID3D11ShaderResourceView* srvForStereoView = nullptr;
            if (params.useQuadViews) {
                srvForStereoView = stereoState.srvFlatImage[viewIndex].Get();
            } else {
                srvForStereoView = m_srvBlankTexture.Get();
            }

            ID3D11ShaderResourceView* srvForFocusView = nullptr;
            if (params.sharpenFocusView) {
                srvForFocusView = focusState.srvSharpenedImage[viewIndex].Get();
            } else {
                srvForFocusView = focusState.srvFlatImage[xr::StereoView::Count + viewIndex].Get();
            }

            // Recreate RTV if the destination texture changed (swapchain rotates through multiple images)
            // We cache the texture pointer to detect changes without storing raw COM pointers.
            static thread_local ID3D11Texture2D* lastDestTexture[2] = {nullptr, nullptr};
            if (!stereoState.rtvDestination[viewIndex] || lastDestTexture[viewIndex] != destinationImage) {
                stereoState.rtvDestination[viewIndex].Reset();
                D3D11_RENDER_TARGET_VIEW_DESC desc{};
                desc.Format = (DXGI_FORMAT)stereoSwapchain.createInfo.format;
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.FirstArraySlice = viewIndex;
                desc.Texture2DArray.ArraySize = 1;
                CHECK_HRCMD(device->CreateRenderTargetView(destinationImage, &desc,
                    stereoState.rtvDestination[viewIndex].ReleaseAndGetAddressOf()));
                lastDestTexture[viewIndex] = destinationImage;
            }
            auto rtv = stereoState.rtvDestination[viewIndex].Get();

            // Performance: Batch both constant buffer updates
            ProjectionVSConstants projection{};
            {
                const DirectX::XMMATRIX baseLayerViewProjection =
                    ComposeProjectionMatrix(params.cachedEyeFov, NearFar{0.1f, 20.f});
                const DirectX::XMMATRIX layerViewProjection =
                    ComposeProjectionMatrix(focusView.fov, NearFar{0.1f, 20.f});

                DirectX::XMStoreFloat4x4(
                    &projection.focusProjection,
                    DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, baseLayerViewProjection) *
                                               layerViewProjection));
            }

            ProjectionPSConstants drawing{};
            drawing.smoothingArea = params.useQuadViews ? params.smoothenFocusViewEdges : 0;
            drawing.ignoreAlpha = ~(params.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
            drawing.isUnpremultipliedAlpha = params.layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            drawing.debugFocusView = params.debugFocusView;
            drawing.sharpenFocusView = params.sharpenFocusView;
            drawing.chromaticAberrationCorrection = params.chromaticAberrationCorrection;
            drawing.frameCount = params.frameCount;
            {
                D3D11_MAPPED_SUBRESOURCE mappedResources;
                CHECK_HRCMD(context->Map(m_projectionVSConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                memcpy(mappedResources.pData, &projection, sizeof(projection));
                context->Unmap(m_projectionVSConstants.Get(), 0);
            }
            {
                D3D11_MAPPED_SUBRESOURCE mappedResources;
                CHECK_HRCMD(context->Map(m_projectionPSConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResources));
                memcpy(mappedResources.pData, &drawing, sizeof(drawing));
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
            // Bind stereo(t0), focus(t1), history(t2) textures
            ID3D11ShaderResourceView* srvs[3] = {srvForStereoView, srvForFocusView, nullptr};
            if (stereoState.srvHistoryImage[viewIndex]) {
                srvs[2] = stereoState.srvHistoryImage[viewIndex].Get();
            }
            context->PSSetShaderResources(0, 3, srvs);
            context->PSSetShader(m_projectionPS.Get(), nullptr, 0);
            context->Draw(3, 0);

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

        // Timer stop (optional profiling - disabled in refactored compositor)
        // Performance: Clear all bound shader resources, UAVs, and render targets
        // to avoid state leakage between frames (0.05-0.1ms/frame savings)
        context->ClearState();
        // Release full FOV swapchain image on view 1 only (shared array swapchain)
        // Call base class virtual method directly to bypass layer's deferred release quirk
        if (params.viewIndex == xr::StereoView::Right) {
            const XrResult releaseResult =
                m_openXrApi->OpenXrApi::xrReleaseSwapchainImage(stereoSwapchain.fullFovSwapchain, nullptr);
            if (XR_FAILED(releaseResult)) {
                LogWarning("D3D11: xrReleaseSwapchainImage (full FOV) failed: {}\n", xr::ToCString(releaseResult));
            }
        }

        return destinationImage;
    }

    void D3D11Compositor::destroy() {
        // Flush cached context to ensure all pending GPU work is submitted
        // and completed before we release resources.
        if (m_context) {
            m_context->Flush();
        }

        // Idempotent: safe to call multiple times (ComPtr::Reset() is a no-op on already-null pointers).
        m_swapchainStates.clear();
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
    }

    void D3D11Compositor::waitForGpuIdle() {
        if (m_context) {
            m_context->Flush();
        }
    }

    std::unique_ptr<ICompositor> createD3D11Compositor(ID3D11Device* device, OpenXrApi* openXrApi) {
        return std::make_unique<D3D11Compositor>(device, openXrApi);
    }

} // namespace openxr_api_layer
