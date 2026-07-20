// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to this software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do, or other dealings in the Software.

#include "pch.h"
#include <gtest/gtest.h>
#include "MockOpenXrApi.h"
#include "logic/eye_tracker.h"
#include "logic/config.h"
#include "logic/view_resolution.h"

namespace openxr_api_layer {

    class ViewResolutionTest : public ::testing::Test {
    protected:
        MockOpenXrApi mockApi;
        FoveationConfig config;
        ViewManager viewManager{&mockApi, config};
        EyeTracker eyeTracker{&mockApi, config};
        ViewResolutionCalculator calculator{config, viewManager, eyeTracker};

        static constexpr uint32_t kBaseW = 2000;
        static constexpr uint32_t kBaseH = 2000;

        void SetUp() override {
            mockApi.initializeForTesting();
            // No eye tracker -> foveated rendering inactive unless requested.
            eyeTracker.setType(EyeTracker::Tracker::None);
            config.m_peripheralPixelDensity = 0.5f;
            config.m_focusPixelDensity = 1.0f;
            config.m_horizontalFovSection[0] = 0.5f;
            config.m_verticalFovSection[0] = 0.45f;
            config.m_horizontalFovSection[1] = 0.35f;
            config.m_verticalFovSection[1] = 0.35f;
            config.m_preferFoveatedRendering = true;
        }
    };

    namespace {
        // Build a stereo view configuration with a known base resolution.
        XrViewConfigurationView makeStereoView(uint32_t w, uint32_t h) {
            XrViewConfigurationView v{XR_TYPE_VIEW_CONFIGURATION_VIEW};
            v.recommendedImageRectWidth = w;
            v.recommendedImageRectHeight = h;
            v.maxImageRectWidth = w * 4;
            v.maxImageRectHeight = h * 4;
            return v;
        }
    } // namespace

    TEST_F(ViewResolutionTest, QuadViews_PeripheralUsesPeripheralDensity) {
        XrViewConfigurationView stereo[2] = {makeStereoView(kBaseW, kBaseH), makeStereoView(kBaseW, kBaseH)};
        XrViewConfigurationView out[4]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

        calculator.computeViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4, stereo, out, false);

        // Peripheral (stereo) views scale by m_peripheralPixelDensity.
        EXPECT_EQ(out[xr::StereoView::Left].recommendedImageRectWidth, (uint32_t)(kBaseW * 0.5f));
        EXPECT_EQ(out[xr::StereoView::Left].recommendedImageRectHeight, (uint32_t)(kBaseH * 0.5f));
        EXPECT_EQ(out[xr::StereoView::Right].recommendedImageRectWidth, (uint32_t)(kBaseW * 0.5f));
        EXPECT_EQ(out[xr::StereoView::Right].recommendedImageRectHeight, (uint32_t)(kBaseH * 0.5f));
    }

    TEST_F(ViewResolutionTest, QuadViews_FocusUsesFovSectionAndFocusDensity) {
        XrViewConfigurationView stereo[2] = {makeStereoView(kBaseW, kBaseH), makeStereoView(kBaseW, kBaseH)};
        XrViewConfigurationView out[4]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

        calculator.computeViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4, stereo, out, false);

        // Focus views scale by m_focusPixelDensity * m_*FovSection[0] (non-foveated active).
        const uint32_t expectedW = (uint32_t)(kBaseW * config.m_focusPixelDensity * config.m_horizontalFovSection[0]);
        const uint32_t expectedH = (uint32_t)(kBaseH * config.m_focusPixelDensity * config.m_verticalFovSection[0]);
        EXPECT_EQ(out[xr::QuadView::FocusLeft].recommendedImageRectWidth, expectedW);
        EXPECT_EQ(out[xr::QuadView::FocusLeft].recommendedImageRectHeight, expectedH);
        EXPECT_EQ(out[xr::QuadView::FocusRight].recommendedImageRectWidth, expectedW);
        EXPECT_EQ(out[xr::QuadView::FocusRight].recommendedImageRectHeight, expectedH);
    }

    TEST_F(ViewResolutionTest, QuadViews_FoveatedRequestUsesFoveatedSection) {
        XrViewConfigurationView stereo[2] = {makeStereoView(kBaseW, kBaseH), makeStereoView(kBaseW, kBaseH)};
        XrViewConfigurationView out[4]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

        // Chain a foveated view configuration on the focus views to request foveated rendering.
        XrFoveatedViewConfigurationViewVARJO fov[2]{{XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO, nullptr, XR_TRUE},
                                                   {XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO, nullptr, XR_TRUE}};
        out[xr::QuadView::FocusLeft].next = &fov[0];
        out[xr::QuadView::FocusRight].next = &fov[1];

        calculator.computeViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4, stereo, out, true);

        const uint32_t expectedW = (uint32_t)(kBaseW * config.m_focusPixelDensity * config.m_horizontalFovSection[1]);
        const uint32_t expectedH = (uint32_t)(kBaseH * config.m_focusPixelDensity * config.m_verticalFovSection[1]);
        EXPECT_EQ(out[xr::QuadView::FocusLeft].recommendedImageRectWidth, expectedW);
        EXPECT_EQ(out[xr::QuadView::FocusLeft].recommendedImageRectHeight, expectedH);
    }

    TEST_F(ViewResolutionTest, QuadViews_ResolutionIsAlignedToTwo) {
        // Odd base resolution must be rounded up to an even value.
        XrViewConfigurationView stereo[2] = {makeStereoView(1001, 1003), makeStereoView(1001, 1003)};
        XrViewConfigurationView out[4]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

        calculator.computeViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4, stereo, out, false);

        EXPECT_EQ(out[xr::StereoView::Left].recommendedImageRectWidth % 2, 0u);
        EXPECT_EQ(out[xr::StereoView::Left].recommendedImageRectHeight % 2, 0u);
        EXPECT_EQ(out[xr::QuadView::FocusLeft].recommendedImageRectWidth % 2, 0u);
        EXPECT_EQ(out[xr::QuadView::FocusLeft].recommendedImageRectHeight % 2, 0u);
    }

    TEST_F(ViewResolutionTest, QuadViews_ClampedToMaxImageRect) {
        XrViewConfigurationView stereo[2] = {makeStereoView(kBaseW, kBaseH), makeStereoView(kBaseW, kBaseH)};
        // Force a tiny max so the computed resolution is clamped.
        stereo[0].maxImageRectWidth = 100;
        stereo[0].maxImageRectHeight = 100;
        stereo[1].maxImageRectWidth = 100;
        stereo[1].maxImageRectHeight = 100;

        XrViewConfigurationView out[4]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

        calculator.computeViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4, stereo, out, false);

        EXPECT_LE(out[xr::StereoView::Left].recommendedImageRectWidth, 100u);
        EXPECT_LE(out[xr::StereoView::Left].recommendedImageRectHeight, 100u);
    }

    TEST_F(ViewResolutionTest, FovTangentMode_ScalesByTangents) {
        config.m_fovTangentX = 0.8f;
        config.m_fovTangentY = 0.6f;

        XrViewConfigurationView stereo[2] = {makeStereoView(kBaseW, kBaseH), makeStereoView(kBaseW, kBaseH)};
        XrViewConfigurationView out[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

        calculator.computeViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, stereo, out, false);

        EXPECT_EQ(out[xr::StereoView::Left].recommendedImageRectWidth, (uint32_t)(kBaseW * 0.8f));
        EXPECT_EQ(out[xr::StereoView::Left].recommendedImageRectHeight, (uint32_t)(kBaseH * 0.6f));
        EXPECT_EQ(out[xr::StereoView::Right].recommendedImageRectWidth, (uint32_t)(kBaseW * 0.8f));
        EXPECT_EQ(out[xr::StereoView::Right].recommendedImageRectHeight, (uint32_t)(kBaseH * 0.6f));
    }

    TEST_F(ViewResolutionTest, FirstCallLogsResolution) {
        XrViewConfigurationView stereo[2] = {makeStereoView(kBaseW, kBaseH), makeStereoView(kBaseW, kBaseH)};
        XrViewConfigurationView out[4]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW}};

        // First call should report that resolution was logged.
        calculator.setLoggedResolution();
        calculator.computeViews(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4, stereo, out, false);
        EXPECT_TRUE(calculator.isLoggedResolution());
    }

} // namespace openxr_api_layer
