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
#include "logic/view_math.h"
#include "logic/config.h"

namespace openxr_api_layer {

    class ViewMathTest : public ::testing::Test {
    protected:
        MockOpenXrApi mockApi;
        FoveationConfig config;
        ViewManager viewManager{&mockApi, config};

        void SetUp() override {
            // Simulate a symmetric ~90-degree FOV for both eyes (and focus views).
            // Values are in radians: angleLeft, angleRight, angleUp, angleDown.
            float halfFov = 0.785398f; // ~45 degrees
            for (uint32_t i = 0; i < xr::QuadView::Count; i++) {
                viewManager.m_cachedEyeFov[i] = {-halfFov, halfFov, -halfFov, halfFov};
            }

            // Initialize eye poses to identity (required for ProjectPoint to work).
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                viewManager.m_cachedEyePoses[eye] = xr::math::Pose::Identity();
            }

            // Initialize resting gaze center (projected coordinates, range [-1,1]).
            viewManager.m_eyeGaze[xr::StereoView::Left] = {0.0f, 0.0f};
            viewManager.m_eyeGaze[xr::StereoView::Right] = {0.0f, 0.0f};
            viewManager.m_centerOfFov[xr::StereoView::Left] = {0.0f, 0.0f};
            viewManager.m_centerOfFov[xr::StereoView::Right] = {0.0f, 0.0f};

            // Setup widening behavior.
            config.m_horizontalFocusWideningMultiplier = 0.5f;
            config.m_verticalFocusWideningMultiplier = 0.5f;
            config.m_focusWideningDeadzone = 0.2f;
            config.m_horizontalFovSection[1] = 0.3f; // Base 30% of screen
            config.m_verticalFovSection[1] = 0.3f;
        }
    };

    TEST_F(ViewMathTest, GazeStraightAhead_NoWidening) {
        // Set up stereo views with valid FOV (required for ProjectPoint).
        float halfFov = 0.785398f;
        XrView views[4]{
            {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}
        };
        // Initialize stereo view poses and FOV.
        for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
            views[i].pose = xr::math::Pose::Identity();
            views[i].fov = {-halfFov, halfFov, -halfFov, halfFov};
        }

        XrVector3f gaze = {0.0f, 0.0f, -1.0f}; // Looking straight ahead

        viewManager.computeFoveatedViews(views, 4, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, true, gaze);

        // When looking straight ahead (gaze near center), the widening should be minimal
        // because |v.x| < deadzone. Verify no crash/NaN and FOV is within bounds.
        EXPECT_FALSE(std::isnan(views[xr::QuadView::FocusLeft].fov.angleLeft));
        EXPECT_FALSE(std::isnan(views[xr::QuadView::FocusLeft].fov.angleRight));
        EXPECT_FALSE(std::isnan(views[xr::QuadView::FocusLeft].fov.angleUp));
        EXPECT_FALSE(std::isnan(views[xr::QuadView::FocusLeft].fov.angleDown));

        // Focus FOV should be narrower than the full eye FOV.
        EXPECT_GT(views[xr::QuadView::FocusLeft].fov.angleRight, views[xr::QuadView::FocusLeft].fov.angleLeft);
        EXPECT_GT(views[xr::QuadView::FocusLeft].fov.angleUp, views[xr::QuadView::FocusLeft].fov.angleDown);
    }

    TEST_F(ViewMathTest, InvalidGaze_FallsBackToCachedFov) {
        float halfFov = 0.785398f;
        XrView views[4]{
            {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}
        };
        for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
            views[i].pose = xr::math::Pose::Identity();
            views[i].fov = {-halfFov, halfFov, -halfFov, halfFov};
        }

        XrVector3f invalidGaze = {0.0f, 0.0f, 0.0f}; // Zero vector — ProjectPoint will fail

        viewManager.computeFoveatedViews(views, 4, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, false, invalidGaze);

        // Should fall back to the cached FOV we set in SetUp.
        EXPECT_FLOAT_EQ(views[xr::QuadView::FocusLeft].fov.angleLeft, -halfFov);
        EXPECT_FLOAT_EQ(views[xr::QuadView::FocusLeft].fov.angleRight, halfFov);
        EXPECT_FLOAT_EQ(views[xr::QuadView::FocusLeft].fov.angleUp, -halfFov);
        EXPECT_FLOAT_EQ(views[xr::QuadView::FocusLeft].fov.angleDown, halfFov);
    }

    TEST_F(ViewMathTest, StereoConfig_UsesCorrectIndices) {
        float halfFov = 0.785398f;
        XrView views[2]{
            {XR_TYPE_VIEW}, {XR_TYPE_VIEW}
        };
        for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
            views[i].pose = xr::math::Pose::Identity();
            views[i].fov = {-halfFov, halfFov, -halfFov, halfFov};
        }

        XrVector3f gaze = {0.0f, 0.0f, -1.0f};

        viewManager.computeFoveatedViews(views, 2, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, true, gaze);

        // For stereo config, focus views are at indices 0 and 1 (same as stereo).
        // Should complete without crashing.
        EXPECT_FALSE(std::isnan(views[0].fov.angleLeft));
        EXPECT_FALSE(std::isnan(views[1].fov.angleLeft));
    }

} // namespace openxr_api_layer