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
#include "logic/gaze_space_manager.h"
#include "logic/eye_tracker.h"
#include "logic/config.h"

namespace openxr_api_layer {

    class GazeSpaceManagerTest : public ::testing::Test {
    protected:
        MockOpenXrApi mockApi;
        FoveationConfig config;
        EyeTracker eyeTracker{&mockApi, config};
        GazeSpaceManager manager;

        static constexpr XrSpace kGazeSpace = (XrSpace)0x100;
        static constexpr XrSpace kNormalSpace = (XrSpace)0x200;
        static constexpr XrSession kSession = (XrSession)1;

        void SetUp() override {
            mockApi.initializeForTesting();
            eyeTracker.setType(EyeTracker::Tracker::SimulatedTracking);
        }
    };

    TEST_F(GazeSpaceManagerTest, NonGazeSpace_IsNotRegistered) {
        XrReferenceSpaceCreateInfo createInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        createInfo.poseInReferenceSpace = xr::math::Pose::Identity();

        EXPECT_CALL(mockApi, xrCreateReferenceSpace(kSession, testing::_, testing::_))
            .WillOnce(testing::DoAll(testing::SetArgPointee<2>(kNormalSpace), testing::Return(XR_SUCCESS)));

        XrSpace space;
        EXPECT_EQ(manager.createReferenceSpace(kSession, &createInfo, &space, &mockApi), XR_SUCCESS);
        EXPECT_EQ(space, kNormalSpace);
        EXPECT_FALSE(manager.isGazeSpace(kNormalSpace));
    }

    TEST_F(GazeSpaceManagerTest, CombinedEyeSpace_IsRegisteredAndLocatable) {
        XrReferenceSpaceCreateInfo createInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO;
        createInfo.poseInReferenceSpace = xr::math::Pose::Identity();

        EXPECT_CALL(mockApi, xrCreateReferenceSpace(kSession, testing::_, testing::_))
            .WillOnce(testing::DoAll(testing::SetArgPointee<2>(kGazeSpace), testing::Return(XR_SUCCESS)));

        XrSpace space;
        EXPECT_EQ(manager.createReferenceSpace(kSession, &createInfo, &space, &mockApi), XR_SUCCESS);
        EXPECT_TRUE(manager.isGazeSpace(kGazeSpace));

        // Locating the gaze space should succeed and report a tracked orientation.
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        EXPECT_EQ(manager.locateSpace(kGazeSpace, XR_NULL_HANDLE, 1'000'000'000, &location, eyeTracker, kSession),
                  XR_SUCCESS);
        EXPECT_EQ(location.locationFlags, XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
        EXPECT_EQ(location.pose.position.x, 0.0f);
        EXPECT_EQ(location.pose.position.y, 0.0f);
        EXPECT_EQ(location.pose.position.z, 0.0f);
        EXPECT_EQ(location.pose.orientation.x, 0.0f);
        EXPECT_EQ(location.pose.orientation.y, 0.0f);
        EXPECT_EQ(location.pose.orientation.z, 0.0f);
        EXPECT_EQ(location.pose.orientation.w, 1.0f);
    }

    TEST_F(GazeSpaceManagerTest, LocateUnknownSpace_ReturnsHandleInvalid) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        // Not a registered gaze space -> caller should fall through to OpenXrApi.
        EXPECT_EQ(manager.locateSpace(kNormalSpace, XR_NULL_HANDLE, 1'000'000'000, &location, eyeTracker, kSession),
                  XR_ERROR_HANDLE_INVALID);
    }

    TEST_F(GazeSpaceManagerTest, LocateGazeSpace_InvalidTime_ReturnsTimeInvalid) {
        XrReferenceSpaceCreateInfo createInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO;
        createInfo.poseInReferenceSpace = xr::math::Pose::Identity();

        EXPECT_CALL(mockApi, xrCreateReferenceSpace(kSession, testing::_, testing::_))
            .WillOnce(testing::DoAll(testing::SetArgPointee<2>(kGazeSpace), testing::Return(XR_SUCCESS)));

        XrSpace space;
        ASSERT_EQ(manager.createReferenceSpace(kSession, &createInfo, &space, &mockApi), XR_SUCCESS);

        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        EXPECT_EQ(manager.locateSpace(kGazeSpace, XR_NULL_HANDLE, 0, &location, eyeTracker, kSession),
                  XR_ERROR_TIME_INVALID);
    }

    TEST_F(GazeSpaceManagerTest, DestroySpace_RemovesRegistration) {
        XrReferenceSpaceCreateInfo createInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO;
        createInfo.poseInReferenceSpace = xr::math::Pose::Identity();

        EXPECT_CALL(mockApi, xrCreateReferenceSpace(kSession, testing::_, testing::_))
            .WillOnce(testing::DoAll(testing::SetArgPointee<2>(kGazeSpace), testing::Return(XR_SUCCESS)));
        EXPECT_CALL(mockApi, xrDestroySpace(kGazeSpace)).WillOnce(testing::Return(XR_SUCCESS));

        XrSpace space;
        ASSERT_EQ(manager.createReferenceSpace(kSession, &createInfo, &space, &mockApi), XR_SUCCESS);
        ASSERT_TRUE(manager.isGazeSpace(kGazeSpace));

        EXPECT_EQ(manager.destroySpace(kGazeSpace, &mockApi), XR_SUCCESS);
        EXPECT_FALSE(manager.isGazeSpace(kGazeSpace));
    }

    TEST_F(GazeSpaceManagerTest, Clear_RemovesAllSpaces) {
        XrReferenceSpaceCreateInfo createInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO;
        createInfo.poseInReferenceSpace = xr::math::Pose::Identity();

        EXPECT_CALL(mockApi, xrCreateReferenceSpace(kSession, testing::_, testing::_))
            .WillOnce(testing::DoAll(testing::SetArgPointee<2>(kGazeSpace), testing::Return(XR_SUCCESS)));

        XrSpace space;
        ASSERT_EQ(manager.createReferenceSpace(kSession, &createInfo, &space, &mockApi), XR_SUCCESS);
        ASSERT_TRUE(manager.isGazeSpace(kGazeSpace));

        manager.clear();
        EXPECT_FALSE(manager.isGazeSpace(kGazeSpace));
    }

    TEST_F(GazeSpaceManagerTest, InvalidCreateInfoType_ReturnsValidationFailure) {
        XrReferenceSpaceCreateInfo createInfo{XR_TYPE_UNKNOWN};
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO;
        XrSpace space;
        EXPECT_EQ(manager.createReferenceSpace(kSession, &createInfo, &space, &mockApi),
                  XR_ERROR_VALIDATION_FAILURE);
    }

} // namespace openxr_api_layer
