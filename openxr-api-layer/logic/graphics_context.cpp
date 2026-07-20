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
#include "logic/graphics_context.h"
#include "framework/log.h"
#include "framework/util.h"
#include "compositor.h"
#include "d3d11_compositor.h"
#include "d3d12_compositor.h"
#include <utils/graphics.h>

namespace openxr_api_layer {

    // Convenience alias for trace provider access within this module
    using log::g_traceProvider;

    void GraphicsContext::initializeD3D11(ID3D11Device* device, OpenXrApi* openXrApi) {
        UINT creationFlags = 0;
        if (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) {
            creationFlags |= D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED;
        }
        const D3D_FEATURE_LEVEL featureLevel = device->GetFeatureLevel();

        CHECK_HRCMD(device->QueryInterface(m_applicationDevice.ReleaseAndGetAddressOf()));

        // Create a switchable context state for the API layer.
        CHECK_HRCMD(m_applicationDevice->CreateDeviceContextState(creationFlags,
                                                                  &featureLevel,
                                                                  1,
                                                                  D3D11_SDK_VERSION,
                                                                  __uuidof(ID3D11Device),
                                                                  nullptr,
                                                                  m_layerContextState.ReleaseAndGetAddressOf()));

        ComPtr<ID3D11DeviceContext> context;
        m_applicationDevice->GetImmediateContext(context.ReleaseAndGetAddressOf());
        CHECK_HRCMD(context->QueryInterface(m_renderContext.ReleaseAndGetAddressOf()));

        // Create D3D11 compositor
        m_compositor = createD3D11Compositor(m_applicationDevice.Get(), openXrApi);

        // For statistics,
        {
            XrGraphicsBindingD3D11KHR bindings{};
            bindings.device = device;
            std::shared_ptr<utils::graphics::IGraphicsDevice> graphicsDevice =
                utils::graphics::internal::wrapApplicationDevice(bindings);
            for (uint32_t i = 0; i < std::size(m_appFrameGpuTimer); i++) {
                m_appFrameGpuTimer[i] = graphicsDevice->createTimer();
            }
            m_appFrameCpuTimer = utils::general::createTimer();
            m_appRenderCpuTimer = utils::general::createTimer();
        }
    }

    void GraphicsContext::initializeD3D12(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi) {
        QVF_TRACE("initializeDeviceContext",
                  TLArg("D3D12", "Api"),
                  TLPArg(device, "Device"),
                  TLPArg(queue, "Queue"));

        m_isD3D12 = true;

        m_applicationDeviceD3D12 = device;
        m_renderQueue = queue;

        // Create D3D12 compositor
        m_compositor = createD3D12Compositor(device, queue, openXrApi);

        // For statistics,
        {
            XrGraphicsBindingD3D12KHR bindings{};
            bindings.device = device;
            bindings.queue = queue;
            std::shared_ptr<utils::graphics::IGraphicsDevice> graphicsDevice =
                utils::graphics::internal::wrapApplicationDevice(bindings);
            for (uint32_t i = 0; i < std::size(m_appFrameGpuTimer); i++) {
                m_appFrameGpuTimer[i] = graphicsDevice->createTimer();
            }
            m_appFrameCpuTimer = utils::general::createTimer();
            m_appRenderCpuTimer = utils::general::createTimer();
        }
    }

    void GraphicsContext::swapDeviceContextState(ComPtr<ID3DDeviceContextState>& outApplicationContextState) {
        if (m_isD3D12) {
            // D3D12 uses command lists, no context swap needed
            return;
        }
        m_renderContext->SwapDeviceContextState(m_layerContextState.Get(),
                                                outApplicationContextState.ReleaseAndGetAddressOf());
        m_renderContext->ClearState();
    }

    void GraphicsContext::restoreDeviceContextState(ComPtr<ID3DDeviceContextState> applicationContextState) {
        if (m_isD3D12) {
            // D3D12 uses command lists, no context swap needed
            return;
        }
        m_renderContext->SwapDeviceContextState(applicationContextState.Get(), nullptr);
    }

    void GraphicsContext::destroy() {
        if (m_compositor) {
            m_compositor->destroy();
            m_compositor.reset();
        }
        for (uint32_t i = 0; i < std::size(m_appFrameGpuTimer); i++) {
            m_appFrameGpuTimer[i].reset();
        }
        m_appFrameCpuTimer.reset();
        m_appRenderCpuTimer.reset();
        m_layerContextState.Reset();
        m_applicationDevice.Reset();
        m_renderContext.Reset();
    }

} // namespace openxr_api_layer
