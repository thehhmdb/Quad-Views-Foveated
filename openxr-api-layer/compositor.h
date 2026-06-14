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

#include "pch.h"
#include "framework/dispatch.gen.h"
#include <views.h>

namespace openxr_api_layer {

    // Swapchain information passed from the main layer to the compositor.
    // Contains OpenXR swapchain handles and creation info.
    // The compositor maintains its own per-swapchain graphics state (flat images, image caches, etc.).
    struct SwapchainInfo {
        XrSwapchain handle;
        XrSwapchainCreateInfo createInfo;
        XrSwapchain fullFovSwapchain;
        uint32_t lastReleasedIndex;
    };

    // Per-frame parameters passed from the main layer to the compositor.
    // Contains all configuration and state needed for composition.
    struct CompositorParams {
        uint32_t viewIndex;
        XrFovf cachedEyeFov;          // Cached base eye FOV (for focus projection matrix)
        XrExtent2Di fullFovResolution; // Full FOV resolution for viewport/scissor
        bool useQuadViews;
        float smoothenFocusViewEdges;
        float sharpenFocusView;
        bool debugFocusView;
        bool debugEyeGaze;
        XrVector2f eyeGaze;
        XrCompositionLayerFlags layerFlags;
    };

    // Abstract interface for graphics API-specific composition.
    // Each graphics API (D3D11, D3D12) implements this interface.
    // The compositor owns:
    //   - Device/queue references
    //   - Composition resources (shaders, buffers, heaps, PSOs)
    //   - Per-swapchain graphics state (flat images, sharpened images, image caches)
    class ICompositor {
    public:
        virtual ~ICompositor() = default;

        // Initialize composition resources (shaders, buffers, heaps, etc.).
        // Called lazily on the first compositeView call.
        // Returns true on success.
        virtual bool initialize(int32_t swapchainFormat) = 0;

        // Compose a single eye's view content.
        // This includes:
        //   - Populate swapchain image caches (if not already done)
        //   - Acquire/release full FOV swapchain images (D3D12: on view 0/1)
        //   - Flatten source images to flat 2D textures
        //   - Sharpen focus image (if enabled)
        //   - Project and blend to destination
        // Returns the destination image pointer (for patching the projection view).
        // Returns nullptr on failure.
        virtual void* compositeView(const CompositorParams& params,
                                    const SwapchainInfo& stereoSwapchain,
                                    const XrCompositionLayerProjectionView& stereoView,
                                    const SwapchainInfo& focusSwapchain,
                                    const XrCompositionLayerProjectionView& focusView) = 0;

        // Cleanup compositor resources.
        virtual void destroy() = 0;

        // Check if composition resources have been initialized.
        virtual bool isInitialized() const = 0;
    };

    // Factory function to create the appropriate compositor for the detected graphics API.
    // The caller must pass the OpenXrApi pointer for OpenXR function calls.
    std::unique_ptr<ICompositor> createD3D11Compositor(ID3D11Device* device, OpenXrApi* openXrApi);
    std::unique_ptr<ICompositor> createD3D12Compositor(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi);

} // namespace openxr_api_layer
