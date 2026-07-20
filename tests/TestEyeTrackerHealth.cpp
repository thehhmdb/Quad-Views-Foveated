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
#include <gtest/gtest.h>
#include "MockOpenXrApi.h"
#include "logic/eye_tracker.h"
#include "logic/config.h"

namespace openxr_api_layer {

    // Forward-declared so friend declarations in eye_tracker.h resolve
    class EyeTrackerHealthTest;

    class EyeTrackerHealthTest : public ::testing::Test {
    protected:
        MockOpenXrApi mockApi;
        FoveationConfig config;
        EyeTracker tracker{&mockApi, config};

        // A fixed "now" for deterministic stale-check tests.
        std::chrono::steady_clock::time_point m_now = std::chrono::steady_clock::now();
    };

    // --- Action handle getters ---

    TEST_F(EyeTrackerHealthTest, ActionHandles_DefaultNull) {
        EXPECT_EQ(tracker.eyeTrackerActionSet(), XR_NULL_HANDLE);
        EXPECT_EQ(tracker.eyeGazeAction(), XR_NULL_HANDLE);
    }

    // --- consumeStaleTrackingWarning ---

    TEST_F(EyeTrackerHealthTest, NoWarningWhenTrackerTypeIsNone) {
        tracker.setType(EyeTracker::Tracker::None);
        EXPECT_FALSE(tracker.consumeStaleTrackingWarning(std::chrono::seconds(60), m_now));
    }

    TEST_F(EyeTrackerHealthTest, WarningFiresOnceThenLatches) {
        tracker.setType(EyeTracker::Tracker::EyeTrackerFB);
        // Zero threshold means any age > 0 is stale.
        EXPECT_TRUE(tracker.consumeStaleTrackingWarning(std::chrono::nanoseconds(0), m_now));
        // Latched — second call returns false.
        EXPECT_FALSE(tracker.consumeStaleTrackingWarning(std::chrono::nanoseconds(0), m_now));
    }

    TEST_F(EyeTrackerHealthTest, GoodDataClearsLatchedWarning) {
        tracker.setType(EyeTracker::Tracker::EyeTrackerFB);
        ASSERT_TRUE(tracker.consumeStaleTrackingWarning(std::chrono::nanoseconds(0), m_now));

        // Simulate a fresh sample — advances internal clock and clears latch.
        tracker.noteGoodEyeTrackingData({0.f, 0.f, -1.f});

        // Advance "now" past the threshold so the next check fires.
        const auto later = m_now + std::chrono::seconds(1);
        EXPECT_TRUE(tracker.consumeStaleTrackingWarning(std::chrono::nanoseconds(0), later));
    }

    TEST_F(EyeTrackerHealthTest, NoWarningWhenNotStale) {
        tracker.setType(EyeTracker::Tracker::EyeTrackerFB);
        // Record a fresh sample.
        tracker.noteGoodEyeTrackingData({0.f, 0.f, -1.f});
        // Check immediately — should not be stale.
        EXPECT_FALSE(tracker.consumeStaleTrackingWarning(std::chrono::hours(1), m_now));
    }

    // --- resetEyeTrackingHealth ---

    TEST_F(EyeTrackerHealthTest, ResetClearsGazeAndWarning) {
        tracker.setType(EyeTracker::Tracker::EyeTrackerFB);
        tracker.noteGoodEyeTrackingData({0.f, 0.f, -1.f});
        EXPECT_TRUE(tracker.lastGoodEyeGaze().has_value());

        tracker.resetEyeTrackingHealth();
        EXPECT_FALSE(tracker.lastGoodEyeGaze().has_value());
    }

    TEST_F(EyeTrackerHealthTest, ResetClearsLatch) {
        tracker.setType(EyeTracker::Tracker::EyeTrackerFB);
        ASSERT_TRUE(tracker.consumeStaleTrackingWarning(std::chrono::nanoseconds(0), m_now));

        tracker.resetEyeTrackingHealth();

        // After reset, the latch is cleared — a future stale check can fire again.
        const auto later = m_now + std::chrono::seconds(1);
        EXPECT_TRUE(tracker.consumeStaleTrackingWarning(std::chrono::nanoseconds(0), later));
    }

    // --- lastGoodEyeGaze ---

    TEST_F(EyeTrackerHealthTest, LastGoodGaze_ReturnsRecordedValue) {
        XrVector3f gaze{1.f, 2.f, 3.f};
        tracker.noteGoodEyeTrackingData(gaze);
        const auto& opt = tracker.lastGoodEyeGaze();
        ASSERT_TRUE(opt.has_value());
        EXPECT_EQ(opt->x, 1.f);
        EXPECT_EQ(opt->y, 2.f);
        EXPECT_EQ(opt->z, 3.f);
    }

} // namespace openxr_api_layer
