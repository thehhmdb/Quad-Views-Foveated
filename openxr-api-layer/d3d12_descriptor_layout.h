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

namespace openxr_api_layer {

/// Compile-time descriptor heap layout for the 16-slot CBV/SRV heap.
/// Each eye gets its own heap instance (per-frame, per-eye), so indices
/// are relative to a single heap's start.
///
/// Layout (16 descriptors):
///   [0]  Left eye VS CBV (pre-created at init)
///   [1]  Left eye PS CBV (pre-created at init)
///   [2]  Right eye VS CBV (pre-created at init)
///   [3]  Right eye PS CBV (pre-created at init)
///   [4]  Stereo texture SRV
///   [5]  Focus texture SRV
///   [6]  Blank texture SRV (shared across eyes)
///   [7]  Reserved
///   [8]  Sharpening input SRV (flat focus)
///   [9]  Sharpening output UAV (sharpened focus)
///   [10-11] Reserved
///   [12] Sharpening CS CBV (static)
///   [13-15] Reserved
struct DescriptorLayout {
    static constexpr uint32_t kHeapSize = 16;

    // Pre-created at initialization (static per heap)
    // VS CBV at slot viewIndex*2, PS CBV at slot viewIndex*2+1
    static constexpr uint32_t VsCbv(uint32_t viewIndex) { return viewIndex * 2; }
    static constexpr uint32_t PsCbv(uint32_t viewIndex) { return viewIndex * 2 + 1; }

    // Per-frame SRV bindings for the projection pass
    static constexpr uint32_t kStereoSrv = 4;
    static constexpr uint32_t kFocusSrv  = 5;
    static constexpr uint32_t kBlankSrv  = 6;

    // Sharpening compute pass bindings
    static constexpr uint32_t kSharpenSrv = 8;
    static constexpr uint32_t kSharpenUav = 9;
    static constexpr uint32_t kSharpenCbv = 12;

    /// Compute the GPU descriptor handle for a given slot.
    static CD3DX12_GPU_DESCRIPTOR_HANDLE GpuHandle(
        ID3D12DescriptorHeap* heap, uint32_t slot, uint32_t incrementSize) {
        CD3DX12_GPU_DESCRIPTOR_HANDLE handle(heap->GetGPUDescriptorHandleForHeapStart());
        handle.Offset(slot * incrementSize);
        return handle;
    }

    /// Compute the CPU descriptor handle for a given slot.
    static CD3DX12_CPU_DESCRIPTOR_HANDLE CpuHandle(
        ID3D12DescriptorHeap* heap, uint32_t slot, uint32_t incrementSize) {
        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(heap->GetCPUDescriptorHandleForHeapStart());
        handle.Offset(slot * incrementSize);
        return handle;
    }
};

// Compile-time validation of descriptor slot assignments
static_assert(DescriptorLayout::VsCbv(0) == 0, "Left VS CBV must be at slot 0");
static_assert(DescriptorLayout::PsCbv(0) == 1, "Left PS CBV must be at slot 1");
static_assert(DescriptorLayout::VsCbv(1) == 2, "Right VS CBV must be at slot 2");
static_assert(DescriptorLayout::PsCbv(1) == 3, "Right PS CBV must be at slot 3");
static_assert(DescriptorLayout::kStereoSrv == 4, "Stereo SRV must be at slot 4");
static_assert(DescriptorLayout::kFocusSrv == 5, "Focus SRV must be at slot 5");
static_assert(DescriptorLayout::kBlankSrv == 6, "Blank SRV must be at slot 6");
static_assert(DescriptorLayout::kSharpenSrv == 8, "Sharpen SRV must be at slot 8");
static_assert(DescriptorLayout::kSharpenUav == 9, "Sharpen UAV must be at slot 9");
static_assert(DescriptorLayout::kSharpenCbv == 12, "Sharpen CBV must be at slot 12");

} // namespace openxr_api_layer
