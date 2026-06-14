// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "pch.h"

namespace openxr_api_layer::log {

    TRACELOGGING_DECLARE_PROVIDER(g_traceProvider);

    extern TraceLoggingActivity<g_traceProvider> g_traceGlobal;

#define IsTraceEnabled() TraceLoggingProviderEnabled(g_traceProvider, 0, 0)

#define TraceLocalActivity(activity) TraceLoggingActivity<g_traceProvider> activity;

#define TLArg(var, ...) TraceLoggingValue(var, ##__VA_ARGS__)
#define TLPArg(var, ...) TraceLoggingPointer(var, ##__VA_ARGS__)
#ifdef _M_IX86
#define TLXArg TLArg
#else
#define TLXArg TLPArg
#endif

    // Log levels - messages are logged if their level >= current log level
    enum class LogLevel {
        Verbose = 0,
        Debug = 1,
        Information = 2,
        Warning = 3,
        Error = 4,
        Fatal = 5,
    };

    // Forward declaration of Log (defined below) - needed by template functions
    void Log(const char* fmt, ...);

    // Get/set the current log level (default: Information)
    LogLevel GetLogLevel();
    void SetLogLevel(LogLevel level);

    // Parse log level from string (used by settings parser)
    // Returns true if the string is a valid log level name
    bool ParseLogLevel(const char* value);

    // Level-gated logging functions.
    // The format string is only evaluated (via fmt::format) if the level is enabled,
    // avoiding allocation overhead for filtered messages.
    template<typename... Args>
    inline void LogVerbose(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Verbose) {
            Log(fmt::format(fmt, args...).c_str());
        }
    }

    template<typename... Args>
    inline void LogDebug(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Debug) {
            Log(fmt::format(fmt, args...).c_str());
        }
    }

    template<typename... Args>
    inline void LogInformation(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Information) {
            Log(fmt::format(fmt, args...).c_str());
        }
    }

    template<typename... Args>
    inline void LogWarning(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Warning) {
            Log(fmt::format(fmt, args...).c_str());
        }
    }

    template<typename... Args>
    inline void LogError(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Error) {
            Log(fmt::format(fmt, args...).c_str());
        }
    }

    template<typename... Args>
    inline void LogFatal(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Fatal) {
            Log(fmt::format(fmt, args...).c_str());
        }
    }

    // Legacy logging functions (kept for backward compatibility).
    // Log() maps to Information level.
    void Log(const char* fmt, ...);
    static inline void Log(const std::string_view& str) {
        Log(str.data());
    }

    // DebugLog() - compile-gated to _DEBUG builds, maps to Debug level.
    void DebugLog(const char* fmt, ...);
    static inline void DebugLog(const std::string_view& str) {
        Log(str.data());
    }

    // ErrorLog() - rate-limited, maps to Error level.
    void ErrorLog(const char* fmt, ...);
    static inline void ErrorLog(const std::string_view& str) {
        Log(str.data());
    }

} // namespace openxr_api_layer::log
