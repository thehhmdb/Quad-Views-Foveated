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
#include <filesystem>
#include <string>

namespace openxr_api_layer {

    struct FoveationConfig {
        float m_peripheralPixelDensity{0.5f};
        float m_focusPixelDensity{1.f};
        // [0] = non-foveated, [1] = foveated
        float m_horizontalFovSection[2]{0.5f, 0.35f};
        float m_verticalFovSection[2]{0.45f, 0.35f};
        float m_horizontalFocusOffset{0.f};
        float m_verticalFocusOffset{0.f};
        float m_horizontalFixedOffset{0.f};
        float m_verticalFixedOffset{0.f};
        float m_horizontalFocusWideningMultiplier{0.5f};
        float m_verticalFocusWideningMultiplier{0.2f};
        float m_focusWideningDeadzone{0.15f};
        bool m_preferFoveatedRendering{true};
        bool m_forceNoEyeTracking{false};
        float m_smoothenFocusViewEdges{0.2f};
        float m_sharpenFocusView{0.7f};
        float m_chromaticAberrationCorrection{0.001f};
        float m_fovTangentX{1.f};
        float m_fovTangentY{1.f};
        bool m_useTurboMode{true};
        bool m_unadvertiseQuadViews{false};

        bool m_debugSimulateTracking{false};
        bool m_debugFocusView{false};
        bool m_debugEyeGaze{false};
        bool m_debugKeys{false};

        float m_eyeTrackingConfidenceThreshold{0.5f};
        uint32_t m_eyeGazeCacheTimeoutMs{600};

        // 1-Euro Filter parameters for eye tracking smoothing
        float m_eyeTrackingMinCutoff{1.0f};    // Lower = more smoothing when still
        float m_eyeTrackingBeta{0.007f};       // Higher = less smoothing when moving fast

        // Context needed for parsing sections
        std::string m_runtimeName;
        std::string m_systemName;
        std::string m_applicationName;
        std::string m_applicationExecutableName;

        // Quirk flags parsed from config
        bool m_needFocusFovCorrectionQuirk{false};

        void LoadConfiguration(const std::filesystem::path& configPath);
        bool ParseConfigurationStatement(const std::string& line, unsigned int lineNumber, bool active,
                                         const std::string& applicationName, const std::string& applicationExecutableName);
    };

} // namespace openxr_api_layer
