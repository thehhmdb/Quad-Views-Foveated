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
#include "logic/compositor_shared.h"

namespace openxr_api_layer {

/// Writes typed constants into a persistently-mapped upload heap.
/// Each eye occupies a 256-byte aligned slot within the buffer.
class ConstantBufferWriter {
public:
    ConstantBufferWriter() = default;

    void initialize(uint8_t* mappedPtr) {
        m_mappedPtr = mappedPtr;
    }

    /// Write VS constants for a given eye (256-byte aligned slot).
    void writeVS(uint32_t viewIndex, const ProjectionVSConstants& constants) {
        static_assert(sizeof(ProjectionVSConstants) <= 256);
        memcpy(m_mappedPtr + viewIndex * 256, &constants, sizeof(constants));
    }

    /// Write PS constants for a given eye (256-byte aligned slot).
    void writePS(uint32_t viewIndex, const ProjectionPSConstants& constants) {
        static_assert(sizeof(ProjectionPSConstants) <= 256);
        memcpy(m_mappedPtr + viewIndex * 256, &constants, sizeof(constants));
    }

private:
    uint8_t* m_mappedPtr{nullptr};
};

} // namespace openxr_api_layer
