// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to this software and associated documentation files
// (the "Software"), to deal in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
// to permit persons to whom the Software is furnished to do, or other dealings in the Software.

#include "pch.h"
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MockOpenXrApi.h"
#include "logic/view_math.h"
#include "logic/eye_tracker.h"
#include "logic/view_resolution.h"
#include "logic/frame_pipeline.h"
#include "logic/config.h"

namespace openxr_api_layer {

    // Logic-level integration test: wires ViewManager + EyeTracker + ViewResolutionCalculator +
    // FramePipeline together with a MockOpenXrApi and simulates a few frames of the headless
    // (no-GPU) portion of the frame loop. This catches interaction bugs (e.g. a config change not
    // propagating to resolution) that isolated unit tests miss.
    class EndToEndLogicTest : public ::testing::Test {
    protected:
        MockOpenXrApi mockApi;
        FoveationConfig config;
        ViewManager viewManager{&mockApi, config};
        EyeTracker eyeTracker{&mockApi, config};
        ViewResolutionCalculator resolutionCalculator{config, viewManager, eyeTracker};
        FramePipeline framePipeline;

        static constexpr XrSession kSession = (XrSession)1;
        static constexpr uint32_t kBaseW = 2000;
        static constexpr uint32_t kBaseH = 2000;

        void SetUp() override {
            mockApi.initializeForTesting();
            eyeTracker.setType(EyeTracker::Tracker::SimulatedTracking);

            // Symmetric ~90-degree FOV for both eyes.
            const float halfFov = 0.785398f;
            for (uint32_t i = 0; i < xr::QuadView::Count; i++) {
                viewManager.m_cachedEyeFov[i] = {-halfFov, halfFov, -halfFov, halfFov};
            }
            for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
                viewManager.m_cachedEyePoses[eye] = xr::math::Pose::Identity();
            }
            viewManager.m_centerOfFov[xr::StereoView::Left] = {0.0f, 0.0f};
            viewManager.m_centerOfFov[xr::StereoView::Right] = {0.0f, 0.0f};
            viewManager.m_eyeGaze[xr::StereoView::Left] = {0.0f, 0.0f};
            viewManager.m_eyeGaze[xr::StereoView::Right] = {0.0f, 0.0f};

            config.m_peripheralPixelDensity = 0.5f;
            config.m_focusPixelDensity = 1.0f;
            config.m_horizontalFovSection[0] = 0.5f;
            config.m_verticalFovSection[0] = 0.45f;
            config.m_horizontalFovSection[1] = 0.35f;
            config.m_verticalFovSection[1] = 0.35f;
        }

        void TearDown() override {
            framePipeline.destroy();
        }
    };

    TEST_F(EndToEndLogicTest, FrameLoop_ComputesViewsAndResolution) {
        // 1. waitFrame -> beginFrame (normal mode, no async).
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        bool isAsyncMode = false;
        EXPECT_CALL(mockApi, xrWaitFrame(kSession, testing::_, testing::_))
            .WillOnce(testing::Return(XR_SUCCESS));
        EXPECT_CALL(mockApi, xrBeginFrame(kSession, testing::_)).WillOnce(testing::Return(XR_SUCCESS));
        EXPECT_CALL(mockApi, xrEndFrame(kSession, testing::_)).WillOnce(testing::Return(XR_SUCCESS));

        EXPECT_EQ(framePipeline.waitFrame(&mockApi, kSession, nullptr, &frameState, false, &isAsyncMode),
                  XR_SUCCESS);
        EXPECT_FALSE(isAsyncMode);
        EXPECT_EQ(framePipeline.beginFrame(&mockApi, kSession, nullptr, isAsyncMode), XR_SUCCESS);

        // 2. xrLocateViews: compute foveated views from a straight-ahead gaze.
        XrView views[4]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
            views[i].pose = xr::math::Pose::Identity();
            views[i].fov = viewManager.m_cachedEyeFov[i];
        }
        XrVector3f gaze = {0.0f, 0.0f, -1.0f};
        viewManager.computeFoveatedViews(views, 4, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, true, gaze);

        // Focus views must be valid (non-degenerate) and centered on the optical axis.
        for (uint32_t i = xr::StereoView::Count; i < xr::QuadView::Count; i++) {
            EXPECT_GT(views[i].fov.angleRight, views[i].fov.angleLeft);
            EXPECT_GT(views[i].fov.angleUp, views[i].fov.angleDown);
            EXPECT_NEAR(views[i].fov.angleLeft, -views[i].fov.angleRight, 1e-3f);
            EXPECT_NEAR(views[i].fov.angleUp, -views[i].fov.angleDown, 1e-3f);
        }

        // 3. xrEnumerateViewConfigurationViews: compute recommended resolutions.
        XrViewConfigurationView stereo[2] = {
            XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
            XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        stereo[0].recommendedImageRectWidth = kBaseW;
        stereo[0].recommendedImageRectHeight = kBaseH;
        stereo[0].maxImageRectWidth = kBaseW * 4;
        stereo[0].maxImageRectHeight = kBaseH * 4;
        stereo[1] = stereo[0];

        XrViewConfigurationView outViews[4]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                            {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                            {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                            {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        resolutionCalculator.computeViews(
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4, stereo, outViews, false);

        // Peripheral resolution scales by m_peripheralPixelDensity.
        EXPECT_EQ(outViews[xr::StereoView::Left].recommendedImageRectWidth, (uint32_t)(kBaseW * 0.5f));
        // With a SimulatedTracking eye tracker, foveated rendering is active, so the focus
        // resolution uses m_horizontalFovSection[1] (not [0]).
        EXPECT_EQ(outViews[xr::QuadView::FocusLeft].recommendedImageRectWidth,
                  (uint32_t)(kBaseW * config.m_focusPixelDensity * config.m_horizontalFovSection[1]));

        // 4. endFrame.
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        EXPECT_EQ(framePipeline.endFrame(&mockApi, kSession, &endInfo, false, &isAsyncMode), XR_SUCCESS);
    }

    TEST_F(EndToEndLogicTest, ConfigChange_PropagatesToResolution) {
        XrViewConfigurationView stereo[2] = {
            XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},
            XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        stereo[0].recommendedImageRectWidth = kBaseW;
        stereo[0].recommendedImageRectHeight = kBaseH;
        stereo[0].maxImageRectWidth = kBaseW * 4;
        stereo[0].maxImageRectHeight = kBaseH * 4;
        stereo[1] = stereo[0];

        XrViewConfigurationView outViews[4]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                            {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                            {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                            {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

        // Baseline.
        resolutionCalculator.computeViews(
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4, stereo, outViews, false);
        const uint32_t baselineFocusW = outViews[xr::QuadView::FocusLeft].recommendedImageRectWidth;

        // Change the focus density and recompute. The new resolution must reflect it.
        config.m_focusPixelDensity = 0.5f;
        resolutionCalculator.computeViews(
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4, stereo, outViews, false);
        const uint32_t newFocusW = outViews[xr::QuadView::FocusLeft].recommendedImageRectWidth;

        EXPECT_LT(newFocusW, baselineFocusW);
        // With SimulatedTracking, foveated rendering is active -> uses m_horizontalFovSection[1].
        EXPECT_EQ(newFocusW, (uint32_t)(kBaseW * 0.5f * config.m_horizontalFovSection[1]));
    }

} // namespace openxr_api_layer
