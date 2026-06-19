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
#include "d3d12_compositor.h"

#include "log.h"
#include "util.h"
#include "views.h"
#include "framework/dispatch.gen.h"
#include <utils/graphics.h>

#define A_CPU
#include <ffx_a.h>
#include <ffx_cas.h>

#include <ProjectionVS.h>
#include <ProjectionPS.h>
#include <SharpeningCS.h>

namespace openxr_api_layer {

    using namespace log;
    using namespace xr::math;

    // Anonymous namespace to avoid ODR violations with FFX-CAS inline functions
    namespace {
        // Constant buffer structs (shared between D3D11 and D3D12 compositors)
        struct ProjectionVSConstants {
        alignas(16) DirectX::XMFLOAT4X4 focusProjection;
    };

    struct ProjectionPSConstants {
        alignas(4) float smoothingArea;
        alignas(4) bool ignoreAlpha;
        alignas(4) bool isUnpremultipliedAlpha;
        alignas(4) bool debugFocusView;
        alignas(4) float sharpenFocusView;
        alignas(4) float _padding[2]; // Match D3D11 struct layout
    };

    struct SharpeningCSConstants {
        alignas(4) uint32_t Const0[4];
        alignas(4) uint32_t Const1[4];
    };

    // Format used for the sharpened (CAS output) texture.
    // R32G32B32A32_FLOAT matches the CAS shader's RWTexture2D<float4> declaration.
    // R16G16B16A16_FLOAT caused GPU device-removed crashes on some drivers when the CAS
    // shader writes float4 values that exceed the 16-bit range (HDR content).
    constexpr DXGI_FORMAT kSharpenedFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

    } // end anonymous namespace

    D3D12Compositor::D3D12Compositor(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi)
        : m_device(device)
        , m_queue(queue)
        , m_openXrApi(openXrApi) {
        // ComPtr's initializing constructor already AddRefs the device/queue; no explicit AddRef needed.
    }

    bool D3D12Compositor::isInitialized() const {
        return m_projectionPSO != nullptr;
    }

    void D3D12Compositor::populateSwapchainImagesCache(D3D12SwapchainGraphicsState& state, XrSwapchain swapchain, bool isFullFov) {
        auto& images = isFullFov ? state.fullFovSwapchainImages : state.images;
        if (!images.empty()) {
            return;
        }

        uint32_t count;
        CHECK_XRCMD(m_openXrApi->xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr));
        LogDebug("  D3D12: Populating swapchain images cache: swapchain={:x}, count={}\n", (uint64_t)swapchain, count);

        std::vector<XrSwapchainImageD3D12KHR> d3d12Images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        CHECK_XRCMD(m_openXrApi->xrEnumerateSwapchainImages(
            swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(d3d12Images.data())));

        for (uint32_t i = 0; i < count; i++) {
            LogDebug("    Image {}: texture={:p}\n", i, static_cast<void*>(d3d12Images[i].texture));
            images.push_back(d3d12Images[i].texture);
        }
    }

    bool D3D12Compositor::initialize(int32_t swapchainFormat) {
        TraceLoggingWrite(g_traceProvider, "InitializeCompositionResources", TLArg("D3D12", "Api"));
        LogDebug("D3D12 initializeCompositionResources: starting... (format={})\n", swapchainFormat);

        auto device = m_device.Get();

        // Create descriptor heaps.
        // CBV/SRV heap layout (16 descriptors):
        //   0-1: Left eye VS_CBV + PS_CBV (created at runtime per frame)
        //   2-3: Right eye VS_CBV + PS_CBV (created at runtime per frame)
        //   4-5: Left eye SRVs (stereo + focus textures)
        //   8-9: Right eye SRVs (stereo + focus textures)
        //   10-11: Sharpening per-eye SRV/UAV
        //   12: Sharpening CS CBV (static)
        for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.NumDescriptors = 16;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            LogDebug("D3D12: Creating CBV/SRV descriptor heap {}...\n", i);
            CHECK_HRCMD(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_cbvSrvHeap[i].ReleaseAndGetAddressOf())));
            LogDebug("D3D12: CBV/SRV descriptor heap {} created\n", i);
        }
        m_cbvSrvHeapFrameIndex = 0;

        // Sampler heap
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.NumDescriptors = 2;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            LogDebug("D3D12: Creating sampler descriptor heap...\n");
            CHECK_HRCMD(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_samplerHeap.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: Sampler heap created\n");
        }

        // RTV heap
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.NumDescriptors = 1;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            LogDebug("D3D12: Creating RTV descriptor heap...\n");
            CHECK_HRCMD(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_rtvHeap.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: RTV heap created\n");
        }

        // Create sampler descriptor
        {
            D3D12_SAMPLER_DESC samplerDesc{};
            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplerDesc.MipLODBias = 0;
            samplerDesc.MaxAnisotropy = 1;
            samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            samplerDesc.BorderColor[0] = 0;
            samplerDesc.BorderColor[1] = 0;
            samplerDesc.BorderColor[2] = 0;
            samplerDesc.BorderColor[3] = 0;
            samplerDesc.MinLOD = 0;
            samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
            device->CreateSampler(&samplerDesc, m_samplerHeap->GetCPUDescriptorHandleForHeapStart());
            LogDebug("D3D12: Sampler descriptor created\n");
        }

        // Create upload heaps for constant buffers
        {
            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heapProps.CreationNodeMask = heapProps.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Alignment = 0;
            desc.Width = 512;  // 2 eyes x 256-byte CBV stride each
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;

            CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        nullptr,
                                                        IID_PPV_ARGS(m_projectionVSConstants.ReleaseAndGetAddressOf())));
            CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        nullptr,
                                                        IID_PPV_ARGS(m_projectionPSConstants.ReleaseAndGetAddressOf())));
            CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        nullptr,
                                                        IID_PPV_ARGS(m_sharpeningCSConstants.ReleaseAndGetAddressOf())));
        }

        // Create static CBV descriptor for sharpening CS
        {
            const auto incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            for (uint32_t h = 0; h < xr::StereoView::Count; h++) {
                CD3DX12_CPU_DESCRIPTOR_HANDLE cbvHandle(m_cbvSrvHeap[h]->GetCPUDescriptorHandleForHeapStart());
                cbvHandle.Offset(12 * incrementSize);
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                cbvDesc.BufferLocation = m_sharpeningCSConstants->GetGPUVirtualAddress();
                cbvDesc.SizeInBytes = 256;
                device->CreateConstantBufferView(&cbvDesc, cbvHandle);
            }
        }

        // Create blank texture
        {
            const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
            const uint32_t width = 32;
            const uint32_t height = 32;

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            heapProps.CreationNodeMask = heapProps.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, D3D12_RESOURCE_FLAG_NONE);

            HRESULT hr = device->CreateCommittedResource(&heapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &desc,
                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                        nullptr,
                                                        IID_PPV_ARGS(m_blankTexture.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                LogDebug("  Blank texture creation failed with HRESULT=0x{:08X}\n", hr);
            } else {
                LogDebug("D3D12: Blank texture created successfully\n");
            }
        }

        // Create blank texture SRV
        {
            const auto incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;

            for (uint32_t h = 0; h < xr::StereoView::Count; h++) {
                CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_cbvSrvHeap[h]->GetCPUDescriptorHandleForHeapStart());
                srvHandle.Offset(3 * incrementSize);
                device->CreateShaderResourceView(m_blankTexture.Get(), &srvDesc, srvHandle);
            }
            LogDebug("D3D12: Blank texture SRV created\n");
        }

        // Create command allocators
        for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
            LogDebug("D3D12: Creating command allocator {}...\n", i);
            CHECK_HRCMD(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(m_compositionAllocator[i].ReleaseAndGetAddressOf())));
            LogDebug("D3D12: Command allocator {} created\n", i);
        }

        // Create fence
        CHECK_HRCMD(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_compositionFence.ReleaseAndGetAddressOf())));
        m_fenceValue = 0;
        LogDebug("D3D12: Composition fence created\n");

        // Create projection PSO
        {
            LogDebug("D3D12: Creating projection PSO...\n");

            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
            psoDesc.VS = {g_ProjectionVS, sizeof(g_ProjectionVS)};
            psoDesc.PS = {g_ProjectionPS, sizeof(g_ProjectionPS)};
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
            psoDesc.RasterizerState.DepthClipEnable = TRUE;
            psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
            psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0x0F;
            psoDesc.RTVFormats[0] = (DXGI_FORMAT)swapchainFormat;
            psoDesc.SampleMask = UINT_MAX;
            psoDesc.NumRenderTargets = 1;
            psoDesc.SampleDesc.Count = 1;

            // Root signature
            CD3DX12_DESCRIPTOR_RANGE cbvRangeVS(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE cbvRangePS(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE samplerRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);

            CD3DX12_ROOT_PARAMETER rootParams[4];
            rootParams[0].InitAsDescriptorTable(1, &cbvRangeVS, D3D12_SHADER_VISIBILITY_VERTEX);
            rootParams[1].InitAsDescriptorTable(1, &cbvRangePS, D3D12_SHADER_VISIBILITY_PIXEL);
            rootParams[2].InitAsDescriptorTable(1, &samplerRange, D3D12_SHADER_VISIBILITY_PIXEL);
            rootParams[3].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

            D3D12_ROOT_SIGNATURE_DESC rsDesc{};
            rsDesc.NumParameters = 4;
            rsDesc.pParameters = rootParams;
            rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ComPtr<ID3DBlob> signature, error;
            HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), error.GetAddressOf());
            if (FAILED(hr)) {
                LogDebug("  Root signature serialization failed: 0x{:08X}\n", hr);
                if (error) {
                    LogDebug("  Error: {}\n", std::string(static_cast<char*>(error->GetBufferPointer()), error->GetBufferSize()));
                }
            }
            hr = device->CreateRootSignature(0,
                                             signature->GetBufferPointer(),
                                             signature->GetBufferSize(),
                                             IID_PPV_ARGS(m_projectionRootSignature.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                LogDebug("  Root signature creation failed: 0x{:08X}\n", hr);
            }

            psoDesc.pRootSignature = m_projectionRootSignature.Get();
            hr = device->CreateGraphicsPipelineState(&psoDesc,
                                                     IID_PPV_ARGS(m_projectionPSO.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                LogDebug("  PSO creation failed with HRESULT=0x{:08X}\n", hr);
            } else {
                LogDebug("D3D12: Projection PSO created\n");
            }
        }

        // Create sharpening PSO
        {
            LogDebug("D3D12: Creating sharpening PSO...\n");
            CD3DX12_DESCRIPTOR_RANGE cbvRange(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            CD3DX12_ROOT_PARAMETER rootParams[3];
            rootParams[0].InitAsDescriptorTable(1, &cbvRange, D3D12_SHADER_VISIBILITY_ALL);
            rootParams[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
            rootParams[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

            D3D12_ROOT_SIGNATURE_DESC rsDesc{};
            rsDesc.NumParameters = 3;
            rsDesc.pParameters = rootParams;
            ComPtr<ID3DBlob> signature, error;
            HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), error.GetAddressOf());
            if (FAILED(hr)) {
                LogDebug("  Sharpening root signature serialization failed: 0x{:08X}\n", hr);
            }
            hr = device->CreateRootSignature(0,
                                             signature->GetBufferPointer(),
                                             signature->GetBufferSize(),
                                             IID_PPV_ARGS(m_sharpeningRootSignature.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                LogDebug("  Sharpening root signature creation failed: 0x{:08X}\n", hr);
            }

            D3D12_COMPUTE_PIPELINE_STATE_DESC csDesc{};
            csDesc.CS = {g_SharpeningCS, sizeof(g_SharpeningCS)};
            csDesc.pRootSignature = m_sharpeningRootSignature.Get();
            csDesc.NodeMask = 0;
            hr = device->CreateComputePipelineState(&csDesc,
                                                    IID_PPV_ARGS(m_sharpeningPSO.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                LogDebug("  Sharpening PSO creation failed with HRESULT=0x{:08X}\n", hr);
            } else {
                LogDebug("D3D12: Sharpening PSO created\n");
            }
        }

        LogDebug("D3D12: Composition resources initialized successfully!\n");
        return true;
    }

    void* D3D12Compositor::compositeView(const CompositorParams& params,
                                          const SwapchainInfo& stereoSwapchain,
                                          const XrCompositionLayerProjectionView& stereoView,
                                          const SwapchainInfo& focusSwapchain,
                                          const XrCompositionLayerProjectionView& focusView) {
        auto device = m_device.Get();
        auto queue = m_queue.Get();
        const uint32_t viewIndex = params.viewIndex;

        LogDebug("D3D12 compositeView: viewIndex={}, device={:p}, queue={:p}\n",
                        viewIndex, static_cast<void*>(device), static_cast<void*>(queue));

        // Get or create swapchain graphics state
        auto& stereoState = m_swapchainStates[stereoSwapchain.handle];
        auto& focusState = m_swapchainStates[focusSwapchain.handle];

        // Populate swapchain images cache
        populateSwapchainImagesCache(stereoState, stereoSwapchain.handle, false);
        populateSwapchainImagesCache(focusState, focusSwapchain.handle, false);

        // Grab input/output textures
        ID3D12Resource* sourceImage = nullptr;
        ID3D12Resource* sourceFocusImage = nullptr;
        ID3D12Resource* destinationImage = nullptr;
        {
            if (params.useQuadViews) {
                sourceImage = stereoState.images[stereoSwapchain.lastReleasedIndex];
                LogDebug("  sourceImage={:p}, lastReleasedIndex={}\n",
                    static_cast<void*>(sourceImage), stereoSwapchain.lastReleasedIndex);
            }
            sourceFocusImage = focusState.images[focusSwapchain.lastReleasedIndex];
            LogDebug("  sourceFocusImage={:p}, lastReleasedIndex={}\n",
                static_cast<void*>(sourceFocusImage), focusSwapchain.lastReleasedIndex);

            // Acquire/release full FOV swapchain image.
            // Call base class virtual method directly to bypass layer's deferred release quirk,
            // because the full FOV swapchain is not tracked in m_swapchains.
            if (viewIndex == 0) {
                // Release previous swapchain image first (if any) to avoid call order errors.
                // The previous image may not have been acquired yet (first frame), in which case
                // the runtime returns XR_ERROR_CALL_ORDER_INVALID — this is expected and harmless.
                m_openXrApi->OpenXrApi::xrReleaseSwapchainImage(stereoSwapchain.fullFovSwapchain, nullptr);

                uint32_t acquiredImageIndex;
                CHECK_XRCMD(m_openXrApi->OpenXrApi::xrAcquireSwapchainImage(stereoSwapchain.fullFovSwapchain, nullptr, &acquiredImageIndex));
                XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                waitInfo.timeout = 10000000000;
                CHECK_XRCMD(m_openXrApi->OpenXrApi::xrWaitSwapchainImage(stereoSwapchain.fullFovSwapchain, &waitInfo));

                stereoState.acquiredFullFovImageIndex = acquiredImageIndex;
                LogDebug("  Acquired full-FOV swapchain image index={}\n", acquiredImageIndex);
            }

            populateSwapchainImagesCache(stereoState, stereoSwapchain.fullFovSwapchain, true);
            destinationImage = stereoState.fullFovSwapchainImages[stereoState.acquiredFullFovImageIndex];
            LogDebug("  destinationImage={:p}, acquiredIndex={}\n", static_cast<void*>(destinationImage),
                            stereoState.acquiredFullFovImageIndex);
        }

        // Timer (optional profiling - disabled in refactored compositor)

        // Wait for GPU (view 0 only)
        if (viewIndex == 0 && m_compositionFence) {
            UINT64 fenceValueToWait = m_fenceValue;
            if (m_compositionFence->GetCompletedValue() < fenceValueToWait) {
                HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                m_compositionFence->SetEventOnCompletion(fenceValueToWait, event);
                WaitForSingleObject(event, INFINITE);
                CloseHandle(event);
            }
        }

        // Reset command allocator and create a fresh command list for this eye.
        //
        // We intentionally create a new ID3D12GraphicsCommandList every frame rather than caching
        // and Reset()-ing one. A cached list that ever fails Close() (e.g. due to an invalid
        // recorded command) is left in the open/recording state, after which Reset() permanently
        // returns E_INVALIDARG — the compositor can never recover and every subsequent frame is
        // blank. A fresh list per frame is slightly more expensive but is self-healing: each frame
        // starts from a known-good, implicitly-opened list. See quad-views-improvement-plan §4.2.
        CHECK_HRCMD(m_compositionAllocator[viewIndex]->Reset());
        ComPtr<ID3D12GraphicsCommandList> cmdList;
        CHECK_HRCMD(device->CreateCommandList(0,
                                              D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              m_compositionAllocator[viewIndex].Get(),
                                              nullptr,
                                              IID_PPV_ARGS(cmdList.ReleaseAndGetAddressOf())));

        // Lambda: flatten source image
        const auto flattenSourceImage = [&](ID3D12Resource* image,
                                            const XrCompositionLayerProjectionView& view,
                                            const SwapchainInfo& swapchainInfo,
                                            D3D12SwapchainGraphicsState& state,
                                            uint32_t startSlot) {
            // Ensure flat image exists
            if (!state.flatImage[startSlot + viewIndex] ||
                state.flatImage[startSlot + viewIndex]->GetDesc().Width !=
                    (UINT64)view.subImage.imageRect.extent.width ||
                state.flatImage[startSlot + viewIndex]->GetDesc().Height !=
                    (UINT64)view.subImage.imageRect.extent.height) {
                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
                heapProps.CreationNodeMask = heapProps.VisibleNodeMask = 1;

                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Alignment = 0;
                desc.Width = view.subImage.imageRect.extent.width;
                desc.Height = view.subImage.imageRect.extent.height;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.Format = (DXGI_FORMAT)swapchainInfo.createInfo.format;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                desc.Flags = D3D12_RESOURCE_FLAG_NONE;

                CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &desc,
                                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                            nullptr,
                                                            IID_PPV_ARGS(state.flatImage[startSlot + viewIndex].ReleaseAndGetAddressOf())));
            }

            // Barriers: source RT->COPY_SOURCE, dest PSR->COPY_DEST.
            //
            // The source is the application's swapchain image, which the app leaves in the
            // RENDER_TARGET state after drawing. We transition RENDER_TARGET -> COPY_SOURCE here
            // and restore to RENDER_TARGET after the copy. This matches the proven-working original
            // behavior. (The improvement plan §4.1 suggested transitioning from COMMON based on the
            // OpenXR spec, but SteamVR's D3D11-on-D3D12 runtime does not hand over images in COMMON
            // state, so a COMMON -> COPY_SOURCE barrier is an invalid transition that causes
            // cmdList->Close() to return E_INVALIDARG and blanks the headset.)
            D3D12_RESOURCE_BARRIER barriers[2];
            barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(image,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                               D3D12_RESOURCE_STATE_COPY_SOURCE);
            barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(state.flatImage[startSlot + viewIndex].Get(),
                                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                               D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->ResourceBarrier(2, barriers);

            // Copy subresource region
            D3D12_TEXTURE_COPY_LOCATION srcLocation{};
            srcLocation.pResource = image;
            srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcLocation.SubresourceIndex = view.subImage.imageArrayIndex;

            D3D12_TEXTURE_COPY_LOCATION dstLocation{};
            dstLocation.pResource = state.flatImage[startSlot + viewIndex].Get();
            dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLocation.SubresourceIndex = 0;

            D3D12_BOX srcBox{};
            srcBox.left = view.subImage.imageRect.offset.x;
            srcBox.top = view.subImage.imageRect.offset.y;
            srcBox.front = 0;
            srcBox.right = srcBox.left + view.subImage.imageRect.extent.width;
            srcBox.bottom = srcBox.top + view.subImage.imageRect.extent.height;
            srcBox.back = 1;

            cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, &srcBox);

            // Restore: source COPY_SOURCE->RENDER_TARGET, dest COPY_DEST->PSR
            barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(image,
                                                                D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                D3D12_RESOURCE_STATE_RENDER_TARGET);
            barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(state.flatImage[startSlot + viewIndex].Get(),
                                                                D3D12_RESOURCE_STATE_COPY_DEST,
                                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(2, barriers);
        };

        // Flatten images
        if (params.useQuadViews) {
            flattenSourceImage(sourceImage, stereoView, stereoSwapchain, stereoState, 0);
        }
        flattenSourceImage(sourceFocusImage, focusView, focusSwapchain, focusState, xr::StereoView::Count);

        // Sharpen if needed
        if (params.sharpenFocusView) {
            bool isSharpenedImageNew = false;
            if (!focusState.sharpenedImage[viewIndex] ||
                focusState.sharpenedImage[viewIndex]->GetDesc().Width !=
                    (UINT64)focusView.subImage.imageRect.extent.width ||
                focusState.sharpenedImage[viewIndex]->GetDesc().Height !=
                    (UINT64)focusView.subImage.imageRect.extent.height ||
                focusState.sharpenedImage[viewIndex]->GetDesc().Format !=
                    kSharpenedFormat) {
                isSharpenedImageNew = true;
                D3D12_HEAP_PROPERTIES heapProps{};
                heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
                heapProps.CreationNodeMask = heapProps.VisibleNodeMask = 1;

                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Alignment = 0;
                desc.Width = focusView.subImage.imageRect.extent.width;
                desc.Height = focusView.subImage.imageRect.extent.height;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                // Match the D3D11 compositor's R16G16B16A16_FLOAT to halve memory/bandwidth vs
                // R32G32B32A32_FLOAT while preserving plenty of precision for a sharpened HDR image.
                desc.Format = kSharpenedFormat;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

                CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &desc,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                            nullptr,
                                                            IID_PPV_ARGS(focusState.sharpenedImage[viewIndex].ReleaseAndGetAddressOf())));
            }

            // Update constant buffer
            SharpeningCSConstants sharpening{};
            CasSetup(sharpening.Const0,
                     sharpening.Const1,
                     std::clamp(params.sharpenFocusView, 0.f, 1.f),
                     (AF1)focusView.subImage.imageRect.extent.width,
                     (AF1)focusView.subImage.imageRect.extent.height,
                     (AF1)focusView.subImage.imageRect.extent.width,
                     (AF1)focusView.subImage.imageRect.extent.height);

            void* constData;
            D3D12_RANGE range = {0, 0};
            CHECK_HRCMD(m_sharpeningCSConstants->Map(0, &range, &constData));
            memcpy(constData, &sharpening, sizeof(sharpening));
            m_sharpeningCSConstants->Unmap(0, nullptr);

            // Dispatch compute shader
            cmdList->SetPipelineState(m_sharpeningPSO.Get());
            cmdList->SetComputeRootSignature(m_sharpeningRootSignature.Get());
            const uint32_t heapIndex = (m_cbvSrvHeapFrameIndex + viewIndex) % xr::StereoView::Count;
            cmdList->SetDescriptorHeaps(1, m_cbvSrvHeap[heapIndex].GetAddressOf());

            // Create SRV/UAV descriptors
            {
                const auto incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                const uint32_t srvIndex = 8 + (viewIndex * 2);
                const uint32_t uavIndex = 9 + (viewIndex * 2);

                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                // Specify the format explicitly. The flat focus image is created with the
                // swapchain format, so use it here instead of relying on DXGI_FORMAT_UNKNOWN
                // (which depends on the runtime inferring the format and may not work on all drivers).
                srvDesc.Format = (DXGI_FORMAT)focusSwapchain.createInfo.format;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;

                D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                uavDesc.Format = kSharpenedFormat;
                uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

                CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_cbvSrvHeap[heapIndex]->GetCPUDescriptorHandleForHeapStart());
                srvHandle.Offset(srvIndex * incrementSize);
                device->CreateShaderResourceView(focusState.flatImage[xr::StereoView::Count + viewIndex].Get(), &srvDesc, srvHandle);

                CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle(m_cbvSrvHeap[heapIndex]->GetCPUDescriptorHandleForHeapStart());
                uavHandle.Offset(uavIndex * incrementSize);
                device->CreateUnorderedAccessView(focusState.sharpenedImage[viewIndex].Get(), nullptr, &uavDesc, uavHandle);
            }

            // Bind root descriptors
            CD3DX12_GPU_DESCRIPTOR_HANDLE csCbvHandle(m_cbvSrvHeap[heapIndex]->GetGPUDescriptorHandleForHeapStart());
            csCbvHandle.Offset(12 * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
            cmdList->SetComputeRootDescriptorTable(0, csCbvHandle);

            CD3DX12_GPU_DESCRIPTOR_HANDLE csSrvHandle(m_cbvSrvHeap[heapIndex]->GetGPUDescriptorHandleForHeapStart());
            csSrvHandle.Offset((8 + (viewIndex * 2)) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
            cmdList->SetComputeRootDescriptorTable(1, csSrvHandle);

            CD3DX12_GPU_DESCRIPTOR_HANDLE csUavHandle(m_cbvSrvHeap[heapIndex]->GetGPUDescriptorHandleForHeapStart());
            csUavHandle.Offset((9 + (viewIndex * 2)) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
            cmdList->SetComputeRootDescriptorTable(2, csUavHandle);

            // Barriers for compute
            D3D12_RESOURCE_STATES sharpenedBeginState = isSharpenedImageNew
                ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            D3D12_RESOURCE_BARRIER barriersCsIn[2];
            barriersCsIn[0] = CD3DX12_RESOURCE_BARRIER::Transition(
                focusState.flatImage[xr::StereoView::Count + viewIndex].Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barriersCsIn[1] = CD3DX12_RESOURCE_BARRIER::Transition(
                focusState.sharpenedImage[viewIndex].Get(),
                sharpenedBeginState,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmdList->ResourceBarrier(2, barriersCsIn);

            static const int threadGroupWorkRegionDim = 16;
            int dispatchX = (focusView.subImage.imageRect.extent.width + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            int dispatchY = (focusView.subImage.imageRect.extent.height + (threadGroupWorkRegionDim - 1)) / threadGroupWorkRegionDim;
            cmdList->Dispatch(dispatchX, dispatchY, 1);

            // Restore barriers
            D3D12_RESOURCE_BARRIER barriersCsOut[2];
            barriersCsOut[0] = CD3DX12_RESOURCE_BARRIER::Transition(
                focusState.flatImage[xr::StereoView::Count + viewIndex].Get(),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barriersCsOut[1] = CD3DX12_RESOURCE_BARRIER::Transition(
                focusState.sharpenedImage[viewIndex].Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(2, barriersCsOut);
        }

        // Composite - update constant buffers
        const size_t vsConstantsStride = 256;
        const size_t psConstantsStride = 256;
        const size_t vsOffset = viewIndex * vsConstantsStride;
        const size_t psOffset = viewIndex * psConstantsStride;

        ProjectionVSConstants projection;
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
        {
            void* constData;
            D3D12_RANGE range = {0, 0};
            CHECK_HRCMD(m_projectionVSConstants->Map(0, &range, &constData));
            memcpy((uint8_t*)constData + vsOffset, &projection, sizeof(projection));
            m_projectionVSConstants->Unmap(0, nullptr);
        }

        ProjectionPSConstants drawing{};
        drawing.smoothingArea = params.useQuadViews ? params.smoothenFocusViewEdges : 0;
        drawing.ignoreAlpha = ~(params.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT);
        drawing.isUnpremultipliedAlpha = params.layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        drawing.debugFocusView = params.debugFocusView;
        drawing.sharpenFocusView = params.sharpenFocusView;
        {
            void* constData;
            D3D12_RANGE range = {0, 0};
            CHECK_HRCMD(m_projectionPSConstants->Map(0, &range, &constData));
            memcpy((uint8_t*)constData + psOffset, &drawing, sizeof(drawing));
            m_projectionPSConstants->Unmap(0, nullptr);
        }

        // Create per-eye CBV descriptors
        {
            const auto incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const uint32_t vsCbvIndex = viewIndex * 2;
            const uint32_t psCbvIndex = viewIndex * 2 + 1;
            const uint32_t heapIndex = (m_cbvSrvHeapFrameIndex + viewIndex) % xr::StereoView::Count;

            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
            cbvDesc.SizeInBytes = 256;

            CD3DX12_CPU_DESCRIPTOR_HANDLE vsCbvHandle(m_cbvSrvHeap[heapIndex]->GetCPUDescriptorHandleForHeapStart());
            vsCbvHandle.Offset(vsCbvIndex * incrementSize);
            cbvDesc.BufferLocation = m_projectionVSConstants->GetGPUVirtualAddress() + vsOffset;
            device->CreateConstantBufferView(&cbvDesc, vsCbvHandle);

            CD3DX12_CPU_DESCRIPTOR_HANDLE psCbvHandle(m_cbvSrvHeap[heapIndex]->GetCPUDescriptorHandleForHeapStart());
            psCbvHandle.Offset(psCbvIndex * incrementSize);
            cbvDesc.BufferLocation = m_projectionPSConstants->GetGPUVirtualAddress() + psOffset;
            device->CreateConstantBufferView(&cbvDesc, psCbvHandle);
        }

        // Transition destination to render target
        {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                destinationImage,
                D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList->ResourceBarrier(1, &barrier);
        }

        // Set up render target
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        DXGI_FORMAT rtFormat = (DXGI_FORMAT)stereoSwapchain.createInfo.format;
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = rtFormat;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.ArraySize = 1;
        rtvDesc.Texture2DArray.FirstArraySlice = viewIndex;
        device->CreateRenderTargetView(destinationImage, &rtvDesc, rtvHandle);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.Width = (float)params.fullFovResolution.width;
        viewport.Height = (float)params.fullFovResolution.height;
        viewport.MinDepth = 0.f;
        viewport.MaxDepth = 1.f;
        cmdList->RSSetViewports(1, &viewport);

        D3D12_RECT scissor = {0, 0, (LONG)params.fullFovResolution.width, (LONG)params.fullFovResolution.height};
        cmdList->RSSetScissorRects(1, &scissor);

        // Create SRVs for source textures
        ID3D12Resource* stereoTex = nullptr;
        ID3D12Resource* focusTex = nullptr;
        {
            const auto incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const uint32_t srvBase = 4 + (viewIndex * 4);
            const uint32_t heapIndex = (m_cbvSrvHeapFrameIndex + viewIndex) % xr::StereoView::Count;
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;

            CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(m_cbvSrvHeap[heapIndex]->GetCPUDescriptorHandleForHeapStart());
            srvHandle.Offset(srvBase * incrementSize);
            // Flat images are created with the swapchain format; specify it explicitly instead
            // of DXGI_FORMAT_UNKNOWN so the format does not depend on driver inference.
            srvDesc.Format = (DXGI_FORMAT)stereoSwapchain.createInfo.format;
            if (params.useQuadViews && stereoState.flatImage[viewIndex]) {
                stereoTex = stereoState.flatImage[viewIndex].Get();
                device->CreateShaderResourceView(stereoTex, &srvDesc, srvHandle);
            } else {
                stereoTex = focusState.flatImage[xr::StereoView::Count + viewIndex].Get();
                device->CreateShaderResourceView(stereoTex, &srvDesc, srvHandle);
            }

            srvHandle.Offset(incrementSize);
            if (params.sharpenFocusView && focusState.sharpenedImage[viewIndex]) {
                focusTex = focusState.sharpenedImage[viewIndex].Get();
                // The sharpened image uses kSharpenedFormat, not the swapchain format.
                srvDesc.Format = kSharpenedFormat;
            } else if (params.useQuadViews) {
                focusTex = focusState.flatImage[xr::StereoView::Count + viewIndex].Get();
                srvDesc.Format = (DXGI_FORMAT)focusSwapchain.createInfo.format;
            } else {
                focusTex = m_blankTexture.Get();
                // The blank texture is created as B8G8R8A8_UNORM (see initialize()).
                srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            }
            device->CreateShaderResourceView(focusTex, &srvDesc, srvHandle);
        }

        // Bind and draw
        const uint32_t heapIndex = (m_cbvSrvHeapFrameIndex + viewIndex) % xr::StereoView::Count;

        cmdList->SetPipelineState(m_projectionPSO.Get());
        cmdList->SetGraphicsRootSignature(m_projectionRootSignature.Get());
        ID3D12DescriptorHeap* heaps[2] = {m_cbvSrvHeap[heapIndex].Get(), m_samplerHeap.Get()};
        cmdList->SetDescriptorHeaps(2, heaps);

        const uint32_t vsCbvIndex = viewIndex * 2;
        const uint32_t psCbvIndex = viewIndex * 2 + 1;
        const auto descInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        CD3DX12_GPU_DESCRIPTOR_HANDLE vsCbvHandle(m_cbvSrvHeap[heapIndex]->GetGPUDescriptorHandleForHeapStart());
        vsCbvHandle.Offset(vsCbvIndex * descInc);
        cmdList->SetGraphicsRootDescriptorTable(0, vsCbvHandle);

        CD3DX12_GPU_DESCRIPTOR_HANDLE psCbvHandle(m_cbvSrvHeap[heapIndex]->GetGPUDescriptorHandleForHeapStart());
        psCbvHandle.Offset(psCbvIndex * descInc);
        cmdList->SetGraphicsRootDescriptorTable(1, psCbvHandle);

        cmdList->SetGraphicsRootDescriptorTable(2, m_samplerHeap->GetGPUDescriptorHandleForHeapStart());

        const uint32_t srvBase = 4 + (viewIndex * 4);
        CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(m_cbvSrvHeap[heapIndex]->GetGPUDescriptorHandleForHeapStart());
        srvHandle.Offset(srvBase * descInc);
        cmdList->SetGraphicsRootDescriptorTable(3, srvHandle);

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmdList->DrawInstanced(3, 1, 0, 0);

        // Transition destination to present
        {
            D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                destinationImage,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT);
            cmdList->ResourceBarrier(1, &barrier);
        }

        CHECK_HRCMD(cmdList->Close());
        queue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList* const*>(cmdList.GetAddressOf()));

        // Timer stop (optional profiling - disabled in refactored compositor)

        // Signal fence and wait (view 1 only)
        if (viewIndex == xr::StereoView::Right) {
            m_fenceValue++;
            queue->Signal(m_compositionFence.Get(), m_fenceValue);
            m_cbvSrvHeapFrameIndex++;

            if (m_compositionFence->GetCompletedValue() < m_fenceValue) {
                HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                m_compositionFence->SetEventOnCompletion(m_fenceValue, event);
                WaitForSingleObject(event, INFINITE);
                CloseHandle(event);
            }
            LogDebug("  D3D12 composition complete (fence={})\n", m_fenceValue);

            // Call base class virtual method directly to bypass layer's deferred release quirk
            const XrResult releaseResult =
                m_openXrApi->OpenXrApi::xrReleaseSwapchainImage(stereoSwapchain.fullFovSwapchain, nullptr);
            if (XR_FAILED(releaseResult)) {
                LogWarning("D3D12: xrReleaseSwapchainImage (full FOV) failed: {}\n", xr::ToCString(releaseResult));
            }
        }

        return destinationImage;
    }

    void D3D12Compositor::destroy() {
        // Idempotent: safe to call multiple times (ComPtr::Reset() is a no-op on already-null pointers).
        m_swapchainStates.clear();
        m_cbvSrvHeap[0].Reset();
        m_cbvSrvHeap[1].Reset();
        m_rtvHeap.Reset();
        m_samplerHeap.Reset();
        m_projectionPSO.Reset();
        m_sharpeningPSO.Reset();
        m_projectionRootSignature.Reset();
        m_sharpeningRootSignature.Reset();
        m_projectionVSConstants.Reset();
        m_projectionPSConstants.Reset();
        m_sharpeningCSConstants.Reset();
        m_blankTexture.Reset();
        m_compositionAllocator[0].Reset();
        m_compositionAllocator[1].Reset();
        m_compositionFence.Reset();
    }

    std::unique_ptr<ICompositor> createD3D12Compositor(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi) {
        return std::make_unique<D3D12Compositor>(device, queue, openXrApi);
    }

} // namespace openxr_api_layer
