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

namespace openxr_api_layer::utils::d3d12 {

    /// Lightweight barrier batch that auto-filters no-op transitions.
    ///
    /// A transition barrier whose BeforeState equals AfterState is invalid per
    /// the D3D12 spec and causes cmdList->Close() to return E_INVALIDARG on
    /// SteamVR's D3D12 runtime. This class skips such no-op barriers.
    class BarrierBatch {
    public:
        explicit BarrierBatch(ID3D12GraphicsCommandList* cmdList)
            : m_cmdList(cmdList) {}

        /// Add a transition barrier, skipping no-op transitions.
        void Add(ID3D12Resource* resource,
                 D3D12_RESOURCE_STATES beforeState,
                 D3D12_RESOURCE_STATES afterState,
                 UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
            if (beforeState == afterState)
                return; // Skip no-op — avoids E_INVALIDARG on SteamVR D3D12 runtime

            m_barriers[m_count++] = CD3DX12_RESOURCE_BARRIER::Transition(
                resource, beforeState, afterState, subresource);
        }

        /// Flush all accumulated barriers to the command list.
        void Flush() {
            if (m_count > 0) {
                m_cmdList->ResourceBarrier(m_count, m_barriers);
                m_count = 0;
            }
        }

        /// Flush and clear (use at end of a scope).
        ~BarrierBatch() {
            Flush();
        }

    private:
        ID3D12GraphicsCommandList* m_cmdList;
        D3D12_RESOURCE_BARRIER m_barriers[8];
        uint32_t m_count = 0;
    };

} // namespace openxr_api_layer::utils::d3d12
