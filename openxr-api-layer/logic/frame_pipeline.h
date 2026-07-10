// MIT License
//
// Copyright(c) 2023 Matthieu Bucchianeri
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
#include <mutex>
#include <future>
#include <deque>
#include <chrono>

namespace openxr_api_layer {

    class OpenXrApi;
    namespace utils::general { struct ITimer; }
    namespace utils::graphics { struct IGraphicsTimer; }

    // Manages frame pipeline: turbo mode async waiting, frame timing, and frame statistics.
    // Extracted from OpenXrLayer to reduce monolithic class size.
    class FramePipeline {
      public:
        FramePipeline();
        ~FramePipeline() = default;

        // Non-copyable
        FramePipeline(const FramePipeline&) = delete;
        FramePipeline& operator=(const FramePipeline&) = delete;

        // Wait for the next frame. Returns XR_SUCCESS on success.
        // In turbo mode, this may return immediately with predicted values.
        XrResult waitFrame(OpenXrApi* openXrApi,
                          XrSession session,
                          const XrFrameWaitInfo* frameWaitInfo,
                          XrFrameState* frameState,
                          bool useTurboMode,
                          bool* outIsAsyncMode);

        // Begin frame processing.
        XrResult beginFrame(OpenXrApi* openXrApi,
                           XrSession session,
                           const XrFrameBeginInfo* frameBeginInfo,
                           bool isAsyncMode);

        // End frame processing. Handles turbo mode async completion and re-dispatch.
        XrResult endFrame(OpenXrApi* openXrApi,
                         XrSession session,
                         const XrFrameEndInfo* frameEndInfo,
                         bool useTurboMode,
                         bool* outIsAsyncMode);

        // Cleanup pending async work (called during session destroy).
        void destroy();

        // Frame timing management
        void recordFrameTime();
        uint32_t getFrameRate() const;
        
        // Frame state accessors
        uint64_t getFramesElapsed() const { return m_framesElapsed; }
        XrTime getWaitedFrameTime() const { return m_waitedFrameTime; }
        void incrementFrame() { m_framesElapsed++; }
        void setWaitedFrameTime(XrTime time) { m_waitedFrameTime = time; }
        void resetFrameCount() { m_framesElapsed = 0; }

        // Timer management
        void startFrameCpuTimer();
        void stopRenderCpuTimer(uint64_t& outRenderTime);
        void stopAndRotateGpuTimer(uint64_t& outGpuTime);

        // Set timers (called after graphics context initialization)
        void setCpuTimers(std::shared_ptr<utils::general::ITimer> frameTimer,
                         std::shared_ptr<utils::general::ITimer> renderTimer);
        void setGpuTimer(uint32_t index, std::shared_ptr<utils::graphics::IGraphicsTimer> timer);
        static constexpr uint32_t kGpuTimerCount = 3;

        // Frame mutex for synchronizing turbo mode state
        std::mutex& getFrameMutex() { return m_frameMutex; }

      private:
        XrResult executeWaitFrame(OpenXrApi* openXrApi,
                                 XrSession session,
                                 const XrFrameWaitInfo* frameWaitInfo,
                                 XrFrameState* frameState);
        XrResult executeBeginFrame(OpenXrApi* openXrApi,
                                  XrSession session,
                                  const XrFrameBeginInfo* frameBeginInfo);
        XrResult executeEndFrame(OpenXrApi* openXrApi,
                                XrSession session,
                                const XrFrameEndInfo* frameEndInfo);

        // Frame state
        std::mutex m_frameMutex;
        std::chrono::time_point<std::chrono::steady_clock> m_lastFrameWaitTimestamp{};
        XrTime m_waitedFrameTime{0};
        uint64_t m_framesElapsed{0};

        // Turbo mode async state
        std::mutex m_asyncWaitMutex;
        std::future<void> m_asyncWaitPromise;
        XrTime m_lastPredictedDisplayTime{0};
        XrTime m_lastPredictedDisplayPeriod{0};
        std::atomic<bool> m_lastShouldRender{true};
        std::atomic<bool> m_asyncWaitPolled{false};
        std::atomic<bool> m_asyncWaitCompleted{false};

        // Performance: Persistent worker thread for turbo mode (avoids per-frame thread creation)
        std::thread m_asyncWorkerThread;
        std::atomic<bool> m_asyncWorkerRunning{false};
        std::mutex m_asyncWorkMutex;
        std::condition_variable m_asyncWorkCv;
        struct AsyncWork {
            OpenXrApi* openXrApi{nullptr};
            XrSession session{XR_NULL_HANDLE};
            std::promise<void> promise;
        };
        AsyncWork m_currentWork;
        AsyncWork m_pendingWork;

        // Timers
        std::shared_ptr<utils::general::ITimer> m_appFrameCpuTimer;
        std::shared_ptr<utils::general::ITimer> m_appRenderCpuTimer;
        std::shared_ptr<utils::graphics::IGraphicsTimer> m_appFrameGpuTimer[kGpuTimerCount];
        uint32_t m_appFrameGpuTimerIndex{0};

        // Frame statistics
        std::deque<std::chrono::time_point<std::chrono::steady_clock>> m_frameTimes;
    };

} // namespace openxr_api_layer
