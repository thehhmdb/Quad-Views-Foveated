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
#include "logic/compositor_shared.h"
#include "utils/d3d12_helpers.h"

#include <ProjectionVS.h>
#include <ProjectionPS.h>
#include <SharpeningCS.h>

namespace openxr_api_layer {

    using namespace log;
    using namespace xr::math;

    // Format used for the sharpened (CAS output) texture.
    // R32G32B32A32_FLOAT matches the CAS shader's RWTexture2D<float4> declaration.
    // R16G16B16A16_FLOAT caused GPU device-removed crashes on some drivers when the CAS
    // shader writes float4 values that exceed the 16-bit range (HDR content).
    constexpr DXGI_FORMAT kSharpenedFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;

    D3D12Compositor::D3D12Compositor(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi)
        : BaseCompositor(openXrApi)
        , m_device(device)
        , m_queue(queue) {
        // ComPtr's initializing constructor already AddRefs the device/queue; no explicit AddRef needed.
    }

    bool D3D12Compositor::isInitialized() const {
        return m_initialized;
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
        QVF_TRACE("InitializeCompositionResources", TLArg("D3D12", "Api"));
        LogDebug("D3D12 initializeCompositionResources: starting... (format={})\n", swapchainFormat);

        auto device = m_device.Get();

        // Enable D3D12 debug layer in debug builds for validation
#ifdef _DEBUG
        {
            ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
                debugController->EnableDebugLayer();
                LogDebug("D3D12: Debug layer enabled\n");
            }
        }
#endif

        // Create descriptor heaps - triple buffered for 3-frame pipelining (Item 9)
        // CBV/SRV heap layout (16 descriptors): see DescriptorLayout for slot assignments.
        for (uint32_t f = 0; f < kFrameCount; f++) {
            for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
                D3D12_DESCRIPTOR_HEAP_DESC desc{};
                desc.NumDescriptors = DescriptorLayout::kHeapSize;
                desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                LogDebug("D3D12: Creating CBV/SRV descriptor heap [{}][{}]...\n", f, i);
                CHECK_HRCMD(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_cbvSrvHeap[f][i].ReleaseAndGetAddressOf())));
            }
        }
        m_currentFrameIndex = 0;

        // CPU descriptor heap (non-shader-visible cache to avoid per-frame CreateShaderResourceView overhead)
        {
            D3D12_DESCRIPTOR_HEAP_DESC cpuHeapDesc{};
            cpuHeapDesc.NumDescriptors = 1024;
            cpuHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            cpuHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            CHECK_HRCMD(device->CreateDescriptorHeap(&cpuHeapDesc,
                IID_PPV_ARGS(m_cpuCbvSrvHeap.ReleaseAndGetAddressOf())));
        }

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

        // RTV heap (8 descriptors for per-image caching: 3 swapchain images x 2 eyes = 6, rounded up)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.NumDescriptors = 8;
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

        // Create upload heaps for constant buffers - triple buffered (Item 9)
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

            for (uint32_t f = 0; f < kFrameCount; f++) {
                CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &desc,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr,
                                                            IID_PPV_ARGS(m_projectionVSConstants[f].ReleaseAndGetAddressOf())));
                CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &desc,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr,
                                                            IID_PPV_ARGS(m_projectionPSConstants[f].ReleaseAndGetAddressOf())));
                CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &desc,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr,
                                                            IID_PPV_ARGS(m_sharpeningCSConstants[f].ReleaseAndGetAddressOf())));
            }

            // FIX (Item 10): Persistently map the constant buffer upload heaps
            // and initialize typed constant buffer writers.
            D3D12_RANGE readRange{0, 0};
            for (uint32_t f = 0; f < kFrameCount; f++) {
                uint8_t* vsMapped = nullptr;
                uint8_t* psMapped = nullptr;
                CHECK_HRCMD(m_projectionVSConstants[f]->Map(0, &readRange, reinterpret_cast<void**>(&vsMapped)));
                CHECK_HRCMD(m_projectionPSConstants[f]->Map(0, &readRange, reinterpret_cast<void**>(&psMapped)));
                m_vsConstantWriters[f].initialize(vsMapped);
                m_psConstantWriters[f].initialize(psMapped);
            }

            // FIX (Item 9): Pre-create per-eye VS/PS CBV descriptors and sharpening CS CBV
            const auto cbvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            for (uint32_t f = 0; f < kFrameCount; f++) {
                for (uint32_t v = 0; v < xr::StereoView::Count; v++) {
                    const size_t vsOffset = v * 256;
                    const size_t psOffset = v * 256;

                    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                    cbvDesc.SizeInBytes = 256;

                    // VS CBV
                    cbvDesc.BufferLocation = m_projectionVSConstants[f]->GetGPUVirtualAddress() + vsOffset;
                    device->CreateConstantBufferView(&cbvDesc,
                        DescriptorLayout::CpuHandle(m_cbvSrvHeap[f][v].Get(), DescriptorLayout::VsCbv(v), cbvInc));

                    // PS CBV
                    cbvDesc.BufferLocation = m_projectionPSConstants[f]->GetGPUVirtualAddress() + psOffset;
                    device->CreateConstantBufferView(&cbvDesc,
                        DescriptorLayout::CpuHandle(m_cbvSrvHeap[f][v].Get(), DescriptorLayout::PsCbv(v), cbvInc));

                    // Sharpening CS CBV (static per heap)
                    cbvDesc.BufferLocation = m_sharpeningCSConstants[f]->GetGPUVirtualAddress();
                    device->CreateConstantBufferView(&cbvDesc,
                        DescriptorLayout::CpuHandle(m_cbvSrvHeap[f][v].Get(), DescriptorLayout::kSharpenCbv, cbvInc));
                }
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

            CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &desc,
                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                        nullptr,
                                                        IID_PPV_ARGS(m_blankTexture.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: Blank texture created successfully\n");
        }

        // Create blank texture SRV at DescriptorLayout::kBlankSrv slot
        {
            const auto incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;

            for (uint32_t f = 0; f < kFrameCount; f++) {
                for (uint32_t h = 0; h < xr::StereoView::Count; h++) {
                    device->CreateShaderResourceView(m_blankTexture.Get(), &srvDesc,
                        DescriptorLayout::CpuHandle(m_cbvSrvHeap[f][h].Get(), DescriptorLayout::kBlankSrv, incrementSize));
                }
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
            CHECK_HRCMD(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), error.GetAddressOf()));
            CHECK_HRCMD(device->CreateRootSignature(0,
                                             signature->GetBufferPointer(),
                                             signature->GetBufferSize(),
                                             IID_PPV_ARGS(m_projectionRootSignature.ReleaseAndGetAddressOf())));

            psoDesc.pRootSignature = m_projectionRootSignature.Get();
            CHECK_HRCMD(device->CreateGraphicsPipelineState(&psoDesc,
                                                     IID_PPV_ARGS(m_projectionPSO.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: Projection PSO created\n");
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
            CHECK_HRCMD(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), error.GetAddressOf()));
            CHECK_HRCMD(device->CreateRootSignature(0,
                                             signature->GetBufferPointer(),
                                             signature->GetBufferSize(),
                                             IID_PPV_ARGS(m_sharpeningRootSignature.ReleaseAndGetAddressOf())));

            D3D12_COMPUTE_PIPELINE_STATE_DESC csDesc{};
            csDesc.CS = {g_SharpeningCS, sizeof(g_SharpeningCS)};
            csDesc.pRootSignature = m_sharpeningRootSignature.Get();
            csDesc.NodeMask = 0;
            CHECK_HRCMD(device->CreateComputePipelineState(&csDesc,
                                                    IID_PPV_ARGS(m_sharpeningPSO.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: Sharpening PSO created\n");
        }

        LogDebug("D3D12: Composition resources initialized successfully!\n");
        m_initialized = true;
        return true;
    }

    // =======================================================================
    // CRTP Hook: Stage 1 - Acquire destination image and resolve source textures
    // Also handles D3D12-specific GPU sync and command list reset
    // =======================================================================
    bool D3D12Compositor::acquireAndResolveImages(
            const CompositorParams& params,
            const SwapchainInfo& stereoSwapchain,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& stereoState,
            D3D12SwapchainGraphicsState& focusState,
            void*& outSourceStereo,
            void*& outSourceFocus,
            void*& outDestination) {
        const uint32_t viewIndex = params.viewIndex;

        // Populate swapchain images cache (called from base class for stereo/focus, but we
        // also need to populate the full-FOV cache below).
        populateSwapchainImagesCache(stereoState, stereoSwapchain.handle, false);
        populateSwapchainImagesCache(focusState, focusSwapchain.handle, false);

        if (params.useQuadViews) {
            outSourceStereo = stereoState.images[stereoSwapchain.lastReleasedIndex].Get();
#ifdef _DEBUG
            LogDebug("  sourceImage={:p}, lastReleasedIndex={}\n",
                outSourceStereo, stereoSwapchain.lastReleasedIndex);
#endif
        }
        outSourceFocus = focusState.images[focusSwapchain.lastReleasedIndex].Get();
#ifdef _DEBUG
        LogDebug("  sourceFocusImage={:p}, lastReleasedIndex={}\n",
            outSourceFocus, focusSwapchain.lastReleasedIndex);
#endif

        // Acquire/release full FOV swapchain image.
        // Call base class virtual method directly to bypass layer's deferred release quirk,
        // because the full FOV swapchain is not tracked in m_swapchains.
        if (viewIndex == 0) {
            const uint32_t idx = acquireFullFovImage(stereoSwapchain.fullFovSwapchain, stereoState);
            if (idx == UINT32_MAX) {
                return false; // acquisition failed — skip this view
            }
#ifdef _DEBUG
            LogDebug("  Acquired full-FOV swapchain index={}\n", idx);
#endif
        }

        populateSwapchainImagesCache(stereoState, stereoSwapchain.fullFovSwapchain, true);
        outDestination = stereoState.fullFovSwapchainImages[stereoState.acquiredFullFovImageIndex].Get();
#ifdef _DEBUG
        LogDebug("  destinationImage={:p}, acquiredIndex={}\n", outDestination,
                        stereoState.acquiredFullFovImageIndex);
#endif

        // D3D12-specific: GPU sync and command list reset
        waitForPreviousFrame(viewIndex);
        auto& cmdList = resetCommandList(viewIndex);
        m_currentCmdList = cmdList.Get();
        m_currentDestination = static_cast<ID3D12Resource*>(outDestination);
        m_frameIndex = m_currentFrameIndex;
        m_directBoundStereo = nullptr;
        m_directBoundFocus = nullptr;

        return true;
    }

    // =======================================================================
    // GPU sync: wait for previous frame (view 0 only)
    // =======================================================================
    void D3D12Compositor::waitForPreviousFrame(uint32_t viewIndex) {
        if (viewIndex == 0 && m_compositionFence) {
            UINT64 fenceValueToWait = m_fenceValue;
            if (m_compositionFence->GetCompletedValue() < fenceValueToWait) {
                HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                m_compositionFence->SetEventOnCompletion(fenceValueToWait, event);
                WaitForSingleObject(event, INFINITE);
                CloseHandle(event);
            }
        }
    }

    // =======================================================================
    // CRTP Hook: Bind a source image directly (no flattening needed)
    // =======================================================================
    void D3D12Compositor::BindDirectSource(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, void* sourceImage, uint32_t format) {
        ID3D12Resource* res = static_cast<ID3D12Resource*>(sourceImage);
        state.flatImage[targetSlot] = res;

        // Track which resource is directly bound, so cleanupAndRelease can restore barriers
        if (targetSlot < xr::StereoView::Count) {
            m_directBoundStereo = res;
        } else {
            m_directBoundFocus = res;
        }

        // Transition RENDER_TARGET -> PIXEL_SHADER_RESOURCE (BarrierBatch skips no-op)
        {
            utils::d3d12::BarrierBatch barriers(m_currentCmdList);
            barriers.Add(res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    // =======================================================================
    // CRTP Hook: Check if flat image needs reallocation
    // =======================================================================
    bool D3D12Compositor::NeedsFlatReallocate(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, uint32_t width, uint32_t height, uint32_t format) {
        return NeedsReallocate(state.flatImage[targetSlot].Get(), width, height, format);
    }

    // =======================================================================
    // CRTP Hook: Create flat image with correct dimensions
    // =======================================================================
    void D3D12Compositor::CreateFlatImage(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, uint32_t width, uint32_t height, uint32_t format) {
        auto device = m_device.Get();

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CreationNodeMask = heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = (DXGI_FORMAT)format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &desc,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                    nullptr,
                                                    IID_PPV_ARGS(state.flatImage[targetSlot].ReleaseAndGetAddressOf())));
    }

    // =======================================================================
    // CRTP Hook: Copy sub-image region from source to flat image
    // =======================================================================
    void D3D12Compositor::CopySubImage(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, void* sourceImage, const XrCompositionLayerProjectionView& view) {
        ID3D12Resource* src = static_cast<ID3D12Resource*>(sourceImage);
        ID3D12Resource* dst = state.flatImage[targetSlot].Get();

        // Barriers: source RT->COPY_SOURCE, dest PSR->COPY_DEST
        {
            utils::d3d12::BarrierBatch barriers(m_currentCmdList);
            barriers.Add(src, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barriers.Add(dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }

        // Copy subresource region
        D3D12_TEXTURE_COPY_LOCATION srcLocation{};
        srcLocation.pResource = src;
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = view.subImage.imageArrayIndex;

        D3D12_TEXTURE_COPY_LOCATION dstLocation{};
        dstLocation.pResource = dst;
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLocation.SubresourceIndex = 0;

        D3D12_BOX srcBox{};
        srcBox.left = view.subImage.imageRect.offset.x;
        srcBox.top = view.subImage.imageRect.offset.y;
        srcBox.front = 0;
        srcBox.right = srcBox.left + view.subImage.imageRect.extent.width;
        srcBox.bottom = srcBox.top + view.subImage.imageRect.extent.height;
        srcBox.back = 1;

        m_currentCmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, &srcBox);

        // Restore: source COPY_SOURCE->RT, dest COPY_DEST->PSR
        {
            utils::d3d12::BarrierBatch barriers(m_currentCmdList);
            barriers.Add(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            barriers.Add(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    // =======================================================================
    // CRTP Hook: Stage 3 - Run the CAS sharpening compute pass
    // =======================================================================
    void D3D12Compositor::sharpenFocusView(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& focusState) {
        auto device = m_device.Get();
        const uint32_t viewIndex = params.viewIndex;
        auto cmdList = m_currentCmdList;
        const uint32_t frameIndex = m_frameIndex;

        const uint32_t sharpWidth = (uint32_t)focusView.subImage.imageRect.extent.width;
        const uint32_t sharpHeight = (uint32_t)focusView.subImage.imageRect.extent.height;
        const uint32_t sharpFormat = (uint32_t)kSharpenedFormat;

        if (NeedsReallocate(focusState.sharpenedImage[viewIndex].Get(),
                             sharpWidth, sharpHeight, sharpFormat)) {
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

        // Update constant buffer using SharpeningPass (using per-frame resource, Item 9)
        SharpeningCSConstants sharpening{};
        SharpeningPass{}.PrepareConstants(sharpening, params.sharpenFocusView,
                                          focusView.subImage.imageRect.extent.width,
                                          focusView.subImage.imageRect.extent.height);

        void* constData;
        D3D12_RANGE range = {0, 0};
        CHECK_HRCMD(m_sharpeningCSConstants[frameIndex]->Map(0, &range, &constData));
        memcpy(constData, &sharpening, sizeof(sharpening));
        m_sharpeningCSConstants[frameIndex]->Unmap(0, nullptr);

        // Dispatch compute shader
        cmdList->SetPipelineState(m_sharpeningPSO.Get());
        cmdList->SetComputeRootSignature(m_sharpeningRootSignature.Get());
        cmdList->SetDescriptorHeaps(1, m_cbvSrvHeap[frameIndex][viewIndex].GetAddressOf());

        // --- DESCRIPTOR CACHING LOGIC ---
        // Cache SRV/UAV descriptors in a per-state CPU heap to avoid per-frame
        // CreateShaderResourceView / CreateUnorderedAccessView overhead.
        ID3D12Resource* flatFocusRes = focusState.flatImage[xr::StereoView::Count + viewIndex].Get();
        ID3D12Resource* sharpenedRes = focusState.sharpenedImage[viewIndex].Get();

        // Check if we need to (re)create the cached descriptors
        if (!focusState.srvUavCached ||
            focusState.cachedFlatFocusAddr != (uint64_t)flatFocusRes ||
            focusState.cachedSharpenedAddr != (uint64_t)sharpenedRes) {

            if (!focusState.cpuSrvUavHeap) {
                D3D12_DESCRIPTOR_HEAP_DESC cpuHeapDesc{};
                cpuHeapDesc.NumDescriptors = 2; // 1 SRV + 1 UAV
                cpuHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                cpuHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                CHECK_HRCMD(device->CreateDescriptorHeap(&cpuHeapDesc,
                    IID_PPV_ARGS(focusState.cpuSrvUavHeap.ReleaseAndGetAddressOf())));
            }

            const auto cpuIncSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            CD3DX12_CPU_DESCRIPTOR_HANDLE cpuSrvHandle(focusState.cpuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, cpuIncSize);
            CD3DX12_CPU_DESCRIPTOR_HANDLE cpuUavHandle(focusState.cpuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, cpuIncSize);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = (DXGI_FORMAT)focusSwapchain.createInfo.format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(flatFocusRes, &srvDesc, cpuSrvHandle);

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = kSharpenedFormat;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(sharpenedRes, nullptr, &uavDesc, cpuUavHandle);

            focusState.cachedFlatFocusAddr = (uint64_t)flatFocusRes;
            focusState.cachedSharpenedAddr = (uint64_t)sharpenedRes;
            focusState.srvUavCached = true;
        }

        // Copy cached descriptors to the GPU-visible heap using DescriptorLayout
        const auto gpuIncSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        CD3DX12_CPU_DESCRIPTOR_HANDLE cpuSrcSrv(focusState.cpuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, gpuIncSize);
        CD3DX12_CPU_DESCRIPTOR_HANDLE cpuSrcUav(focusState.cpuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, gpuIncSize);

        device->CopyDescriptorsSimple(1,
            DescriptorLayout::CpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kSharpenSrv, gpuIncSize),
            cpuSrcSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CopyDescriptorsSimple(1,
            DescriptorLayout::CpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kSharpenUav, gpuIncSize),
            cpuSrcUav, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Bind root descriptors using DescriptorLayout
        const auto descInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cmdList->SetComputeRootDescriptorTable(0,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kSharpenCbv, descInc));
        cmdList->SetComputeRootDescriptorTable(1,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kSharpenSrv, descInc));
        cmdList->SetComputeRootDescriptorTable(2,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kSharpenUav, descInc));

        // Barriers for compute
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            barriers.Add(focusState.flatImage[xr::StereoView::Count + viewIndex].Get(),
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barriers.Add(focusState.sharpenedImage[viewIndex].Get(),
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        // Compute dispatch dimensions using SharpeningPass
        SharpeningPass sharpeningPass;
        sharpeningPass(params, focusView, focusSwapchain);
        cmdList->Dispatch(sharpeningPass.dispatchX, sharpeningPass.dispatchY, 1);

        // Restore barriers
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            barriers.Add(focusState.flatImage[xr::StereoView::Count + viewIndex].Get(),
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barriers.Add(focusState.sharpenedImage[viewIndex].Get(),
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    // =======================================================================
    // CRTP Hook: Stage 4 - Render the projection pass
    // =======================================================================
    void D3D12Compositor::renderProjection(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& stereoSwapchain,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& stereoState,
            D3D12SwapchainGraphicsState& focusState,
            void* destination) {
        auto device = m_device.Get();
        const uint32_t viewIndex = params.viewIndex;
        auto cmdList = m_currentCmdList;
        const uint32_t frameIndex = m_frameIndex;
        ID3D12Resource* destRes = static_cast<ID3D12Resource*>(destination);

        // Update constant buffers using typed writers
        ProjectionVSConstants projection{};
        ComputeProjectionConstants(projection, params.cachedEyeFov, focusView.fov);
        projection.stereoSubRect = {0, 0, 0, 0};
        projection.focusSubRect = {0, 0, 0, 0};
        projection.stereoSwapchainSize = {0, 0};
        projection.focusSwapchainSize = {0, 0};

        ProjectionPSConstants drawing{};
        ComputePixelShaderConstants(drawing, params);
        drawing.stereoSubRect = {0, 0, 0, 0};
        drawing.focusSubRect = {0, 0, 0, 0};
        drawing.stereoSwapchainSize = {0, 0};
        drawing.focusSwapchainSize = {0, 0};
        drawing.useDirectStereoSampling = false;
        drawing.useDirectFocusSampling = false;

        m_vsConstantWriters[frameIndex].writeVS(viewIndex, projection);
        m_psConstantWriters[frameIndex].writePS(viewIndex, drawing);

        // Transition destination to render target
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            barriers.Add(destRes, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        // Set up render target using RTV cache helper
        const auto rtvHandle = getOrCreateRTV(stereoState, destRes, viewIndex,
                                              (DXGI_FORMAT)stereoSwapchain.createInfo.format);
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

        // Bind SRVs for source textures
        bindProjectionSRVs(params, stereoSwapchain, focusView, focusSwapchain,
                           stereoState, focusState, frameIndex);

        // Bind and draw
        cmdList->SetPipelineState(m_projectionPSO.Get());
        cmdList->SetGraphicsRootSignature(m_projectionRootSignature.Get());
        ID3D12DescriptorHeap* heaps[2] = {m_cbvSrvHeap[frameIndex][viewIndex].Get(), m_samplerHeap.Get()};
        cmdList->SetDescriptorHeaps(2, heaps);

        const auto descInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        cmdList->SetGraphicsRootDescriptorTable(0,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::VsCbv(viewIndex), descInc));
        cmdList->SetGraphicsRootDescriptorTable(1,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::PsCbv(viewIndex), descInc));
        cmdList->SetGraphicsRootDescriptorTable(2, m_samplerHeap->GetGPUDescriptorHandleForHeapStart());
        cmdList->SetGraphicsRootDescriptorTable(3,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kStereoSrv, descInc));

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    // =======================================================================
    // Helper: bind SRVs for the projection pass
    // =======================================================================
    void D3D12Compositor::bindProjectionSRVs(
            const CompositorParams& params,
            const SwapchainInfo& stereoSwapchain,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& stereoState,
            D3D12SwapchainGraphicsState& focusState,
            uint32_t frameIndex) {
        auto device = m_device.Get();
        const uint32_t viewIndex = params.viewIndex;

        const auto incrementSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        // Stereo SRV (slot kStereoSrv)
        ID3D12Resource* stereoTex = nullptr;
        srvDesc.Format = (DXGI_FORMAT)stereoSwapchain.createInfo.format;
        if (params.useQuadViews && stereoState.flatImage[viewIndex]) {
            stereoTex = stereoState.flatImage[viewIndex].Get();
        } else {
            stereoTex = focusState.flatImage[xr::StereoView::Count + viewIndex].Get();
        }
        device->CreateShaderResourceView(stereoTex, &srvDesc,
            DescriptorLayout::CpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kStereoSrv, incrementSize));

        // Focus SRV (slot kFocusSrv)
        srvDesc.Format = (DXGI_FORMAT)focusSwapchain.createInfo.format;
        ID3D12Resource* focusTex = nullptr;
        if (params.sharpenFocusView && focusState.sharpenedImage[viewIndex]) {
            focusTex = focusState.sharpenedImage[viewIndex].Get();
            srvDesc.Format = kSharpenedFormat;
        } else if (params.useQuadViews) {
            focusTex = focusState.flatImage[xr::StereoView::Count + viewIndex].Get();
        } else {
            focusTex = m_blankTexture.Get();
            srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        device->CreateShaderResourceView(focusTex, &srvDesc,
            DescriptorLayout::CpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kFocusSrv, incrementSize));
    }

    // =======================================================================
    // Helper: get or create cached RTV handle
    // =======================================================================
    D3D12_CPU_DESCRIPTOR_HANDLE D3D12Compositor::getOrCreateRTV(
            D3D12SwapchainGraphicsState& state,
            ID3D12Resource* destination,
            uint32_t arraySlice,
            DXGI_FORMAT format) {
        auto device = m_device.Get();

        const RtvCacheKey key{destination, arraySlice};
        auto it = state.rtvCache.find(key);
        if (it != state.rtvCache.end()) {
            return it->second;
        }

        // Guard against heap overflow (8 descriptors max)
        if (state.rtvCache.size() >= 8) {
            LogWarning("D3D12: RTV heap exhausted (8/8). Clearing cache.\n");
            state.rtvCache.clear();
        }

        const auto rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
        rtvHandle.Offset((INT)state.rtvCache.size() * (INT)rtvInc);

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = format;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.ArraySize = 1;
        rtvDesc.Texture2DArray.FirstArraySlice = arraySlice;
        device->CreateRenderTargetView(destination, &rtvDesc, rtvHandle);

        state.rtvCache[key] = rtvHandle;
        return rtvHandle;
    }

    // =======================================================================
    // CRTP Hook: Stage 5 - Cleanup, restore barriers, submit, and sync
    //
    // CRITICAL: ExecuteCommandLists expects an array of pointers
    // (ID3D12CommandList* const*). Passing the command list pointer directly
    // via a reinterpret_cast causes the runtime to read the object's vtable
    // as memory addresses, leading to immediate device removal.
    // =======================================================================
    void D3D12Compositor::cleanupAndRelease(
            const CompositorParams& params,
            D3D12SwapchainGraphicsState& stereoState) {
        auto queue = m_queue.Get();
        const uint32_t viewIndex = params.viewIndex;
        auto cmdList = m_currentCmdList;
        auto destination = m_currentDestination;

        // Restore directly bound images to RENDER_TARGET before closing
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            if (m_directBoundStereo)
                barriers.Add(m_directBoundStereo, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (m_directBoundFocus)
                barriers.Add(m_directBoundFocus, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        // Transition destination to present
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            barriers.Add(destination, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }

        CHECK_HRCMD(cmdList->Close());

        // SAFE EXECUTION: Pass a stack array of pointers, NOT a casted pointer.
        ID3D12CommandList* const ppCommandLists[] = { cmdList };
        queue->ExecuteCommandLists(1, ppCommandLists);

        // Signal fence and wait (view 1 only)
        if (viewIndex == xr::StereoView::Right) {
            m_fenceValue++;
            queue->Signal(m_compositionFence.Get(), m_fenceValue);
            m_currentFrameIndex = (m_currentFrameIndex + 1) % kFrameCount;
            m_cmdListIndex++;

            if (m_compositionFence->GetCompletedValue() < m_fenceValue) {
                HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                m_compositionFence->SetEventOnCompletion(m_fenceValue, event);
                WaitForSingleObject(event, INFINITE);
                CloseHandle(event);
            }
#ifdef _DEBUG
            LogDebug("  D3D12 composition complete (fence={})\n", m_fenceValue);
#endif

            // Call base class virtual method directly to bypass layer's deferred release quirk
            releaseFullFovImage(stereoState);
        }

        // Reset tracking members for next frame
        m_currentCmdList = nullptr;
        m_currentDestination = nullptr;
        m_directBoundStereo = nullptr;
        m_directBoundFocus = nullptr;
    }

    // =======================================================================
    // Helper: reset command allocator and acquire command list
    // =======================================================================
    ComPtr<ID3D12GraphicsCommandList>& D3D12Compositor::resetCommandList(uint32_t viewIndex) {
        auto device = m_device.Get();

        CHECK_HRCMD(m_compositionAllocator[viewIndex]->Reset());

        const uint32_t listSlot = m_cmdListIndex % 2;
        ComPtr<ID3D12GraphicsCommandList>& cmdList = m_cmdList[viewIndex][listSlot];

        if (!cmdList) {
            HRESULT hr = device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_compositionAllocator[viewIndex].Get(), nullptr,
                IID_PPV_ARGS(cmdList.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                HRESULT removedReason = device->GetDeviceRemovedReason();
                LogError("D3D12: CreateCommandList failed: {:#x}, DeviceRemovedReason: {:#x}\n",
                         (uint32_t)hr, (uint32_t)removedReason);
                // Common reasons:
                //   0x887A0006 = DXGI_ERROR_DEVICE_HUNG (TDR — GPU took too long)
                //   0x887A0005 = DXGI_ERROR_DEVICE_REMOVED (driver crash / resource conflict)
                //   0x887A0020 = DXGI_ERROR_DEVICE_RESET (driver update / TDR)
                //   0x887A0021 = DXGI_ERROR_DEVICE_REMOVED (DXGI_ERROR_INVALID_CALL from debug layer)
                // FIX: Throw a standard exception so the dispatcher's catch(std::exception&)
                // can safely catch it and log the error context.
                throw std::runtime_error(fmt::format(
                    "D3D12 CreateCommandList failed: 0x{:08X}, DeviceRemoved: 0x{:08X}",
                    static_cast<uint32_t>(hr),
                    static_cast<uint32_t>(removedReason)));
            }
        } else {
            CHECK_HRCMD(cmdList->Reset(m_compositionAllocator[viewIndex].Get(), nullptr));
        }
        return cmdList;
    }

    void D3D12Compositor::destroy() {
        // FIX: Skip all cleanup during DLL unload. The process is exiting and the app's D3D12
        // device/queue may already be dead. Calling Release() or Unmap() would crash.
        // The OS reclaims all memory on exit anyway.
        if (g_isUnloading) {
            return;
        }

        // FIX: Wait for GPU to finish all composition work before releasing anything.
        if (m_queue && m_compositionFence) {
            m_fenceValue++;
            CHECK_HRCMD(m_queue->Signal(m_compositionFence.Get(), m_fenceValue));
            if (m_compositionFence->GetCompletedValue() < m_fenceValue) {
                HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                CHECK_HRCMD(m_compositionFence->SetEventOnCompletion(m_fenceValue, event));
                WaitForSingleObject(event, 5000); // 5-second timeout to prevent infinite hangs
                CloseHandle(event);
            }
        }

        // Idempotent: safe to call multiple times (ComPtr::Reset() is a no-op on already-null pointers).
        m_swapchainStates.clear();
        m_cpuCbvSrvHeap.Reset();
        m_rtvHeap.Reset();
        m_samplerHeap.Reset();
        m_projectionPSO.Reset();
        m_sharpeningPSO.Reset();
        m_projectionRootSignature.Reset();
        m_sharpeningRootSignature.Reset();
        m_blankTexture.Reset();
        m_compositionAllocator[0].Reset();
        m_compositionAllocator[1].Reset();
        m_compositionFence.Reset();

        // FIX (Item 10): Unmap persistently mapped resources before resetting
        for (uint32_t f = 0; f < kFrameCount; f++) {
            if (m_projectionVSConstants[f]) {
                m_projectionVSConstants[f]->Unmap(0, nullptr);
            }
            if (m_projectionPSConstants[f]) {
                m_projectionPSConstants[f]->Unmap(0, nullptr);
            }
            m_projectionVSConstants[f].Reset();
            m_projectionPSConstants[f].Reset();
            m_sharpeningCSConstants[f].Reset();
            m_cbvSrvHeap[f][0].Reset();
            m_cbvSrvHeap[f][1].Reset();
        }
    }

    void D3D12Compositor::waitForGpuIdle() {
        // FIX: Skip GPU sync during DLL unload.
        if (g_isUnloading) {
            return;
        }

        if (m_queue && m_compositionFence) {
            m_fenceValue++;
            CHECK_HRCMD(m_queue->Signal(m_compositionFence.Get(), m_fenceValue));
            if (m_compositionFence->GetCompletedValue() < m_fenceValue) {
                HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                CHECK_HRCMD(m_compositionFence->SetEventOnCompletion(m_fenceValue, event));
                WaitForSingleObject(event, 5000); // 5-second timeout
                CloseHandle(event);
            }
        }
    }

    std::unique_ptr<ICompositor> createD3D12Compositor(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi) {
        return std::make_unique<D3D12Compositor>(device, queue, openXrApi);
    }

} // namespace openxr_api_layer
