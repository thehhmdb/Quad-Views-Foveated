// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so subject to the conditions :
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
#include "logic/swapchain_manager.h"

namespace openxr_api_layer {

    TEST(SwapchainManagerTest, DeferredReleaseRoundTrip) {
        SwapchainManager mgr;
        MockOpenXrApi api;
        mgr.setDeferredReleaseQuirk(true);

        XrSwapchainCreateInfo ci{};
        mgr.trackSwapchain((XrSwapchain)1, ci);

        // 1. Simulate release -> should defer
        EXPECT_TRUE(mgr.handleRelease((XrSwapchain)1));
        auto entry = mgr.getSwapchain((XrSwapchain)1);
        ASSERT_NE(entry, nullptr);
        EXPECT_TRUE(entry->deferredRelease);

        // 2. Simulate next acquire -> should release previous
        EXPECT_CALL(api, xrReleaseSwapchainImage((XrSwapchain)1, testing::_))
            .WillOnce(testing::Return(XR_SUCCESS));

        mgr.handleAcquire((XrSwapchain)1, &api);
        EXPECT_FALSE(entry->deferredRelease);
    }

    TEST(SwapchainManagerTest, FullFovSwapchainDestroyedOnUntrack) {
        SwapchainManager mgr;
        MockOpenXrApi api;
        api.initializeForTesting();
        XrSwapchainCreateInfo ci{};
        mgr.trackSwapchain((XrSwapchain)1, ci);

        auto entry = mgr.getSwapchain((XrSwapchain)1);
        entry->fullFovSwapchain = (XrSwapchain)42;

        // Note: SwapchainManager calls openXrApi->OpenXrApi::xrDestroySwapchain
        // to bypass the layer's override and avoid recursive locking. The mock's
        // xrDestroySwapchain override is what gets invoked via the dispatch table.
        EXPECT_CALL(api, xrDestroySwapchain((XrSwapchain)42))
            .WillOnce(testing::Return(XR_SUCCESS));

        mgr.untrackSwapchain((XrSwapchain)1, &api);
        EXPECT_EQ(mgr.getSwapchain((XrSwapchain)1), nullptr);
    }

} // namespace openxr_api_layer
