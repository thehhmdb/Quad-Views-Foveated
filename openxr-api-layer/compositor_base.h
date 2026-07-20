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
#include "framework/log.h"
#include "logic/compositor_shared.h"
#include <memory>
#include <mutex>
#include <unordered_map>

namespace openxr_api_layer {

    // ---------------------------------------------------------------------------
    // Base for per-swapchain graphics state.
    // Holds fields that are identical between D3D11 and D3D12 implementations.
    // ---------------------------------------------------------------------------
    struct SwapchainGraphicsStateBase {
        uint32_t acquiredFullFovImageIndex{0};
        bool fullFovImageAcquired{false};
        XrSwapchain fullFovSwapchain{XR_NULL_HANDLE};
    };

    // ---------------------------------------------------------------------------
    // CRTP base: DerivedT is D3D11Compositor or D3D12Compositor.
    // StateT is the corresponding SwapchainGraphicsState struct.
    //
    // Provides:
    //   - Shared swapchain state map (m_swapchainStates) with mutex
    //   - getOrCreateSwapchainState() — thread-safe lookup
    //   - evictSwapchainState() — inherited implementation
    //   - Full-FOV acquire/release lifecycle helpers
    //   - NeedsReallocate() — CRTP-dispatched texture size check
    // ---------------------------------------------------------------------------
    template <typename DerivedT, typename StateT>
    class BaseCompositor : public ICompositor {
    public:
        explicit BaseCompositor(OpenXrApi* openXrApi)
            : m_openXrApi(openXrApi) {}

        bool isInitialized() const override {
            return m_initialized;
        }

        void evictSwapchainState(XrSwapchain handle) override {
            std::lock_guard<std::mutex> lock(m_swapchainStatesMutex);
            auto it = m_swapchainStates.find(handle);
            if (it != m_swapchainStates.end()) {
                evictFullFovImageIfHeld(*it->second);
                m_swapchainStates.erase(it);
            }
        }

        // -------------------------------------------------------------------
        // Pipeline orchestrator — shared between D3D11 and D3D12.
        // Derived classes implement narrow CRTP hooks for API-specific work.
        // -------------------------------------------------------------------
        void* compositeView(const CompositorParams& params,
                            const SwapchainInfo& stereoSwapchain,
                            const XrCompositionLayerProjectionView& stereoView,
                            const SwapchainInfo& focusSwapchain,
                            const XrCompositionLayerProjectionView& focusView) override {
            auto stereoState = getOrCreateSwapchainState(stereoSwapchain.handle);
            auto focusState = getOrCreateSwapchainState(focusSwapchain.handle);

            // Derived classes populate their swapchain image caches here
            static_cast<DerivedT*>(this)->populateSwapchainImagesCache(*stereoState, stereoSwapchain.handle, false);
            static_cast<DerivedT*>(this)->populateSwapchainImagesCache(*focusState, focusSwapchain.handle, false);

            // Stage 1: Acquire destination, resolve source textures
            void* sourceStereo = nullptr;
            void* sourceFocus = nullptr;
            void* destination = nullptr;
            if (!static_cast<DerivedT*>(this)->acquireAndResolveImages(params, stereoSwapchain, focusSwapchain,
                                                                       *stereoState, *focusState,
                                                                       sourceStereo, sourceFocus, destination)) {
                return nullptr;
            }

            // Stage 2: Flatten source images
            flattenImages(params, stereoView, stereoSwapchain, focusView, focusSwapchain,
                          *stereoState, *focusState, sourceStereo, sourceFocus);

            // Stage 3: Sharpen (conditional)
            if (params.sharpenFocusView) {
                static_cast<DerivedT*>(this)->sharpenFocusView(params, focusView, focusSwapchain, *focusState);
            }

            // Stage 4: Projection render pass
            static_cast<DerivedT*>(this)->renderProjection(params, focusView, stereoSwapchain, focusSwapchain,
                                                           *stereoState, *focusState, destination);

            // Stage 5: Unbind and release
            static_cast<DerivedT*>(this)->cleanupAndRelease(params, *stereoState);

            return destination;
        }

        // Public accessors used by the layer
        OpenXrApi* m_openXrApi{nullptr};
        bool m_initialized{false};

    protected:
        // Typed swapchain state map — each derived class gets the right type
        // without the base class needing to know the D3D details.
        std::unordered_map<XrSwapchain, std::shared_ptr<StateT>> m_swapchainStates;
        mutable std::mutex m_swapchainStatesMutex;

        // -------------------------------------------------------------------
        // Swapchain state lookup
        // -------------------------------------------------------------------

        // Returns (existing or newly created) swapchain state. Thread-safe.
        std::shared_ptr<StateT> getOrCreateSwapchainState(XrSwapchain handle) {
            std::lock_guard<std::mutex> lock(m_swapchainStatesMutex);
            auto it = m_swapchainStates.find(handle);
            if (it != m_swapchainStates.end()) {
                return it->second;
            }
            auto state = std::make_shared<StateT>();
            m_swapchainStates[handle] = state;
            return state;
        }

        // -------------------------------------------------------------------
        // Full-FOV swapchain lifecycle helpers
        //
        // The full-FOV swapchain uses a shared array swapchain (arraySize=2).
        // View 0 acquires at the start of the frame; view 1 releases at the end.
        // These helpers centralize the state machine so both compositors stay
        // in sync and the acquire/release contract is never violated.
        // -------------------------------------------------------------------

        // Acquire the full-FOV swapchain image. Call only on view 0.
        // Returns the acquired image index, or logs a warning and returns
        // UINT32_MAX on failure.
        uint32_t acquireFullFovImage(XrSwapchain fullFovSwapchain,
                                      SwapchainGraphicsStateBase& state) {
            // Release any lingering acquire from a previous frame (e.g. if
            // view 1's release was skipped due to an early-out).
            if (state.fullFovImageAcquired) {
                const XrResult relResult =
                    m_openXrApi->OpenXrApi::xrReleaseSwapchainImage(fullFovSwapchain, nullptr);
                if (XR_FAILED(relResult)) {
                    log::LogWarning("BaseCompositor: stale xrReleaseSwapchainImage failed: {}\n",
                                    xr::ToCString(relResult));
                }
                state.fullFovImageAcquired = false;
            }

            uint32_t acquiredIndex = 0;
            XrResult result =
                m_openXrApi->OpenXrApi::xrAcquireSwapchainImage(fullFovSwapchain, nullptr, &acquiredIndex);
            if (XR_FAILED(result)) {
                log::LogWarning("BaseCompositor: xrAcquireSwapchainImage (full FOV) failed: {}\n",
                                xr::ToCString(result));
                return UINT32_MAX;
            }

            XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = 10000000000; // 10 seconds
            result = m_openXrApi->OpenXrApi::xrWaitSwapchainImage(fullFovSwapchain, &waitInfo);
            if (XR_FAILED(result)) {
                log::LogWarning("BaseCompositor: xrWaitSwapchainImage (full FOV) failed: {}\n",
                                xr::ToCString(result));
                // Release the acquired image to avoid leaking the acquire.
                m_openXrApi->OpenXrApi::xrReleaseSwapchainImage(fullFovSwapchain, nullptr);
                return UINT32_MAX;
            }

            state.acquiredFullFovImageIndex = acquiredIndex;
            state.fullFovImageAcquired = true;
            state.fullFovSwapchain = fullFovSwapchain;
            return acquiredIndex;
        }

        // Release the full-FOV swapchain image. Call only on view 1.
        void releaseFullFovImage(SwapchainGraphicsStateBase& state) {
            if (!state.fullFovImageAcquired) {
                return;
            }
            const XrResult result =
                m_openXrApi->OpenXrApi::xrReleaseSwapchainImage(state.fullFovSwapchain, nullptr);
            if (XR_FAILED(result)) {
                log::LogWarning("BaseCompositor: xrReleaseSwapchainImage (full FOV) failed: {}\n",
                                xr::ToCString(result));
            }
            state.fullFovImageAcquired = false;
        }

        // Release any held full-FOV image during swapchain eviction.
        // Call from evictSwapchainState while the mutex is held.
        void evictFullFovImageIfHeld(SwapchainGraphicsStateBase& state) {
            if (state.fullFovImageAcquired && state.fullFovSwapchain != XR_NULL_HANDLE) {
                m_openXrApi->OpenXrApi::xrReleaseSwapchainImage(state.fullFovSwapchain, nullptr);
                state.fullFovImageAcquired = false;
            }
        }

        // -------------------------------------------------------------------
        // Flatten images orchestration — shared between D3D11 and D3D12.
        // Calls CRTP hooks for API-specific resource creation and copying.
        // -------------------------------------------------------------------
        void flattenImages(const CompositorParams& params,
                           const XrCompositionLayerProjectionView& stereoView,
                           const SwapchainInfo& stereoSwapchain,
                           const XrCompositionLayerProjectionView& focusView,
                           const SwapchainInfo& focusSwapchain,
                           StateT& stereoState,
                           StateT& focusState,
                           void* sourceStereo,
                           void* sourceFocus) {
            const uint32_t viewIndex = params.viewIndex;

            const auto flattenOne = [&](void* sourceImage,
                                        const XrCompositionLayerProjectionView& view,
                                        const SwapchainInfo& swapchainInfo,
                                        StateT& state,
                                        uint32_t startSlot) {
                const uint32_t targetSlot = startSlot + viewIndex;
                const uint32_t format = (uint32_t)swapchainInfo.createInfo.format;

                if (!NeedsFlattening(view, swapchainInfo)) {
                    static_cast<DerivedT*>(this)->BindDirectSource(state, targetSlot, sourceImage, format);
                    return;
                }

                const uint32_t flatWidth = (uint32_t)view.subImage.imageRect.extent.width;
                const uint32_t flatHeight = (uint32_t)view.subImage.imageRect.extent.height;

                if (static_cast<DerivedT*>(this)->NeedsFlatReallocate(state, targetSlot, flatWidth, flatHeight, format)) {
                    static_cast<DerivedT*>(this)->CreateFlatImage(state, targetSlot, flatWidth, flatHeight, format);
                }
                static_cast<DerivedT*>(this)->CopySubImage(state, targetSlot, sourceImage, view);
            };

            if (params.useQuadViews) {
                flattenOne(sourceStereo, stereoView, stereoSwapchain, stereoState, 0);
            }
            flattenOne(sourceFocus, focusView, focusSwapchain, focusState, xr::StereoView::Count);
        }

        // -------------------------------------------------------------------
        // Texture reallocation helper
        // -------------------------------------------------------------------

        // Returns true if a texture needs to be (re)created because it's null
        // or its dimensions/format don't match the target.
        // Derived classes implement GetTextureDesc to extract width/height/format
        // from their API-specific texture type.
        bool NeedsReallocate(const void* texture,
                              uint32_t targetWidth,
                              uint32_t targetHeight,
                              uint32_t targetFormat) {
            if (!texture) return true;

            uint32_t currWidth = 0, currHeight = 0, currFormat = 0;
            static_cast<DerivedT*>(this)->GetTextureDesc(
                texture, currWidth, currHeight, currFormat);

            return currWidth != targetWidth ||
                   currHeight != targetHeight ||
                   currFormat != targetFormat;
        }

        // -------------------------------------------------------------------
        // CRTP hooks — pure virtual-like interface for derived classes.
        // Each hook is a narrow, API-specific operation.
        // -------------------------------------------------------------------

        // Populate swapchain image cache from the mock/runtime API
        // Derived: void populateSwapchainImagesCache(StateT&, XrSwapchain, bool);
        // Called from compositeView before stage 1.

        // Stage 1: Acquire full-FOV destination and resolve source textures
        // Derived: bool acquireAndResolveImages(..., void*&, void*&, void*&);

        // Flatten hooks (called from flattenImages in the base class)
        // Derived: void BindDirectSource(StateT&, uint32_t, void*, uint32_t);
        // Derived: bool NeedsFlatReallocate(StateT&, uint32_t, uint32_t, uint32_t, uint32_t);
        // Derived: void CreateFlatImage(StateT&, uint32_t, uint32_t, uint32_t, uint32_t);
        // Derived: void CopySubImage(StateT&, uint32_t, void*, const XrCompositionLayerProjectionView&);

        // Stage 3: Sharpen focus view (CAS compute pass)
        // Derived: void sharpenFocusView(CompositorParams, XrCompositionLayerProjectionView, SwapchainInfo, StateT&);

        // Stage 4: Render projection pass
        // Derived: void renderProjection(CompositorParams, XrCompositionLayerProjectionView, SwapchainInfo, SwapchainInfo, StateT&, StateT&, void*);

        // Stage 5: Unbind resources and release full-FOV image
        // Derived: void cleanupAndRelease(CompositorParams, StateT&);
    };

} // namespace openxr_api_layer
