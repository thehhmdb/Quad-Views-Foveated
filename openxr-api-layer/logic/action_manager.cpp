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
#include "logic/action_manager.h"
#include "framework/log.h"
#include "framework/util.h"

namespace openxr_api_layer {

    using namespace log;
    using namespace xr;

    ActionManager::ActionManager(OpenXrApi* openXrApi, EyeTracker& eyeTracker)
        : m_openXrApi(openXrApi), m_eyeTracker(eyeTracker) {
    }

    XrResult ActionManager::attachSessionActionSets(XrSession session,
                                                     const XrSessionActionSetsAttachInfo* attachInfo) {
        QVF_TRACE("xrAttachSessionActionSets", TLXArg(session, "Session"));

        XrSessionActionSetsAttachInfo chainAttachInfo = *attachInfo;
        std::vector<XrActionSet> actionSets;
        if (m_eyeTracker.eyeTrackerActionSet() != XR_NULL_HANDLE) {
            // Suggest the bindings for the eye tracker. We do this last in order to override previous bindings the
            // application may have done.
            XrActionSuggestedBinding binding;
            binding.action = m_eyeTracker.eyeGazeAction();

            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                                                   nullptr};
            CHECK_XRCMD(
                m_openXrApi->OpenXrApi::xrStringToPath(m_openXrApi->GetXrInstance(), "/user/eyes_ext/input/gaze_ext/pose", &binding.binding));
            CHECK_XRCMD(m_openXrApi->OpenXrApi::xrStringToPath(m_openXrApi->GetXrInstance(),
                                                                "/interaction_profiles/ext/eye_gaze_interaction",
                                                                &suggestedBindings.interactionProfile));
            suggestedBindings.suggestedBindings = &binding;
            suggestedBindings.countSuggestedBindings = 1;
            CHECK_XRCMD(m_openXrApi->OpenXrApi::xrSuggestInteractionProfileBindings(m_openXrApi->GetXrInstance(), &suggestedBindings));

            // Inject our actionset.
            actionSets.assign(chainAttachInfo.actionSets,
                              chainAttachInfo.actionSets + chainAttachInfo.countActionSets);
            actionSets.push_back(m_eyeTracker.eyeTrackerActionSet());

            chainAttachInfo.actionSets = actionSets.data();
            chainAttachInfo.countActionSets = static_cast<uint32_t>(actionSets.size());
        }

        const XrResult result = m_openXrApi->OpenXrApi::xrAttachSessionActionSets(session, &chainAttachInfo);

        m_needAttachActionSets = false;

        return result;
    }

    XrResult ActionManager::syncActions(XrSession session, const XrActionsSyncInfo* syncInfo) {
        QVF_TRACE("xrSyncActions", TLXArg(session, "Session"));

        std::vector<XrActiveActionSet> activeActionSets;
        XrActionsSyncInfo chainSyncInfo = *syncInfo;
        // Inject our own actionset if needed.
        if (m_eyeTracker.eyeTrackerActionSet() != XR_NULL_HANDLE) {
            activeActionSets.assign(chainSyncInfo.activeActionSets,
                                    chainSyncInfo.activeActionSets + chainSyncInfo.countActiveActionSets);
            activeActionSets.push_back({m_eyeTracker.eyeTrackerActionSet(), XR_NULL_PATH});

            chainSyncInfo.activeActionSets = activeActionSets.data();
            chainSyncInfo.countActiveActionSets = (uint32_t)activeActionSets.size();
        }

        const XrResult result = m_openXrApi->OpenXrApi::xrSyncActions(session, &chainSyncInfo);

        m_needSyncActions = false;

        return result;
    }

    void ActionManager::pollAndSyncIfNeeded(XrSession session, uint64_t framesElapsed,
                                             bool useQuadViews, EyeTracker::Tracker trackerType) {
        if (!useQuadViews || trackerType != EyeTracker::Tracker::EyeGazeInteraction) {
            return;
        }

        // Give the app 100 frames to tell us what it intends to do regarding the action system.
        if (framesElapsed <= 100) {
            return;
        }

        // Some applications may not advance the instance event state machine (via
        // xrPollEvent()), which causes actions to always return an inactive state. Force
        // xrPollEvent() here if needed.
        if (m_needPollEvent) {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "xrBeginFrame_PollEvent");
            XrEventDataBuffer buf{XR_TYPE_EVENT_DATA_BUFFER};
            m_openXrApi->OpenXrApi::xrPollEvent(m_openXrApi->GetXrInstance(), &buf);
            QVF_TRACE_STOP(local, "xrBeginFrame_PollEvent");
        }

        if (m_needAttachActionSets) {
            // This will clear the m_needAttachActionSets flag.
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "xrBeginFrame_AttachSessionActionSets");
            XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
            const XrResult attachResult = attachSessionActionSets(session, &attachInfo);
            if (XR_FAILED(attachResult) && attachResult != XR_ERROR_ACTIONSETS_ALREADY_ATTACHED) {
                LogWarning("xrBeginFrame: attachSessionActionSets failed: {}\n", xr::ToCString(attachResult));
            }
            QVF_TRACE_STOP(local, "xrBeginFrame_AttachSessionActionSets");
        }

        // If an application does not use motion controllers, it is not calling xrSyncActions().
        // Make a call here in order to synchronize our action set.
        if (m_needSyncActions) {
            XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
            XrActiveActionSet actionSet{};
            actionSet.actionSet = m_eyeTracker.eyeTrackerActionSet();
            syncInfo.activeActionSets = &actionSet;
            syncInfo.countActiveActionSets = 1;
            {
                TraceLocalActivity(local);
                QVF_TRACE_START(local, "xrBeginFrame_SyncActions");
                CHECK_XRCMD(m_openXrApi->OpenXrApi::xrSyncActions(session, &syncInfo));
                QVF_TRACE_STOP(local, "xrBeginFrame_SyncActions");
            }
        }
    }

    void ActionManager::reset() {
        m_needPollEvent = true;
        m_needAttachActionSets = true;
        m_needSyncActions = true;
    }

    void ActionManager::setPollEventDone() {
        m_needPollEvent = false;
    }

} // namespace openxr_api_layer
