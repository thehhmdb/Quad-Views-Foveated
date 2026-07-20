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
#include "logic/eye_tracker.h"
#include "logic/config.h"
#include "logic/action_manager.h"

namespace openxr_api_layer {

    // Forward-declared so friend declarations in action_manager.h resolve
    class ActionManagerTest;
    class EndToEndLogicTest;

    class ActionManagerTest : public ::testing::Test {
    protected:
        MockOpenXrApi mockApi;
        FoveationConfig config;
        EyeTracker eyeTracker{&mockApi, config};
        ActionManager actionManager{&mockApi, eyeTracker};

        static constexpr XrSession kSession = (XrSession)1;

        void SetUp() override {
            mockApi.initializeForTesting();
            // Default: no eye tracker action set injected (simplest forward path).
            eyeTracker.setEyeTrackerActionSetForTesting(XR_NULL_HANDLE);
            eyeTracker.setEyeGazeActionForTesting(XR_NULL_HANDLE);
        }
    };

    // --- pollAndSyncIfNeeded state machine ---

    TEST_F(ActionManagerTest, NotQuadViews_ReturnsWithoutCallingRuntime) {
        // When not using quad views, the manager must not touch the action system.
        EXPECT_CALL(mockApi, xrPollEvent(testing::_, testing::_)).Times(0);
        EXPECT_CALL(mockApi, xrAttachSessionActionSets(testing::_, testing::_)).Times(0);
        EXPECT_CALL(mockApi, xrSyncActions(testing::_, testing::_)).Times(0);

        actionManager.pollAndSyncIfNeeded(kSession, 200, false, EyeTracker::Tracker::EyeGazeInteraction);
    }

    TEST_F(ActionManagerTest, WrongTrackerType_ReturnsWithoutCallingRuntime) {
        EXPECT_CALL(mockApi, xrPollEvent(testing::_, testing::_)).Times(0);
        EXPECT_CALL(mockApi, xrAttachSessionActionSets(testing::_, testing::_)).Times(0);
        EXPECT_CALL(mockApi, xrSyncActions(testing::_, testing::_)).Times(0);

        actionManager.pollAndSyncIfNeeded(kSession, 200, true, EyeTracker::Tracker::SimulatedTracking);
    }

    TEST_F(ActionManagerTest, EarlyFrames_ReturnsWithoutCallingRuntime) {
        // Give the app 100 frames before forcing the action system.
        EXPECT_CALL(mockApi, xrPollEvent(testing::_, testing::_)).Times(0);
        EXPECT_CALL(mockApi, xrAttachSessionActionSets(testing::_, testing::_)).Times(0);
        EXPECT_CALL(mockApi, xrSyncActions(testing::_, testing::_)).Times(0);

        actionManager.pollAndSyncIfNeeded(kSession, 100, true, EyeTracker::Tracker::EyeGazeInteraction);
    }

    TEST_F(ActionManagerTest, After100Frames_PollsAttachesAndSyncs) {
        EXPECT_CALL(mockApi, xrPollEvent(mockApi.GetXrInstance(), testing::_)).WillOnce(testing::Return(XR_SUCCESS));
        EXPECT_CALL(mockApi, xrAttachSessionActionSets(kSession, testing::_)).WillOnce(testing::Return(XR_SUCCESS));
        EXPECT_CALL(mockApi, xrSyncActions(kSession, testing::_)).WillOnce(testing::Return(XR_SUCCESS));

        actionManager.pollAndSyncIfNeeded(kSession, 200, true, EyeTracker::Tracker::EyeGazeInteraction);

        // The attach flag is cleared after the first forced attach.
        EXPECT_FALSE(actionManager.needAttachActionSets());
        // pollAndSyncIfNeeded forces xrSyncActions every frame and does NOT clear the sync flag
        // itself (only the app's xrSyncActions override clears it), so it stays set.
        EXPECT_TRUE(actionManager.needSyncActions());
        // The poll flag is only cleared by setPollEventDone() (driven by xrPollEvent in the layer).
        EXPECT_TRUE(actionManager.needPollEvent());
    }

    TEST_F(ActionManagerTest, PollFlagPersistsUntilSetPollEventDone) {
        // First call polls (needPollEvent still set). Second call must NOT poll because we
        // simulate the app draining the event queue via setPollEventDone().
        EXPECT_CALL(mockApi, xrPollEvent(mockApi.GetXrInstance(), testing::_))
            .Times(1)
            .WillRepeatedly(testing::Return(XR_SUCCESS));
        // Attach happens only on the first call (flag cleared afterwards).
        EXPECT_CALL(mockApi, xrAttachSessionActionSets(kSession, testing::_))
            .WillOnce(testing::Return(XR_SUCCESS));
        // Sync is forced on every frame (flag is never cleared by pollAndSyncIfNeeded).
        EXPECT_CALL(mockApi, xrSyncActions(kSession, testing::_))
            .Times(2)
            .WillRepeatedly(testing::Return(XR_SUCCESS));

        actionManager.pollAndSyncIfNeeded(kSession, 200, true, EyeTracker::Tracker::EyeGazeInteraction);
        EXPECT_TRUE(actionManager.needPollEvent());

        // Simulate the app draining the event queue.
        actionManager.setPollEventDone();
        EXPECT_FALSE(actionManager.needPollEvent());

        // Second call: no poll, no attach (cleared), but still syncs (flag persists).
        actionManager.pollAndSyncIfNeeded(kSession, 201, true, EyeTracker::Tracker::EyeGazeInteraction);
        EXPECT_FALSE(actionManager.needAttachActionSets());
        EXPECT_TRUE(actionManager.needSyncActions());
    }

    TEST_F(ActionManagerTest, Reset_RestoresAllFlags) {
        EXPECT_CALL(mockApi, xrPollEvent(mockApi.GetXrInstance(), testing::_)).WillOnce(testing::Return(XR_SUCCESS));
        EXPECT_CALL(mockApi, xrAttachSessionActionSets(kSession, testing::_)).WillOnce(testing::Return(XR_SUCCESS));
        EXPECT_CALL(mockApi, xrSyncActions(kSession, testing::_)).WillOnce(testing::Return(XR_SUCCESS));

        actionManager.pollAndSyncIfNeeded(kSession, 200, true, EyeTracker::Tracker::EyeGazeInteraction);
        // Attach flag cleared; sync/poll flags persist (see After100Frames_PollsAttachesAndSyncs).
        ASSERT_FALSE(actionManager.needAttachActionSets());
        ASSERT_TRUE(actionManager.needSyncActions());
        ASSERT_TRUE(actionManager.needPollEvent());

        actionManager.reset();
        EXPECT_TRUE(actionManager.needPollEvent());
        EXPECT_TRUE(actionManager.needAttachActionSets());
        EXPECT_TRUE(actionManager.needSyncActions());
    }

    // --- Direct attach/sync injection ---

    TEST_F(ActionManagerTest, AttachSessionActionSets_ForwardsAndClearsFlag) {
        XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        EXPECT_CALL(mockApi, xrAttachSessionActionSets(kSession, testing::_)).WillOnce(testing::Return(XR_SUCCESS));

        EXPECT_EQ(actionManager.attachSessionActionSets(kSession, &attachInfo), XR_SUCCESS);
        EXPECT_FALSE(actionManager.needAttachActionSets());
    }

    TEST_F(ActionManagerTest, SyncActions_ForwardsAndClearsFlag) {
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        EXPECT_CALL(mockApi, xrSyncActions(kSession, testing::_)).WillOnce(testing::Return(XR_SUCCESS));

        EXPECT_EQ(actionManager.syncActions(kSession, &syncInfo), XR_SUCCESS);
        EXPECT_FALSE(actionManager.needSyncActions());
    }

    TEST_F(ActionManagerTest, AttachInjectsEyeTrackerActionSet) {
        // With a real action set handle, the manager suggests bindings and injects it.
        eyeTracker.setEyeTrackerActionSetForTesting((XrActionSet)1);
        eyeTracker.setEyeGazeActionForTesting((XrAction)2);

        EXPECT_CALL(mockApi, xrStringToPath(mockApi.GetXrInstance(), testing::_, testing::_))
            .Times(2)
            .WillRepeatedly(testing::Return(XR_SUCCESS));
        EXPECT_CALL(mockApi, xrSuggestInteractionProfileBindings(mockApi.GetXrInstance(), testing::_))
            .WillOnce(testing::Return(XR_SUCCESS));
        EXPECT_CALL(mockApi, xrAttachSessionActionSets(kSession, testing::_)).WillOnce(testing::Return(XR_SUCCESS));

        XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        EXPECT_EQ(actionManager.attachSessionActionSets(kSession, &attachInfo), XR_SUCCESS);
        EXPECT_FALSE(actionManager.needAttachActionSets());
    }

} // namespace openxr_api_layer
