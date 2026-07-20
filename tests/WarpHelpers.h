// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
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

#pragma once

#include "pch.h"
#include <d3d11.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <dxgi1_4.h>

namespace openxr_api_layer {

    // Creates a D3D11 WARP device and immediate context.
    inline ComPtr<ID3D11Device> CreateD3D11WarpDevice(ComPtr<ID3D11DeviceContext>& context) {
        ComPtr<ID3D11Device> device;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        CHECK_HRCMD(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, 0, flags,
            &featureLevel, 1, D3D11_SDK_VERSION,
            device.ReleaseAndGetAddressOf(), nullptr, context.ReleaseAndGetAddressOf()));
        return device;
    }

    // Creates a D3D12 WARP device and a command queue.
    inline ComPtr<ID3D12Device> CreateD3D12WarpDevice(ComPtr<ID3D12CommandQueue>& queue) {
#ifdef _DEBUG
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
        }
#endif
        // Enumerate adapters to find WARP
        ComPtr<IDXGIFactory1> dxgiFactory;
        CHECK_HRCMD(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));

        ComPtr<IDXGIAdapter1> warpAdapter;
        for (uint32_t i = 0; ; i++) {
            ComPtr<IDXGIAdapter1> adapter;
            if (dxgiFactory->EnumAdapters1(i, &adapter) != S_OK) break;

            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                warpAdapter = std::move(adapter);
                break;
            }
        }
        if (!warpAdapter) {
            throw std::runtime_error("WARP adapter not found");
        }

        ComPtr<ID3D12Device> device;
        CHECK_HRCMD(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.ReleaseAndGetAddressOf())));

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        CHECK_HRCMD(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.ReleaseAndGetAddressOf())));

        return device;
    }

} // namespace openxr_api_layer
