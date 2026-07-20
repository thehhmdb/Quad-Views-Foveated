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
#include "logic/focus_fov_quirk.h"
#include "views.h"

namespace openxr_api_layer {

    TEST(FocusFovQuirkTest, StoreLookupRoundTrip) {
        FocusFovQuirk q;
        XrFovf left{-0.5f, 0.5f, 0.5f, -0.5f};
        XrFovf right{-0.4f, 0.6f, 0.4f, -0.6f};

        q.storeFov(1234, left, right);

        XrFovf out{};
        ASSERT_TRUE(q.lookupFov(1234, xr::QuadView::FocusLeft, out));
        EXPECT_FLOAT_EQ(out.angleLeft, left.angleLeft);

        ASSERT_TRUE(q.lookupFov(1234, xr::QuadView::FocusRight, out));
        EXPECT_FLOAT_EQ(out.angleRight, right.angleRight);
    }

    TEST(FocusFovQuirkTest, AgeRemovesOldEntries) {
        FocusFovQuirk q;
        q.storeFov(1'000'000'000, {}, {}); // 1s
        q.storeFov(2'500'000'000, {}, {}); // 2.5s

        q.ageOldEntries(2'500'000'000); // Cutoff is 1.5s

        XrFovf out{};
        EXPECT_FALSE(q.lookupFov(1'000'000'000, 0, out)); // evicted
        EXPECT_TRUE(q.lookupFov(2'500'000'000, 0, out));  // kept
    }

} // namespace openxr_api_layer
