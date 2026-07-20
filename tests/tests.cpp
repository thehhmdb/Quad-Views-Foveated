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
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "framework/log.h" // For g_traceProvider

// Definition of the global unloading flag for the test build (the layer's
// layer.cpp is not linked into the test executable). Mirrors the single
// definition in the layer. Declared extern in tests/pch.h.
namespace openxr_api_layer {
    bool g_isUnloading = false;
} // namespace openxr_api_layer

class GlobalTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // FIX: Register the trace provider so TraceLoggingWrite doesn't crash
        TraceLoggingRegister(openxr_api_layer::log::g_traceProvider);
    }

    void TearDown() override {
        TraceLoggingUnregister(openxr_api_layer::log::g_traceProvider);
    }
};

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::InitGoogleMock(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new GlobalTestEnvironment());
    return RUN_ALL_TESTS();
}