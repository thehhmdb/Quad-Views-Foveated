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

    // Per-swapchain graphics state for D3D12.
    // Maintained internally by the compositor, keyed by XrSwapchain handle.
    struct D3D12SwapchainGraphicsState {
        // Cached swapchain images (enumerated once per swapchain)
        std::vector<ID3D12Resource*> images;
        std::vector<ID3D12Resource*> fullFovSwapchainImages;

        // Flat images for quad views (4 views: 2 stereo + 2 focus)
        ComPtr<ID3D12Resource> flatImage[xr::QuadView::Count];

        // Sharpened images (2 views: left + right)
        ComPtr<ID3D12Resource> sharpenedImage[xr::StereoView::Count];

        // History textures for temporal stability (TAA-lite)
        ComPtr<ID3D12Resource> historyImage[xr::StereoView::Count];

        // Current acquired full FOV image index
        uint32_t acquiredFullFovImageIndex{0};
    };

    // D3D12 implementation of the compositor interface.
    class D3D12Compositor : public ICompositor {
    public:
        D3D12Compositor(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi);
        // Call destroy() in the destructor so resources are released even if destroy() was not
        // called explicitly (e.g. due to an exception). destroy() is idempotent.
        ~D3D12Compositor() override {
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
        void evictSwapchainState(XrSwapchain handle) override {
            m_swapchainStates.erase(handle);
        }

        void waitForGpuIdle() override;

        // Returns the composition fence and current value for external synchronization.
        // Callers can wait on this fence before destroying swapchains to ensure
        // all GPU work referencing the swapchain images has completed.
        void* getCompositionFence() const override {
            return m_compositionFence.Get();
        }
        uint64_t getCompositionFenceValue() const override {
            return m_fenceValue;
        }

    private:
        void populateSwapchainImagesCache(D3D12SwapchainGraphicsState& state, XrSwapchain swapchain, bool isFullFov);

        ComPtr<ID3D12Device> m_device;
        ComPtr<ID3D12CommandQueue> m_queue;
        OpenXrApi* m_openXrApi{nullptr};

        // 3-frame pipelining: we keep 3 frames in flight to allow CPU/GPU parallelism.
        // Frame N is being recorded, Frame N-1 is being executed, Frame N-2 is being waited on.
        static constexpr uint32_t kFrameCount = 3;
        uint32_t m_currentFrameIndex{0};
        
        // Per-frame resources (triple-buffered)
        ComPtr<ID3D12CommandAllocator> m_compositionAllocator[kFrameCount][xr::StereoView::Count];
        ComPtr<ID3D12GraphicsCommandList> m_cachedCmdList[kFrameCount][xr::StereoView::Count];
        ComPtr<ID3D12DescriptorHeap> m_cbvSrvHeap[kFrameCount][xr::StereoView::Count];
        ComPtr<ID3D12Resource> m_projectionVSConstants[kFrameCount];
        ComPtr<ID3D12Resource> m_projectionPSConstants[kFrameCount];
        ComPtr<ID3D12Resource> m_sharpeningCSConstants[kFrameCount];
        
        // Fence for synchronization - one per frame in flight
        ComPtr<ID3D12Fence> m_frameFence[kFrameCount];
        UINT64 m_frameFenceValue[kFrameCount]{0};
        
        // Global composition fence (for external synchronization)
        ComPtr<ID3D12Fence> m_compositionFence;
        UINT64 m_fenceValue{0};

        // Shared resources (not per-frame)
        ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        ComPtr<ID3D12DescriptorHeap> m_samplerHeap;
        ComPtr<ID3D12PipelineState> m_projectionPSO;
        ComPtr<ID3D12PipelineState> m_sharpeningPSO;
        ComPtr<ID3D12RootSignature> m_projectionRootSignature;
        ComPtr<ID3D12RootSignature> m_sharpeningRootSignature;
        ComPtr<ID3D12Resource> m_blankTexture;

        // Per-swapchain graphics state (keyed by stereo swapchain handle)
        std::unordered_map<XrSwapchain, D3D12SwapchainGraphicsState> m_swapchainStates;
    };

} // namespace openxr_api_layer
