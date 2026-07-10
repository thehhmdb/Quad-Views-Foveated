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
#include "logic/debug_keys.h"
#include "framework/log.h"

namespace openxr_api_layer {

    using namespace log;

    DebugKeyHandler::DebugKeyHandler(FoveationConfig& config)
        : m_config(config) {
    }

    void DebugKeyHandler::handle() {
        if (m_config.m_debugKeys) {
            bool log = false;

#define DEBUG_ACTION(label, key, action)                                                                               \
    static bool wasCtrl##label##Pressed = false;                                                                       \
    const bool isCtrl##label##Pressed = GetAsyncKeyState(VK_CONTROL) < 0 && GetAsyncKeyState(key) < 0;                 \
    if (!wasCtrl##label##Pressed && isCtrl##label##Pressed) {                                                          \
        log = true;                                                                                                    \
        action;                                                                                                        \
    }                                                                                                                  \
    wasCtrl##label##Pressed = isCtrl##label##Pressed;

            DEBUG_ACTION(SharpenLess, 'J', {
                if (!GetAsyncKeyState(VK_SHIFT)) {
                    m_config.m_sharpenFocusView = std::clamp(m_config.m_sharpenFocusView + 0.1f, 0.f, 1.f);
                } else {
                    m_config.m_horizontalFocusWideningMultiplier =
                        std::clamp(m_config.m_horizontalFocusWideningMultiplier + 0.05f, 0.f, 2.f);
                }
            });
            DEBUG_ACTION(SharpenMore, 'U', {
                if (!GetAsyncKeyState(VK_SHIFT)) {
                    m_config.m_sharpenFocusView = std::clamp(m_config.m_sharpenFocusView - 0.1f, 0.f, 1.f);
                } else {
                    m_config.m_horizontalFocusWideningMultiplier =
                        std::clamp(m_config.m_horizontalFocusWideningMultiplier - 0.05f, 0.f, 2.f);
                }
            });
            DEBUG_ACTION(ToggleSharpen, 'N', {
                static float lastSharpenFocusView = m_config.m_sharpenFocusView;
                if (m_config.m_sharpenFocusView) {
                    lastSharpenFocusView = m_config.m_sharpenFocusView;
                    m_config.m_sharpenFocusView = 0;
                } else {
                    m_config.m_sharpenFocusView = lastSharpenFocusView;
                }
            });
            DEBUG_ACTION(SmoothenThicknessLess, 'I', {
                if (!GetAsyncKeyState(VK_SHIFT)) {
                    m_config.m_smoothenFocusViewEdges = std::clamp(m_config.m_smoothenFocusViewEdges + 0.01f, 0.f, 1.f);
                } else {
                    m_config.m_verticalFocusWideningMultiplier =
                        std::clamp(m_config.m_verticalFocusWideningMultiplier + 0.05f, 0.f, 2.f);
                }
            });
            DEBUG_ACTION(SmoothenThicknessMore, 'K', {
                if (!GetAsyncKeyState(VK_SHIFT)) {
                    m_config.m_smoothenFocusViewEdges = std::clamp(m_config.m_smoothenFocusViewEdges - 0.01f, 0.f, 1.f);
                } else {
                    m_config.m_verticalFocusWideningMultiplier =
                        std::clamp(m_config.m_verticalFocusWideningMultiplier - 0.05f, 0.f, 2.f);
                }
            });
            DEBUG_ACTION(ToggleSmoothen, 'M', {
                static float lastSmoothenFocusViewEdges = m_config.m_smoothenFocusViewEdges;
                if (m_config.m_smoothenFocusViewEdges) {
                    lastSmoothenFocusViewEdges = m_config.m_smoothenFocusViewEdges;
                    m_config.m_smoothenFocusViewEdges = 0;
                } else {
                    m_config.m_smoothenFocusViewEdges = lastSmoothenFocusViewEdges;
                }
            });
            DEBUG_ACTION(VerticalFocusOffsetUp, 'O', {
                if (!GetAsyncKeyState(VK_SHIFT)) {
                    m_config.m_verticalFocusOffset = std::clamp(m_config.m_verticalFocusOffset + 0.01f, -1.f, 1.f);
                } else {
                    m_config.m_horizontalFocusOffset = std::clamp(m_config.m_horizontalFocusOffset + 0.01f, -1.f, 1.f);
                }
            });
            DEBUG_ACTION(VerticalFocusOffsetDown, 'L', {
                if (!GetAsyncKeyState(VK_SHIFT)) {
                    m_config.m_verticalFocusOffset = std::clamp(m_config.m_verticalFocusOffset - 0.01f, -1.f, 1.f);
                } else {
                    m_config.m_horizontalFocusOffset = std::clamp(m_config.m_horizontalFocusOffset - 0.01f, -1.f, 1.f);
                }
            });

            if (log) {
                LogInformation("sharpen_focus_view={:.1f}\n", m_config.m_sharpenFocusView);
                LogInformation("smoothen_focus_view_edges={:.2f}\n", m_config.m_smoothenFocusViewEdges);
                LogInformation("horizontal_focus_offset={:.2f}\n", m_config.m_horizontalFocusOffset);
                LogInformation("vertical_focus_offset={:.2f}\n", m_config.m_verticalFocusOffset);
                LogInformation("focus_horizontal_widening_multiplier={:.2f}\n",
                                m_config.m_horizontalFocusWideningMultiplier);
                LogInformation("focus_vertical_widening_multiplier={:.2f}\n", m_config.m_verticalFocusWideningMultiplier);
            }
        }
    }

} // namespace openxr_api_layer
