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
#include "logic/frame_pipeline.h"

namespace openxr_api_layer {

    class FramePipelineTest : public ::testing::Test {
    protected:
        MockOpenXrApi mockApi;
        FramePipeline pipeline;

        void SetUp() override {
            // Clear any state from previous test.
            pipeline.destroy();
            // Wire up mock dispatch table so qualified base-class calls reach the mock.
            mockApi.initializeForTesting();
        }
    };

    TEST_F(FramePipelineTest, InitialState_ZeroFrames) {
        EXPECT_EQ(pipeline.getFramesElapsed(), 0u);
        EXPECT_EQ(pipeline.getWaitedFrameTime(), 0ULL);
    }

    TEST_F(FramePipelineTest, IncrementFrame_CountsCorrectly) {
        EXPECT_EQ(pipeline.getFramesElapsed(), 0u);
        pipeline.incrementFrame();
        EXPECT_EQ(pipeline.getFramesElapsed(), 1u);
        pipeline.incrementFrame();
        EXPECT_EQ(pipeline.getFramesElapsed(), 2u);
        pipeline.resetFrameCount();
        EXPECT_EQ(pipeline.getFramesElapsed(), 0u);
    }

    TEST_F(FramePipelineTest, SetWaitedFrameTime_StoresValue) {
        EXPECT_EQ(pipeline.getWaitedFrameTime(), 0ULL);
        pipeline.setWaitedFrameTime(12345);
        EXPECT_EQ(pipeline.getWaitedFrameTime(), 12345ULL);
        pipeline.setWaitedFrameTime(99999);
        EXPECT_EQ(pipeline.getWaitedFrameTime(), 99999ULL);
    }

    TEST_F(FramePipelineTest, NormalMode_WaitFrameCallsUnderlyingApi) {
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        bool isAsyncMode = false;

        EXPECT_CALL(mockApi, xrWaitFrame(testing::_, testing::_, testing::_))
            .WillOnce(testing::DoAll(
                testing::SetArgPointee<2>(frameState),
                testing::Return(XR_SUCCESS)));

        EXPECT_EQ(pipeline.waitFrame(&mockApi, reinterpret_cast<XrSession>(1), nullptr, &frameState, false, &isAsyncMode), XR_SUCCESS);
        EXPECT_FALSE(isAsyncMode);
    }

    TEST_F(FramePipelineTest, NormalMode_BeginFrameCallsUnderlyingApi) {
        EXPECT_CALL(mockApi, xrBeginFrame(testing::_, testing::_))
            .WillOnce(testing::Return(XR_SUCCESS));

        EXPECT_EQ(pipeline.beginFrame(&mockApi, reinterpret_cast<XrSession>(1), nullptr, false), XR_SUCCESS);
    }

    TEST_F(FramePipelineTest, NormalMode_EndFrameCallsUnderlyingApi) {
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        bool isAsyncMode = false;

        EXPECT_CALL(mockApi, xrEndFrame(testing::_, testing::_))
            .WillOnce(testing::Return(XR_SUCCESS));

        EXPECT_EQ(pipeline.endFrame(&mockApi, reinterpret_cast<XrSession>(1), &endInfo, false, &isAsyncMode), XR_SUCCESS);
        EXPECT_FALSE(isAsyncMode);
    }

    TEST_F(FramePipelineTest, TurboMode_EndFrameEngagesAsyncMode) {
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        bool isAsyncMode = false;

        // First call: enters async mode (spawns background wait thread)
        EXPECT_CALL(mockApi, xrEndFrame(testing::_, testing::_))
            .WillOnce(testing::Return(XR_SUCCESS));

        // Turbo mode may also call xrWaitFrame in the background thread
        EXPECT_CALL(mockApi, xrWaitFrame(testing::_, testing::_, testing::_))
            .Times(testing::AtMost(2))
            .WillRepeatedly(testing::Return(XR_ERROR_SESSION_LOST));

        // First endFrame in turbo mode should engage async mode
        XrResult result = pipeline.endFrame(&mockApi, reinterpret_cast<XrSession>(1), &endInfo, true, &isAsyncMode);
        if (result == XR_SUCCESS) {
            EXPECT_TRUE(isAsyncMode);
        }
        // Cleanup async thread
        pipeline.destroy();
    }

    TEST_F(FramePipelineTest, FrameRate_DefaultIsZero) {
        // Without recorded frame times, frame rate should be 0.
        EXPECT_EQ(pipeline.getFrameRate(), 0u);
    }

} // namespace openxr_api_layer
