// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// to deal
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
#include <gmock/gmock.h>
#include "framework/dispatch.gen.h"

namespace openxr_api_layer {

    class MockOpenXrApi : public OpenXrApi {
    public:
        // Instance management
        MOCK_METHOD(XrResult, xrCreateInstance, (const XrInstanceCreateInfo*), (override));
        MOCK_METHOD(XrResult, xrDestroyInstance, (XrInstance), (override));
        MOCK_METHOD(XrResult, xrGetInstanceProcAddr, (XrInstance, const char*, PFN_xrVoidFunction*), (override));
        MOCK_METHOD(XrResult, xrGetInstanceProperties, (XrInstance, XrInstanceProperties*), (override));

        // System
        MOCK_METHOD(XrResult, xrGetSystem, (XrInstance, const XrSystemGetInfo*, XrSystemId*), (override));
        MOCK_METHOD(XrResult, xrGetSystemProperties, (XrInstance, XrSystemId, XrSystemProperties*), (override));

        // Session
        MOCK_METHOD(XrResult, xrCreateSession, (XrInstance, const XrSessionCreateInfo*, XrSession*), (override));
        MOCK_METHOD(XrResult, xrDestroySession, (XrSession), (override));
        MOCK_METHOD(XrResult, xrBeginSession, (XrSession, const XrSessionBeginInfo*), (override));

        // Frame loop
        MOCK_METHOD(XrResult, xrWaitFrame, (XrSession, const XrFrameWaitInfo*, XrFrameState*), (override));
        MOCK_METHOD(XrResult, xrBeginFrame, (XrSession, const XrFrameBeginInfo*), (override));
        MOCK_METHOD(XrResult, xrEndFrame, (XrSession, const XrFrameEndInfo*), (override));

        // Views
        MOCK_METHOD(XrResult, xrLocateViews, (XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t, uint32_t*, XrView*), (override));
        MOCK_METHOD(XrResult, xrEnumerateViewConfigurations, (XrInstance, XrSystemId, uint32_t, uint32_t*, XrViewConfigurationType*), (override));
        MOCK_METHOD(XrResult, xrGetViewConfigurationProperties, (XrInstance, XrSystemId, XrViewConfigurationType, XrViewConfigurationProperties*), (override));
        MOCK_METHOD(XrResult, xrEnumerateViewConfigurationViews, (XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*, XrViewConfigurationView*), (override));

        // Spaces
        MOCK_METHOD(XrResult, xrCreateReferenceSpace, (XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*), (override));
        MOCK_METHOD(XrResult, xrCreateActionSpace, (XrSession, const XrActionSpaceCreateInfo*, XrSpace*), (override));
        MOCK_METHOD(XrResult, xrDestroySpace, (XrSpace), (override));
        MOCK_METHOD(XrResult, xrLocateSpace, (XrSpace, XrSpace, XrTime, XrSpaceLocation*), (override));

        // Swapchains
        MOCK_METHOD(XrResult, xrCreateSwapchain, (XrSession, const XrSwapchainCreateInfo*, XrSwapchain*), (override));
        MOCK_METHOD(XrResult, xrDestroySwapchain, (XrSwapchain), (override));
        MOCK_METHOD(XrResult, xrEnumerateSwapchainImages, (XrSwapchain, uint32_t, uint32_t*, XrSwapchainImageBaseHeader*), (override));
        MOCK_METHOD(XrResult, xrAcquireSwapchainImage, (XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*), (override));
        MOCK_METHOD(XrResult, xrWaitSwapchainImage, (XrSwapchain, const XrSwapchainImageWaitInfo*), (override));
        MOCK_METHOD(XrResult, xrReleaseSwapchainImage, (XrSwapchain, const XrSwapchainImageReleaseInfo*), (override));

        // Actions
        MOCK_METHOD(XrResult, xrCreateActionSet, (XrInstance, const XrActionSetCreateInfo*, XrActionSet*), (override));
        MOCK_METHOD(XrResult, xrCreateAction, (XrActionSet, const XrActionCreateInfo*, XrAction*), (override));
        MOCK_METHOD(XrResult, xrSuggestInteractionProfileBindings, (XrInstance, const XrInteractionProfileSuggestedBinding*), (override));
        MOCK_METHOD(XrResult, xrAttachSessionActionSets, (XrSession, const XrSessionActionSetsAttachInfo*), (override));
        MOCK_METHOD(XrResult, xrSyncActions, (XrSession, const XrActionsSyncInfo*), (override));
        MOCK_METHOD(XrResult, xrGetActionStatePose, (XrSession, const XrActionStateGetInfo*, XrActionStatePose*), (override));

        // Paths
        MOCK_METHOD(XrResult, xrStringToPath, (XrInstance, const char*, XrPath*), (override));
        MOCK_METHOD(XrResult, xrPathToString, (XrInstance, XrPath, uint32_t, uint32_t*, char*), (override));

        // Events
        MOCK_METHOD(XrResult, xrPollEvent, (XrInstance, XrEventDataBuffer*), (override));

        // Blend modes
        MOCK_METHOD(XrResult, xrEnumerateEnvironmentBlendModes, (XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*, XrEnvironmentBlendMode*), (override));

        // Extensions
        MOCK_METHOD(XrResult, xrEnumerateInstanceExtensionProperties, (const char*, uint32_t, uint32_t*, XrExtensionProperties*), (override));
        MOCK_METHOD(XrResult, xrGetVisibilityMaskKHR, (XrSession, XrViewConfigurationType, uint32_t, XrVisibilityMaskTypeKHR, XrVisibilityMaskKHR*), (override));

        // Eye tracking (VARJO/FB)
        MOCK_METHOD(XrResult, xrCreateEyeTrackerFB, (XrSession, const XrEyeTrackerCreateInfoFB*, XrEyeTrackerFB*), (override));
        MOCK_METHOD(XrResult, xrGetEyeGazesFB, (XrEyeTrackerFB, const XrEyeGazesInfoFB*, XrEyeGazesFB*), (override));
    };

} // namespace openxr_api_layer