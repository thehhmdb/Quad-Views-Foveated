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

#include "general.h"

namespace openxr_api_layer::utils::graphics {

    enum class Api {
#ifdef XR_USE_GRAPHICS_API_D3D11
        D3D11,
#endif
#ifdef XR_USE_GRAPHICS_API_D3D12
        D3D12,
#endif
    };

    // We (arbitrarily) use DXGI as a common conversion point for all graphics APIs.
    using GenericFormat = DXGI_FORMAT;

    // A timer on the GPU.
    struct IGraphicsTimer : openxr_api_layer::utils::general::ITimer {
        virtual ~IGraphicsTimer() = default;
        virtual Api getApi() const = 0;
    };

    // A graphics device and execution context.
    struct IGraphicsDevice {
        virtual ~IGraphicsDevice() = default;

        virtual Api getApi() const = 0;
        virtual void* getNativeDevicePtr() const = 0;
        virtual void* getNativeContextPtr() const = 0;

        virtual std::shared_ptr<IGraphicsTimer> createTimer() = 0;

        virtual GenericFormat translateToGenericFormat(int64_t format) const = 0;
        virtual int64_t translateFromGenericFormat(GenericFormat format) const = 0;

        virtual LUID getAdapterLuid() const = 0;
    };

    namespace internal {

#ifdef XR_USE_GRAPHICS_API_D3D11
        std::shared_ptr<IGraphicsDevice> wrapApplicationDevice(const XrGraphicsBindingD3D11KHR& bindings);
#endif

#ifdef XR_USE_GRAPHICS_API_D3D12
        std::shared_ptr<IGraphicsDevice> wrapApplicationDevice(const XrGraphicsBindingD3D12KHR& bindings);
#endif

    } // namespace internal

} // namespace openxr_api_layer::utils::graphics
