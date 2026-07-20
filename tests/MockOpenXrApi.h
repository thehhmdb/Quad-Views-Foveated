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
        // Pointer to the active mock instance, used by the dispatch-table trampolines below so
        // that qualified base-class calls (openXrApi->OpenXrApi::xrWaitFrame(...)) reach this mock.
        static MockOpenXrApi* s_current;

        // Populate the base-class dispatch table so that qualified calls like
        // openXrApi->OpenXrApi::xrWaitFrame(...) (used internally by the layer) forward to
        // this mock's virtual overrides instead of dereferencing a null function pointer.
        void initializeForTesting() {
            s_current = this;
            SetGetInstanceProcAddr(
                +[](XrInstance, const char* name, PFN_xrVoidFunction* function) -> XrResult {
                    *function = nullptr;
                    return XR_SUCCESS;
                },
                XR_NULL_HANDLE);

            setDispatchTableForTesting(
                +[](XrSession s, const XrFrameWaitInfo* i, XrFrameState* f) -> XrResult {
                    return s_current->xrWaitFrame(s, i, f);
                },
                +[](XrSession s, const XrFrameBeginInfo* i) -> XrResult {
                    return s_current->xrBeginFrame(s, i);
                },
                +[](XrSession s, const XrFrameEndInfo* i) -> XrResult {
                    return s_current->xrEndFrame(s, i);
                },
                +[](XrSwapchain s) -> XrResult {
                    return s_current->xrDestroySwapchain(s);
                },
                +[](XrSwapchain s, const XrSwapchainImageReleaseInfo* i) -> XrResult {
                    return s_current->xrReleaseSwapchainImage(s, i);
                });

            // Wire swapchain acquire/wait/enumerate so compositor's qualified base-class
            // calls (m_openXrApi->OpenXrApi::xrAcquireSwapchainImage(...)) forward to mocks.
            setDispatchTableForTestingSwapchain(
                +[](XrSwapchain s, const XrSwapchainImageAcquireInfo* i, uint32_t* idx) -> XrResult {
                    return s_current->xrAcquireSwapchainImage(s, i, idx);
                },
                +[](XrSwapchain s, const XrSwapchainImageWaitInfo* i) -> XrResult {
                    return s_current->xrWaitSwapchainImage(s, i);
                },
                +[](XrSwapchain s, uint32_t in, uint32_t* out, XrSwapchainImageBaseHeader* imgs) -> XrResult {
                    return s_current->xrEnumerateSwapchainImages(s, in, out, imgs);
                });

            // Wire the remaining dispatch-table entries used by the logic classes
            // (GazeSpaceManager, ActionManager) so their qualified base-class calls forward here.
            setDispatchTableForTestingEx(
                +[](XrSession s, const XrReferenceSpaceCreateInfo* i, XrSpace* sp) -> XrResult {
                    return s_current->xrCreateReferenceSpace(s, i, sp);
                },
                +[](XrSpace s) -> XrResult {
                    return s_current->xrDestroySpace(s);
                },
                +[](XrSpace s, XrSpace b, XrTime t, XrSpaceLocation* l) -> XrResult {
                    return s_current->xrLocateSpace(s, b, t, l);
                },
                +[](XrInstance i, const char* n, XrPath* p) -> XrResult {
                    return s_current->xrStringToPath(i, n, p);
                },
                +[](XrInstance i, const XrInteractionProfileSuggestedBinding* b) -> XrResult {
                    return s_current->xrSuggestInteractionProfileBindings(i, b);
                },
                +[](XrSession s, const XrSessionActionSetsAttachInfo* i) -> XrResult {
                    return s_current->xrAttachSessionActionSets(s, i);
                },
                +[](XrSession s, const XrActionsSyncInfo* i) -> XrResult {
                    return s_current->xrSyncActions(s, i);
                },
                +[](XrInstance i, XrEventDataBuffer* b) -> XrResult {
                    return s_current->xrPollEvent(i, b);
                });
        }

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

        // Swapchains
        MOCK_METHOD(XrResult, xrEnumerateSwapchainImages, (XrSwapchain, uint32_t, uint32_t*, XrSwapchainImageBaseHeader*), (override));
        MOCK_METHOD(XrResult, xrAcquireSwapchainImage, (XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*), (override));
        MOCK_METHOD(XrResult, xrWaitSwapchainImage, (XrSwapchain, const XrSwapchainImageWaitInfo*), (override));
        MOCK_METHOD(XrResult, xrReleaseSwapchainImage, (XrSwapchain, const XrSwapchainImageReleaseInfo*), (override));

        // WARP texture registry for compositor tests
        struct MockSwapchain {
            std::vector<void*> textures;
            uint32_t currentAcquiredIndex{0};
            uint32_t arraySize{1};
            MockSwapchain() = default;
            MockSwapchain(std::vector<void*> t, uint32_t idx, uint32_t arr)
                : textures(std::move(t)), currentAcquiredIndex(idx), arraySize(arr) {}
        };
        std::unordered_map<XrSwapchain, MockSwapchain> m_mockSwapchains;
        std::unordered_map<XrSwapchain, uint32_t> m_mockAcquiredIndices;
        bool m_isD3D12Mode{false};

        void RegisterMockSwapchain(XrSwapchain handle, std::vector<void*> textures, uint32_t arraySize = 1) {
            m_mockSwapchains.emplace(handle, MockSwapchain{std::move(textures), 0, arraySize});
        }

        // Configure gmock defaults for WARP swapchain mocking
        void SetupWarpSwapchainMocks() {
            ON_CALL(*this, xrEnumerateSwapchainImages(testing::_, testing::_, testing::_, testing::_))
                .WillByDefault([this](XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t* imageCountOutput, XrSwapchainImageBaseHeader* images) -> XrResult {
                    auto it = m_mockSwapchains.find(swapchain);
                    if (it == m_mockSwapchains.end()) return XR_ERROR_HANDLE_INVALID;
                    *imageCountOutput = (uint32_t)it->second.textures.size();
                    if (imageCapacityInput > 0 && images) {
                        if (m_isD3D12Mode) {
                            auto* d3d12Images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
                            for (uint32_t i = 0; i < it->second.textures.size() && i < imageCapacityInput; i++) {
                                d3d12Images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
                                d3d12Images[i].next = nullptr;
                                d3d12Images[i].texture = static_cast<ID3D12Resource*>(it->second.textures[i]);
                            }
                        } else {
                            auto* d3d11Images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
                            for (uint32_t i = 0; i < it->second.textures.size() && i < imageCapacityInput; i++) {
                                d3d11Images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
                                d3d11Images[i].next = nullptr;
                                d3d11Images[i].texture = static_cast<ID3D11Texture2D*>(it->second.textures[i]);
                            }
                        }
                    }
                    return XR_SUCCESS;
                });
            ON_CALL(*this, xrAcquireSwapchainImage(testing::_, testing::_, testing::_))
                .WillByDefault([this](XrSwapchain swapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index) -> XrResult {
                    auto it = m_mockSwapchains.find(swapchain);
                    if (it == m_mockSwapchains.end()) return XR_ERROR_HANDLE_INVALID;
                    *index = it->second.currentAcquiredIndex;
                    m_mockAcquiredIndices[swapchain] = *index;
                    return XR_SUCCESS;
                });
            ON_CALL(*this, xrWaitSwapchainImage(testing::_, testing::_))
                .WillByDefault([](XrSwapchain, const XrSwapchainImageWaitInfo*) -> XrResult {
                    return XR_SUCCESS;
                });
            ON_CALL(*this, xrReleaseSwapchainImage(testing::_, testing::_))
                .WillByDefault([this](XrSwapchain swapchain, const XrSwapchainImageReleaseInfo*) -> XrResult {
                    auto it = m_mockSwapchains.find(swapchain);
                    if (it == m_mockSwapchains.end()) return XR_ERROR_HANDLE_INVALID;
                    it->second.currentAcquiredIndex = (it->second.currentAcquiredIndex + 1) % it->second.textures.size();
                    return XR_SUCCESS;
                });
        }

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