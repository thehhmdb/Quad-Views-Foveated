// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MockOpenXrApi.h"
#include "WarpHelpers.h"
#include "compositor.h"
#include "d3d11_compositor.h"
#include "d3d12_compositor.h"
#include "logic/compositor_shared.h"
#include "views.h"

// Shader bytecode is generated at build time — include the headers
#include <ProjectionVS11.h>
#include <ProjectionPS11.h>
#include <SharpeningCS11.h>
#include <ProjectionVS.h>
#include <ProjectionPS.h>
#include <SharpeningCS.h>

namespace openxr_api_layer {

    class CompositorWarpTest : public ::testing::TestWithParam<bool> {
    protected:
        MockOpenXrApi mockApi;
        bool isD3D12;

        // D3D11 state
        ComPtr<ID3D11Device> d3d11Device;
        ComPtr<ID3D11DeviceContext> d3d11Context;

        // D3D12 state
        ComPtr<ID3D12Device> d3d12Device;
        ComPtr<ID3D12CommandQueue> d3d12Queue;

        std::unique_ptr<ICompositor> compositor;
        uint32_t width = 512;
        uint32_t height = 512;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;

        // Track created textures for cleanup
        std::vector<ComPtr<ID3D11Texture2D>> d3d11Textures;
        std::vector<ComPtr<ID3D12Resource>> d3d12Textures;

        void SetUp() override {
            mockApi.initializeForTesting();
            isD3D12 = GetParam();
            mockApi.m_isD3D12Mode = isD3D12;
            mockApi.SetupWarpSwapchainMocks();

            if (isD3D12) {
                d3d12Device = CreateD3D12WarpDevice(d3d12Queue);
                compositor = createD3D12Compositor(d3d12Device.Get(), d3d12Queue.Get(), &mockApi);
            } else {
                d3d11Device = CreateD3D11WarpDevice(d3d11Context);
                compositor = createD3D11Compositor(d3d11Device.Get(), &mockApi);
            }
        }

        // Create a source texture (D3D11 or D3D12) and return as void*
        void* CreateSourceTexture(uint32_t arraySize = 1) {
            if (isD3D12) {
                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
                    format, width, height, static_cast<UINT16>(arraySize),
                    1,  // mipLevels
                    1,  // sampleCount
                    0,  // sampleQuality
                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
                ComPtr<ID3D12Resource> tex;
                CHECK_HRCMD(d3d12Device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                    IID_PPV_ARGS(tex.ReleaseAndGetAddressOf())));
                d3d12Textures.push_back(tex);
                return tex.Get();
            } else {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = width;
                desc.Height = height;
                desc.MipLevels = 1;
                desc.ArraySize = arraySize;
                desc.Format = format;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                ComPtr<ID3D11Texture2D> tex;
                CHECK_HRCMD(d3d11Device->CreateTexture2D(&desc, nullptr, tex.ReleaseAndGetAddressOf()));
                d3d11Textures.push_back(tex);
                return tex.Get();
            }
        }

        void TearDown() override {
            if (compositor) {
                compositor->waitForGpuIdle();
                compositor->destroy();
            }
        }
    };

    TEST_P(CompositorWarpTest, InitializeAndComposite_SmokeTest) {
        ASSERT_NE(compositor, nullptr);

        // 1. Initialize compositor
        ASSERT_TRUE(compositor->initialize(static_cast<int32_t>(format)));

        // 2. Create mock swapchains and textures
        XrSwapchain stereoHandle = reinterpret_cast<XrSwapchain>(1);
        XrSwapchain focusHandle = reinterpret_cast<XrSwapchain>(2);
        XrSwapchain fullFovHandle = reinterpret_cast<XrSwapchain>(3);

        // Full FOV needs arraySize = 2 (StereoView::Count)
        void* fullFovTex1 = CreateSourceTexture(2);
        void* fullFovTex2 = CreateSourceTexture(2);
        mockApi.RegisterMockSwapchain(fullFovHandle, {fullFovTex1, fullFovTex2}, 2);

        // Source textures (arraySize = 1)
        void* stereoTex = CreateSourceTexture(1);
        void* focusTex = CreateSourceTexture(1);
        mockApi.RegisterMockSwapchain(stereoHandle, {stereoTex}, 1);
        mockApi.RegisterMockSwapchain(focusHandle, {focusTex}, 1);

        // 3. Setup SwapchainInfo
        SwapchainInfo stereoInfo{};
        stereoInfo.handle = stereoHandle;
        stereoInfo.createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        stereoInfo.createInfo.next = nullptr;
        stereoInfo.createInfo.createFlags = 0;
        stereoInfo.createInfo.format = static_cast<int64_t>(format);
        stereoInfo.createInfo.sampleCount = 1;
        stereoInfo.createInfo.width = width;
        stereoInfo.createInfo.height = height;
        stereoInfo.createInfo.faceCount = 1;
        stereoInfo.createInfo.arraySize = 1;
        stereoInfo.createInfo.mipCount = 1;
        stereoInfo.fullFovSwapchain = fullFovHandle;
        stereoInfo.lastReleasedIndex = 0;

        SwapchainInfo focusInfo{};
        focusInfo.handle = focusHandle;
        focusInfo.createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        focusInfo.createInfo.next = nullptr;
        focusInfo.createInfo.createFlags = 0;
        focusInfo.createInfo.format = static_cast<int64_t>(format);
        focusInfo.createInfo.sampleCount = 1;
        focusInfo.createInfo.width = width / 2;
        focusInfo.createInfo.height = height / 2;
        focusInfo.createInfo.faceCount = 1;
        focusInfo.createInfo.arraySize = 1;
        focusInfo.createInfo.mipCount = 1;
        focusInfo.fullFovSwapchain = fullFovHandle;
        focusInfo.lastReleasedIndex = 0;

        // 4. Setup ProjectionViews
        XrCompositionLayerProjectionView stereoView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        stereoView.next = nullptr;
        stereoView.subImage.swapchain = stereoHandle;
        stereoView.subImage.imageArrayIndex = 0;
        stereoView.subImage.imageRect.offset = {0, 0};
        stereoView.subImage.imageRect.extent = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
        stereoView.fov = {-0.5f, 0.5f, -0.5f, 0.5f};

        XrCompositionLayerProjectionView focusView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        focusView.next = nullptr;
        focusView.subImage.swapchain = focusHandle;
        focusView.subImage.imageArrayIndex = 0;
        focusView.subImage.imageRect.offset = {0, 0};
        focusView.subImage.imageRect.extent = {static_cast<int32_t>(width / 2), static_cast<int32_t>(height / 2)};
        focusView.fov = {-0.2f, 0.2f, -0.2f, 0.2f};

        // 5. Setup CompositorParams
        CompositorParams params{};
        params.viewIndex = 0; // Left eye
        params.cachedEyeFov = stereoView.fov;
        params.fullFovResolution = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
        params.useQuadViews = true;
        params.smoothenFocusViewEdges = 0.2f;
        params.sharpenFocusView = 0.5f; // Test the CAS sharpening path
        params.debugFocusView = false;
        params.debugEyeGaze = false;
        params.eyeGaze = {0.0f, 0.0f};
        params.layerFlags = 0;
        params.ditheringAmount = 0.04f;
        params.frameCount = 0;

        // 6. Execute compositeView for Left Eye
        void* resultLeft = compositor->compositeView(params, stereoInfo, stereoView, focusInfo, focusView);
        EXPECT_NE(resultLeft, nullptr);

        // 7. Execute compositeView for Right Eye
        params.viewIndex = 1;
        void* resultRight = compositor->compositeView(params, stereoInfo, stereoView, focusInfo, focusView);
        EXPECT_NE(resultRight, nullptr);

        // If pipeline states, shaders, or barriers were invalid, WARP would trigger
        // Device Removed and the compositor would return null or throw.
    }

    TEST_P(CompositorWarpTest, CompositeWithoutSharpening) {
        ASSERT_NE(compositor, nullptr);
        ASSERT_TRUE(compositor->initialize(static_cast<int32_t>(format)));

        XrSwapchain stereoHandle = reinterpret_cast<XrSwapchain>(10);
        XrSwapchain focusHandle = reinterpret_cast<XrSwapchain>(11);
        XrSwapchain fullFovHandle = reinterpret_cast<XrSwapchain>(12);

        void* fullFovTex1 = CreateSourceTexture(2);
        void* fullFovTex2 = CreateSourceTexture(2);
        mockApi.RegisterMockSwapchain(fullFovHandle, {fullFovTex1, fullFovTex2}, 2);

        void* stereoTex = CreateSourceTexture(1);
        void* focusTex = CreateSourceTexture(1);
        mockApi.RegisterMockSwapchain(stereoHandle, {stereoTex}, 1);
        mockApi.RegisterMockSwapchain(focusHandle, {focusTex}, 1);

        SwapchainInfo stereoInfo{};
        stereoInfo.handle = stereoHandle;
        stereoInfo.createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        stereoInfo.createInfo.format = static_cast<int64_t>(format);
        stereoInfo.createInfo.width = width;
        stereoInfo.createInfo.height = height;
        stereoInfo.createInfo.faceCount = 1;
        stereoInfo.createInfo.arraySize = 1;
        stereoInfo.createInfo.mipCount = 1;
        stereoInfo.fullFovSwapchain = fullFovHandle;
        stereoInfo.lastReleasedIndex = 0;

        SwapchainInfo focusInfo{};
        focusInfo.handle = focusHandle;
        focusInfo.createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        focusInfo.createInfo.format = static_cast<int64_t>(format);
        focusInfo.createInfo.width = width / 2;
        focusInfo.createInfo.height = height / 2;
        focusInfo.createInfo.faceCount = 1;
        focusInfo.createInfo.arraySize = 1;
        focusInfo.createInfo.mipCount = 1;
        focusInfo.fullFovSwapchain = fullFovHandle;
        focusInfo.lastReleasedIndex = 0;

        XrCompositionLayerProjectionView stereoView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        stereoView.subImage.swapchain = stereoHandle;
        stereoView.subImage.imageRect.extent = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
        stereoView.fov = {-0.5f, 0.5f, -0.5f, 0.5f};

        XrCompositionLayerProjectionView focusView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        focusView.subImage.swapchain = focusHandle;
        focusView.subImage.imageRect.extent = {static_cast<int32_t>(width / 2), static_cast<int32_t>(height / 2)};
        focusView.fov = {-0.2f, 0.2f, -0.2f, 0.2f};

        CompositorParams params{};
        params.viewIndex = 0;
        params.cachedEyeFov = stereoView.fov;
        params.fullFovResolution = {static_cast<int32_t>(width), static_cast<int32_t>(height)};
        params.useQuadViews = true;
        params.sharpenFocusView = 0.0f; // No sharpening
        params.smoothenFocusViewEdges = 0.2f;
        params.ditheringAmount = 0.04f;

        void* result = compositor->compositeView(params, stereoInfo, stereoView, focusInfo, focusView);
        EXPECT_NE(result, nullptr);
    }

    // Instantiate for both D3D11 and D3D12
    INSTANTIATE_TEST_SUITE_P(GraphicsApis, CompositorWarpTest, ::testing::Values(false, true));

} // namespace openxr_api_layer
