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
#include "logic/gaze_space_manager.h"
#include "framework/log.h"
#include "framework/util.h"
#include "views.h"

namespace openxr_api_layer {

    using namespace log;
    using namespace xr;
    using namespace xr::math;

    bool GazeSpaceManager::isGazeSpace(XrSpace space) const {
        std::unique_lock lock(m_mutex);
        return m_gazeSpaces.count(space) > 0;
    }

    XrResult GazeSpaceManager::createReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space, OpenXrApi* openXrApi) {
        if (createInfo->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) {
            return XR_ERROR_VALIDATION_FAILURE;
        }

        QVF_TRACE("xrCreateReferenceSpace",
                  TLXArg(session, "Session"),
                  TLArg(xr::ToCString(createInfo->referenceSpaceType), "ReferenceSpaceType"),
                  TLArg(xr::ToString(createInfo->poseInReferenceSpace).c_str(), "PoseInReferenceSpace"));

        XrReferenceSpaceCreateInfo chainCreateInfo = *createInfo;

        const bool isVarjoCombinedEyeSpace = createInfo->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO;
        if (isVarjoCombinedEyeSpace) {
            // Create a dummy space, we will keep track of those handles below.
            chainCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        }

        const XrResult result = openXrApi->OpenXrApi::xrCreateReferenceSpace(session, &chainCreateInfo, space);

        if (XR_SUCCEEDED(result)) {
            QVF_TRACE("xrCreateReferenceSpace", TLXArg(*space, "Space"));

            if (isVarjoCombinedEyeSpace) {
                std::unique_lock lock(m_mutex);
                m_gazeSpaces.insert(*space);
            }
        }

        return result;
    }

    XrResult GazeSpaceManager::destroySpace(XrSpace space, OpenXrApi* openXrApi) {
        QVF_TRACE("xrDestroySpace", TLXArg(space, "Space"));

        const XrResult result = openXrApi->OpenXrApi::xrDestroySpace(space);

        if (XR_SUCCEEDED(result)) {
            std::unique_lock lock(m_mutex);
            m_gazeSpaces.erase(space);
        }

        return result;
    }

    XrResult GazeSpaceManager::locateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location, EyeTracker& eyeTracker, XrSession session) {
        QVF_TRACE("xrLocateSpace",
                  TLXArg(space, "Space"),
                  TLXArg(baseSpace, "BaseSpace"),
                  TLArg(time, "Time"));

        std::unique_lock lock(m_mutex);

        XrResult result = XR_ERROR_RUNTIME_FAILURE;
        if (m_gazeSpaces.count(space)) {
            if (location->type != XR_TYPE_SPACE_LOCATION) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            if (time <= 0) {
                return XR_ERROR_TIME_INVALID;
            }

            XrVector3f dummyVector{};
            if (eyeTracker.getEyeGaze(session, time, true, dummyVector)) {
                location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
            } else {
                location->locationFlags = 0;
            }
            location->pose = xr::math::Pose::Identity();

            result = XR_SUCCESS;
        } else {
            result = XR_ERROR_HANDLE_INVALID; // Will be handled by caller to call OpenXrApi
        }

        if (XR_SUCCEEDED(result)) {
            QVF_TRACE("xrLocateSpace",
                      TLArg(location->locationFlags, "LocationFlags"),
                      TLArg(xr::ToString(location->pose).c_str(), "Pose"));
        }

        return result;
    }

    void GazeSpaceManager::clear() {
        std::unique_lock lock(m_mutex);
        m_gazeSpaces.clear();
    }

} // namespace openxr_api_layer