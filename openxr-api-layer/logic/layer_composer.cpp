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
#include "logic/layer_composer.h"
#include "framework/log.h"
#include "framework/util.h"
#include "compositor.h"
#include "views.h"

namespace openxr_api_layer {

    using namespace log;
    using namespace xr;

    LayerComposer::LayerComposer(OpenXrApi* openXrApi,
                                 FoveationConfig& config,
                                 ViewManager& viewManager,
                                 SwapchainManager& swapchainManager,
                                 GraphicsContext& graphicsContext,
                                 EyeTracker& eyeTracker,
                                 FocusFovQuirk& focusFovQuirk)
        : m_openXrApi(openXrApi), m_config(config), m_viewManager(viewManager),
          m_swapchainManager(swapchainManager), m_graphicsContext(graphicsContext),
          m_eyeTracker(eyeTracker), m_focusFovQuirk(focusFovQuirk) {
    }

    XrResult LayerComposer::processLayers(XrSession session,
                                          const XrFrameEndInfo* frameEndInfo,
                                          bool useQuadViews,
                                          bool useFovTangent,
                                          bool requestedDepthSubmission,
                                          std::vector<XrCompositionLayerProjection>& projectionAllocator,
                                          std::vector<std::array<XrCompositionLayerProjectionView, xr::StereoView::Count>>& projectionViewAllocator,
                                          std::vector<const XrCompositionLayerBaseHeader*>& outLayers,
                                          std::set<XrSwapchain>& outSwapchainsToRelease) {
        // FIX (Item 5): If compositor previously failed, pass layers through unmodified
        if (m_compositorFailed) {
            outLayers.assign(frameEndInfo->layers, frameEndInfo->layers + frameEndInfo->layerCount);
            return XR_SUCCESS;
        }

        std::set<XrSwapchain> swapchainsToRelease;

        for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
            if (!frameEndInfo->layers[i]) {
                return XR_ERROR_LAYER_INVALID;
            }

            if (frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const XrCompositionLayerProjection* proj =
                    reinterpret_cast<const XrCompositionLayerProjection*>(frameEndInfo->layers[i]);

                QVF_TRACE("xrEndFrame_Layer",
                          TLArg(xr::ToCString(proj->type), "Type"),
                          TLArg(proj->layerFlags, "Flags"),
                          TLXArg(proj->space, "Space"));

                if (proj->viewCount != (useQuadViews ? xr::QuadView::Count : xr::StereoView::Count)) {
                    return XR_ERROR_VALIDATION_FAILURE;
                }

                projectionViewAllocator.push_back(
                    {proj->views[xr::StereoView::Left], proj->views[xr::StereoView::Right]});

                for (uint32_t viewIndex = 0; viewIndex < xr::StereoView::Count; viewIndex++) {
                    if (useQuadViews) {
                        for (uint32_t j = viewIndex; j < xr::QuadView::Count; j += xr::StereoView::Count) {
                            QVF_TRACE("xrEndFrame_View",
                                      TLArg("Color", "Type"),
                                      TLArg(j, "ViewIndex"),
                                      TLXArg(proj->views[j].subImage.swapchain, "Swapchain"),
                                      TLArg(proj->views[j].subImage.imageArrayIndex, "ImageArrayIndex"),
                                      TLArg(xr::ToString(proj->views[j].subImage.imageRect).c_str(), "ImageRect"),
                                      TLArg(xr::ToString(proj->views[j].pose).c_str(), "Pose"),
                                      TLArg(xr::ToString(proj->views[j].fov).c_str(), "Fov"));
                        }
                    }

                    const uint32_t focusViewIndex =
                        useQuadViews ? (viewIndex + xr::StereoView::Count) : viewIndex;

                    // FIX (Item 8): Capture as shared_ptr to keep the Swapchain alive for this frame
                    std::shared_ptr<SwapchainManager::Swapchain> swapchainForStereoView =
                        m_swapchainManager.getSwapchain(proj->views[viewIndex].subImage.swapchain);
                    std::shared_ptr<SwapchainManager::Swapchain> swapchainForFocusView =
                        m_swapchainManager.getSwapchain(proj->views[focusViewIndex].subImage.swapchain);
                    if (!swapchainForStereoView || !swapchainForFocusView) {
                        return XR_ERROR_HANDLE_INVALID;
                    }

                    if (swapchainForStereoView->deferredRelease) {
                        swapchainsToRelease.insert(proj->views[viewIndex].subImage.swapchain);
                        swapchainForStereoView->deferredRelease = false;
                    }
                    if (swapchainForFocusView->deferredRelease) {
                        swapchainsToRelease.insert(proj->views[focusViewIndex].subImage.swapchain);
                        swapchainForFocusView->deferredRelease = false;
                    }

                    // Allocate a shared destination swapchain (arraySize=2, one layer per eye).
                    // SteamVR D3D12 runtime may reject multiple large swapchains, so we use a single
                    // array swapchain shared by both eyes.
                    if (swapchainForStereoView->fullFovSwapchain == XR_NULL_HANDLE) {
                        XrSwapchainCreateInfo createInfo = swapchainForStereoView->createInfo;
                        createInfo.arraySize = xr::StereoView::Count;
                        createInfo.width = m_viewManager.m_fullFovResolution.width;
                        createInfo.height = m_viewManager.m_fullFovResolution.height;
                        // Use only the flags needed for composition (render target + shader resource).
                        // Don't inherit extra flags from the app swapchain.
                        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                        // Clear the dangling next pointer from the shallow copy.
                        // D3D12 graphics bindings are session-level, not swapchain-level.
                        createInfo.next = nullptr;

                        LogDebug("xrEndFrame_CreateSwapchain: format={}, usageFlags=0x{:x}, arraySize={}, width={}x{}\n",
                                        createInfo.format, createInfo.usageFlags, createInfo.arraySize,
                                        createInfo.width, createInfo.height);
                        QVF_TRACE("xrEndFrame_CreateSwapchain",
                                  TLArg(m_viewManager.m_fullFovResolution.width, "Width"),
                                  TLArg(m_viewManager.m_fullFovResolution.height, "Height"));
                        const XrResult swapchainResult = m_openXrApi->OpenXrApi::xrCreateSwapchain(
                            session, &createInfo, &swapchainForStereoView->fullFovSwapchain);
                        if (swapchainResult != XR_SUCCESS) {
                            LogWarning("xrEndFrame_CreateSwapchain failed with XrResult={}\n", static_cast<int>(swapchainResult));
                            return XR_ERROR_RUNTIME_FAILURE;
                        }
                    }

                    XrCompositionLayerProjectionView focusView = proj->views[focusViewIndex];
                    if (useQuadViews && m_focusFovQuirk.isEnabled()) {
                        // Quirk for DCS World: the application does not pass the correct FOV for the
                        // focus views in xrEndFrame(). We must keep track of the correct values for
                        // each frame.
                        m_focusFovQuirk.lookupFov(frameEndInfo->displayTime, focusViewIndex, focusView.fov);
                    }

                    // Composite the focus view and the stereo view together into a single stereo view.
                    compositeViewContent(viewIndex,
                                         proj->views[viewIndex],
                                         *swapchainForStereoView,
                                         focusView,
                                         *swapchainForFocusView,
                                         proj->layerFlags,
                                         useQuadViews);

                    // FIX (Item 5): If the compositor failed mid-frame, abort processing and fallback
                    if (m_compositorFailed) {
                        outLayers.assign(frameEndInfo->layers, frameEndInfo->layers + frameEndInfo->layerCount);
                        return XR_SUCCESS;
                    }

                    // Patch the view to reference the new swapchain at full FOV.
                    XrCompositionLayerProjectionView& patchedView =
                        projectionViewAllocator.back()[viewIndex];
                    patchedView.fov = m_viewManager.m_cachedEyeFov[viewIndex];
                    patchedView.subImage.swapchain = swapchainForStereoView->fullFovSwapchain;
                    patchedView.subImage.imageArrayIndex = viewIndex;
                    patchedView.subImage.imageRect.offset = {0, 0};
                    patchedView.subImage.imageRect.extent = m_viewManager.m_fullFovResolution;

                    if (requestedDepthSubmission && m_swapchainManager.getDeferredReleaseQuirk()) {
                        const XrBaseInStructure* entry =
                            reinterpret_cast<const XrBaseInStructure*>(proj->views[viewIndex].next);
                        while (entry) {
                            if (entry->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                                const XrCompositionLayerDepthInfoKHR* depth =
                                    reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(entry);

                                QVF_TRACE("xrEndFrame_View",
                                          TLArg("Depth", "Type"),
                                          TLArg(viewIndex, "ViewIndex"),
                                          TLXArg(depth->subImage.swapchain, "Swapchain"),
                                          TLArg(depth->subImage.imageArrayIndex, "ImageArrayIndex"),
                                          TLArg(xr::ToString(depth->subImage.imageRect).c_str(), "ImageRect"),
                                          TLArg(depth->nearZ, "Near"),
                                          TLArg(depth->farZ, "Far"),
                                          TLArg(depth->minDepth, "MinDepth"),
                                          TLArg(depth->maxDepth, "MaxDepth"));

                                std::shared_ptr<SwapchainManager::Swapchain> swapchainForDepthInfo =
                                    m_swapchainManager.getSwapchain(depth->subImage.swapchain);
                                if (!swapchainForDepthInfo) {
                                    return XR_ERROR_HANDLE_INVALID;
                                }

                                if (swapchainForDepthInfo->deferredRelease) {
                                    swapchainsToRelease.insert(depth->subImage.swapchain);
                                    swapchainForDepthInfo->deferredRelease = false;
                                }
                            }
                            entry = entry->next;
                        }
                    }
                }

                // Note: if a depth buffer was attached, we will use it as-is (per copy of the proj
                // struct below, and therefore its entire chain of next structs). This is good: we will
                // submit a depth that matches the composited view, but that is lower resolution.

                projectionAllocator.push_back(*proj);
                // Our shader always premultiplies the alpha channel.
                projectionAllocator.back().layerFlags &= ~XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                projectionAllocator.back().views = projectionViewAllocator.back().data();
                projectionAllocator.back().viewCount = xr::StereoView::Count;
                outLayers.push_back(
                    reinterpret_cast<XrCompositionLayerBaseHeader*>(&projectionAllocator.back()));

            } else {
                if (m_swapchainManager.getDeferredReleaseQuirk()) {
                    if (frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                        const XrCompositionLayerQuad* quad =
                            reinterpret_cast<const XrCompositionLayerQuad*>(frameEndInfo->layers[i]);

                        std::shared_ptr<SwapchainManager::Swapchain> swapchainEntry =
                            m_swapchainManager.getSwapchain(quad->subImage.swapchain);
                        if (swapchainEntry && swapchainEntry->deferredRelease) {
                            swapchainsToRelease.insert(quad->subImage.swapchain);
                            swapchainEntry->deferredRelease = false;
                        }
                    }
                    // TODO: We need to handle all other types of composition layers in order to mark
                    // the swapchains for deferred release. Luckily we only need this quirk on Varjo and
                    // the runtime does not support any other type of composition layers.
                }

                QVF_TRACE("xrEndFrame_Layer",
                          TLArg(xr::ToCString(frameEndInfo->layers[i]->type), "Type"));
                outLayers.push_back(frameEndInfo->layers[i]);
            }
        }

        outSwapchainsToRelease = std::move(swapchainsToRelease);
        return XR_SUCCESS;
    }

    void LayerComposer::compositeViewContent(uint32_t viewIndex,
                                              const XrCompositionLayerProjectionView& stereoView,
                                              SwapchainManager::Swapchain& swapchainForStereoView,
                                              const XrCompositionLayerProjectionView& focusView,
                                              SwapchainManager::Swapchain& swapchainForFocusView,
                                              XrCompositionLayerFlags layerFlags,
                                              bool useQuadViews) {
        // Lazy initialization of the compositor resources.
        if (!m_graphicsContext.getCompositor()->isInitialized()) {
            LogDebug("Initializing compositor resources (format={})\n",
                            swapchainForStereoView.createInfo.format);

            try {
                bool initSuccess = m_graphicsContext.getCompositor()->initialize(static_cast<int32_t>(swapchainForStereoView.createInfo.format));
                if (!initSuccess) {
                    throw std::runtime_error("Compositor initialize() returned false");
                }
                LogDebug("Compositor resources initialized\n");
            } catch (const std::exception& e) {
                LogError("Compositor initialization failed: {}\n", e.what());
                m_compositorFailed = true;
                return;
            } catch (...) {
                LogError("Compositor initialization failed with unknown exception\n");
                m_compositorFailed = true;
                return;
            }
        }

        // Re-check isInitialized() as a safety net: if initialization threw and was caught
        // upstream, the compositor will be in an uninitialized state. Skip composition to
        // prevent null dereference crash.
        if (!m_graphicsContext.getCompositor()->isInitialized()) {
            LogError("Compositor initialization failed, skipping composition for this frame.\n");
            m_compositorFailed = true;
            return;
        }

        // Build compositor parameters
        CompositorParams params;
        params.viewIndex = viewIndex;
        params.cachedEyeFov = m_viewManager.m_cachedEyeFov[viewIndex];
        params.fullFovResolution = m_viewManager.m_fullFovResolution;
        params.useQuadViews = useQuadViews;
        params.smoothenFocusViewEdges = m_config.m_smoothenFocusViewEdges;
        params.sharpenFocusView = m_config.m_sharpenFocusView;
        params.debugFocusView = m_config.m_debugFocusView;
        params.debugEyeGaze = m_config.m_debugEyeGaze;
        params.eyeGaze = m_viewManager.m_eyeGaze[viewIndex];
        params.layerFlags = layerFlags;
        params.ditheringAmount = m_config.m_ditheringAmount;
        static uint32_t s_frameCount = 0;
        params.frameCount = s_frameCount++;

        // Build swapchain info
        SwapchainInfo stereoSwapchainInfo;
        stereoSwapchainInfo.handle = stereoView.subImage.swapchain;
        stereoSwapchainInfo.createInfo = swapchainForStereoView.createInfo;
        stereoSwapchainInfo.fullFovSwapchain = swapchainForStereoView.fullFovSwapchain;
        stereoSwapchainInfo.lastReleasedIndex = swapchainForStereoView.lastReleasedIndex;

        SwapchainInfo focusSwapchainInfo;
        focusSwapchainInfo.handle = focusView.subImage.swapchain;
        focusSwapchainInfo.createInfo = swapchainForFocusView.createInfo;
        focusSwapchainInfo.fullFovSwapchain = swapchainForFocusView.fullFovSwapchain;
        focusSwapchainInfo.lastReleasedIndex = swapchainForFocusView.lastReleasedIndex;

        // Delegate to compositor
        void* result = m_graphicsContext.getCompositor()->compositeView(params,
                                     stereoSwapchainInfo,
                                     stereoView,
                                     focusSwapchainInfo,
                                     focusView);
        if (!result) {
            LogError("Compositor returned null destination — aborting composition.\n");
            m_compositorFailed = true;
            return;
        }
    }

} // namespace openxr_api_layer
