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
#include "logic/view_resolution.h"
#include "framework/log.h"
#include "framework/util.h"
#include "views.h"

namespace openxr_api_layer {

    using namespace log;
    using namespace xr;
    using namespace xr::math;

    ViewResolutionCalculator::ViewResolutionCalculator(FoveationConfig& config, ViewManager& viewManager, EyeTracker& eyeTracker)
        : m_config(config), m_viewManager(viewManager), m_eyeTracker(eyeTracker) {
    }

    bool ViewResolutionCalculator::computeViews(XrViewConfigurationType viewConfigurationType,
                                                 uint32_t viewCountOutput,
                                                 const XrViewConfigurationView* stereoViews,
                                                 XrViewConfigurationView* outViews,
                                                 bool requestedFoveatedRendering) {
        XrExtent2Di stereoResolution = {
            (int32_t)stereoViews[xr::StereoView::Left].recommendedImageRectWidth,
            (int32_t)stereoViews[xr::StereoView::Left].recommendedImageRectHeight};

        // Override default to specify whether foveated rendering is desired when the application does
        // not specify.
        bool foveatedRenderingActive = m_eyeTracker.getType() != EyeTracker::Tracker::None && m_config.m_preferFoveatedRendering;

        // When foveated rendering extension is active, look whether the application is requesting it
        // for the views. The spec is a little questionable and calls for each view to have the flag
        // specified. Here we check that at least one view has the flag on.
        if (requestedFoveatedRendering) {
            for (uint32_t i = 0; i < viewCountOutput; i++) {
                const XrFoveatedViewConfigurationViewVARJO* foveatedViewConfiguration =
                    reinterpret_cast<const XrFoveatedViewConfigurationViewVARJO*>(outViews[i].next);
                while (foveatedViewConfiguration) {
                    if (foveatedViewConfiguration->type ==
                        XR_TYPE_FOVEATED_VIEW_CONFIGURATION_VIEW_VARJO) {
                        foveatedRenderingActive = foveatedRenderingActive ||
                                                  foveatedViewConfiguration->foveatedRenderingActive;
                        break;
                    }
                    foveatedViewConfiguration =
                        reinterpret_cast<const XrFoveatedViewConfigurationViewVARJO*>(
                            foveatedViewConfiguration->next);
                }
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateViewConfigurationViews",
                              TLArg(foveatedRenderingActive, "FoveatedRenderingActive"));
        }

        const float basePixelDensity = stereoViews[xr::StereoView::Left].recommendedImageRectWidth /
                                       (-m_viewManager.m_cachedEyeFov[xr::StereoView::Left].angleLeft +
                                        m_viewManager.m_cachedEyeFov[xr::StereoView::Left].angleRight);

        for (uint32_t i = 0; i < viewCountOutput; i++) {
            uint32_t referenceFovIndex = i;

            // When using quad views, we use 2 peripheral views with lower pixel densities, and 2 focus
            // views with higher pixel densities.
            float pixelDensityMultiplier = m_config.m_peripheralPixelDensity;
            if (i >= xr::StereoView::Count) {
                pixelDensityMultiplier = m_config.m_focusPixelDensity;
                if (foveatedRenderingActive) {
                    referenceFovIndex = i + 2;
                }
            }

            float newWidth, newHeight;
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                if (i < xr::StereoView::Count) {
                    newWidth = pixelDensityMultiplier *
                               stereoViews[i % xr::StereoView::Count].recommendedImageRectWidth;
                    newHeight = pixelDensityMultiplier *
                                stereoViews[i % xr::StereoView::Count].recommendedImageRectHeight;
                } else {
                    newWidth = pixelDensityMultiplier *
                               m_config.m_horizontalFovSection[foveatedRenderingActive ? 1 : 0] *
                               stereoViews[i % xr::StereoView::Count].recommendedImageRectWidth;
                    newHeight = pixelDensityMultiplier *
                                m_config.m_verticalFovSection[foveatedRenderingActive ? 1 : 0] *
                                stereoViews[i % xr::StereoView::Count].recommendedImageRectHeight;
                }
            } else {
                // Apply FOV tangent.
                newWidth = m_config.m_fovTangentX * stereoViews[i].recommendedImageRectWidth;
                newHeight = m_config.m_fovTangentY * stereoViews[i].recommendedImageRectHeight;
            }

            outViews[i] = stereoViews[i % xr::StereoView::Count];
            outViews[i].recommendedImageRectWidth =
                std::min(AlignTo<2>((uint32_t)newWidth), outViews[i].maxImageRectWidth);
            outViews[i].recommendedImageRectHeight =
                std::min(AlignTo<2>((uint32_t)newHeight), outViews[i].maxImageRectHeight);
        }

        if (!m_loggedResolution) {
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                LogInformation("Recommended peripheral resolution: {}x{} ({:.3f}x density)\n",
                                outViews[xr::StereoView::Left].recommendedImageRectWidth,
                                outViews[xr::StereoView::Left].recommendedImageRectHeight,
                                m_config.m_peripheralPixelDensity);
                LogInformation("Recommended focus resolution: {}x{} ({:.3f}x density)\n",
                                outViews[xr::QuadView::FocusLeft].recommendedImageRectWidth,
                                outViews[xr::QuadView::FocusLeft].recommendedImageRectHeight,
                                m_config.m_focusPixelDensity);
            } else {
                LogInformation("Recommended resolution: {}x{} ({:.3f}/{:.3f} tangents)\n",
                                outViews[xr::StereoView::Left].recommendedImageRectWidth,
                                outViews[xr::StereoView::Left].recommendedImageRectHeight,
                                m_config.m_fovTangentX,
                                m_config.m_fovTangentY);
            }

            const int32_t stereoPixelsCount =
                xr::StereoView::Count * stereoResolution.width * stereoResolution.height;
            LogInformation("  Stereo pixel count was: {:L} ({}x{})\n",
                            stereoPixelsCount,
                            stereoResolution.width,
                            stereoResolution.height);

            uint32_t newPixelsCount;
            if (viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                newPixelsCount = xr::StereoView::Count *
                                 (outViews[xr::StereoView::Left].recommendedImageRectWidth *
                                      outViews[xr::StereoView::Left].recommendedImageRectHeight +
                                  outViews[xr::QuadView::FocusLeft].recommendedImageRectWidth *
                                      outViews[xr::QuadView::FocusLeft].recommendedImageRectHeight);
                LogInformation("  Quad views pixel count is: {:L}\n", newPixelsCount);
            } else {
                newPixelsCount = xr::StereoView::Count *
                                 outViews[xr::StereoView::Left].recommendedImageRectWidth *
                                 outViews[xr::StereoView::Left].recommendedImageRectHeight;
                LogInformation("  FOV tangents pixel count is: {:L}\n", newPixelsCount);
            }

            LogInformation("  Savings: -{:.1f}%%\n",
                            100.f * (1.f - (float)newPixelsCount / stereoPixelsCount));

            m_loggedResolution = true;
        }

        return true;
    }

} // namespace openxr_api_layer
