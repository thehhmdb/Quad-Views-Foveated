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
#include <gmock/gmock.h>
#include "compositor_base.h"
#include "MockOpenXrApi.h"

namespace openxr_api_layer {

    // ---------------------------------------------------------------------------
    // Mock swapchain state for testing — inherits the base but adds no D3D fields.
    // ---------------------------------------------------------------------------
    struct MockSwapchainState : SwapchainGraphicsStateBase {
        // Encode width/height/format into a single pointer-sized value for
        // NeedsReallocate testing.  Layout: [fmt:8][height:16][width:16] (bits).
        static uint64_t EncodeDesc(uint32_t width, uint32_t height, uint32_t format) {
            return (static_cast<uint64_t>(format) << 32) |
                   (static_cast<uint64_t>(height) << 16) |
                   static_cast<uint64_t>(width);
        }
    };

    // ---------------------------------------------------------------------------
    // Minimal compositor derived from BaseCompositor for unit testing.
    // ---------------------------------------------------------------------------
    class MockCompositor : public BaseCompositor<MockCompositor, MockSwapchainState> {
    public:
        explicit MockCompositor(OpenXrApi* openXrApi)
            : BaseCompositor<MockCompositor, MockSwapchainState>(openXrApi) {}

        // Stub implementations for abstract ICompositor methods
        bool initialize(int32_t) override { return true; }
        void* compositeView(const CompositorParams&, const SwapchainInfo&,
                            const XrCompositionLayerProjectionView&,
                            const SwapchainInfo&,
                            const XrCompositionLayerProjectionView&) override { return nullptr; }
        void destroy() override {}
        void waitForGpuIdle() override {}

        // Expose protected helpers for testing
        using BaseCompositor<MockCompositor, MockSwapchainState>::getOrCreateSwapchainState;
        using BaseCompositor<MockCompositor, MockSwapchainState>::NeedsReallocate;
        using BaseCompositor<MockCompositor, MockSwapchainState>::acquireFullFovImage;
        using BaseCompositor<MockCompositor, MockSwapchainState>::releaseFullFovImage;

        // GetTextureDesc: extract width/height/format from an encoded pointer.
        // Called by NeedsReallocate via CRTP dispatch.
        void GetTextureDesc(const void* texture,
                            uint32_t& width,
                            uint32_t& height,
                            uint32_t& format) const {
            if (!texture) {
                width = height = format = 0;
                return;
            }
            uint64_t val = reinterpret_cast<uint64_t>(const_cast<void*>(texture));
            width  = static_cast<uint32_t>(val & 0xFFFF);
            height = static_cast<uint32_t>((val >> 16) & 0xFFFF);
            format = static_cast<uint32_t>((val >> 32) & 0xFFFFFFFF);
        }

        // Stub CRTP hooks (required for template instantiation; not called at runtime
        // because compositeView is overridden above).
        void populateSwapchainImagesCache(MockSwapchainState&, XrSwapchain, bool) {}
        bool acquireAndResolveImages(const CompositorParams&, const SwapchainInfo&,
                                     const SwapchainInfo&, MockSwapchainState&,
                                     MockSwapchainState&, void*&, void*&, void*&) { return false; }
        void BindDirectSource(MockSwapchainState&, uint32_t, void*, uint32_t) {}
        bool NeedsFlatReallocate(MockSwapchainState&, uint32_t, uint32_t, uint32_t, uint32_t) { return false; }
        void CreateFlatImage(MockSwapchainState&, uint32_t, uint32_t, uint32_t, uint32_t) {}
        void CopySubImage(MockSwapchainState&, uint32_t, void*, const XrCompositionLayerProjectionView&) {}
        void sharpenFocusView(const CompositorParams&, const XrCompositionLayerProjectionView&,
                              const SwapchainInfo&, MockSwapchainState&) {}
        void renderProjection(const CompositorParams&, const XrCompositionLayerProjectionView&,
                              const SwapchainInfo&, const SwapchainInfo&, MockSwapchainState&,
                              MockSwapchainState&, void*) {}
        void cleanupAndRelease(const CompositorParams&, MockSwapchainState&) {}
    };

    // ===========================================================================
    // NeedsReallocate tests
    // ===========================================================================
    TEST(TestCompositorBase, NeedsReallocate_NullTexture) {
        MockOpenXrApi mockApi;
        MockCompositor compositor(&mockApi);

        EXPECT_TRUE(compositor.NeedsReallocate(nullptr, 1920, 1080, 87));
    }

    TEST(TestCompositorBase, NeedsReallocate_SameDimensions) {
        MockOpenXrApi mockApi;
        MockCompositor compositor(&mockApi);

        void* texture = const_cast<void*>(
            reinterpret_cast<const void*>(MockSwapchainState::EncodeDesc(1920, 1080, 87)));

        EXPECT_FALSE(compositor.NeedsReallocate(texture, 1920, 1080, 87));
    }

    TEST(TestCompositorBase, NeedsReallocate_DifferentWidth) {
        MockOpenXrApi mockApi;
        MockCompositor compositor(&mockApi);

        void* texture = const_cast<void*>(
            reinterpret_cast<const void*>(MockSwapchainState::EncodeDesc(1920, 1080, 87)));

        EXPECT_TRUE(compositor.NeedsReallocate(texture, 2048, 1080, 87));
    }

    TEST(TestCompositorBase, NeedsReallocate_DifferentFormat) {
        MockOpenXrApi mockApi;
        MockCompositor compositor(&mockApi);

        void* texture = const_cast<void*>(
            reinterpret_cast<const void*>(MockSwapchainState::EncodeDesc(1920, 1080, 87)));

        EXPECT_TRUE(compositor.NeedsReallocate(texture, 1920, 1080, 88));
    }

    // ===========================================================================
    // getOrCreateSwapchainState tests
    // ===========================================================================
    TEST(TestCompositorBase, GetOrCreateSwapchainState_CreatesNew) {
        MockOpenXrApi mockApi;
        MockCompositor compositor(&mockApi);

        XrSwapchain handle = reinterpret_cast<XrSwapchain>(0x12345ULL);
        auto state = compositor.getOrCreateSwapchainState(handle);

        ASSERT_NE(state, nullptr);
        EXPECT_FALSE(state->fullFovImageAcquired);
        EXPECT_EQ(state->fullFovSwapchain, XR_NULL_HANDLE);
    }

    TEST(TestCompositorBase, GetOrCreateSwapchainState_ReturnsExisting) {
        MockOpenXrApi mockApi;
        MockCompositor compositor(&mockApi);

        XrSwapchain handle = reinterpret_cast<XrSwapchain>(0x12345ULL);
        auto state1 = compositor.getOrCreateSwapchainState(handle);
        auto state2 = compositor.getOrCreateSwapchainState(handle);

        EXPECT_EQ(state1, state2);
    }

    TEST(TestCompositorBase, GetOrCreateSwapchainState_DifferentHandles) {
        MockOpenXrApi mockApi;
        MockCompositor compositor(&mockApi);

        XrSwapchain h1 = reinterpret_cast<XrSwapchain>(0x11111ULL);
        XrSwapchain h2 = reinterpret_cast<XrSwapchain>(0x22222ULL);
        auto s1 = compositor.getOrCreateSwapchainState(h1);
        auto s2 = compositor.getOrCreateSwapchainState(h2);

        EXPECT_NE(s1, s2);
    }

    // ===========================================================================
    // evictSwapchainState tests
    // ===========================================================================
    TEST(TestCompositorBase, EvictSwapchainState_RemovesState) {
        MockOpenXrApi mockApi;
        MockCompositor compositor(&mockApi);

        XrSwapchain handle = reinterpret_cast<XrSwapchain>(0x12345);
        compositor.getOrCreateSwapchainState(handle);
        compositor.evictSwapchainState(handle);

        // After eviction, a lookup creates a fresh state
        auto state = compositor.getOrCreateSwapchainState(handle);
        EXPECT_FALSE(state->fullFovImageAcquired);
    }

    TEST(TestCompositorBase, EvictSwapchainState_NonexistentHandle) {
        MockOpenXrApi mockApi;
        MockCompositor compositor(&mockApi);

        // No-op — should not crash
        XrSwapchain handle = reinterpret_cast<XrSwapchain>(0xDEADBEEFULL);
        compositor.evictSwapchainState(handle);
    }

    // ===========================================================================
    // releaseFullFovImage tests
    // ===========================================================================
    TEST(TestCompositorBase, ReleaseFullFovImage_NoOpWhenNotAcquired) {
        MockOpenXrApi mockApi;
        MockCompositor compositor(&mockApi);

        auto state = compositor.getOrCreateSwapchainState(
            reinterpret_cast<XrSwapchain>(0x12345ULL));

        // No expectation on mock — release should be a no-op
        compositor.releaseFullFovImage(*state);
        EXPECT_FALSE(state->fullFovImageAcquired);
    }

    TEST(TestCompositorBase, ReleaseFullFovImage_CallsReleaseWhenAcquired) {
        MockOpenXrApi mockApi;
        mockApi.initializeForTesting();

        EXPECT_CALL(mockApi, xrReleaseSwapchainImage(::testing::_, ::testing::_))
            .WillOnce(::testing::Return(XR_SUCCESS));

        MockCompositor compositor(&mockApi);
        auto state = compositor.getOrCreateSwapchainState(
            reinterpret_cast<XrSwapchain>(0x12345ULL));

        // Simulate a prior acquire
        state->fullFovImageAcquired = true;
        state->fullFovSwapchain = reinterpret_cast<XrSwapchain>(0x99999ULL);

        compositor.releaseFullFovImage(*state);
        EXPECT_FALSE(state->fullFovImageAcquired);
    }

} // namespace openxr_api_layer
