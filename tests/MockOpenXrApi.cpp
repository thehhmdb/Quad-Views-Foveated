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

#include "MockOpenXrApi.h"

namespace openxr_api_layer {

    // Default implementations for all mocked methods
    XrResult MockOpenXrApi::xrEnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*, XrViewConfigurationView*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrBeginFrame(XrSession, const XrFrameBeginInfo*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrEndFrame(XrSession, const XrFrameEndInfo*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrDestroySwapchain(XrSwapchain) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrGetInstanceProperties(XrInstance, XrInstanceProperties*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrStringToPath(XrInstance, const char*, XrPath*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrSuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrAttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrSyncActions(XrSession, const XrActionsSyncInfo*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrPollEvent(XrInstance, XrEventDataBuffer*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrLocateViews(XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t, uint32_t*, XrView*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrDestroySpace(XrSpace) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrEnumerateViewConfigurations(XrInstance, XrSystemId, uint32_t, uint32_t*, XrViewConfigurationType*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrGetViewConfigurationProperties(XrInstance, XrSystemId, XrViewConfigurationType, XrViewConfigurationProperties*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrEnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*, XrEnvironmentBlendMode*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrGetVisibilityMaskKHR(XrSession, XrViewConfigurationType, uint32_t, XrVisibilityMaskTypeKHR, XrVisibilityMaskKHR*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrDestroySession(XrSession) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrBeginSession(XrSession, const XrSessionBeginInfo*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrEndSession(XrSession) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrCreateInstance(const XrInstanceCreateInfo*) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrDestroyInstance(XrInstance) {
        return XR_SUCCESS;
    }

    XrResult MockOpenXrApi::xrGetInstanceProcAddr(XrInstance, const char*, PFN_xrVoidFunction*) {
        return XR_SUCCESS;
    }

} // namespace openxr_api_layer