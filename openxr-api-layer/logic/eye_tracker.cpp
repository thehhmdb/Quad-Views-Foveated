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
#include "eye_tracker.h"
#include "framework/log.h"
#include "framework/util.h"

namespace openxr_api_layer {

    using namespace log;
    using namespace xr;
    using namespace xr::math;

    // Anonymous namespace to avoid ODR violations
    namespace {
        constexpr float kPi = 3.14159265358979323846f;

        float LowPassAlpha(float dt, float cutoff) {
            float tau = 1.0f / (2.0f * kPi * cutoff);
            return 1.0f / (1.0f + tau / dt);
        }

        XrVector3f LowPassFilter(const XrVector3f& current, const XrVector3f& previous, float alpha) {
            return {
                current.x + alpha * (previous.x - current.x),
                current.y + alpha * (previous.y - current.y),
                current.z + alpha * (previous.z - current.z)
            };
        }

        float VectorLength(const XrVector3f& v) {
            return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        }
    } // namespace

    EyeTracker::EyeTracker(OpenXrApi* openXrApi, FoveationConfig& config)
        : m_openXrApi(openXrApi), m_config(config) {}

    EyeTracker::~EyeTracker() {
        // If simulated tracking is active, release the mouse cursor clip
        // that was applied in getSimulatedTracking().
        if (m_trackerType == Tracker::SimulatedTracking) {
            ClipCursor(nullptr);
        }
    }

    void EyeTracker::initialize(XrSession session) {
        switch (m_trackerType) {
        case Tracker::EyeTrackerFB:
            initializeEyeTrackingFB(session);
            break;
        case Tracker::EyeGazeInteraction:
            initializeEyeGazeInteraction(session);
            break;
        }

        if (m_trackerType != Tracker::None) {
            XrReferenceSpaceCreateInfo spaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            spaceCreateInfo.poseInReferenceSpace = Pose::Identity();
            CHECK_XRCMD(m_openXrApi->xrCreateReferenceSpace(session, &spaceCreateInfo, &m_viewSpace));
        }
    }

    void EyeTracker::initializeEyeTrackingFB(XrSession session) {
        XrEyeTrackerCreateInfoFB createInfo{XR_TYPE_EYE_TRACKER_CREATE_INFO_FB};
        CHECK_XRCMD(m_openXrApi->xrCreateEyeTrackerFB(session, &createInfo, &m_eyeTrackerFB));
        QVF_TRACE("EyeTrackerFB", TLXArg(m_eyeTrackerFB, "Handle"));
    }

    void EyeTracker::initializeEyeGazeInteraction(XrSession session) {
        if (m_eyeTrackerActionSet == XR_NULL_HANDLE) {
            XrActionSetCreateInfo actionSetCreateInfo{XR_TYPE_ACTION_SET_CREATE_INFO, nullptr};
            strcpy_s(actionSetCreateInfo.actionSetName, "quad_views_foveated_eye_tracker");
            strcpy_s(actionSetCreateInfo.localizedActionSetName, "Eye Tracker");
            actionSetCreateInfo.priority = 0;
            CHECK_XRCMD(
                m_openXrApi->xrCreateActionSet(m_openXrApi->GetXrInstance(), &actionSetCreateInfo, &m_eyeTrackerActionSet));

            XrActionCreateInfo actionCreateInfo{XR_TYPE_ACTION_CREATE_INFO, nullptr};
            strcpy_s(actionCreateInfo.actionName, "quad_views_foveated_eye_tracker");
            strcpy_s(actionCreateInfo.localizedActionName, "Eye Tracker");
            actionCreateInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
            actionCreateInfo.countSubactionPaths = 0;
            CHECK_XRCMD(m_openXrApi->xrCreateAction(m_eyeTrackerActionSet, &actionCreateInfo, &m_eyeGazeAction));
        }

        XrActionSpaceCreateInfo actionSpaceCreateInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO, nullptr};
        actionSpaceCreateInfo.action = m_eyeGazeAction;
        actionSpaceCreateInfo.subactionPath = XR_NULL_PATH;
        actionSpaceCreateInfo.poseInActionSpace = Pose::Identity();
        CHECK_XRCMD(m_openXrApi->xrCreateActionSpace(session, &actionSpaceCreateInfo, &m_eyeSpace));

        QVF_TRACE("EyeGazeInteraction",
                  TLXArg(m_eyeTrackerActionSet, "ActionSet"),
                  TLXArg(m_eyeGazeAction, "Action"),
                  TLXArg(m_eyeSpace, "ActionSpace"));
    }

    bool EyeTracker::getSimulatedTracking(XrTime time, bool getStateOnly, XrVector3f& unitVector) {
        // Use the mouse to simulate eye tracking.
        if (!getStateOnly) {
            RECT rect;
            rect.left = 1;
            rect.right = 999;
            rect.top = 1;
            rect.bottom = 999;
            ClipCursor(&rect);

            POINT cursor{};
            GetCursorPos(&cursor);

            XrVector2f point = {(float)cursor.x / 1000.f, (float)cursor.y / 1000.f};
            unitVector = Normalize({point.x - 0.5f, 0.5f - point.y, -0.35f});
        }

        return true;
    }

    bool EyeTracker::getEyeTrackerFB(XrTime time, bool getStateOnly, XrVector3f& unitVector) {
        XrEyeGazesInfoFB eyeGazeInfo{XR_TYPE_EYE_GAZES_INFO_FB};
        eyeGazeInfo.baseSpace = m_viewSpace;
        eyeGazeInfo.time = time;

        XrEyeGazesFB eyeGaze{XR_TYPE_EYE_GAZES_FB};
        // Do not abort on eye tracking failure: degrade gracefully and let the gaze cache
        // (in getEyeGaze) smooth over transient dropouts.
        const XrResult gazeResult = m_openXrApi->xrGetEyeGazesFB(m_eyeTrackerFB, &eyeGazeInfo, &eyeGaze);
        if (XR_FAILED(gazeResult)) {
            LogWarning("xrGetEyeGazesFB failed: {}\n", xr::ToCString(gazeResult));
            return false;
        }
        QVF_TRACE("EyeTrackerFB",
                  TLArg(!!eyeGaze.gaze[xr::StereoView::Left].isValid, "LeftValid"),
                  TLArg(eyeGaze.gaze[xr::StereoView::Left].gazeConfidence, "LeftConfidence"),
                  TLArg(!!eyeGaze.gaze[xr::StereoView::Right].isValid, "RightValid"),
                  TLArg(eyeGaze.gaze[xr::StereoView::Right].gazeConfidence, "RightConfidence"));

        if (!(eyeGaze.gaze[xr::StereoView::Left].isValid && eyeGaze.gaze[xr::StereoView::Right].isValid)) {
            return false;
        }

        if (!(eyeGaze.gaze[xr::StereoView::Left].gazeConfidence > m_config.m_eyeTrackingConfidenceThreshold &&
              eyeGaze.gaze[xr::StereoView::Right].gazeConfidence > m_config.m_eyeTrackingConfidenceThreshold)) {
            return false;
        }

        if (!getStateOnly) {
            // Average the poses from both eyes.
            const auto gaze = LoadXrPose(Pose::Slerp(
                eyeGaze.gaze[xr::StereoView::Left].gazePose, eyeGaze.gaze[xr::StereoView::Right].gazePose, 0.5f));
            const auto gazeProjectedPoint =
                DirectX::XMVector3Transform(DirectX::XMVectorSet(0.f, 0.f, 1.f, 1.f), gaze);

            unitVector = Normalize(
                {gazeProjectedPoint.m128_f32[0], gazeProjectedPoint.m128_f32[1], gazeProjectedPoint.m128_f32[2]});
        }

        return true;
    }

    bool EyeTracker::getEyeGazeInteraction(XrSession session, XrTime time, bool getStateOnly, XrVector3f& unitVector) {
        XrActionStatePose actionStatePose{XR_TYPE_ACTION_STATE_POSE, nullptr};
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr};
        getInfo.action = m_eyeGazeAction;
        const XrResult result = m_openXrApi->xrGetActionStatePose(session, &getInfo, &actionStatePose);
        QVF_TRACE("EyeGazeInteraction",
                  TLArg(xr::ToCString(result), "Result"),
                  TLArg(!!actionStatePose.isActive, "Active"));

        if (XR_FAILED(result) || !actionStatePose.isActive) {
            return false;
        }

        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, nullptr};
        CHECK_XRCMD(m_openXrApi->xrLocateSpace(m_eyeSpace, m_viewSpace, time, &location));
        QVF_TRACE("EyeGazeInteraction", TLArg(location.locationFlags, "LocationFlags"));

        if (!Pose::IsPoseValid(location.locationFlags)) {
            return false;
        }

        if (!getStateOnly) {
            const auto gaze = LoadXrPose(location.pose);
            const auto gazeProjectedPoint =
                DirectX::XMVector3Transform(DirectX::XMVectorSet(0.f, 0.f, 1.f, 1.f), gaze);

            unitVector = Normalize(
                {gazeProjectedPoint.m128_f32[0], gazeProjectedPoint.m128_f32[1], gazeProjectedPoint.m128_f32[2]});
        }

        return true;
    }

    bool EyeTracker::getEyeGaze(XrSession session, XrTime time, bool getStateOnly, XrVector3f& unitVector) {
        // Clear the cache once the configured timeout has elapsed since the last good sample.
        const auto now = std::chrono::steady_clock::now();
        if ((now - m_lastGoodEyeTrackingData).count() >=
            static_cast<int64_t>(m_config.m_eyeGazeCacheTimeoutMs) * 1'000'000) {
            m_lastGoodEyeGaze.reset();
            m_filterState.initialized = false;
        }

        bool result = false;
        switch (m_trackerType) {
        case Tracker::SimulatedTracking:
            result = getSimulatedTracking(time, getStateOnly, unitVector);
            break;

        case Tracker::EyeTrackerFB:
            result = getEyeTrackerFB(time, getStateOnly, unitVector);
            break;

        case Tracker::EyeGazeInteraction:
            result = getEyeGazeInteraction(session, time, getStateOnly, unitVector);
            break;
        }

        if (result) {
            m_lastGoodEyeTrackingData = now;
            if (!getStateOnly) {
                // Apply 1-Euro Filter and Prediction
                if (!m_filterState.initialized) {
                    // First good sample: initialize filter state to raw values
                    m_filterState.prevRawGaze = unitVector;
                    m_filterState.prevFilteredGaze = unitVector;
                    m_filterState.prevFilteredGazeDeriv = {0, 0, 0};
                    m_filterState.prevGazeTime = time;
                    m_filterState.initialized = true;
                } else {
                    // Save the raw input before it gets overwritten by prediction
                    XrVector3f rawGaze = unitVector;

                    float dt = static_cast<float>(time - m_filterState.prevGazeTime) / 1e9f;
                    if (dt <= 0.0f) dt = 0.011f; // Assume 90fps if times are identical or out of order

                    // 1. Compute raw derivative (velocity)
                    XrVector3f dx = {
                        (rawGaze.x - m_filterState.prevRawGaze.x) / dt,
                        (rawGaze.y - m_filterState.prevRawGaze.y) / dt,
                        (rawGaze.z - m_filterState.prevRawGaze.z) / dt
                    };

                    // 2. Filter derivative (fixed cutoff of 1.0 Hz as per 1-Euro paper)
                    float alphaD = LowPassAlpha(dt, 1.0f);
                    m_filterState.prevFilteredGazeDeriv = LowPassFilter(dx, m_filterState.prevFilteredGazeDeriv, alphaD);

                    // 3. Adjust cutoff frequency based on speed
                    float speed = VectorLength(m_filterState.prevFilteredGazeDeriv);
                    float cutoff = m_config.m_eyeTrackingMinCutoff + m_config.m_eyeTrackingBeta * speed;

                    // 4. Filter position
                    float alphaP = LowPassAlpha(dt, cutoff);
                    XrVector3f filteredGaze = LowPassFilter(rawGaze, m_filterState.prevFilteredGaze, alphaP);

                    // 5. Apply forward prediction using stabilized derivative, scaled to refresh rate
                    float predictionTimeSec;
                    if (m_predictedDisplayPeriod > 0) {
                        predictionTimeSec = (static_cast<float>(m_predictedDisplayPeriod) / 1'000'000'000.0f) * 1.5f;
                        predictionTimeSec = std::clamp(predictionTimeSec, 0.004f, 0.025f);
                    } else {
                        predictionTimeSec = 0.011f; // Fallback for 90Hz
                    }
                    XrVector3f predictedGaze = {
                        filteredGaze.x + m_filterState.prevFilteredGazeDeriv.x * predictionTimeSec,
                        filteredGaze.y + m_filterState.prevFilteredGazeDeriv.y * predictionTimeSec,
                        filteredGaze.z + m_filterState.prevFilteredGazeDeriv.z * predictionTimeSec
                    };

                    // 6. Renormalize to unit sphere
                    float len = VectorLength(predictedGaze);
                    if (len > 0.0f) {
                        unitVector.x = predictedGaze.x / len;
                        unitVector.y = predictedGaze.y / len;
                        unitVector.z = predictedGaze.z / len;
                    } else {
                        unitVector = filteredGaze; // Fallback if length is 0
                    }

                    // Update filter state for next frame
                    m_filterState.prevFilteredGaze = filteredGaze;
                    m_filterState.prevRawGaze = rawGaze; // Use raw input, not predicted output
                    m_filterState.prevGazeTime = time;
                }

                m_lastGoodEyeGaze = unitVector;
            }
            m_loggedEyeTrackingWarning = false;
        }

        // To avoid warping during blinking, we use a reasonably recent cached gaze vector.
        bool useCache = false;
        if (!result && m_lastGoodEyeGaze) {
            unitVector = m_lastGoodEyeGaze.value();
            result = useCache = true;
        }

        QVF_TRACE("EyeGaze",
                  TLArg(result, "Valid"),
                  TLArg(useCache, "UsingCache"),
                  TLArg(xr::ToString(unitVector).c_str(), "GazeUnitVector"));

        return result;
    }

} // namespace openxr_api_layer
