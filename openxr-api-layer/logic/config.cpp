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
#include "config.h"
#include "framework/log.h"

namespace openxr_api_layer {

    using namespace log;

    void FoveationConfig::LoadConfiguration(const std::filesystem::path& configPath) {
        // Look in %LocalAppData% first, then fallback to your installation folder.
        LogInformation("Trying to locate configuration file at '{}'...\n", configPath.string());
        std::ifstream configFile;
        configFile.open(configPath);
        if (configFile.is_open()) {
            bool active = true;
            unsigned int lineNumber = 0;
            std::string line;
            while (std::getline(configFile, line)) {
                lineNumber++;
                active = ParseConfigurationStatement(line, lineNumber, active, m_applicationName, m_applicationExecutableName);
            }
            configFile.close();
        } else {
            LogInformation("Not found\n");
        }
    }

    bool FoveationConfig::ParseConfigurationStatement(const std::string& line, unsigned int lineNumber, bool active,
                                                       const std::string& applicationName, const std::string& applicationExecutableName) {
        try {
            if (line.empty()) {
                return active;
            }

            // Handle comments.
            if ((line[0] == '/' && line.size() > 1 && line[1] == '/') || line[0] == '#') {
                return active;
            }

            // Toggle active section.
            if (line[0] == '[' && line[line.size() - 1] == ']') {
                // Guard against malformed/short section headers (e.g. "[]" or "[a]").
                if (line.size() < 3) {
                    LogWarning("L%u: Malformed section header (too short)\n", lineNumber);
                    return active;
                }
                if (line.size() >= 6 && line.substr(1, 4) == "app:") {
                    const std::string pattern = line.substr(5, line.size() - 6);
                    if (pattern.size() >= 6 && pattern.substr(0, 6) == "exact:") {
                        return applicationName == pattern.substr(6);
                    }
                    return applicationName.find(pattern) != std::string::npos;
                } else if (line.size() >= 6 && line.substr(1, 4) == "exe:") {
                    const std::string pattern = line.substr(5, line.size() - 6);
                    if (pattern.size() >= 6 && pattern.substr(0, 6) == "exact:") {
                        return applicationExecutableName == pattern.substr(6);
                    }
                    return applicationExecutableName.find(pattern) != std::string::npos;
                } else {
                    return m_runtimeName.find(line.substr(1, line.size() - 2)) != std::string::npos ||
                           m_systemName.find(line.substr(1, line.size() - 2)) != std::string::npos;
                }
            }

            // Skip sections not for the current runtime.
            if (!active) {
                return active;
            }

            const auto offset = line.find('=');
            if (offset != std::string::npos) {
                const std::string name = line.substr(0, offset);
                const std::string value = line.substr(offset + 1);

                bool parsed = false;
                if (name == "peripheral_multiplier") {
                    const float v = std::stof(value);
                    if (v < 0.1f) {
                        LogWarning("L%u: peripheral_multiplier {} below minimum 0.1, clamped\n", lineNumber, v);
                    }
                    m_peripheralPixelDensity = std::max(0.1f, v);
                    parsed = true;
                } else if (name == "focus_multiplier") {
                    const float v = std::stof(value);
                    if (v < 0.1f) {
                        LogWarning("L%u: focus_multiplier {} below minimum 0.1, clamped\n", lineNumber, v);
                    }
                    m_focusPixelDensity = std::max(0.1f, v);
                    parsed = true;
                } else if (name == "horizontal_fixed_section") {
                    const float v = std::stof(value);
                    m_horizontalFovSection[0] = std::clamp(v, 0.1f, 0.9f);
                    if (m_horizontalFovSection[0] != v) {
                        LogWarning("L%u: horizontal_fixed_section {} out of [0.1, 0.9], clamped to {}\n",
                                   lineNumber, v, m_horizontalFovSection[0]);
                    }
                    parsed = true;
                } else if (name == "vertical_fixed_section") {
                    const float v = std::stof(value);
                    m_verticalFovSection[0] = std::clamp(v, 0.1f, 0.9f);
                    if (m_verticalFovSection[0] != v) {
                        LogWarning("L%u: vertical_fixed_section {} out of [0.1, 0.9], clamped to {}\n",
                                   lineNumber, v, m_verticalFovSection[0]);
                    }
                    parsed = true;
                } else if (name == "horizontal_focus_section") {
                    const float v = std::stof(value);
                    m_horizontalFovSection[1] = std::clamp(v, 0.1f, 0.9f);
                    if (m_horizontalFovSection[1] != v) {
                        LogWarning("L%u: horizontal_focus_section {} out of [0.1, 0.9], clamped to {}\n",
                                   lineNumber, v, m_horizontalFovSection[1]);
                    }
                    parsed = true;
                } else if (name == "vertical_focus_section") {
                    const float v = std::stof(value);
                    m_verticalFovSection[1] = std::clamp(v, 0.1f, 0.9f);
                    if (m_verticalFovSection[1] != v) {
                        LogWarning("L%u: vertical_focus_section {} out of [0.1, 0.9], clamped to {}\n",
                                   lineNumber, v, m_verticalFovSection[1]);
                    }
                    parsed = true;
                } else if (name == "horizontal_fixed_offset") {
                    m_horizontalFixedOffset = std::clamp(std::stof(value), -0.5f, 0.5f);
                    parsed = true;
                } else if (name == "vertical_fixed_offset") {
                    m_verticalFixedOffset = std::clamp(std::stof(value), -0.5f, 0.5f);
                    parsed = true;
                } else if (name == "horizontal_focus_offset") {
                    m_horizontalFocusOffset = std::clamp(std::stof(value), -0.5f, 0.5f);
                    parsed = true;
                } else if (name == "vertical_focus_offset") {
                    m_verticalFocusOffset = std::clamp(std::stof(value), -0.5f, 0.5f);
                    parsed = true;
                } else if (name == "horizontal_focus_widening_multiplier") {
                    m_horizontalFocusWideningMultiplier = std::clamp(std::stof(value), 0.f, 2.f);
                    parsed = true;
                } else if (name == "vertical_focus_widening_multiplier") {
                    m_verticalFocusWideningMultiplier = std::clamp(std::stof(value), 0.f, 2.f);
                    parsed = true;
                } else if (name == "focus_widening_deadzone") {
                    m_focusWideningDeadzone = std::clamp(std::stof(value), 0.f, 0.5f);
                    parsed = true;
                } else if (name == "prefer_foveated_rendering") {
                    m_preferFoveatedRendering = std::stoi(value);
                    parsed = true;
                } else if (name == "force_no_eye_tracking") {
                    m_forceNoEyeTracking = std::stoi(value);
                    parsed = true;
                } else if (name == "force_focus_fov_quirk") {
                    m_needFocusFovCorrectionQuirk = m_needFocusFovCorrectionQuirk || std::stoi(value);
                    parsed = true;
                } else if (name == "smoothen_focus_view_edges") {
                    m_smoothenFocusViewEdges = std::clamp(std::stof(value), 0.f, 0.5f);
                    parsed = true;
                } else if (name == "sharpen_focus_view") {
                    m_sharpenFocusView = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "fov_tangent_x") {
                    m_fovTangentX = std::clamp(std::stof(value), 0.1f, 1.f);
                    parsed = true;
                } else if (name == "fov_tangent_y") {
                    m_fovTangentY = std::clamp(std::stof(value), 0.1f, 1.f);
                    parsed = true;
                } else if (name == "turbo_mode") {
                    m_useTurboMode = std::stoi(value);
                    parsed = true;
                } else if (name == "unadvertise") {
                    m_unadvertiseQuadViews = std::stoi(value);
                    parsed = true;
                } else if (name == "debug_simulate_tracking") {
                    m_debugSimulateTracking = std::stoi(value);
                    parsed = true;
                } else if (name == "debug_focus_view") {
                    m_debugFocusView = std::stoi(value);
                    parsed = true;
                } else if (name == "debug_eye_gaze") {
                    m_debugEyeGaze = std::stoi(value);
                    parsed = true;
                } else if (name == "debug_keys") {
                    m_debugKeys = std::stoi(value);
                    parsed = true;
                } else if (name == "eye_tracking_confidence_threshold") {
                    m_eyeTrackingConfidenceThreshold = std::clamp(std::stof(value), 0.f, 1.f);
                    parsed = true;
                } else if (name == "eye_gaze_cache_timeout_ms") {
                    m_eyeGazeCacheTimeoutMs = std::stoul(value);
                    parsed = true;
                } else if (name == "eye_tracking_min_cutoff") {
                    m_eyeTrackingMinCutoff = std::clamp(std::stof(value), 0.001f, 10.0f);
                    parsed = true;
                } else if (name == "eye_tracking_beta") {
                    m_eyeTrackingBeta = std::clamp(std::stof(value), 0.0f, 10.0f);
                    parsed = true;
                } else if (name == "dithering_amount") {
                    m_ditheringAmount = std::clamp(std::stof(value), 0.f, 0.1f);
                    parsed = true;
                } else if (name == "log_level") {
                    if (!log::ParseLogLevel(value.c_str())) {
                        LogWarning("  Invalid log_level '{}', using default (Information).", value);
                    }
                    parsed = true;
                } else {
                    LogWarning("L%u: Unrecognized option\n", lineNumber);
                }

                if (parsed) {
                    LogInformation("  Found option '{}={}'\n", name, value);
                }
            } else {
                LogWarning("L%u: Improperly formatted option\n", lineNumber);
            }
        } catch (const std::exception& e) {
            LogWarning("L%u: Parsing error: {}\n", lineNumber, e.what());
        } catch (...) {
            LogWarning("L%u: Parsing error (unknown exception)\n", lineNumber);
        }

        return active;
    }

} // namespace openxr_api_layer
