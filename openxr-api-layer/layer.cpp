// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Based on https://github.com/mbucchia/OpenXR-Layer-Template.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "layer.h"
#include <log.h>
#include <util.h>

#include "views.h"
#include "compositor.h"
#include "d3d11_compositor.h"
#include "d3d12_compositor.h"
#include "logic/config.h"
#include "logic/view_math.h"
#include "logic/eye_tracker.h"
#include "logic/swapchain_manager.h"
#include "logic/graphics_context.h"
#include "logic/frame_pipeline.h"
#include "logic/debug_keys.h"
#include "logic/focus_fov_quirk.h"
#include "logic/action_manager.h"
#include "logic/view_resolution.h"
#include "logic/layer_composer.h"
#include "logic/gaze_space_manager.h"
#include "logic/swapchain_interceptor.h"

namespace openxr_api_layer {

    // Define the global unloading flag (declared as extern in pch.h)
    bool g_isUnloading = false;

    using namespace log;
    using namespace xr::math;
    using namespace openxr_api_layer::utils;

    // Session state context - groups all session-related state variables
    struct SessionContext {
        std::string applicationExecutableName;
        std::string runtimeName;
        std::string systemName;

        bool bypassApiLayer{false};
        bool requestedQuadViews{false};
        bool useQuadViews{false};
        bool requestedFoveatedRendering{false};
        bool requestedDepthSubmission{false};
        bool requestedD3D11{false};
        bool requestedD3D12{false};
        bool useFovTangent{false};
        bool isSupportedGraphicsApi{false};

        XrSystemId systemId{XR_NULL_SYSTEM_ID};
        XrSession session{XR_NULL_HANDLE};
    };

    // Our API layer implement these extensions, and their specified version.
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {
        {XR_VARJO_QUAD_VIEWS_EXTENSION_NAME, 1}, {XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME, 3}};

    // Initialize these vectors with arrays of extensions to block and implicitly request for the instance.
    const std::vector<std::string> blockedExtensions = {XR_VARJO_QUAD_VIEWS_EXTENSION_NAME,
                                                        XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME};
    const std::vector<std::string> implicitExtensions = {XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
                                                         XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME};

    // This class implements our API layer.
    class OpenXrLayer : public openxr_api_layer::OpenXrApi {
      public:
        OpenXrLayer() : m_viewManager(this, m_config), m_eyeTracker(this, m_config),
                         m_swapchainManager(), m_graphicsContext(), m_framePipeline(),
                         m_debugKeyHandler(m_config),
                         m_actionManager(this, m_eyeTracker),
                         m_viewResolutionCalculator(m_config, m_viewManager, m_eyeTracker),
                         m_layerComposer(this, m_config, m_viewManager, m_swapchainManager,
                                         m_graphicsContext, m_eyeTracker, m_focusFovQuirk),
                         m_gazeSpaceManager(),
                         m_swapchainInterceptor(m_swapchainManager, m_graphicsContext) {}
        ~OpenXrLayer() = default;

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetInstanceProcAddr
        XrResult xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) override {
            QVF_TRACE("xrGetInstanceProcAddr",
                      TLXArg(instance, "Instance"),
                      TLArg(name, "Name"),
                      TLArg(m_ctx.bypassApiLayer, "Bypass"));

            XrResult result = m_ctx.bypassApiLayer ? m_xrGetInstanceProcAddr(instance, name, function)
                                                   : OpenXrApi::xrGetInstanceProcAddr(instance, name, function);

            QVF_TRACE("xrGetInstanceProcAddr", TLPArg(*function, "Function"));

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateInstance
        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            // Needed to resolve the requested function pointers.
            OpenXrApi::xrCreateInstance(createInfo);

            // Dump the application name, OpenXR runtime information and other useful things for debugging.
            QVF_TRACE("xrCreateInstance",
                      TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                      TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                      TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                      TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                      TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                      TLArg(createInfo->createFlags, "CreateFlags"));
            {
                char path[_MAX_PATH];
                GetModuleFileNameA(nullptr, path, sizeof(path));
                std::string_view fullPath(path);
                size_t offset = fullPath.rfind('\\');
                if (offset != std::string::npos) {
                    m_ctx.applicationExecutableName = fullPath.substr(offset + 1);
                } else {
                    m_ctx.applicationExecutableName = fullPath;
                }
            }
            LogInformation(
                "Application: {} ({})\n", createInfo->applicationInfo.applicationName, GetApplicationExecutableName());

            for (uint32_t i = 0; i < createInfo->enabledApiLayerCount; i++) {
                QVF_TRACE("xrCreateInstance", TLArg(createInfo->enabledApiLayerNames[i], "ApiLayerName"));
            }

            for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++) {
                const std::string_view ext(createInfo->enabledExtensionNames[i]);
                QVF_TRACE("xrCreateInstance", TLArg(ext.data(), "ExtensionName"));
                if (ext == XR_VARJO_QUAD_VIEWS_EXTENSION_NAME) {
                    m_ctx.requestedQuadViews = true;
                } else if (ext == XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME) {
                    m_ctx.requestedFoveatedRendering = true;
                } else if (ext == XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) {
                    m_ctx.requestedDepthSubmission = true;
                } else if (ext == XR_KHR_D3D11_ENABLE_EXTENSION_NAME) {
                    m_ctx.requestedD3D11 = true;
                } else if (ext == XR_KHR_D3D12_ENABLE_EXTENSION_NAME) {
                    m_ctx.requestedD3D12 = true;
                }
            }

            if (!m_ctx.requestedQuadViews) {
                m_ctx.requestedFoveatedRendering = false;
            }

            // We support D3D11 and D3D12.
            m_ctx.bypassApiLayer = !(m_ctx.requestedD3D11 || m_ctx.requestedD3D12);
            if (m_ctx.bypassApiLayer) {
                LogInformation("{} layer will be bypassed\n", LayerName);
                return XR_SUCCESS;
            }

            XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
            CHECK_XRCMD(OpenXrApi::xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
            m_ctx.runtimeName = instanceProperties.runtimeName;
            const auto runtimeName = fmt::format("{} {}.{}.{}",
                                                 instanceProperties.runtimeName,
                                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion));
            QVF_TRACE("xrCreateInstance", TLArg(runtimeName.c_str(), "RuntimeName"));
            LogInformation("Using OpenXR runtime: {}\n", runtimeName);

            // Platform-specific quirks.
            m_swapchainManager.setDeferredReleaseQuirk(runtimeName.find("Varjo") != std::string::npos);
            return XR_SUCCESS;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystem
        XrResult xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) override {
            if (getInfo->type != XR_TYPE_SYSTEM_GET_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            QVF_TRACE("xrGetSystem",
                      TLXArg(instance, "Instance"),
                      TLArg(xr::ToCString(getInfo->formFactor), "FormFactor"));

            const XrResult result = OpenXrApi::xrGetSystem(instance, getInfo, systemId);

            if (XR_SUCCEEDED(result) && getInfo->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
                if (*systemId != m_ctx.systemId) {
                    // Check if the system supports eye tracking.
                    XrSystemEyeGazeInteractionPropertiesEXT eyeGazeInteractionProperties{
                        XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
                    XrSystemEyeTrackingPropertiesFB eyeTrackingProperties{XR_TYPE_SYSTEM_EYE_TRACKING_PROPERTIES_FB,
                                                                          &eyeGazeInteractionProperties};
                    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                    systemProperties.next = &eyeTrackingProperties;
                    CHECK_XRCMD(OpenXrApi::xrGetSystemProperties(instance, *systemId, &systemProperties));
                    m_ctx.systemName = systemProperties.systemName;
                    QVF_TRACE("xrGetSystem",
                              TLArg(systemProperties.systemName, "SystemName"),
                              TLArg(eyeGazeInteractionProperties.supportsEyeGazeInteraction, "SupportsEyeGazeInteraction"),
                              TLArg(eyeTrackingProperties.supportsEyeTracking, "SupportsEyeTracking"));
                    LogInformation("Using OpenXR system: {}\n", systemProperties.systemName);

                    // Parse the configuration. Load the file shipped with the layer first, followed by the file the
                    // users may edit.
                    m_config.m_runtimeName = m_ctx.runtimeName;
                    m_config.m_systemName = m_ctx.systemName;
                    m_config.m_applicationName = GetApplicationName();
                    m_config.m_applicationExecutableName = GetApplicationExecutableName();
                    m_config.LoadConfiguration(dllHome / "settings.cfg");
                    m_config.LoadConfiguration(localAppData / "settings.cfg");
                    m_ctx.useFovTangent = m_config.m_fovTangentX != 1.f || m_config.m_fovTangentY != 1.f;

                    if (m_swapchainManager.getDeferredReleaseQuirk() && m_config.m_useTurboMode) {
                        LogInformation("Denying Turbo Mode due to deferred swapchain release!\n");
                        m_config.m_useTurboMode = false;
                    }

                    // Apply config-driven quirks
                    m_focusFovQuirk.setEnabled(m_config.m_needFocusFovCorrectionQuirk);

                    QVF_TRACE("xrGetSystem",
                              TLArg(m_config.m_peripheralPixelDensity, "PeripheralResolutionFactor"),
                              TLArg(m_config.m_focusPixelDensity, "FocusResolutionFactor"),
                              TLArg(m_config.m_horizontalFovSection[0], "FixedHorizontalSection"),
                              TLArg(m_config.m_verticalFovSection[0], "FixedVerticalSection"),
                              TLArg(m_config.m_horizontalFovSection[1], "FoveatedHorizontalSection"),
                              TLArg(m_config.m_verticalFovSection[1], "FoveatedVerticalSection"),
                              TLArg(m_config.m_horizontalFixedOffset, "FixedHorizontalOffset"),
                              TLArg(m_config.m_verticalFixedOffset, "FixedVerticalOffset"),
                              TLArg(m_config.m_horizontalFocusOffset, "FoveatedHorizontalOffset"),
                              TLArg(m_config.m_verticalFocusOffset, "FoveatedVerticalOffset"),
                              TLArg(m_config.m_horizontalFocusWideningMultiplier, "HorizontalFocusWideningMultiplier"),
                              TLArg(m_config.m_verticalFocusWideningMultiplier, "VerticalFocusWideningMultiplier"),
                              TLArg(m_config.m_focusWideningDeadzone, "FocusWideningDeadzone"),
                              TLArg(m_config.m_preferFoveatedRendering, "PreferFoveatedRendering"),
                              TLArg(m_config.m_forceNoEyeTracking, "ForceNoEyeTracking"),
                              TLArg(m_config.m_smoothenFocusViewEdges, "SmoothenEdges"),
                              TLArg(m_config.m_sharpenFocusView, "SharpenFocusView"),
                              TLArg(m_config.m_fovTangentX, "FovTangentX"),
                              TLArg(m_config.m_fovTangentY, "FovTangentY"),
                              TLArg(m_config.m_useTurboMode, "TurboMode"));

                    m_eyeTracker.setType(EyeTracker::Tracker::None);
                    if (m_ctx.requestedQuadViews) {
                        if (!m_config.m_forceNoEyeTracking) {
                            if (m_config.m_debugSimulateTracking) {
                                m_eyeTracker.setType(EyeTracker::Tracker::SimulatedTracking);
                            } else if (eyeGazeInteractionProperties.supportsEyeGazeInteraction) {
                                // Prefer the eye gaze interaction extension over the social eye tracking extension.
                                m_eyeTracker.setType(EyeTracker::Tracker::EyeGazeInteraction);
                            } else if (eyeTrackingProperties.supportsEyeTracking) {
                                // Last resort if the "social eye tracking".
                                m_eyeTracker.setType(EyeTracker::Tracker::EyeTrackerFB);
                            }
                        }

                        LogInformation("Eye tracking is {}\n",
                                        m_eyeTracker.getType() != EyeTracker::Tracker::None ? "supported" : "not supported");
                    }
                }

                m_ctx.systemId = *systemId;
            }

            QVF_TRACE("xrGetSystem", TLArg((int)*systemId, "SystemId"));

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystemProperties
        XrResult xrGetSystemProperties(XrInstance instance,
                                       XrSystemId systemId,
                                       XrSystemProperties* properties) override {
            QVF_TRACE("xrGetSystemProperties",
                      TLXArg(instance, "Instance"),
                      TLArg((int)systemId, "SystemId"));

            const XrResult result = OpenXrApi::xrGetSystemProperties(instance, systemId, properties);

            if (XR_SUCCEEDED(result)) {
                if (isSystemHandled(systemId) && m_ctx.requestedFoveatedRendering) {
                    XrSystemFoveatedRenderingPropertiesVARJO* foveatedProperties =
                        reinterpret_cast<XrSystemFoveatedRenderingPropertiesVARJO*>(properties->next);
                    while (foveatedProperties) {
                        if (foveatedProperties->type == XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO) {
                            foveatedProperties->supportsFoveatedRendering =
                                m_eyeTracker.getType() != EyeTracker::Tracker::None ? XR_TRUE : XR_FALSE;

                            QVF_TRACE("xrGetSystemProperties",
                                      TLArg(!!foveatedProperties->supportsFoveatedRendering, "SupportsFoveatedRendering"));
                            break;
                        }
                        foveatedProperties =
                            reinterpret_cast<XrSystemFoveatedRenderingPropertiesVARJO*>(foveatedProperties->next);
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurations
        XrResult xrEnumerateViewConfigurations(XrInstance instance,
                                               XrSystemId systemId,
                                               uint32_t viewConfigurationTypeCapacityInput,
                                               uint32_t* viewConfigurationTypeCountOutput,
                                               XrViewConfigurationType* viewConfigurationTypes) override {
            QVF_TRACE("xrEnumerateViewConfigurations",
                      TLXArg(instance, "Instance"),
                      TLArg((int)systemId, "SystemId"),
                      TLArg(viewConfigurationTypeCapacityInput, "ViewConfigurationTypeCapacityInput"));

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (isSystemHandled(systemId) && m_ctx.requestedQuadViews && !m_config.m_unadvertiseQuadViews) {
                if (viewConfigurationTypeCapacityInput) {
                    result = OpenXrApi::xrEnumerateViewConfigurations(instance,
                                                                      systemId,
                                                                      viewConfigurationTypeCapacityInput - 1,
                                                                      viewConfigurationTypeCountOutput,
                                                                      viewConfigurationTypes + 1);
                    if (XR_SUCCEEDED(result)) {
                        // Prepend (since we prefer quad views).
                        viewConfigurationTypes[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
                        (*viewConfigurationTypeCountOutput)++;
                    }
                } else {
                    result = OpenXrApi::xrEnumerateViewConfigurations(
                        instance, systemId, 0, viewConfigurationTypeCountOutput, nullptr);
                    if (XR_SUCCEEDED(result)) {
                        (*viewConfigurationTypeCountOutput)++;
                    }
                }
            } else {
                result = OpenXrApi::xrEnumerateViewConfigurations(instance,
                                                                  systemId,
                                                                  viewConfigurationTypeCapacityInput,
                                                                  viewConfigurationTypeCountOutput,
                                                                  viewConfigurationTypes);
            }

            if (XR_SUCCEEDED(result)) {
                QVF_TRACE("xrEnumerateViewConfigurations",
                          TLArg(*viewConfigurationTypeCountOutput, "ViewConfigurationTypeCountOutput"));

                if (viewConfigurationTypeCapacityInput && viewConfigurationTypes) {
                    for (uint32_t i = 0; i < *viewConfigurationTypeCountOutput; i++) {
                        QVF_TRACE("xrEnumerateViewConfigurations",
                                  TLArg(xr::ToCString(viewConfigurationTypes[i]), "ViewConfigurationType"));
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurationViews
        XrResult xrEnumerateViewConfigurationViews(XrInstance instance,
                                                   XrSystemId systemId,
                                                   XrViewConfigurationType viewConfigurationType,
                                                   uint32_t viewCapacityInput,
                                                   uint32_t* viewCountOutput,
                                                   XrViewConfigurationView* views) override {
            QVF_TRACE("xrEnumerateViewConfigurationViews",
                      TLXArg(instance, "Instance"),
                      TLArg((int)systemId, "SystemId"),
                      TLArg(viewCapacityInput, "ViewCapacityInput"),
                      TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"));

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (isSystemHandled(systemId) &&
                ((m_ctx.requestedQuadViews && viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) ||
                 (m_ctx.useFovTangent && viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO))) {
                const uint32_t viewCount = viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO
                                               ? xr::StereoView::Count
                                               : xr::QuadView::Count;
                if (viewCapacityInput) {
                    XrViewConfigurationView stereoViews[xr::StereoView::Count]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                                                               {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
                    if (viewCapacityInput >= viewCount) {
                        result = OpenXrApi::xrEnumerateViewConfigurationViews(instance,
                                                                              systemId,
                                                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                              xr::StereoView::Count,
                                                                              viewCountOutput,
                                                                              stereoViews);
                    } else {
                        result = XR_ERROR_SIZE_INSUFFICIENT;
                    }

                    if (XR_SUCCEEDED(result)) {
                        *viewCountOutput = viewCount;

                        for (uint32_t i = 0; i < *viewCountOutput; i++) {
                            if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) {
                                return XR_ERROR_VALIDATION_FAILURE;
                            }
                        }

                        // Delegate resolution computation to ViewResolutionCalculator.
                        m_viewResolutionCalculator.computeViews(viewConfigurationType,
                                                                 *viewCountOutput,
                                                                 stereoViews,
                                                                 views,
                                                                 m_ctx.requestedFoveatedRendering);
                    }
                } else {
                    result = OpenXrApi::xrEnumerateViewConfigurationViews(
                        instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, viewCountOutput, nullptr);
                    if (XR_SUCCEEDED(result)) {
                        *viewCountOutput = viewCount;
                    }
                }
            } else {
                result = OpenXrApi::xrEnumerateViewConfigurationViews(
                    instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);
            }

            if (XR_SUCCEEDED(result)) {
                if (viewCapacityInput && views) {
                    for (uint32_t i = 0; i < *viewCountOutput; i++) {
                        QVF_TRACE("xrEnumerateViewConfigurationViews",
                                  TLArg(i, "ViewIndex"),
                                  TLArg(views[i].maxImageRectWidth, "MaxImageRectWidth"),
                                  TLArg(views[i].maxImageRectHeight, "MaxImageRectHeight"),
                                  TLArg(views[i].maxSwapchainSampleCount, "MaxSwapchainSampleCount"),
                                  TLArg(views[i].recommendedImageRectWidth, "RecommendedImageRectWidth"),
                                  TLArg(views[i].recommendedImageRectHeight, "RecommendedImageRectHeight"),
                                  TLArg(views[i].recommendedSwapchainSampleCount, "RecommendedSwapchainSampleCount"));
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateEnvironmentBlendModes
        XrResult xrEnumerateEnvironmentBlendModes(XrInstance instance,
                                                  XrSystemId systemId,
                                                  XrViewConfigurationType viewConfigurationType,
                                                  uint32_t environmentBlendModeCapacityInput,
                                                  uint32_t* environmentBlendModeCountOutput,
                                                  XrEnvironmentBlendMode* environmentBlendModes) override {
            QVF_TRACE("xrEnumerateEnvironmentBlendModes",
                      TLXArg(instance, "Instance"),
                      TLArg((int)systemId, "SystemId"),
                      TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"),
                      TLArg(environmentBlendModeCapacityInput, "EnvironmentBlendModeCapacityInput"));

            // We will implement quad views on top of stereo.
            if (isSystemHandled(systemId) && m_ctx.requestedQuadViews &&
                viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            }

            const XrResult result = OpenXrApi::xrEnumerateEnvironmentBlendModes(instance,
                                                                                systemId,
                                                                                viewConfigurationType,
                                                                                environmentBlendModeCapacityInput,
                                                                                environmentBlendModeCountOutput,
                                                                                environmentBlendModes);

            if (XR_SUCCEEDED(result)) {
                QVF_TRACE("xrEnumerateEnvironmentBlendModes",
                          TLArg(*environmentBlendModeCountOutput, "EnvironmentBlendModeCountOutput"));

                if (environmentBlendModeCapacityInput && environmentBlendModes) {
                    for (uint32_t i = 0; i < *environmentBlendModeCountOutput; i++) {
                        QVF_TRACE("xrEnumerateEnvironmentBlendModes",
                                  TLArg(xr::ToCString(environmentBlendModes[i]), "EnvironmentBlendMode"));
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetViewConfigurationProperties
        XrResult xrGetViewConfigurationProperties(XrInstance instance,
                                                  XrSystemId systemId,
                                                  XrViewConfigurationType viewConfigurationType,
                                                  XrViewConfigurationProperties* configurationProperties) override {
            QVF_TRACE("xrGetViewConfigurationProperties",
                      TLXArg(instance, "Instance"),
                      TLArg((int)systemId, "SystemId"),
                      TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"));

            // We will implement quad views on top of stereo.
            const XrViewConfigurationType originalViewConfigurationType = viewConfigurationType;
            if (isSystemHandled(systemId) && m_ctx.requestedQuadViews &&
                viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            }

            const XrResult result = OpenXrApi::xrGetViewConfigurationProperties(
                instance, systemId, viewConfigurationType, configurationProperties);

            if (XR_SUCCEEDED(result)) {
                if (originalViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                    configurationProperties->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
                }

                QVF_TRACE("xrGetViewConfigurationProperties",
                          TLArg(xr::ToCString(configurationProperties->viewConfigurationType), "ViewConfigurationType"),
                          TLArg(!!configurationProperties->fovMutable, "FovMutable"));
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateSession
        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            if (createInfo->type != XR_TYPE_SESSION_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            QVF_TRACE("xrCreateSession",
                      TLXArg(instance, "Instance"),
                      TLArg((int)createInfo->systemId, "SystemId"),
                      TLArg(createInfo->createFlags, "CreateFlags"));

            const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);

            if (XR_SUCCEEDED(result)) {
                QVF_TRACE("xrCreateSession", TLXArg(*session, "Session"));

                if (isSystemHandled(createInfo->systemId)) {
                    // Initialize the minimal resources for the rendering code.
                    const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
                    while (entry) {
                        if (m_ctx.requestedD3D11 && entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                            const XrGraphicsBindingD3D11KHR* d3dBindings =
                                reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry);
                            m_graphicsContext.initializeD3D11(d3dBindings->device, this);
                            m_ctx.isSupportedGraphicsApi = true;
                            
                            // Transfer timers to FramePipeline
                            m_framePipeline.setCpuTimers(
                                m_graphicsContext.getAppFrameCpuTimer(),
                                m_graphicsContext.getAppRenderCpuTimer()
                            );
                            for (uint32_t i = 0; i < GraphicsContext::kGpuTimerCount; i++) {
                                m_framePipeline.setGpuTimer(i, m_graphicsContext.getAppFrameGpuTimer(i));
                            }
                            
                            LogDebug("Graphics API: D3D11 (device={:p})\n", static_cast<void*>(d3dBindings->device));
                            break;
                        }
                        if (m_ctx.requestedD3D12 && entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                            const XrGraphicsBindingD3D12KHR* d3dBindings =
                                reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
                            m_graphicsContext.initializeD3D12(d3dBindings->device, d3dBindings->queue, this);
                            m_ctx.isSupportedGraphicsApi = true;
                            
                            // Transfer timers to FramePipeline
                            m_framePipeline.setCpuTimers(
                                m_graphicsContext.getAppFrameCpuTimer(),
                                m_graphicsContext.getAppRenderCpuTimer()
                            );
                            for (uint32_t i = 0; i < GraphicsContext::kGpuTimerCount; i++) {
                                m_framePipeline.setGpuTimer(i, m_graphicsContext.getAppFrameGpuTimer(i));
                            }
                            
                            LogDebug("Graphics API: D3D12 (device={:p}, queue={:p})\n", static_cast<void*>(d3dBindings->device), static_cast<void*>(d3dBindings->queue));
                            break;
                        }
                        entry = entry->next;
                    }

                    // Initialize the resources for the eye tracker.
                    if (m_eyeTracker.getType() != EyeTracker::Tracker::None) {
                        m_eyeTracker.initialize(*session);
                    }

                    m_actionManager.reset();

                    m_ctx.session = *session;
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySession
        XrResult xrDestroySession(XrSession session) override {
            QVF_TRACE("xrDestroySession", TLXArg(session, "Session"));

            if (isSessionHandled(session)) {
                // 1. Wait for deferred/turbo frames to finish before teardown.
                m_framePipeline.destroy();

                // 2. Hold the frame mutex while tearing down the compositor to prevent
                // any lingering async work from touching graphics state.
                std::unique_lock frameLock(m_framePipeline.getFrameMutex());

                // 3. Wait for GPU to finish all composition work before destroying swapchains.
                if (m_graphicsContext.getCompositor()) {
                    m_graphicsContext.getCompositor()->waitForGpuIdle();
                }

                // 4. Explicitly destroy all full-FOV swapchains created by the layer.
                // This must happen BEFORE xrDestroySession, while the session is still valid.
                m_swapchainManager.destroyAllFullFovSwapchains(this);

                // 5. Destroy graphics context (which destroys the compositor).
                m_graphicsContext.destroy();

                // 6. Clear gaze spaces
                m_gazeSpaceManager.clear();

                m_ctx.session = XR_NULL_HANDLE;
            }

            // 7. Now it is safe to destroy the OpenXR session.
            const XrResult result = OpenXrApi::xrDestroySession(session);

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrBeginSession
        XrResult xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo) override {
            LogInformation(">> xrBeginSession entry\n");
            if (beginInfo->type != XR_TYPE_SESSION_BEGIN_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            QVF_TRACE("xrBeginSession",
                      TLXArg(session, "Session"),
                      TLArg(xr::ToCString(beginInfo->primaryViewConfigurationType), "PrimaryViewConfigurationType"));

            // We will implement quad views on top of stereo.
            XrSessionBeginInfo chainBeginInfo = *beginInfo;
            if (isSessionHandled(session) && m_ctx.requestedQuadViews &&
                beginInfo->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                // The concept of enumerating view configuration types and graphics API are decoupled.
                // We try to fail as gracefully as possible when we cannot support the configuration.
                if (!m_ctx.isSupportedGraphicsApi) {
                    ErrorLog("Session is using an unsupported graphics API\n");
                    return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
                }

                LogInformation("Session is using quad views\n");
                m_ctx.useQuadViews = true;
                chainBeginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            } else {
                // The concept of enumerating view configuration types and graphics API are decoupled.
                // We try to fail as gracefully as possible when we cannot support the configuration.
                if (m_ctx.useFovTangent && !m_ctx.isSupportedGraphicsApi) {
                    ErrorLog("Session is using an unsupported graphics API\n");
                    return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
                }

                LogInformation("Session is not using quad views\n");
            }

            const XrResult result = OpenXrApi::xrBeginSession(session, &chainBeginInfo);

            if (XR_SUCCEEDED(result)) {
                if (isSessionHandled(session)) {
                    // FOV tables are now lazily populated on the first valid xrLocateViews call,
                    // avoiding dummy frame loops during session initialization that crash strict runtimes.

                    if (m_ctx.useQuadViews) {
                        if (m_config.m_smoothenFocusViewEdges) {
                            LogInformation("Edge smoothing: {:.2f}\n", m_config.m_smoothenFocusViewEdges);
                        } else {
                            LogInformation("Edge smoothing: Disabled\n");
                        }
                        if (m_config.m_sharpenFocusView) {
                            LogInformation("Sharpening: {:.2f}\n", m_config.m_sharpenFocusView);
                        } else {
                            LogInformation("Sharpening: Disabled\n");
                        }
                        LogInformation("Turbo: {}\n", m_config.m_useTurboMode ? "Enabled" : "Disabled");
                    }

                    m_eyeTracker.resetEyeTrackingHealth();
                    m_framePipeline.resetFrameCount();
                }
            }

            LogInformation("<< xrBeginSession exit (result={})\n", xr::ToCString(result));
            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrAttachSessionActionSets
        XrResult xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attachInfo) {
            if (attachInfo->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            // Remove m_useQuadViews check so we inject the eye tracker action set even if the app attaches before xrBeginSession
            if (isSessionHandled(session) &&
                m_eyeTracker.getType() == EyeTracker::Tracker::EyeGazeInteraction) {
                return m_actionManager.attachSessionActionSets(session, attachInfo);
            }

            QVF_TRACE("xrAttachSessionActionSets", TLXArg(session, "Session"));
            for (uint32_t i = 0; i < attachInfo->countActionSets; i++) {
                QVF_TRACE("xrAttachSessionActionSets", TLXArg(attachInfo->actionSets[i], "ActionSet"));
            }

            return OpenXrApi::xrAttachSessionActionSets(session, attachInfo);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrLocateViews
        XrResult xrLocateViews(XrSession session,
                               const XrViewLocateInfo* viewLocateInfo,
                               XrViewState* viewState,
                               uint32_t viewCapacityInput,
                               uint32_t* viewCountOutput,
                               XrView* views) override {
            if (viewLocateInfo->type != XR_TYPE_VIEW_LOCATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            QVF_TRACE("xrLocateViews",
                      TLXArg(session, "Session"),
                      TLArg(xr::ToCString(viewLocateInfo->viewConfigurationType), "ViewConfigurationType"),
                      TLArg(viewLocateInfo->displayTime, "DisplayTime"),
                      TLXArg(viewLocateInfo->space, "Space"),
                      TLArg(viewCapacityInput, "ViewCapacityInput"));

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (isSessionHandled(session)) {
                if ((m_ctx.useQuadViews &&
                     viewLocateInfo->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) ||
                    (m_ctx.useFovTangent &&
                     viewLocateInfo->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)) {
                    const uint32_t viewCount =
                        viewLocateInfo->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO
                            ? xr::StereoView::Count
                            : xr::QuadView::Count;

                    XrViewLocateInfo chainViewLocateInfo = *viewLocateInfo;
                    chainViewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

                    if (viewCapacityInput) {
                        if (viewCapacityInput >= viewCount) {
                            result = OpenXrApi::xrLocateViews(session,
                                                              &chainViewLocateInfo,
                                                              viewState,
                                                              xr::StereoView::Count,
                                                              viewCountOutput,
                                                              views);
                        } else {
                            result = XR_ERROR_SIZE_INSUFFICIENT;
                        }

                        if (XR_SUCCEEDED(result)) {
                            *viewCountOutput = viewCount;

                            for (uint32_t i = 0; i < *viewCountOutput; i++) {
                                if (views[i].type != XR_TYPE_VIEW) {
                                    return XR_ERROR_VALIDATION_FAILURE;
                                }
                            }

                            if (viewState->viewStateFlags &
                                (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                                // Lazy initialization of FOV tables on the first valid xrLocateViews.
                                // By this point, the runtime is fully initialized and inside the proper frame loop.
                                if (m_viewManager.m_needComputeBaseFov) {
                                    LogInformation("First valid xrLocateViews: populating FOV tables...\n");
                                    for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
                                        m_viewManager.m_cachedEyeFov[i] = views[i].fov;
                                        m_viewManager.m_cachedEyePoses[i] = views[i].pose;
                                    }
                                    m_viewManager.populateFovTables(m_ctx.systemId, session);
                                    LogInformation("FOV tables populated successfully.\n");
                                }

                                // Override default to specify whether foveated rendering is desired when the
                                // application does not specify.
                                bool foveatedRenderingActive =
                                    m_eyeTracker.getType() != EyeTracker::Tracker::None && m_config.m_preferFoveatedRendering;

                                if (m_ctx.requestedFoveatedRendering) {
                                    const XrViewLocateFoveatedRenderingVARJO* foveatedLocate =
                                        reinterpret_cast<const XrViewLocateFoveatedRenderingVARJO*>(
                                            viewLocateInfo->next);
                                    while (foveatedLocate) {
                                        if (foveatedLocate->type == XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO) {
                                            foveatedRenderingActive = foveatedLocate->foveatedRenderingActive;
                                            break;
                                        }
                                        foveatedLocate = reinterpret_cast<const XrViewLocateFoveatedRenderingVARJO*>(
                                            foveatedLocate->next);
                                    }

                                    QVF_TRACE("xrLocateViews",
                                              TLArg(foveatedRenderingActive, "FoveatedRenderingActive"));
                                }

                                // Query the eye tracker if needed.
                                bool isGazeValid = false;
                                XrVector3f gazeUnitVector{};
                                if (foveatedRenderingActive) {
                                    isGazeValid = m_eyeTracker.getEyeGaze(
                                        session, viewLocateInfo->displayTime, false /* getStateOnly */, gazeUnitVector);
                                }

                                // Delegate foveated view computation to ViewManager.
                                m_viewManager.computeFoveatedViews(views, *viewCountOutput, viewLocateInfo->viewConfigurationType, isGazeValid, gazeUnitVector);

                                // Quirk for DCS World: the application does not pass the correct FOV for the focus
                                // views in xrEndFrame(). We must keep track of the correct values for each frame.
                                if (m_ctx.useQuadViews && m_focusFovQuirk.isEnabled()) {
                                    m_focusFovQuirk.storeFov(viewLocateInfo->displayTime,
                                                             views[xr::QuadView::FocusLeft].fov,
                                                             views[xr::QuadView::FocusRight].fov);
                                }
                            }
                        }
                    } else {
                        result = OpenXrApi::xrLocateViews(
                            session, &chainViewLocateInfo, viewState, 0, viewCountOutput, nullptr);
                        if (XR_SUCCEEDED(result)) {
                            *viewCountOutput = viewCount;
                        }
                    }
                } else {
                    result = OpenXrApi::xrLocateViews(
                        session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
                }
            } else {
                result = OpenXrApi::xrLocateViews(
                    session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);
            }

            if (XR_SUCCEEDED(result)) {
                QVF_TRACE("xrLocateViews", TLArg(*viewCountOutput, "ViewCountOutput"));

                if (viewCapacityInput && views) {
                    for (uint32_t i = 0; i < *viewCountOutput; i++) {
                        QVF_TRACE("xrLocateViews",
                                  TLArg(i, "ViewIndex"),
                                  TLArg(xr::ToString(views[i].pose).c_str(), "Pose"),
                                  TLArg(xr::ToString(views[i].fov).c_str(), "Fov"));
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateSwapchain
        XrResult xrCreateSwapchain(XrSession session,
                                   const XrSwapchainCreateInfo* createInfo,
                                   XrSwapchain* swapchain) override {
            return m_swapchainInterceptor.createSwapchain(session, createInfo, swapchain, this);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySwapchain
        XrResult xrDestroySwapchain(XrSwapchain swapchain) override {
            return m_swapchainInterceptor.destroySwapchain(swapchain, this);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrAcquireSwapchainImage
        XrResult xrAcquireSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageAcquireInfo* acquireInfo,
                                         uint32_t* index) override {
            return m_swapchainInterceptor.acquireSwapchainImage(swapchain, acquireInfo, index, this, m_ctx.useQuadViews || m_ctx.useFovTangent);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrReleaseSwapchainImage
        XrResult xrReleaseSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageReleaseInfo* releaseInfo) override {
            return m_swapchainInterceptor.releaseSwapchainImage(swapchain, releaseInfo, this, m_ctx.useQuadViews || m_ctx.useFovTangent);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrWaitFrame
        XrResult xrWaitFrame(XrSession session,
                             const XrFrameWaitInfo* frameWaitInfo,
                             XrFrameState* frameState) override {
            if (isSessionHandled(session)) {
                XrResult result = m_framePipeline.waitFrame(this, session, frameWaitInfo, frameState,
                                                            m_config.m_useTurboMode, &m_isAsyncFrameMode);
                m_framePipeline.setWaitedFrameTime(frameState->predictedDisplayTime);
                m_eyeTracker.setPredictedDisplayPeriod(frameState->predictedDisplayPeriod);
                return result;
            } else {
                return OpenXrApi::xrWaitFrame(session, frameWaitInfo, frameState);
            }
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrBeginFrame
        XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) override {
            QVF_TRACE("xrBeginFrame", TLXArg(session, "Session"));

            if (isSessionHandled(session)) {
                XrResult result = m_framePipeline.beginFrame(this, session, frameBeginInfo, m_isAsyncFrameMode);

                if (XR_SUCCEEDED(result)) {
                    // Delegate action set poll/attach/sync fallback to ActionManager.
                    m_actionManager.pollAndSyncIfNeeded(session, m_framePipeline.getFramesElapsed(),
                                                        m_ctx.useQuadViews, m_eyeTracker.getType());

                    // Issue a warning if eye tracking was expected but does not seem functional.
                    if (m_eyeTracker.consumeStaleTrackingWarning(std::chrono::seconds(60))) {
                        LogWarning("No data received from the eye tracker in 60 seconds! Image quality may be " 
                            "degraded.\n");
                    }
                }

                return result;
            } else {
                return OpenXrApi::xrBeginFrame(session, frameBeginInfo);
            }
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEndFrame
        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            QVF_TRACE("xrEndFrame",
                      TLXArg(session, "Session"),
                      TLArg(frameEndInfo->displayTime, "DisplayTime"),
                      TLArg(xr::ToCString(frameEndInfo->environmentBlendMode), "EnvironmentBlendMode"));

            if (isSessionHandled(session)) {
                LogDebug("xrEndFrame: layers={}, quadViews={}, d3d12={}\n", frameEndInfo->layerCount, m_ctx.useQuadViews, m_graphicsContext.isD3D12());

                // Record frame timing
                m_framePipeline.recordFrameTime();
                m_debugKeyHandler.handle();
            }

            // We will allocate structures to pass to the real xrEndFrame().
            std::vector<XrCompositionLayerProjection> projectionAllocator;
            std::vector<std::array<XrCompositionLayerProjectionView, xr::StereoView::Count>> projectionViewAllocator;
            std::vector<const XrCompositionLayerBaseHeader*> layers;

            // Ensure pointers within the collections remain stable.
            projectionAllocator.reserve(frameEndInfo->layerCount);
            projectionViewAllocator.reserve(frameEndInfo->layerCount);

            XrFrameEndInfo chainFrameEndInfo = *frameEndInfo;

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (isSessionHandled(session)) {
                if (m_ctx.useQuadViews || m_ctx.useFovTangent) {
                    // Save the application context state.
                    ComPtr<ID3DDeviceContextState> applicationContextState;
                    {
                        TraceLocalActivity(local);
                        QVF_TRACE_START(local, "xrEndFrame_SwapDeviceContextState");
                        if (m_graphicsContext.isD3D12()) {
                            // D3D12 uses command lists, no context swap needed
                            LogDebug("xrEndFrame: D3D12 path, skipping context swap\n");
                        } else {
                            LogDebug("xrEndFrame: Swapping device context state...\n");
                            m_graphicsContext.swapDeviceContextState(applicationContextState);
                            LogDebug("xrEndFrame: Context state swapped, clearing state...\n");
                        }
                        QVF_TRACE_STOP(local, "xrEndFrame_SwapDeviceContextState");
                    }

                    // Restore the application context state upon leaving this scope.
                    auto scopeGuard = MakeScopeGuard([&] {
                        TraceLocalActivity(local);
                        QVF_TRACE_START(local, "xrEndFrame_SwapDeviceContextState");
                        if (!m_graphicsContext.isD3D12()) {
                            m_graphicsContext.restoreDeviceContextState(applicationContextState);
                        }
                        QVF_TRACE_STOP(local, "xrEndFrame_SwapDeviceContextState");
                    });

                    // Delegate layer processing to LayerComposer.
                    std::set<XrSwapchain> swapchainsToRelease;
                    XrResult composeResult = m_layerComposer.processLayers(session,
                                                                           frameEndInfo,
                                                                           m_ctx.useQuadViews,
                                                                           m_ctx.useFovTangent,
                                                                           m_ctx.requestedDepthSubmission,
                                                                           projectionAllocator,
                                                                           projectionViewAllocator,
                                                                           layers,
                                                                           swapchainsToRelease);
                    if (XR_FAILED(composeResult)) {
                        return composeResult;
                    }

                    chainFrameEndInfo.layers = layers.data();
                    chainFrameEndInfo.layerCount = (uint32_t)layers.size();

                    // Age old FOV quirk entries.
                    if (m_focusFovQuirk.isEnabled()) {
                        m_focusFovQuirk.ageOldEntries(frameEndInfo->displayTime);
                    }

                    // Perform deferred swapchains release now.
                    for (auto swapchain : swapchainsToRelease) {
                        QVF_TRACE("xrEndFrame_DeferredSwapchainRelease", TLXArg(swapchain, "Swapchain"));

                        CHECK_XRCMD(OpenXrApi::xrReleaseSwapchainImage(swapchain, nullptr));
                    }
                }

                {
                    bool isAsyncMode = false;
                    result = m_framePipeline.endFrame(this, session, &chainFrameEndInfo,
                                                     m_config.m_useTurboMode, &isAsyncMode);
                }

                m_framePipeline.incrementFrame();
            } else {
                result = OpenXrApi::xrEndFrame(session, frameEndInfo);
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateReferenceSpace
        XrResult xrCreateReferenceSpace(XrSession session,
                                        const XrReferenceSpaceCreateInfo* createInfo,
                                        XrSpace* space) override {
            return m_gazeSpaceManager.createReferenceSpace(session, createInfo, space, this);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySpace
        XrResult xrDestroySpace(XrSpace space) override {
            return m_gazeSpaceManager.destroySpace(space, this);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrLocateSpace
        XrResult xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) override {
            XrResult result = m_gazeSpaceManager.locateSpace(space, baseSpace, time, location, m_eyeTracker, m_ctx.session);
            if (result == XR_ERROR_HANDLE_INVALID) {
                result = OpenXrApi::xrLocateSpace(space, baseSpace, time, location);
            }
            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrSyncActions
        XrResult xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) override {
            if (syncInfo->type != XR_TYPE_ACTIONS_SYNC_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            // Remove m_useQuadViews check so we inject the eye tracker action set
            if (isSessionHandled(session) &&
                m_eyeTracker.getType() == EyeTracker::Tracker::EyeGazeInteraction) {
                return m_actionManager.syncActions(session, syncInfo);
            }

            QVF_TRACE("xrSyncActions", TLXArg(session, "Session"));
            for (uint32_t i = 0; i < syncInfo->countActiveActionSets; i++) {
                QVF_TRACE("xrSyncActions",
                          TLXArg(syncInfo->activeActionSets[i].actionSet, "ActionSet"),
                          TLArg(syncInfo->activeActionSets[i].subactionPath, "SubactionPath"));
            }

            return OpenXrApi::xrSyncActions(session, syncInfo);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrPollEvent
        XrResult xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) override {
            QVF_TRACE("xrPollEvent", TLXArg(instance, "Instance"));

            const XrResult result = OpenXrApi::xrPollEvent(instance, eventData);

            if (result == XR_SUCCESS) {
                QVF_TRACE("xrPollEvent", TLArg(xr::ToCString(eventData->type), "EventType"));

                // Translate visibility mask events.
                if (eventData->type == XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR) {
                    XrEventDataVisibilityMaskChangedKHR* event =
                        reinterpret_cast<XrEventDataVisibilityMaskChangedKHR*>(eventData);
                    // We will implement quad views on top of stereo. If the stereo mask changes, then it means the
                    // quad views mask for the peripheral views changes.
                    // TODO: We should in fact duplicate the event.
                    if (m_ctx.requestedQuadViews &&
                        event->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
                        event->viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
                    }
                }
            }

            m_actionManager.setPollEventDone();

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetVisibilityMaskKHR
        XrResult xrGetVisibilityMaskKHR(XrSession session,
                                        XrViewConfigurationType viewConfigurationType,
                                        uint32_t viewIndex,
                                        XrVisibilityMaskTypeKHR visibilityMaskType,
                                        XrVisibilityMaskKHR* visibilityMask) override {
            if (visibilityMask->type != XR_TYPE_VISIBILITY_MASK_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            QVF_TRACE("xrGetVisibilityMaskKHR",
                      TLXArg(session, "Session"),
                      TLArg(xr::ToCString(viewConfigurationType), "ViewConfigurationType"),
                      TLArg(viewIndex, "ViewIndex"),
                      TLArg(xr::ToCString(visibilityMaskType), "VisibilityMaskType"),
                      TLArg(visibilityMask->vertexCapacityInput, "VertexCapacityInput"),
                      TLArg(visibilityMask->indexCapacityInput, "IndexCapacityInput"));

            XrResult result = XR_ERROR_RUNTIME_FAILURE;
            if (isSessionHandled(session)) {
                if (m_ctx.requestedQuadViews && viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO &&
                    viewIndex >= xr::StereoView::Count) {
                    if (m_ctx.useQuadViews) {
                        // No mask on the focus view.
                        if (viewIndex == xr::QuadView::FocusLeft || viewIndex == xr::QuadView::FocusRight) {
                            visibilityMask->vertexCountOutput = 0;
                            visibilityMask->indexCountOutput = 0;

                            result = XR_SUCCESS;
                        } else {
                            result = XR_ERROR_VALIDATION_FAILURE;
                        }
                    } else {
                        result = XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
                    }
                } else {
                    // We will implement quad views on top of stereo. Use the regular mask for the peripheral view.
                    if (m_ctx.requestedQuadViews &&
                        viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO) {
                        if (m_ctx.useQuadViews) {
                            result = OpenXrApi::xrGetVisibilityMaskKHR(session,
                                                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                       viewIndex,
                                                                       visibilityMaskType,
                                                                       visibilityMask);
                        } else {
                            result = XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
                        }
                    } else if (m_ctx.useFovTangent && viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
                        // No mask when FOV tangent is used.
                        visibilityMask->vertexCountOutput = 0;
                        visibilityMask->indexCountOutput = 0;

                        result = XR_SUCCESS;
                    } else {
                        result = OpenXrApi::xrGetVisibilityMaskKHR(
                            session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
                    }
                }
            } else {
                result = OpenXrApi::xrGetVisibilityMaskKHR(
                    session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
            }

            return result;
        }

      private:
        const std::string& GetApplicationExecutableName() const {
            return m_ctx.applicationExecutableName;
        }

        bool isSystemHandled(XrSystemId systemId) const {
            return systemId == m_ctx.systemId;
        }

        bool isSessionHandled(XrSession session) const {
            return session == m_ctx.session;
        }

        // Module instances for configuration, view math, eye tracking, swapchains, graphics, and frame pipeline.
        FoveationConfig m_config;
        ViewManager m_viewManager;
        EyeTracker m_eyeTracker;
        SwapchainManager m_swapchainManager;
        GraphicsContext m_graphicsContext;
        FramePipeline m_framePipeline;

        // Phase 3 module instances.
        DebugKeyHandler m_debugKeyHandler;
        FocusFovQuirk m_focusFovQuirk;
        ActionManager m_actionManager;
        ViewResolutionCalculator m_viewResolutionCalculator;
        LayerComposer m_layerComposer;

        // Phase 4 module instances.
        GazeSpaceManager m_gazeSpaceManager;
        SwapchainInterceptor m_swapchainInterceptor;

        // Session state context
        SessionContext m_ctx;

        bool m_isAsyncFrameMode{false};

    };

    // This method is required by the framework to instantiate your OpenXrApi implementation.
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

} // namespace openxr_api_layer

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        TraceLoggingRegister(openxr_api_layer::log::g_traceProvider);
        break;

    case DLL_PROCESS_DETACH:
        // Set the flag BEFORE static destructors run so they can skip GPU sync
        openxr_api_layer::g_isUnloading = true;
        TraceLoggingUnregister(openxr_api_layer::log::g_traceProvider);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
