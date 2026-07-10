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
#include <DirectXMath.h>

namespace openxr_api_layer {

    using namespace log;
    using namespace xr;
    using namespace xr::math;

    EyeTracker::EyeTracker(OpenXrApi* openXrApi, FoveationConfig& config)
        : m_openXrApi(openXrApi), m_config(config) {}

    // --- Vec3 Math Helpers ---
    XrVector3f EyeTracker::addVec3(const XrVector3f& a, const XrVector3f& b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
    XrVector3f EyeTracker::subVec3(const XrVector3f& a, const XrVector3f& b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    XrVector3f EyeTracker::scaleVec3(const XrVector3f& a, float s) {
        return {a.x * s, a.y * s, a.z * s};
    }
    float EyeTracker::lengthVec3(const XrVector3f& a) {
        return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    }
    XrVector3f EyeTracker::normalizeVec3(const XrVector3f& a) {
        float len = lengthVec3(a);
        if (len < 1e-8f) return {0, 0, -1};
        float invLen = 1.0f / len;
        return {a.x * invLen, a.y * invLen, a.z * invLen};
    }

    // --- 1-Euro Filter Helpers ---
    float EyeTracker::calcAlpha(float cutoff, float dt) {
        float tau = 1.0f / (2.0f * 3.14159265f * cutoff);
        return 1.0f / (1.0f + tau / dt);
    }

    float EyeTracker::lowPassFilter(float alpha, float prev, float current) {
        return alpha * current + (1.0f - alpha) * prev;
    }

    XrVector3f EyeTracker::lowPassFilterVec3(float alpha, const XrVector3f& prev, const XrVector3f& current) {
        return {
            lowPassFilter(alpha, prev.x, current.x),
            lowPassFilter(alpha, prev.y, current.y),
            lowPassFilter(alpha, prev.z, current.z)
        };
    }

    XrVector3f EyeTracker::filterGaze(const XrVector3f& rawGaze, XrTime time) {
        if (!m_filterInitialized || time <= m_prevGazeTime) {
            m_prevRawGaze = rawGaze;
            m_prevFilteredGaze = rawGaze;
            m_prevFilteredGazeDeriv = {0.0f, 0.0f, 0.0f};
            m_prevGazeTime = time;
            m_filterInitialized = true;
            return rawGaze;
        }

        // dt in seconds
        float dt = static_cast<float>(time - m_prevGazeTime) / 1'000'000'000.0f;
        dt = std::max(dt, 0.0001f); // Prevent division by zero

        // 1. Filter the derivative (speed) to estimate smooth speed
        XrVector3f rawDeriv = scaleVec3(subVec3(rawGaze, m_prevRawGaze), 1.0f / dt);
        float alphaDeriv = calcAlpha(m_config.m_eyeTrackingMinCutoff + 5.0f, dt); // High cutoff for speed
        XrVector3f filteredDeriv = lowPassFilterVec3(alphaDeriv, m_prevFilteredGazeDeriv, rawDeriv);

        float speed = lengthVec3(filteredDeriv);

        // 2. Determine cutoff frequency based on speed
        // When speed is high, cutoff is high (less smoothing). When low, cutoff is low (more smoothing).
        float cutoff = m_config.m_eyeTrackingMinCutoff + (m_config.m_eyeTrackingBeta * speed);

        // 3. Filter the position
        float alphaPos = calcAlpha(cutoff, dt);
        XrVector3f filteredGaze = lowPassFilterVec3(alphaPos, m_prevFilteredGaze, rawGaze);

        // Ensure the result stays on the unit sphere
        filteredGaze = normalizeVec3(filteredGaze);

        // Update state
        m_prevRawGaze = rawGaze;
        m_prevFilteredGaze = filteredGaze;
        m_prevFilteredGazeDeriv = filteredDeriv;
        m_prevGazeTime = time;

        return filteredGaze;
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
        TraceLoggingWrite(g_traceProvider, "EyeTrackerFB", TLXArg(m_eyeTrackerFB, "Handle"));
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

        TraceLoggingWrite(g_traceProvider,
                          "EyeGazeInteraction",
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
        TraceLoggingWrite(g_traceProvider,
                          "EyeTrackerFB",
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
        TraceLoggingWrite(g_traceProvider,
                          "EyeGazeInteraction",
                          TLArg(xr::ToCString(result), "Result"),
                          TLArg(!!actionStatePose.isActive, "Active"));

        if (XR_FAILED(result) || !actionStatePose.isActive) {
            return false;
        }

        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION, nullptr};
        CHECK_XRCMD(m_openXrApi->xrLocateSpace(m_eyeSpace, m_viewSpace, time, &location));
        TraceLoggingWrite(g_traceProvider, "EyeGazeInteraction", TLArg(location.locationFlags, "LocationFlags"));

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
            m_predictedGaze.reset();
            m_filterInitialized = false;
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
                // 1. Apply 1-Euro Adaptive Smoothing to the raw gaze
                XrVector3f smoothedGaze = filterGaze(unitVector, time);

                m_lastGoodEyeGaze = smoothedGaze;

                // 2. Forward prediction to counteract end-to-end system latency
                // Using smoothed derivative from the filter is much more stable than raw velocity!
                if (m_filterInitialized) {
                    float predictionTimeSec = 0.011f; // ~11ms at 90Hz

                    // Extrapolate using the smoothed derivative (velocity)
                    XrVector3f predictedVec = {
                        m_prevFilteredGaze.x + m_prevFilteredGazeDeriv.x * predictionTimeSec,
                        m_prevFilteredGaze.y + m_prevFilteredGazeDeriv.y * predictionTimeSec,
                        m_prevFilteredGaze.z + m_prevFilteredGazeDeriv.z * predictionTimeSec
                    };

                    m_predictedGaze = normalizeVec3(predictedVec);
                } else {
                    m_predictedGaze = smoothedGaze;
                }
            }
            m_loggedEyeTrackingWarning = false;
        }

        // To avoid warping during blinking, we use a reasonably recent cached gaze vector.
        bool useCache = false;
        if (!result && m_lastGoodEyeGaze) {
            // Use predicted gaze if available and within timeout, otherwise fall back to last known
            const auto timeSinceLastGood = now - m_lastGoodEyeTrackingData;
            const int64_t timeoutNs = static_cast<int64_t>(m_config.m_eyeGazeCacheTimeoutMs) * 1'000'000;
            
            if (m_predictedGaze && timeSinceLastGood.count() < timeoutNs) {
                // Use predicted gaze with decay toward center over time
                float decay = std::clamp(static_cast<float>(timeSinceLastGood.count() - 100'000'000) / 200'000'000.0f, 0.0f, 1.0f);
                // Center of FOV is (0, 0, -1)
                XrVector3f centerGaze = {0.0f, 0.0f, -1.0f};
                unitVector = {
                    m_predictedGaze->x * (1.0f - decay) + centerGaze.x * decay,
                    m_predictedGaze->y * (1.0f - decay) + centerGaze.y * decay,
                    m_predictedGaze->z * (1.0f - decay) + centerGaze.z * decay
                };
                // Normalize using 1/sqrtf (compiler optimizes to rsqrt instruction)
                float lenSq = unitVector.x * unitVector.x + unitVector.y * unitVector.y + unitVector.z * unitVector.z;
                if (lenSq > 1e-8f) {
                    float invLen = 1.0f / sqrtf(lenSq);
                    unitVector.x *= invLen;
                    unitVector.y *= invLen;
                    unitVector.z *= invLen;
                }
            } else {
                unitVector = m_lastGoodEyeGaze.value();
            }
            result = useCache = true;
        }

        TraceLoggingWrite(g_traceProvider,
                          "EyeGaze",
                          TLArg(result, "Valid"),
                          TLArg(useCache, "UsingCache"),
                          TLArg(xr::ToString(unitVector).c_str(), "GazeUnitVector"));

        return result;
    }

} // namespace openxr_api_layer
