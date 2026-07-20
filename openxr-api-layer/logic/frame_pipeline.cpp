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

#include "pch.h"
#include "logic/frame_pipeline.h"
#include "framework/log.h"
#include "framework/util.h"

namespace openxr_api_layer {

    // Convenience alias for trace provider access within this module
    using log::g_traceProvider;
    using log::ErrorLog;
    using log::LogDebug;

    FramePipeline::FramePipeline() {
        // Start persistent turbo-mode worker thread
        m_waitThread = std::thread([this]() {
            while (!m_shutdown) {
                std::unique_lock lock(m_waitMutex);
                m_waitCv.wait(lock, [this]() { return m_waitRequested || m_shutdown; });
                if (m_shutdown) break;

                m_threadFrameState = {XR_TYPE_FRAME_STATE};

                // FIX (Item 7): Check for session loss to prevent infinite hangs during shutdown.
                XrResult waitResult = m_threadOpenXrApi->OpenXrApi::xrWaitFrame(
                    m_threadSession, nullptr, &m_threadFrameState);
                if (waitResult == XR_ERROR_SESSION_LOST || waitResult == XR_ERROR_HANDLE_INVALID) {
                    break;
                }
                if (XR_FAILED(waitResult)) {
                    LogDebug("Turbo mode worker: xrWaitFrame failed with {}\n",
                             xr::ToCString(waitResult));
                    break;
                }

                {
                    std::unique_lock alock(m_asyncWaitMutex);
                    m_lastPredictedDisplayTime = m_threadFrameState.predictedDisplayTime;
                    m_lastPredictedDisplayPeriod = m_threadFrameState.predictedDisplayPeriod;
                    m_lastShouldRender = m_threadFrameState.shouldRender;
                    m_asyncWaitCompleted = true;
                }

                m_asyncWaitPromise.set_value(m_threadFrameState);
                m_waitRequested = false;
            }
        });
    }

    XrResult FramePipeline::waitFrame(OpenXrApi* openXrApi,
                                      XrSession session,
                                      const XrFrameWaitInfo* frameWaitInfo,
                                      XrFrameState* frameState,
                                      bool useTurboMode,
                                      bool* outIsAsyncMode) {
        LogDebug(">> xrWaitFrame entry\n");
        QVF_TRACE("xrWaitFrame", TLXArg(session, "Session"));

        const auto lastFrameWaitTimestamp = m_lastFrameWaitTimestamp;
        m_lastFrameWaitTimestamp = std::chrono::steady_clock::now();

        {
            std::unique_lock lock(m_frameMutex);

            // Roundup frame statistics.
            if (log::IsTraceEnabled() && m_appFrameCpuTimer) {
                m_appFrameCpuTimer->stop();

                QVF_TRACE("AppStatistics",
                          TLArg(m_frameTimes.size(), "Fps"),
                          TLArg(m_appFrameCpuTimer->query(), "AppCpuTime"),
                          TLArg(m_appRenderCpuTimer ? m_appRenderCpuTimer->query() : 0, "RenderCpuTime"),
                          TLArg(0, "AppGpuTime"));
            }

            if (m_asyncWaitFuture.valid()) {
                QVF_TRACE("xrWaitFrame_AsyncWaitMode");
                *outIsAsyncMode = true;

                // In Turbo mode, we accept pipelining of exactly one frame.
                if (m_asyncWaitPolled) {
                    TraceLocalActivity(local);

                    // On second frame poll, we must wait.
                    QVF_TRACE_START(local, "xrWaitFrame_AsyncWaitNow");
                    if (m_asyncWaitFuture.wait_for(2s) != std::future_status::ready) {
                        ErrorLog("Turbo mode async wait timed out! Runtime may be hung.\n");
                        QVF_TRACE_STOP(local, "xrWaitFrame_AsyncWaitNow", TLArg("Timeout", "Result"));
                        return XR_ERROR_RUNTIME_FAILURE;
                    }
                    QVF_TRACE_STOP(local, "xrWaitFrame_AsyncWaitNow");
                }
                m_asyncWaitPolled = true;

                // In Turbo mode, we don't actually wait, we make up a predicted time.
                {
                    std::unique_lock lock(m_asyncWaitMutex);

                    frameState->predictedDisplayTime =
                        m_asyncWaitCompleted ? m_lastPredictedDisplayTime
                                             : (m_lastPredictedDisplayTime +
                                                (m_lastFrameWaitTimestamp - lastFrameWaitTimestamp).count());
                    frameState->predictedDisplayPeriod = m_lastPredictedDisplayPeriod;
                }
                frameState->shouldRender = m_lastShouldRender ? XR_TRUE : XR_FALSE;

                return XR_SUCCESS;
            } else {
                *outIsAsyncMode = false;
            }
        }

        // Normal synchronous path
        XrResult result = executeWaitFrame(openXrApi, session, frameWaitInfo, frameState);

        if (XR_SUCCEEDED(result)) {
            {
                std::unique_lock lock(m_frameMutex);
                // We must always store those values to properly handle transitions into Turbo Mode.
                m_lastPredictedDisplayTime = frameState->predictedDisplayTime;
                m_lastPredictedDisplayPeriod = frameState->predictedDisplayPeriod;
                m_lastShouldRender = frameState->shouldRender;
            }

            // Per OpenXR spec, the predicted display must increase monotonically.
            frameState->predictedDisplayTime = std::max(frameState->predictedDisplayTime, m_waitedFrameTime + 1);

            QVF_TRACE("xrWaitFrame",
                      TLArg(!!frameState->shouldRender, "ShouldRender"),
                      TLArg(frameState->predictedDisplayTime, "PredictedDisplayTime"),
                      TLArg(frameState->predictedDisplayPeriod, "PredictedDisplayPeriod"));

            // Record the predicted display time.
            m_waitedFrameTime = frameState->predictedDisplayTime;

            // Start app timers.
            {
                std::unique_lock lock(m_frameMutex);

                if (log::IsTraceEnabled() && m_appFrameCpuTimer) {
                    m_appFrameCpuTimer->start();
                }
            }
        }

        LogDebug("<< xrWaitFrame exit (result={})\n", xr::ToCString(result));
        return result;
    }

    XrResult FramePipeline::beginFrame(OpenXrApi* openXrApi,
                                       XrSession session,
                                       const XrFrameBeginInfo* frameBeginInfo,
                                       bool isAsyncMode) {
        LogDebug(">> xrBeginFrame entry (asyncMode={})\n", isAsyncMode);
        QVF_TRACE("xrBeginFrame", TLXArg(session, "Session"));

        XrResult result = XR_ERROR_RUNTIME_FAILURE;

        {
            std::unique_lock lock(m_frameMutex);

            if (isAsyncMode) {
                // In turbo mode, we do nothing here.
                QVF_TRACE("xrBeginFrame_AsyncWaitMode");
                result = XR_SUCCESS;
            } else {
                TraceLocalActivity(local);
                QVF_TRACE_START(local, "xrBeginFrame_BeginFrame");
                // Explicitly call the base class to avoid infinite recursion through the layer's virtual override
                result = openXrApi->OpenXrApi::xrBeginFrame(session, frameBeginInfo);
                QVF_TRACE_STOP(local, "xrBeginFrame_BeginFrame");
            }

            // Start app timers (inside the same lock scope - no second lock needed)
            if (log::IsTraceEnabled() && m_appFrameCpuTimer) {
                m_appRenderCpuTimer->start();
                m_appFrameGpuTimer[m_appFrameGpuTimerIndex]->start();
            }
        }

        LogDebug("<< xrBeginFrame exit (result={})\n", xr::ToCString(result));
        return result;
    }

    XrResult FramePipeline::endFrame(OpenXrApi* openXrApi,
                                     XrSession session,
                                     const XrFrameEndInfo* frameEndInfo,
                                     bool useTurboMode,
                                     bool* outIsAsyncMode) {
        LogDebug(">> xrEndFrame entry\n");
        *outIsAsyncMode = false;

        std::unique_lock lock(m_frameMutex);

        // Stop render CPU timer and rotate GPU timer
        uint64_t renderTime = 0;
        uint64_t gpuTime = 0;
        if (log::IsTraceEnabled() && m_appFrameCpuTimer) {
            m_appRenderCpuTimer->stop();
            renderTime = m_appRenderCpuTimer->query();

            m_appFrameGpuTimer[m_appFrameGpuTimerIndex]->stop();
            m_appFrameGpuTimerIndex = (m_appFrameGpuTimerIndex + 1) % kGpuTimerCount;
            gpuTime = m_appFrameGpuTimer[m_appFrameGpuTimerIndex]->query();
        }

        const auto now = std::chrono::steady_clock::now();
        m_frameTimes.push_back(now);
        while ((now - m_frameTimes.front()).count() >= 1'000'000'000) {
            m_frameTimes.pop_front();
        }

        XrResult result = XR_SUCCESS;
        if (m_asyncWaitFuture.valid()) {
            {
                TraceLocalActivity(local);

                // This is the latest point we must have fully waited a frame before proceeding.
                QVF_TRACE_START(local, "xrEndFrame_AsyncWaitNow");
                const auto ready = m_asyncWaitFuture.wait_for(1s) == std::future_status::ready;
                QVF_TRACE_STOP(local, "xrEndFrame_AsyncWaitNow", TLArg(ready, "Ready"));
                if (ready) {
                    m_asyncWaitFuture = {};
                }
            }

            {
                TraceLocalActivity(local);
                QVF_TRACE_START(local, "xrEndFrame_BeginFrame");
                // Explicitly call the base class to avoid infinite recursion through the layer's virtual override
                result = openXrApi->OpenXrApi::xrBeginFrame(session, nullptr);
                if (XR_FAILED(result)) {
                    ErrorLog(fmt::format("xrEndFrame: deferred xrBeginFrame failed with {}\n",
                                             xr::ToCString(result)));
                }
                QVF_TRACE_STOP(
                    local, "xrEndFrame_BeginFrame", TLArg(xr::ToCString(result), "Result"));
            }
        }

        if (XR_SUCCEEDED(result)) {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "xrEndFrame_EndFrame");
            // Explicitly call the base class to avoid infinite recursion through the layer's virtual override
            result = openXrApi->OpenXrApi::xrEndFrame(session, frameEndInfo);
            QVF_TRACE_STOP(local, "xrEndFrame_EndFrame");
        }

        if (XR_SUCCEEDED(result) && useTurboMode && !m_asyncWaitFuture.valid()) {
            m_asyncWaitPolled = false;
            m_asyncWaitCompleted = false;

            // In Turbo mode, we signal the persistent worker thread to start waiting.
            QVF_TRACE("xrEndFrame_AsyncWaitStart");
            m_threadOpenXrApi = openXrApi;
            m_threadSession = session;
            m_asyncWaitPromise = std::promise<XrFrameState>();
            m_asyncWaitFuture = m_asyncWaitPromise.get_future();
            {
                std::unique_lock lock(m_waitMutex);
                m_waitRequested = true;
            }
            m_waitCv.notify_one();
            *outIsAsyncMode = true;
        }

        LogDebug("<< xrEndFrame exit (result={})\n", xr::ToCString(result));
        return result;
    }

    void FramePipeline::destroy() {
        // Wait for pending async frame to complete
        if (m_asyncWaitFuture.valid()) {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "xrDestroySession_AsyncWaitNow");
            m_asyncWaitFuture.wait_for(5s);
            QVF_TRACE_STOP(local, "xrDestroySession_AsyncWaitNow");
            m_asyncWaitFuture = {};
        }

        // Shut down persistent worker thread
        m_shutdown = true;
        m_waitCv.notify_one();
        if (m_waitThread.joinable()) {
            // FIX (Item 7): Use a timeout to prevent infinite hangs.
            std::thread::native_handle_type handle = m_waitThread.native_handle();
            if (WaitForSingleObject((HANDLE)handle, 5000) != WAIT_OBJECT_0) {
                ErrorLog("Turbo mode worker thread did not exit gracefully! Detaching.\n");
                m_waitThread.detach();
            } else {
                m_waitThread.join();
            }
        }
    }

    void FramePipeline::recordFrameTime() {
        const auto now = std::chrono::steady_clock::now();
        m_frameTimes.push_back(now);
        while ((now - m_frameTimes.front()).count() >= 1'000'000'000) {
            m_frameTimes.pop_front();
        }
    }

    uint32_t FramePipeline::getFrameRate() const {
        return static_cast<uint32_t>(m_frameTimes.size());
    }

    void FramePipeline::startFrameCpuTimer() {
        if (m_appFrameCpuTimer) {
            m_appFrameCpuTimer->start();
        }
    }

    void FramePipeline::stopRenderCpuTimer(uint64_t& outRenderTime) {
        if (m_appRenderCpuTimer) {
            m_appRenderCpuTimer->stop();
            outRenderTime = m_appRenderCpuTimer->query();
        }
    }

    void FramePipeline::stopAndRotateGpuTimer(uint64_t& outGpuTime) {
        if (m_appFrameGpuTimer[m_appFrameGpuTimerIndex]) {
            m_appFrameGpuTimer[m_appFrameGpuTimerIndex]->stop();
            m_appFrameGpuTimerIndex = (m_appFrameGpuTimerIndex + 1) % kGpuTimerCount;
            outGpuTime = m_appFrameGpuTimer[m_appFrameGpuTimerIndex]->query();
        }
    }

    void FramePipeline::setCpuTimers(std::shared_ptr<utils::general::ITimer> frameTimer,
                                     std::shared_ptr<utils::general::ITimer> renderTimer) {
        m_appFrameCpuTimer = std::move(frameTimer);
        m_appRenderCpuTimer = std::move(renderTimer);
    }

    void FramePipeline::setGpuTimer(uint32_t index, std::shared_ptr<utils::graphics::IGraphicsTimer> timer) {
        if (index < kGpuTimerCount) {
            m_appFrameGpuTimer[index] = std::move(timer);
        }
    }

    XrResult FramePipeline::executeWaitFrame(OpenXrApi* openXrApi,
                                             XrSession session,
                                             const XrFrameWaitInfo* frameWaitInfo,
                                             XrFrameState* frameState) {
        TraceLocalActivity(local);
        QVF_TRACE_START(local, "xrWaitFrame_WaitFrame");
        // Explicitly call the base class to avoid infinite recursion through the layer's virtual override
        XrResult result = openXrApi->OpenXrApi::xrWaitFrame(session, frameWaitInfo, frameState);
        QVF_TRACE_STOP(local, "xrWaitFrame_WaitFrame");
        return result;
    }

    XrResult FramePipeline::executeBeginFrame(OpenXrApi* openXrApi,
                                              XrSession session,
                                              const XrFrameBeginInfo* frameBeginInfo) {
        TraceLocalActivity(local);
        QVF_TRACE_START(local, "xrBeginFrame_BeginFrame");
        // Explicitly call the base class to avoid infinite recursion through the layer's virtual override
        XrResult result = openXrApi->OpenXrApi::xrBeginFrame(session, frameBeginInfo);
        QVF_TRACE_STOP(local, "xrBeginFrame_BeginFrame");
        return result;
    }

    XrResult FramePipeline::executeEndFrame(OpenXrApi* openXrApi,
                                            XrSession session,
                                            const XrFrameEndInfo* frameEndInfo) {
        TraceLocalActivity(local);
        QVF_TRACE_START(local, "xrEndFrame_EndFrame");
        // Explicitly call the base class to avoid infinite recursion through the layer's virtual override
        XrResult result = openXrApi->OpenXrApi::xrEndFrame(session, frameEndInfo);
        QVF_TRACE_STOP(local, "xrEndFrame_EndFrame");
        return result;
    }

} // namespace openxr_api_layer
