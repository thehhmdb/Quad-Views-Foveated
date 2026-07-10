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
#include <memory>

namespace openxr_api_layer {

    class ICompositor;
    namespace utils::general { struct ITimer; }
    namespace utils::graphics { struct IGraphicsTimer; }

    // Manages graphics device context, compositor, and frame timers.
    // Extracted from OpenXrLayer to reduce monolithic class size.
    class GraphicsContext {
      public:
        GraphicsContext() = default;
        ~GraphicsContext() = default;

        // Non-copyable
        GraphicsContext(const GraphicsContext&) = delete;
        GraphicsContext& operator=(const GraphicsContext&) = delete;

        // Initialize for D3D11
        void initializeD3D11(ID3D11Device* device, OpenXrApi* openXrApi);

        // Initialize for D3D12
        void initializeD3D12(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi);

        // Get the compositor instance
        ICompositor* getCompositor() const { return m_compositor.get(); }

        // Check if using D3D12
        bool isD3D12() const { return m_isD3D12; }

        // D3D11 context swapping for composition
        // Returns the application context state for later restoration
        void swapDeviceContextState(ComPtr<ID3DDeviceContextState>& outApplicationContextState);
        void restoreDeviceContextState(ComPtr<ID3DDeviceContextState> applicationContextState);

        // Timer accessors
        std::shared_ptr<utils::general::ITimer> getAppFrameCpuTimer() const { return m_appFrameCpuTimer; }
        std::shared_ptr<utils::general::ITimer> getAppRenderCpuTimer() const { return m_appRenderCpuTimer; }
        std::shared_ptr<utils::graphics::IGraphicsTimer> getAppFrameGpuTimer(uint32_t index) const {
            return m_appFrameGpuTimer[index];
        }
        uint32_t& getAppFrameGpuTimerIndex() { return m_appFrameGpuTimerIndex; }
        static constexpr uint32_t kGpuTimerCount = 3;

        // Cleanup all resources
        void destroy();

        // D3D11 device access (for compositor initialization)
        ID3D11Device5* getD3D11Device() const { return m_applicationDevice.Get(); }
        ID3D11DeviceContext4* getRenderContext() const { return m_renderContext.Get(); }

      private:
        ComPtr<ID3D11Device5> m_applicationDevice;
        ComPtr<ID3D11DeviceContext4> m_renderContext;
        ComPtr<ID3DDeviceContextState> m_layerContextState;
        ComPtr<ID3D12Device> m_applicationDeviceD3D12;
        ComPtr<ID3D12CommandQueue> m_renderQueue;

        std::unique_ptr<ICompositor> m_compositor;
        bool m_isD3D12{false};

        // Timers
        std::shared_ptr<utils::general::ITimer> m_appFrameCpuTimer;
        std::shared_ptr<utils::general::ITimer> m_appRenderCpuTimer;
        std::shared_ptr<utils::graphics::IGraphicsTimer> m_appFrameGpuTimer[3];
        uint32_t m_appFrameGpuTimerIndex{0};
    };

} // namespace openxr_api_layer
