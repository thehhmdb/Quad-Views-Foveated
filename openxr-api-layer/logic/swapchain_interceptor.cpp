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
#include "logic/swapchain_interceptor.h"
#include "framework/log.h"
#include "framework/util.h"
#include "compositor.h"

namespace openxr_api_layer {

    using namespace log;
    using namespace xr;

    SwapchainInterceptor::SwapchainInterceptor(SwapchainManager& swapchainManager, GraphicsContext& graphicsContext)
        : m_swapchainManager(swapchainManager), m_graphicsContext(graphicsContext) {
    }

    XrResult SwapchainInterceptor::createSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain, OpenXrApi* openXrApi) {
        if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        TraceLoggingWrite(g_traceProvider,
                          "xrCreateSwapchain",
                          TLXArg(session, "Session"),
                          TLArg(createInfo->arraySize, "ArraySize"),
                          TLArg(createInfo->width, "Width"),
                          TLArg(createInfo->height, "Height"),
                          TLArg(createInfo->createFlags, "CreateFlags"),
                          TLArg(createInfo->format, "Format"),
                          TLArg(createInfo->faceCount, "FaceCount"),
                          TLArg(createInfo->mipCount, "MipCount"),
                          TLArg(createInfo->sampleCount, "SampleCount"),
                          TLArg(createInfo->usageFlags, "UsageFlags"));

        const XrResult result = openXrApi->OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);

        if (XR_SUCCEEDED(result)) {
            TraceLoggingWrite(g_traceProvider, "xrCreateSwapchain", TLXArg(*swapchain, "Swapchain"));
            LogDebug("xrCreateSwapchain: format={}, size={}x{}, arraySize={}, swapchain={:x}\n",
                            createInfo->format, createInfo->width, createInfo->height, createInfo->arraySize, (uint64_t)*swapchain);

            if (session != XR_NULL_HANDLE) {
                m_swapchainManager.trackSwapchain(*swapchain, *createInfo);
            }
        }

        return result;
    }

    XrResult SwapchainInterceptor::destroySwapchain(XrSwapchain swapchain, OpenXrApi* openXrApi) {
        TraceLoggingWrite(g_traceProvider, "xrDestroySwapchain", TLXArg(swapchain, "Swapchain"));

        // FIX: Wait for GPU to finish all composition work before destroying the swapchain.
        // This prevents the runtime from freeing images that the GPU is still using.
        if (m_graphicsContext.getCompositor()) {
            m_graphicsContext.getCompositor()->waitForGpuIdle();
        }

        const XrResult result = openXrApi->OpenXrApi::xrDestroySwapchain(swapchain);

        if (XR_SUCCEEDED(result)) {
            m_swapchainManager.untrackSwapchain(swapchain, openXrApi);

            // Evict the compositor's cached graphics state for this swapchain so we do not
            // hold dangling raw texture pointers after the runtime freed the swapchain images.
            if (m_graphicsContext.getCompositor()) {
                m_graphicsContext.getCompositor()->evictSwapchainState(swapchain);
            }
        }

        return result;
    }

    XrResult SwapchainInterceptor::acquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* acquireInfo, uint32_t* index, OpenXrApi* openXrApi, bool useFovModes) {
        TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLXArg(swapchain, "Swapchain"));

        // Handle deferred release before acquire
        if (useFovModes && m_swapchainManager.getDeferredReleaseQuirk()) {
            m_swapchainManager.handleAcquire(swapchain, openXrApi);
        }

        const XrResult result = openXrApi->OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);

        if (XR_SUCCEEDED(result)) {
            TraceLoggingWrite(g_traceProvider, "xrAcquireSwapchainImage", TLArg(*index, "Index"));

            // Update acquired index tracking
            auto* swapchainEntry = m_swapchainManager.getSwapchain(swapchain);
            if (swapchainEntry) {
                swapchainEntry->acquiredIndex.push_back(*index);
            }
        }

        return result;
    }

    XrResult SwapchainInterceptor::releaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo, OpenXrApi* openXrApi, bool useFovModes) {
        TraceLoggingWrite(g_traceProvider, "xrReleaseSwapchainImage", TLXArg(swapchain, "Swapchain"));

        bool deferRelease = false;
        if (useFovModes && m_swapchainManager.getDeferredReleaseQuirk()) {
            deferRelease = m_swapchainManager.handleRelease(swapchain);
        }

        XrResult result = XR_ERROR_RUNTIME_FAILURE;
        if (!deferRelease) {
            result = openXrApi->OpenXrApi::xrReleaseSwapchainImage(swapchain, releaseInfo);
        } else {
            TraceLoggingWrite(g_traceProvider, "xrReleaseSwapchainImage_Defer");
            result = XR_SUCCESS;
        }

        if (XR_SUCCEEDED(result)) {
            // Update lastReleasedIndex
            auto* swapchainEntry = m_swapchainManager.getSwapchain(swapchain);
            if (swapchainEntry && !swapchainEntry->acquiredIndex.empty()) {
                swapchainEntry->lastReleasedIndex = swapchainEntry->acquiredIndex.front();
                swapchainEntry->acquiredIndex.pop_front();
            }
        }

        return result;
    }

} // namespace openxr_api_layer