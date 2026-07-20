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
#include "view_math.h"
#include "framework/log.h"
#include "framework/util.h"
#include "views.h"

namespace openxr_api_layer {

    using namespace log;
    using namespace xr;
    using namespace xr::math;

    ViewManager::ViewManager(OpenXrApi* openXrApi, FoveationConfig& config)
        : m_openXrApi(openXrApi), m_config(config) {}

    void ViewManager::populateFovTables(XrSystemId systemId, XrSession session) {
        if (!m_needComputeBaseFov) {
            return;
        }

        // cacheStereoView() call removed: FOV and poses are now lazily provided by the caller
        // (xrLocateViews) which already has valid view data from the proper frame loop.

        XrView view[xr::StereoView::Count]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            view[eye].fov = m_cachedEyeFov[eye];
            view[eye].pose = m_cachedEyePoses[eye];

            // Calculate the "resting" gaze position.
            XrVector2f projectedGaze{};
            ProjectPoint(view[eye], {0.f, 0.f, -1.f}, projectedGaze);
            m_eyeGaze[eye] = m_centerOfFov[eye] = projectedGaze;
            m_eyeGaze[eye] = m_eyeGaze[eye] + XrVector2f{eye == xr::StereoView::Left ? -m_config.m_horizontalFixedOffset
                                                                                     : m_config.m_horizontalFixedOffset,
                                                         m_config.m_verticalFixedOffset};

            // Populate the FOV for the focus view (when no eye tracking is used).
            const XrVector2f min{std::clamp(m_eyeGaze[eye].x - m_config.m_horizontalFovSection[0], -1.f, 1.f),
                                 std::clamp(m_eyeGaze[eye].y - m_config.m_verticalFovSection[0], -1.f, 1.f)};
            const XrVector2f max{std::clamp(m_eyeGaze[eye].x + m_config.m_horizontalFovSection[0], -1.f, 1.f),
                                 std::clamp(m_eyeGaze[eye].y + m_config.m_verticalFovSection[0], -1.f, 1.f)};
            m_cachedEyeFov[eye + xr::StereoView::Count] =
                xr::math::ComputeBoundingFov(m_cachedEyeFov[eye], min, max);
        }

        {
            XrViewConfigurationView stereoViews[xr::StereoView::Count]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
            uint32_t count;
            CHECK_XRCMD(m_openXrApi->xrEnumerateViewConfigurationViews(m_openXrApi->GetXrInstance(),
                                                                     systemId,
                                                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                     xr::StereoView::Count,
                                                                     &count,
                                                                     stereoViews));
            const float newWidth =
                m_config.m_focusPixelDensity * stereoViews[xr::StereoView::Left].recommendedImageRectWidth;
            const float ratio = (float)stereoViews[xr::StereoView::Left].recommendedImageRectHeight /
                                stereoViews[xr::StereoView::Left].recommendedImageRectWidth;
            const float newHeight = newWidth * ratio;

            m_fullFovResolution.width =
                std::min((uint32_t)newWidth, stereoViews[xr::StereoView::Left].maxImageRectWidth);
            m_fullFovResolution.height =
                std::min((uint32_t)newHeight, stereoViews[xr::StereoView::Left].maxImageRectHeight);
        }

        m_needComputeBaseFov = false;
    }

    void ViewManager::computeFoveatedViews(XrView* views,
                                           uint32_t viewCount,
                                           XrViewConfigurationType viewConfigType,
                                           bool isGazeValid,
                                           const XrVector3f& gazeUnitVector) {
        // Set up the focus view or FOV tangent.
        for (uint32_t i = viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ? 0 : xr::StereoView::Count;
             i < viewCount;
             i++) {
            const uint32_t stereoViewIndex = viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ? i : (i - xr::StereoView::Count);

            views[i].pose = views[stereoViewIndex].pose;

            XrView viewForGazeProjection{};
            viewForGazeProjection.pose = m_cachedEyePoses[stereoViewIndex];
            viewForGazeProjection.fov = views[stereoViewIndex].fov;
            XrVector2f projectedGaze;
            if (IsTraceEnabled()) {
                LogDebug("  xrLocateViews[{}]: gazeUnitVector=({},{},{}), isGazeValid={}\n",
                    stereoViewIndex, gazeUnitVector.x, gazeUnitVector.y, gazeUnitVector.z, isGazeValid);
            }
            if (!isGazeValid || !ProjectPoint(viewForGazeProjection, gazeUnitVector, projectedGaze)) {
                views[i].fov = viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO
                                   ? m_cachedEyeFov[xr::StereoView::Count + i]
                                   : m_cachedEyeFov[i];
            } else {
                // Shift FOV according to the eye gaze.
                // We also widen the FOV when near the edges of the headset to make sure
                // there's enough overlap between the two eyes.
                if (IsTraceEnabled()) {
                    QVF_TRACE("xrLocateViews",
                              TLArg(i, "ViewIndex"),
                              TLArg(xr::ToString(projectedGaze).c_str(), "ProjectedGaze"));
                }
                m_eyeGaze[stereoViewIndex] = projectedGaze;
                m_eyeGaze[stereoViewIndex] = m_eyeGaze[stereoViewIndex] +
                                             XrVector2f{stereoViewIndex == xr::StereoView::Left ? -m_config.m_horizontalFocusOffset
                                                                                                  : m_config.m_horizontalFocusOffset,
                                                        m_config.m_verticalFocusOffset};
                const XrVector2f v = m_eyeGaze[stereoViewIndex] - m_centerOfFov[stereoViewIndex];
                const float horizontalFovSection =
                    m_config.m_horizontalFovSection[1] *
                    (1.f + (std::clamp(abs(v.x) - m_config.m_focusWideningDeadzone, 0.f, 1.f) *
                            m_config.m_horizontalFocusWideningMultiplier));
                const float verticalFovSection =
                    m_config.m_verticalFovSection[1] *
                    (1.f + (std::clamp(abs(v.y) - m_config.m_focusWideningDeadzone, 0.f, 1.f) *
                            m_config.m_verticalFocusWideningMultiplier));
                const XrVector2f min{std::clamp(m_eyeGaze[stereoViewIndex].x - horizontalFovSection, -1.f, 1.f),
                                     std::clamp(m_eyeGaze[stereoViewIndex].y - verticalFovSection, -1.f, 1.f)};
                const XrVector2f max{std::clamp(m_eyeGaze[stereoViewIndex].x + horizontalFovSection, -1.f, 1.f),
                                     std::clamp(m_eyeGaze[stereoViewIndex].y + verticalFovSection, -1.f, 1.f)};
                if (IsTraceEnabled()) {
                    QVF_TRACE("xrLocateViews",
                              TLArg(i, "ViewIndex"),
                              TLArg(xr::ToString(min).c_str(), "FocusTopLeft"),
                              TLArg(xr::ToString(max).c_str(), "FocusBottomRight"));
                    LogDebug("  xrLocateViews[{}]: projectedGaze=({},{}), eyeGaze=({},{}), centerOfFov=({},{}), v=({},{}), min=({},{}), max=({},{}), hSection={}, vSection={}\n",
                        stereoViewIndex,
                        projectedGaze.x, projectedGaze.y,
                        m_eyeGaze[stereoViewIndex].x, m_eyeGaze[stereoViewIndex].y,
                        m_centerOfFov[stereoViewIndex].x, m_centerOfFov[stereoViewIndex].y,
                        v.x, v.y,
                        min.x, min.y, max.x, max.y,
                        horizontalFovSection, verticalFovSection);
                }
                views[i].fov = xr::math::ComputeBoundingFov(m_cachedEyeFov[stereoViewIndex], min, max);
            }
        }
    }

    // cacheStereoView() removed: FOV and poses are now lazily provided by the caller
    // (xrLocateViews) which already has valid view data from the proper frame loop.

} // namespace openxr_api_layer
