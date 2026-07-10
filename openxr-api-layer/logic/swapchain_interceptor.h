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
#include "logic/swapchain_manager.h"
#include "logic/graphics_context.h"

namespace openxr_api_layer {

    class SwapchainInterceptor {
      public:
        SwapchainInterceptor(SwapchainManager& swapchainManager, GraphicsContext& graphicsContext);

        XrResult createSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain, OpenXrApi* openXrApi);
        XrResult destroySwapchain(XrSwapchain swapchain, OpenXrApi* openXrApi);
        XrResult acquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index, OpenXrApi* openXrApi, bool useFovModes);
        XrResult releaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo, OpenXrApi* openXrApi, bool useFovModes);

      private:
        SwapchainManager& m_swapchainManager;
        GraphicsContext& m_graphicsContext;
    };

} // namespace openxr_api_layer