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

#pragma once

#include "pch.h"
#include "framework/dispatch.gen.h"
#include "logic/eye_tracker.h"

namespace openxr_api_layer {

    class ActionManager {
      public:
        ActionManager(OpenXrApi* openXrApi, EyeTracker& eyeTracker);

        // Called from xrAttachSessionActionSets — injects eye tracker action set.
        XrResult attachSessionActionSets(XrSession session,
                                         const XrSessionActionSetsAttachInfo* attachInfo);

        // Called from xrSyncActions — injects eye tracker action set.
        XrResult syncActions(XrSession session, const XrActionsSyncInfo* syncInfo);

        // Called from xrBeginFrame — polls events / attaches / syncs if the app hasn't.
        void pollAndSyncIfNeeded(XrSession session, uint64_t framesElapsed,
                                 bool useQuadViews, EyeTracker::Tracker trackerType);

        // Reset all flags (called on session restart).
        void reset();

        // Called from xrPollEvent to clear the needPollEvent flag.
        void setPollEventDone();

        // Accessors for unit tests
        bool needPollEvent() const { return m_needPollEvent; }
        bool needAttachActionSets() const { return m_needAttachActionSets; }
        bool needSyncActions() const { return m_needSyncActions; }

      private:
        OpenXrApi* m_openXrApi;
        EyeTracker& m_eyeTracker;
        bool m_needPollEvent{true};
        bool m_needAttachActionSets{true};
        bool m_needSyncActions{true};
    };

} // namespace openxr_api_layer
