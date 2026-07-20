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

#ifdef XR_USE_GRAPHICS_API_D3D12

#include "log.h"
#include "graphics.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

namespace {

    using namespace openxr_api_layer::log;
    using namespace openxr_api_layer::utils::graphics;

    struct D3D12Timer : IGraphicsTimer {
        D3D12Timer(ID3D12Device* device, ID3D12CommandQueue* queue) : m_queue(queue) {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D12Timer_Create");

            // Create the command context.
            for (uint32_t i = 0; i < 2; i++) {
                CHECK_HRCMD(device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_commandAllocator[i].ReleaseAndGetAddressOf())));
                m_commandAllocator[i]->SetName(L"Timer Command Allocator");
                CHECK_HRCMD(device->CreateCommandList(0,
                                                      D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      m_commandAllocator[i].Get(),
                                                      nullptr,
                                                      IID_PPV_ARGS(m_commandList[i].ReleaseAndGetAddressOf())));
                m_commandList[i]->SetName(L"Timer Command List");
                CHECK_HRCMD(m_commandList[i]->Close());
            }
            CHECK_HRCMD(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.ReleaseAndGetAddressOf())));
            m_fence->SetName(L"Timer Readback Fence");

            // Create the query heap and readback resources.
            D3D12_QUERY_HEAP_DESC heapDesc{};
            heapDesc.Count = 2;
            heapDesc.NodeMask = 0;
            heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            CHECK_HRCMD(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(m_queryHeap.ReleaseAndGetAddressOf())));
            m_queryHeap->SetName(L"Timestamp Query Heap");

            D3D12_HEAP_PROPERTIES heapType{};
            heapType.Type = D3D12_HEAP_TYPE_READBACK;
            heapType.CreationNodeMask = heapType.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC readbackDesc{};
            readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            readbackDesc.Width = heapDesc.Count * sizeof(uint64_t);
            readbackDesc.Height = readbackDesc.DepthOrArraySize = readbackDesc.MipLevels =
                readbackDesc.SampleDesc.Count = 1;
            readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            CHECK_HRCMD(device->CreateCommittedResource(&heapType,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &readbackDesc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr,
                                                        IID_PPV_ARGS(m_queryReadbackBuffer.ReleaseAndGetAddressOf())));
            m_queryReadbackBuffer->SetName(L"Query Readback Buffer");

            QVF_TRACE_STOP(local, "D3D12Timer_Create", TLPArg(this, "Timer"));
        }

        ~D3D12Timer() override {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D12Timer_Destroy", TLPArg(this, "Timer"));
            QVF_TRACE_STOP(local, "D3D12Timer_Destroy");
        }

        Api getApi() const override {
            return Api::D3D12;
        }

        void start() override {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D12Timer_Start", TLPArg(this, "Timer"));

            CHECK_HRCMD(m_commandAllocator[0]->Reset());
            CHECK_HRCMD(m_commandList[0]->Reset(m_commandAllocator[0].Get(), nullptr));
            m_commandList[0]->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            CHECK_HRCMD(m_commandList[0]->Close());
            ID3D12CommandList* const lists[] = {m_commandList[0].Get()};
            m_queue->ExecuteCommandLists(1, lists);

            QVF_TRACE_STOP(local, "D3D12Timer_Start");
        }

        void stop() override {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D12Timer_Stop", TLPArg(this, "Timer"));

            CHECK_HRCMD(m_commandAllocator[1]->Reset());
            CHECK_HRCMD(m_commandList[1]->Reset(m_commandAllocator[1].Get(), nullptr));
            m_commandList[1]->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            m_commandList[1]->ResolveQueryData(
                m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, m_queryReadbackBuffer.Get(), 0);
            CHECK_HRCMD(m_commandList[1]->Close());
            ID3D12CommandList* const lists[] = {m_commandList[1].Get()};
            m_queue->ExecuteCommandLists(1, lists);

            // Signal a fence for completion.
            m_queue->Signal(m_fence.Get(), ++m_fenceValue);
            m_valid = true;

            QVF_TRACE_STOP(local, "D3D12Timer_Stop");
        }

        uint64_t query() const override {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D12Timer_Query", TLPArg(this, "Timer"), TLArg(m_valid, "Valid"));

            uint64_t duration = 0;
            if (m_valid) {
                uint64_t gpuTickFrequency;
                if (m_fence->GetCompletedValue() >= m_fenceValue &&
                    SUCCEEDED(m_queue->GetTimestampFrequency(&gpuTickFrequency))) {
                    uint64_t* mappedBuffer;
                    D3D12_RANGE range{0, 2 * sizeof(uint64_t)};
                    CHECK_HRCMD(m_queryReadbackBuffer->Map(0, &range, reinterpret_cast<void**>(&mappedBuffer)));
                    duration = ((mappedBuffer[1] - mappedBuffer[0]) * 1000000) / gpuTickFrequency;
                    m_queryReadbackBuffer->Unmap(0, nullptr);
                }
                m_valid = false;
            }

            QVF_TRACE_STOP(local, "D3D12Timer_Query", TLArg(duration, "Duration"));

            return duration;
        }

        ComPtr<ID3D12CommandQueue> m_queue;
        ComPtr<ID3D12CommandAllocator> m_commandAllocator[2];
        ComPtr<ID3D12GraphicsCommandList> m_commandList[2];
        ComPtr<ID3D12Fence> m_fence;
        uint64_t m_fenceValue{0};
        ComPtr<ID3D12QueryHeap> m_queryHeap;
        ComPtr<ID3D12Resource> m_queryReadbackBuffer;

        // Can the timer be queried (it might still only read 0).
        mutable bool m_valid{false};
    };

    struct D3D12GraphicsDevice : IGraphicsDevice {
        D3D12GraphicsDevice(ID3D12Device* device, ID3D12CommandQueue* commandQueue)
            : m_device(device), m_commandQueue(commandQueue) {
            TraceLocalActivity(local);
            QVF_TRACE_START(
                local, "D3D12GraphicsDevice_Create", TLPArg(device, "D3D12Device"), TLPArg(commandQueue, "Queue"));

            {
                const LUID adapterLuid = m_device->GetAdapterLuid();

                ComPtr<IDXGIFactory1> dxgiFactory;
                CHECK_HRCMD(CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.ReleaseAndGetAddressOf())));
                ComPtr<IDXGIAdapter1> dxgiAdapter;
                for (UINT adapterIndex = 0;; adapterIndex++) {
                    // EnumAdapters1 will fail with DXGI_ERROR_NOT_FOUND when there are no more adapters to
                    // enumerate.
                    CHECK_HRCMD(dxgiFactory->EnumAdapters1(adapterIndex, dxgiAdapter.ReleaseAndGetAddressOf()));

                    DXGI_ADAPTER_DESC1 desc;
                    CHECK_HRCMD(dxgiAdapter->GetDesc1(&desc));
                    if (!memcmp(&desc.AdapterLuid, &adapterLuid, sizeof(LUID))) {
                        QVF_TRACE_TAGGED(
                            local,
                            "D3D12GraphicsDevice_Create",
                            TLArg(desc.Description, "Adapter"),
                            TLArg(fmt::format("{}:{}", adapterLuid.HighPart, adapterLuid.LowPart).c_str(), " Luid"));
                        break;
                    }
                }
            }

            QVF_TRACE_STOP(local, "D3D12GraphicsDevice_Create", TLPArg(this, "Device"));
        }

        ~D3D12GraphicsDevice() override {
            TraceLocalActivity(local);
            QVF_TRACE_START(local, "D3D12GraphicsDevice_Destroy", TLPArg(this, "Device"));
            QVF_TRACE_STOP(local, "D3D12GraphicsDevice_Destroy");
        }

        Api getApi() const override {
            return Api::D3D12;
        }

        void* getNativeDevicePtr() const override {
            return m_device.Get();
        }

        void* getNativeContextPtr() const override {
            return m_commandQueue.Get();
        }

        std::shared_ptr<IGraphicsTimer> createTimer() override {
            return std::make_shared<D3D12Timer>(m_device.Get(), m_commandQueue.Get());
        }

        GenericFormat translateToGenericFormat(int64_t format) const override {
            return (DXGI_FORMAT)format;
        }

        int64_t translateFromGenericFormat(GenericFormat format) const override {
            return (int64_t)format;
        }

        LUID getAdapterLuid() const override {
            return m_device->GetAdapterLuid();
        }

        const ComPtr<ID3D12Device> m_device;
        const ComPtr<ID3D12CommandQueue> m_commandQueue;
    };

} // namespace

namespace openxr_api_layer::utils::graphics::internal {

    std::shared_ptr<IGraphicsDevice> wrapApplicationDevice(const XrGraphicsBindingD3D12KHR& bindings) {
        return std::make_shared<D3D12GraphicsDevice>(bindings.device, bindings.queue);
    }

} // namespace openxr_api_layer::utils::graphics::internal

#endif
