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
        ~EyeTracker();

        void setType(Tracker type) { m_trackerType = type; }
        Tracker getType() const { return m_trackerType; }
        void setPredictedDisplayPeriod(XrTime period) { m_predictedDisplayPeriod = period; }

        void initialize(XrSession session);

        bool getEyeGaze(XrSession session, XrTime time, bool getStateOnly, XrVector3f& unitVector);

        // --- Read-only action handles for ActionManager binding injection. ---
        XrActionSet eyeTrackerActionSet() const { return m_eyeTrackerActionSet; }
        XrAction eyeGazeAction() const { return m_eyeGazeAction; }

        // --- Eye-tracking health, as one cohesive operation set. ---

        /// Mark that a fresh, valid gaze sample arrived. Clears the stale warning
        /// so it can fire again on the next outage.
        void noteGoodEyeTrackingData(const XrVector3f& gaze) {
            m_lastGoodEyeTrackingData = std::chrono::steady_clock::now();
            m_lastGoodEyeGaze = gaze;
            m_loggedEyeTrackingWarning = false;
        }

        /// Mark that tracking was reset/lost (e.g. on session state change).
        void resetEyeTrackingHealth() {
            m_lastGoodEyeTrackingData = std::chrono::steady_clock::now();
            m_lastGoodEyeGaze.reset();
            m_loggedEyeTrackingWarning = false;
        }

        /// Returns true once per outage when tracking has been stale for >threshold.
        /// Self-latching: repeated calls return false until noteGood/reset is called.
        /// The optional `now` parameter enables deterministic testing.
        bool consumeStaleTrackingWarning(std::chrono::nanoseconds threshold,
                                         std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
            if (m_trackerType == Tracker::None || m_loggedEyeTrackingWarning) {
                return false;
            }
            const auto age = now - m_lastGoodEyeTrackingData;
            if (age > threshold) {
                m_loggedEyeTrackingWarning = true;
                return true;
            }
            return false;
        }

        /// Returns the most recent good gaze sample (if any).
        const std::optional<XrVector3f>& lastGoodEyeGaze() const { return m_lastGoodEyeGaze; }

        // --- Test-only seams (mutate action handles for mock setup). ---
        void setEyeTrackerActionSetForTesting(XrActionSet handle) { m_eyeTrackerActionSet = handle; }
        void setEyeGazeActionForTesting(XrAction handle) { m_eyeGazeAction = handle; }

      private:
        void initializeEyeTrackingFB(XrSession session);
        void initializeEyeGazeInteraction(XrSession session);

        bool getSimulatedTracking(XrTime time, bool getStateOnly, XrVector3f& unitVector);
        bool getEyeTrackerFB(XrTime time, bool getStateOnly, XrVector3f& unitVector);
        bool getEyeGazeInteraction(XrSession session, XrTime time, bool getStateOnly, XrVector3f& unitVector);

        // 1-Euro Filter State
        struct OneEuroFilterState {
            XrVector3f prevRawGaze{0, 0, 0};
            XrVector3f prevFilteredGaze{0, 0, 0};
            XrVector3f prevFilteredGazeDeriv{0, 0, 0};
            XrTime prevGazeTime{0};
            bool initialized{false};
        };

        OneEuroFilterState m_filterState;

        OpenXrApi* m_openXrApi{nullptr};
        FoveationConfig& m_config;
        Tracker m_trackerType{Tracker::None};
        XrTime m_predictedDisplayPeriod{0};

        XrSpace m_viewSpace{XR_NULL_HANDLE};

        XrActionSet m_eyeTrackerActionSet{XR_NULL_HANDLE};
        XrAction m_eyeGazeAction{XR_NULL_HANDLE};

        XrEyeTrackerFB m_eyeTrackerFB{XR_NULL_HANDLE};
        XrSpace m_eyeSpace{XR_NULL_HANDLE};

        std::chrono::time_point<std::chrono::steady_clock> m_lastGoodEyeTrackingData{};
        std::optional<XrVector3f> m_lastGoodEyeGaze;
        bool m_loggedEyeTrackingWarning{false};
    };

} // namespace openxr_api_layer
