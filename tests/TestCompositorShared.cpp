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
#include <gtest/gtest.h>
#include "logic/compositor_shared.h"

namespace openxr_api_layer {

    // Helper to create a minimal SwapchainInfo for testing
    static SwapchainInfo makeSwapchainInfo(uint32_t width, uint32_t height) {
        SwapchainInfo info{};
        info.createInfo = {};
        info.createInfo.width = width;
        info.createInfo.height = height;
        return info;
    }

    // Helper to create a minimal ProjectionView with configurable subImage
    static XrCompositionLayerProjectionView makeProjectionView(
        int32_t offset_x, int32_t offset_y,
        int32_t extent_w, int32_t extent_h,
        uint32_t array_index) {
        XrCompositionLayerProjectionView view{};
        view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        view.next = nullptr;
        view.pose = {};
        view.fov = {};
        view.subImage = {};
        view.subImage.swapchain = nullptr;
        view.subImage.imageRect.offset.x = offset_x;
        view.subImage.imageRect.offset.y = offset_y;
        view.subImage.imageRect.extent.width = extent_w;
        view.subImage.imageRect.extent.height = extent_h;
        view.subImage.imageArrayIndex = array_index;
        return view;
    }

    // ---------------------------------------------------------------------------
    // NeedsFlattening tests
    // ---------------------------------------------------------------------------

    TEST(CompositorSharedTest, NeedsFlattening_FullSwapchain_NoFlatten) {
        // Full swapchain at offset (0,0) and array index 0 → no flatten needed
        auto swapchain = makeSwapchainInfo(1920, 1080);
        auto view = makeProjectionView(0, 0, 1920, 1080, 0);
        EXPECT_FALSE(NeedsFlattening(view, swapchain));
    }

    TEST(CompositorSharedTest, NeedsFlattening_NonZeroOffset_NeedsFlatten) {
        // Non-zero offset → flatten needed
        auto swapchain = makeSwapchainInfo(1920, 1080);
        auto view = makeProjectionView(100, 0, 1820, 1080, 0);
        EXPECT_TRUE(NeedsFlattening(view, swapchain));
    }

    TEST(CompositorSharedTest, NeedsFlattening_SmallerExtent_NeedsFlatten) {
        // Smaller extent than swapchain → flatten needed
        auto swapchain = makeSwapchainInfo(1920, 1080);
        auto view = makeProjectionView(0, 0, 960, 540, 0);
        EXPECT_TRUE(NeedsFlattening(view, swapchain));
    }

    TEST(CompositorSharedTest, NeedsFlattening_NonZeroArrayIndex_NeedsFlatten) {
        // Non-zero array index → flatten needed
        auto swapchain = makeSwapchainInfo(1920, 1080);
        auto view = makeProjectionView(0, 0, 1920, 1080, 1);
        EXPECT_TRUE(NeedsFlattening(view, swapchain));
    }

    TEST(CompositorSharedTest, NeedsFlattening_MultipleDifferences_NeedsFlatten) {
        // Multiple differences → flatten needed
        auto swapchain = makeSwapchainInfo(3840, 2160);
        auto view = makeProjectionView(100, 50, 960, 540, 2);
        EXPECT_TRUE(NeedsFlattening(view, swapchain));
    }

    // ---------------------------------------------------------------------------
    // SharpeningPass tests
    // ---------------------------------------------------------------------------

    TEST(CompositorSharedTest, SharpeningPass_Disabled_ReturnsFalse) {
        SharpeningPass pass;
        CompositorParams params{};
        params.sharpenFocusView = 0.0f; // Disabled

        auto swapchain = makeSwapchainInfo(960, 540);
        auto view = makeProjectionView(0, 0, 960, 540, 0);

        EXPECT_FALSE(pass(params, view, swapchain));
    }

    TEST(CompositorSharedTest, SharpeningPass_Enabled_ReturnsTrueAndDispatchDims) {
        SharpeningPass pass;
        CompositorParams params{};
        params.sharpenFocusView = 0.5f; // Enabled

        auto swapchain = makeSwapchainInfo(960, 540);
        auto view = makeProjectionView(0, 0, 960, 540, 0);

        EXPECT_TRUE(pass(params, view, swapchain));

        // 960 / 16 = 60, 540 / 16 = 33.75 → ceil = 34
        EXPECT_EQ(pass.dispatchX, 60u);
        EXPECT_EQ(pass.dispatchY, 34u);
    }

    TEST(CompositorSharedTest, SharpeningPass_OddDimensions_CeilsCorrectly) {
        SharpeningPass pass;
        CompositorParams params{};
        params.sharpenFocusView = 0.5f;

        // 17x17 → ceil(17/16) = 2x2
        auto swapchain = makeSwapchainInfo(17, 17);
        auto view = makeProjectionView(0, 0, 17, 17, 0);

        EXPECT_TRUE(pass(params, view, swapchain));
        EXPECT_EQ(pass.dispatchX, 2u);
        EXPECT_EQ(pass.dispatchY, 2u);
    }

    TEST(CompositorSharedTest, SharpeningPass_PrepareConstants) {
        SharpeningPass pass;
        SharpeningCSConstants constants{};

        pass.PrepareConstants(constants, 0.5f, 960, 540);

        // Verify that the constants were populated (Const0/Const1 should not be all zeros)
        // FFX-CAS produces non-trivial constants for valid inputs
        bool hasNonZero = false;
        for (int i = 0; i < 4; i++) {
            if (constants.Const0[i] != 0 || constants.Const1[i] != 0) {
                hasNonZero = true;
                break;
            }
        }
        EXPECT_TRUE(hasNonZero);
    }

    TEST(CompositorSharedTest, SharpeningPass_ThreadGroupDim_Is16) {
        // Verify the constant is accessible and equals 16
        EXPECT_EQ(SharpeningPass::ThreadGroupWorkRegionDim, 16);
    }

} // namespace openxr_api_layer
