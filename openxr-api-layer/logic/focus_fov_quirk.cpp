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
#include "logic/focus_fov_quirk.h"
#include "framework/log.h"
#include "views.h"

namespace openxr_api_layer {

    using namespace log;

    void FocusFovQuirk::setEnabled(bool enabled) {
        m_enabled = enabled;
    }

    bool FocusFovQuirk::isEnabled() const {
        return m_enabled;
    }

    void FocusFovQuirk::storeFov(XrTime displayTime, const XrFovf& leftFov, const XrFovf& rightFov) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrLocateViews_StoreFovForQuirk");
        std::unique_lock lock(m_mutex);

        m_fovForDisplayTime.insert_or_assign(
            displayTime,
            std::make_pair(leftFov, rightFov));
        TraceLoggingWriteStop(local, "xrLocateViews_StoreFovForQuirk");
    }

    bool FocusFovQuirk::lookupFov(XrTime displayTime, uint32_t focusViewIndex, XrFovf& outFov) const {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrEndFrame_LookupFovForQuirk");
        std::unique_lock lock(m_mutex);

        bool found = false;
        const auto& cit = m_fovForDisplayTime.find(displayTime);
        if (cit != m_fovForDisplayTime.cend()) {
            outFov = focusViewIndex == xr::QuadView::FocusLeft ? cit->second.first : cit->second.second;
            found = true;
        }
        TraceLoggingWriteStop(local, "xrEndFrame_LookupFovForQuirk", TLArg(found, "Found"));
        return found;
    }

    void FocusFovQuirk::ageOldEntries(XrTime currentDisplayTime) {
        TraceLocalActivity(local);
        TraceLoggingWriteStart(local, "xrEndFrame_AgeFovForQuirk");
        std::unique_lock lock(m_mutex);

        // Delete all entries older than 1s.
        while (!m_fovForDisplayTime.empty() &&
               m_fovForDisplayTime.cbegin()->first < currentDisplayTime - 1'000'000'000) {
            m_fovForDisplayTime.erase(m_fovForDisplayTime.begin());
        }
        TraceLoggingWriteStop(local,
                              "xrEndFrame_AgeFovForQuirk",
                              TLArg(m_fovForDisplayTime.size(), "DictionarySize"));
    }

} // namespace openxr_api_layer
