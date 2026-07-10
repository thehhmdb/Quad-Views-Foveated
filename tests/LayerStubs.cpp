// MIT License
//
// Stub implementations for layer-level symbols needed by unit tests.
// These provide minimal definitions so the logic classes can link without
// pulling in the full layer.cpp and entry.cpp dependencies.

#include "pch.h"
#include "layer.h"
#include "framework/log.h"

namespace openxr_api_layer {
    // Defined in entry.cpp in the real layer
    std::filesystem::path dllHome;
    std::filesystem::path localAppData;
    
    // Defined in layer.cpp in the real layer
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {};
    
    // Stub for GetInstance() — tests should use MockOpenXrApi directly
    static std::unique_ptr<OpenXrApi> g_testInstance;
    OpenXrApi* GetInstance() {
        return g_testInstance.get();
    }
}

namespace openxr_api_layer::log {
    // Defined in entry.cpp in the real layer
    std::ofstream logStream;
}
