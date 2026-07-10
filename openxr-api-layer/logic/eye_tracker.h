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

#pragma once

#include "pch.h"
#include "framework/dispatch.gen.h"
#include "logic/config.h"

namespace openxr_api_layer {

    class EyeTracker {
      public:
        enum Tracker {
            None = 0,
            SimulatedTracking,
            EyeTrackerFB,
            EyeGazeInteraction,
        };

        EyeTracker(OpenXrApi* openXrApi, FoveationConfig& config);

        void setType(Tracker type) { m_trackerType = type; }
        Tracker getType() const { return m_trackerType; }

        void initialize(XrSession session);

        XrSpace m_viewSpace{XR_NULL_HANDLE};

        // Action set and action handles (needed by layer.cpp for action injection)
        XrActionSet m_eyeTrackerActionSet{XR_NULL_HANDLE};
        XrAction m_eyeGazeAction{XR_NULL_HANDLE};

        // Warning state tracking
        std::chrono::time_point<std::chrono::steady_clock> m_lastGoodEyeTrackingData{};
        std::optional<XrVector3f> m_lastGoodEyeGaze;
        bool m_loggedEyeTrackingWarning{false};

        // 1-Euro Filter state for adaptive gaze smoothing
        XrVector3f m_prevRawGaze{0.0f, 0.0f, -1.0f};
        XrVector3f m_prevFilteredGaze{0.0f, 0.0f, -1.0f};
        XrVector3f m_prevFilteredGazeDeriv{0.0f, 0.0f, 0.0f};
        XrTime m_prevGazeTime{0};
        bool m_filterInitialized{false};

        // Predicted gaze for dropout handling
        std::optional<XrVector3f> m_predictedGaze;

        // Helper math functions for XrVector3f
        static XrVector3f addVec3(const XrVector3f& a, const XrVector3f& b);
        static XrVector3f subVec3(const XrVector3f& a, const XrVector3f& b);
        static XrVector3f scaleVec3(const XrVector3f& a, float s);
        static float lengthVec3(const XrVector3f& a);
        static XrVector3f normalizeVec3(const XrVector3f& a);
        static float lowPassFilter(float alpha, float prev, float current);
        static XrVector3f lowPassFilterVec3(float alpha, const XrVector3f& prev, const XrVector3f& current);
        static float calcAlpha(float cutoff, float dt);
        XrVector3f filterGaze(const XrVector3f& rawGaze, XrTime time);

        bool getEyeGaze(XrSession session, XrTime time, bool getStateOnly, XrVector3f& unitVector);

      private:
        void initializeEyeTrackingFB(XrSession session);
        void initializeEyeGazeInteraction(XrSession session);

        bool getSimulatedTracking(XrTime time, bool getStateOnly, XrVector3f& unitVector);
        bool getEyeTrackerFB(XrTime time, bool getStateOnly, XrVector3f& unitVector);
        bool getEyeGazeInteraction(XrSession session, XrTime time, bool getStateOnly, XrVector3f& unitVector);

        OpenXrApi* m_openXrApi{nullptr};
        FoveationConfig& m_config;
        Tracker m_trackerType{Tracker::None};

        XrEyeTrackerFB m_eyeTrackerFB{XR_NULL_HANDLE};
        XrSpace m_eyeSpace{XR_NULL_HANDLE};
    };

} // namespace openxr_api_layer
