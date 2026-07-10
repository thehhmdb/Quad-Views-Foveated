// MIT License
//
// Copyright(c) 2023 Matthieu Bucchianeri
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

// A Vertex Shader that draws a full-screen quad and projects the coordinates of two layers.

cbuffer ConstantBuffer : register(b0) {
    float4x4 focusProjection;
    // Sub-rectangle info for direct swapchain sampling (eliminates flatten copy)
    // xy = offset, zw = extent (in pixels)
    float4 stereoSubRect;
    float4 focusSubRect;
    // Swapchain dimensions for UV normalization
    float2 stereoSwapchainSize;
    float2 focusSwapchainSize;
};

void main(in uint id : SV_VertexID, out float4 position : SV_POSITION, out float2 texcoord : PROJ_COORD0, out float3 projectedFocusCoord : PROJ_COORD1, out float2 stereoTexCoord : PROJ_COORD2, out float2 focusTexCoord : PROJ_COORD3) {
    texcoord = float2((id == 1) ? 2.0 : 0.0, (id == 2) ? 2.0 : 0.0);
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    projectedFocusCoord = mul(position, focusProjection).xyw;
    
    // Compute texture coordinates for direct swapchain sampling
    // stereoTexCoord: UV in [0,1] for the stereo sub-rectangle
    // focusTexCoord: UV in [0,1] for the focus sub-rectangle
    stereoTexCoord = (texcoord * stereoSwapchainSize - stereoSubRect.xy) / stereoSubRect.zw;
    focusTexCoord = (texcoord * focusSwapchainSize - focusSubRect.xy) / focusSubRect.zw;
}
