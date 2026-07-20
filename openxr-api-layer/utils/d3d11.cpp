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
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#ifdef XR_USE_GRAPHICS_API_D3D11

#include "log.h"
#include "graphics.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

namespace {

    using namespace openxr_api_layer::log;
    using namespace openxr_api_layer::utils::graphics;

    constexpr bool PreferNtHandle = false;

    struct D3D11Timer : IGraphicsTimer {
        D3D11Timer(ID3D11Device* device) {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D11Timer_Create");

            device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());

            D3D11_QUERY_DESC queryDesc;
            ZeroMemory(&queryDesc, sizeof(D3D11_QUERY_DESC));
            queryDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            CHECK_HRCMD(device->CreateQuery(&queryDesc, m_timeStampDis.ReleaseAndGetAddressOf()));
            queryDesc.Query = D3D11_QUERY_TIMESTAMP;
            CHECK_HRCMD(device->CreateQuery(&queryDesc, m_timeStampStart.ReleaseAndGetAddressOf()));
            CHECK_HRCMD(device->CreateQuery(&queryDesc, m_timeStampEnd.ReleaseAndGetAddressOf()));

            QVF_TRACE_STOP(local, "D3D11Timer_Create", TLPArg(this, "Timer"));
        }

        ~D3D11Timer() override {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D11Timer_Destroy", TLPArg(this, "Timer"));
            QVF_TRACE_STOP(local, "D3D11Timer_Destroy");
        }

        Api getApi() const override {
            return Api::D3D11;
        }

        void start() override {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D11Timer_Start", TLPArg(this, "Timer"));

            m_context->Begin(m_timeStampDis.Get());
            m_context->End(m_timeStampStart.Get());

            QVF_TRACE_STOP(local, "D3D11Timer_Start");
        }

        void stop() override {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D11Timer_Stop", TLPArg(this, "Timer"));

            m_context->End(m_timeStampEnd.Get());
            m_context->End(m_timeStampDis.Get());
            m_valid = true;

            QVF_TRACE_STOP(local, "D3D11Timer_Stop");
        }

        uint64_t query() const override {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D11Timer_Query", TLPArg(this, "Timer"), TLArg(m_valid, "Valid"));

            uint64_t duration = 0;
            if (m_valid) {
                UINT64 startime = 0, endtime = 0;
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disData = {0};

                if (m_context->GetData(m_timeStampStart.Get(), &startime, sizeof(UINT64), 0) == S_OK &&
                    m_context->GetData(m_timeStampEnd.Get(), &endtime, sizeof(UINT64), 0) == S_OK &&
                    m_context->GetData(
                        m_timeStampDis.Get(), &disData, sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT), 0) == S_OK &&
                    !disData.Disjoint) {
                    duration = static_cast<uint64_t>(((endtime - startime) * 1e6) / disData.Frequency);
                }
                m_valid = false;
            }

            QVF_TRACE_STOP(local, "D3D11Timer_Query", TLArg(duration, "Duration"));

            return duration;
        }

        ComPtr<ID3D11DeviceContext> m_context;
        ComPtr<ID3D11Query> m_timeStampDis;
        ComPtr<ID3D11Query> m_timeStampStart;
        ComPtr<ID3D11Query> m_timeStampEnd;

        // Can the timer be queried (it might still only read 0).
        mutable bool m_valid{false};
    };

    struct D3D11GraphicsDevice : IGraphicsDevice {
        D3D11GraphicsDevice(ID3D11Device* device) : m_device(device) {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D11GraphicsDevice_Create", TLPArg(device, "D3D11Device"));

            {
                ComPtr<IDXGIDevice> dxgiDevice;
                CHECK_HRCMD(m_device->QueryInterface(IID_PPV_ARGS(dxgiDevice.ReleaseAndGetAddressOf())));
                ComPtr<IDXGIAdapter> dxgiAdapter;
                CHECK_HRCMD(dxgiDevice->GetAdapter(dxgiAdapter.ReleaseAndGetAddressOf()));
                DXGI_ADAPTER_DESC desc;
                CHECK_HRCMD(dxgiAdapter->GetDesc(&desc));
                m_adapterLuid = desc.AdapterLuid;

                QVF_TRACE_TAGGED(
                    local,
                    "D3D11GraphicsDevice_Create",
                    TLArg(desc.Description, "Adapter"),
                    TLArg(fmt::format("{}:{}", m_adapterLuid.HighPart, m_adapterLuid.LowPart).c_str(), " Luid"));
            }

            m_device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());

            QVF_TRACE_STOP(local, "D3D11GraphicsDevice_Create", TLPArg(this, "Device"));
        }

        ~D3D11GraphicsDevice() override {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D11GraphicsDevice_Destroy", TLPArg(this, "Device"));
            QVF_TRACE_STOP(local, "D3D11GraphicsDevice_Destroy");
        }

        Api getApi() const override {
            return Api::D3D11;
        }

        void* getNativeDevicePtr() const override {
            return m_device.Get();
        }

        void* getNativeContextPtr() const override {
            return m_context.Get();
        }

        std::shared_ptr<IGraphicsTimer> createTimer() override {
            return std::make_shared<D3D11Timer>(m_device.Get());
        }

        GenericFormat translateToGenericFormat(int64_t format) const override {
            return (DXGI_FORMAT)format;
        }

        int64_t translateFromGenericFormat(GenericFormat format) const override {
            return (int64_t)format;
        }

        LUID getAdapterLuid() const override {
            return m_adapterLuid;
        }

        const ComPtr<ID3D11Device> m_device;
        LUID m_adapterLuid{};

        ComPtr<ID3D11DeviceContext> m_context;
    };

} // namespace

namespace openxr_api_layer::utils::graphics::internal {

    std::shared_ptr<IGraphicsDevice> wrapApplicationDevice(const XrGraphicsBindingD3D11KHR& bindings) {
        return std::make_shared<D3D11GraphicsDevice>(bindings.device);
    }

} // namespace openxr_api_layer::utils::graphics::internal

#endif
