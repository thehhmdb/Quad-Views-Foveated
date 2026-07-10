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
#include "logic/swapchain_manager.h"
#include "framework/log.h"
#include "framework/util.h"

namespace openxr_api_layer {

    // Convenience alias for trace provider access within this module
    using log::g_traceProvider;

    void SwapchainManager::trackSwapchain(XrSwapchain handle, const XrSwapchainCreateInfo& createInfo) {
        std::unique_lock lock(m_mutex);
        Swapchain newEntry{};
        newEntry.createInfo = createInfo;
        m_swapchains.insert_or_assign(handle, std::move(newEntry));
    }

    void SwapchainManager::untrackSwapchain(XrSwapchain handle, OpenXrApi* openXrApi) {
        std::unique_lock lock(m_mutex);
        auto it = m_swapchains.find(handle);
        if (it != m_swapchains.end()) {
            Swapchain& entry = it->second;
            if (entry.fullFovSwapchain != XR_NULL_HANDLE) {
                openXrApi->xrDestroySwapchain(entry.fullFovSwapchain);
            }
            m_swapchains.erase(it);
        }
    }

    SwapchainManager::Swapchain* SwapchainManager::getSwapchain(XrSwapchain handle) {
        std::unique_lock lock(m_mutex);
        auto it = m_swapchains.find(handle);
        if (it != m_swapchains.end()) {
            return &it->second;
        }
        return nullptr;
    }

    void SwapchainManager::handleAcquire(XrSwapchain handle, OpenXrApi* openXrApi) {
        if (m_needDeferredSwapchainReleaseQuirk) {
            std::unique_lock lock(m_mutex);
            auto it = m_swapchains.find(handle);
            if (it != m_swapchains.end()) {
                if (it->second.deferredRelease) {
                    // Release the previous image before acquiring a new one.
                    TraceLoggingWrite(g_traceProvider,
                                      "xrAcquireSwapchainImage_DeferredSwapchainRelease",
                                      TLXArg(handle, "Swapchain"));
                    CHECK_XRCMD(openXrApi->xrReleaseSwapchainImage(handle, nullptr));
                    it->second.deferredRelease = false;
                }
            }
        }
        // Note: acquired index tracking is done by the caller after xrAcquireSwapchainImage returns
    }

    bool SwapchainManager::handleRelease(XrSwapchain handle) {
        if (m_needDeferredSwapchainReleaseQuirk) {
            std::unique_lock lock(m_mutex);
            auto it = m_swapchains.find(handle);
            if (it != m_swapchains.end()) {
                it->second.deferredRelease = true;
                return true;
            }
        }
        // Note: lastReleasedIndex update and acquiredIndex.pop_front()
        // are handled by the caller (layer.cpp) after this returns
        return false;
    }

    std::set<XrSwapchain> SwapchainManager::checkAndResetDeferredReleases() {
        std::set<XrSwapchain> result;
        std::unique_lock lock(m_mutex);
        for (auto& [handle, entry] : m_swapchains) {
            if (entry.deferredRelease) {
                result.insert(handle);
                entry.deferredRelease = false;
            }
        }
        return result;
    }

    void SwapchainManager::destroyAllFullFovSwapchains(OpenXrApi* openXrApi) {
        std::unique_lock lock(m_mutex);
        for (auto& [handle, entry] : m_swapchains) {
            if (entry.fullFovSwapchain != XR_NULL_HANDLE) {
                openXrApi->xrDestroySwapchain(entry.fullFovSwapchain);
                entry.fullFovSwapchain = XR_NULL_HANDLE;
            }
        }
    }

} // namespace openxr_api_layer
