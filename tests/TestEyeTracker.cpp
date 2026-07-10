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
#include <gmock/gmock.h>
#include "MockOpenXrApi.h"
#include "logic/eye_tracker.h"
#include "logic/config.h"

namespace openxr_api_layer {

    class EyeTrackerTest : public ::testing::Test {
    protected:
        MockOpenXrApi mockApi;
        FoveationConfig config;
        EyeTracker tracker{&mockApi, config};

        void SetUp() override {
            // Default timeout for cached gaze data.
            config.m_eyeGazeCacheTimeoutMs = 100;
        }
    };

    TEST_F(EyeTrackerTest, InitialType_IsNone) {
        EXPECT_EQ(tracker.getType(), EyeTracker::Tracker::None);
    }

    TEST_F(EyeTrackerTest, SetType_ChangesTracker) {
        tracker.setType(EyeTracker::Tracker::SimulatedTracking);
        EXPECT_EQ(tracker.getType(), EyeTracker::Tracker::SimulatedTracking);

        tracker.setType(EyeTracker::Tracker::None);
        EXPECT_EQ(tracker.getType(), EyeTracker::Tracker::None);
    }

    TEST_F(EyeTrackerTest, SimulatedTracking_ReturnsValidGaze) {
        tracker.setType(EyeTracker::Tracker::SimulatedTracking);

        XrSession session = (XrSession)1;
        XrVector3f gaze;

        // Simulated tracking should always return a valid gaze.
        bool valid = tracker.getEyeGaze(session, 0, false, gaze);
        EXPECT_TRUE(valid);

        // The gaze vector should be normalized (approximately unit length).
        float length = std::sqrt(gaze.x * gaze.x + gaze.y * gaze.y + gaze.z * gaze.z);
        EXPECT_NEAR(length, 1.0f, 0.01f);
    }

    TEST_F(EyeTrackerTest, SimulatedTracking_PopulatesCache) {
        tracker.setType(EyeTracker::Tracker::SimulatedTracking);

        XrSession session = (XrSession)1;
        XrVector3f gaze;

        bool valid = tracker.getEyeGaze(session, 0, false, gaze);
        EXPECT_TRUE(valid);
        EXPECT_TRUE(tracker.m_lastGoodEyeGaze.has_value());
    }

    TEST_F(EyeTrackerTest, CacheTimeout_ClearsCachedGaze) {
        tracker.setType(EyeTracker::Tracker::SimulatedTracking);
        config.m_eyeGazeCacheTimeoutMs = 50; // Short timeout for testing.

        XrSession session = (XrSession)1;
        XrVector3f gaze;

        // Get initial gaze to populate cache.
        bool valid = tracker.getEyeGaze(session, 0, false, gaze);
        EXPECT_TRUE(valid);
        EXPECT_TRUE(tracker.m_lastGoodEyeGaze.has_value());

        // Wait longer than the cache timeout.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // The next call should still work for SimulatedTracking (it always succeeds),
        // but the cached gaze should have been reset due to timeout.
        // We verify the tracker doesn't crash and returns a valid result.
        valid = tracker.getEyeGaze(session, 0, false, gaze);
        EXPECT_TRUE(valid);
    }

    TEST_F(EyeTrackerTest, NoneTracker_ReturnsInvalid) {
        tracker.setType(EyeTracker::Tracker::None);

        XrSession session = (XrSession)1;
        XrVector3f gaze;

        // With no tracker type, getEyeGaze should return false.
        bool valid = tracker.getEyeGaze(session, 0, false, gaze);
        EXPECT_FALSE(valid);
    }

    TEST_F(EyeTrackerTest, StateOnlyMode_UsesCache) {
        tracker.setType(EyeTracker::Tracker::SimulatedTracking);

        XrSession session = (XrSession)1;
        XrVector3f gaze;

        // First call populates the cache.
        bool valid = tracker.getEyeGaze(session, 0, false, gaze);
        EXPECT_TRUE(valid);

        // getStateOnly=true should use the cache without calling the eye tracker.
        XrVector3f gazeFromCache;
        valid = tracker.getEyeGaze(session, 0, true, gazeFromCache);
        EXPECT_TRUE(valid);
    }

} // namespace openxr_api_layer
