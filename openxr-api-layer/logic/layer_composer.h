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

#pragma once

#include "pch.h"
#include "framework/dispatch.gen.h"
#include "logic/config.h"
#include "logic/view_math.h"
#include "logic/eye_tracker.h"
#include "logic/swapchain_manager.h"
#include "logic/graphics_context.h"
#include "logic/focus_fov_quirk.h"
#include "views.h"
#include <set>
#include <vector>

namespace openxr_api_layer {

    class LayerComposer {
      public:
        LayerComposer(OpenXrApi* openXrApi,
                      FoveationConfig& config,
                      ViewManager& viewManager,
                      SwapchainManager& swapchainManager,
                      GraphicsContext& graphicsContext,
                      EyeTracker& eyeTracker,
                      FocusFovQuirk& focusFovQuirk);

        // Processes the composition layers from xrEndFrame.
        // Fills the output layers vector with patched projection layers.
        // Returns XR_SUCCESS or an error code.
        XrResult processLayers(XrSession session,
                               const XrFrameEndInfo* frameEndInfo,
                               bool useQuadViews,
                               bool useFovTangent,
                               bool requestedDepthSubmission,
                               std::vector<XrCompositionLayerProjection>& projectionAllocator,
                               std::vector<std::array<XrCompositionLayerProjectionView, xr::StereoView::Count>>& projectionViewAllocator,
                               std::vector<const XrCompositionLayerBaseHeader*>& outLayers,
                               std::set<XrSwapchain>& outSwapchainsToRelease);

      private:
        // Composites the focus view and stereo view into a single stereo view.
        void compositeViewContent(uint32_t viewIndex,
                                  const XrCompositionLayerProjectionView& stereoView,
                                  SwapchainManager::Swapchain& swapchainForStereoView,
                                  const XrCompositionLayerProjectionView& focusView,
                                  SwapchainManager::Swapchain& swapchainForFocusView,
                                  XrCompositionLayerFlags layerFlags,
                                  bool useQuadViews);

        OpenXrApi* m_openXrApi;
        FoveationConfig& m_config;
        ViewManager& m_viewManager;
        SwapchainManager& m_swapchainManager;
        GraphicsContext& m_graphicsContext;
        EyeTracker& m_eyeTracker;
        FocusFovQuirk& m_focusFovQuirk;

        // FIX (Item 5): Fallback flag for compositor initialization failure
        bool m_compositorFailed{false};
    };

} // namespace openxr_api_layer
