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

// A Pixel Shader that paints the content of a layer given the specified coordinates.

cbuffer ConstantBuffer : register(b0) {
    float smoothingArea;
    bool ignoreAlpha;
    bool isUnpremultipliedAlpha;
    bool debugFocusView;
    float sharpenFocusView;
};

SamplerState sourceSampler : register(s0);
Texture2D sourceStereoTexture : register(t0);
Texture2D sourceFocusTexture : register(t1);

float4 premultiplyAlpha(float4 color) {
    return float4(color.rgb * color.a, color.a);
}

float4 unpremultiplyAlpha(float4 color) {
    if (color.a != 0) {
        return float4(color.rgb / color.a, color.a);
    } else {
        return 0;
    }
}

float4 main(in float4 position : SV_POSITION, in float2 texcoord : PROJ_COORD0, in float3 projectedFocusCoord : PROJ_COORD1) : SV_TARGET {
    // Convert to texcoord and pick the pixel from each layer.
    float4 color0 = sourceStereoTexture.Sample(sourceSampler, texcoord);

    // Guard against division by zero when the projected focus Z is 0 (degenerate projection),
    // which would produce NaN/Inf and corrupt the composited output.
    float projectedZ = abs(projectedFocusCoord.z) > 1e-6f ? projectedFocusCoord.z : 1e-6f;
    float2 layer1ProjectedCoordNdc = projectedFocusCoord.xy / projectedZ;
    float2 layer1TexCoord = layer1ProjectedCoordNdc * float2(0.5f, -0.5f) + 0.5f;
    // For pixels outside of the focus view, the sampler will give us a fully transparent pixel.
    float4 color1 = sourceFocusTexture.Sample(sourceSampler, layer1TexCoord);

    if (ignoreAlpha) {
        color0.a = color1.a = 1;
    }

    if (!isUnpremultipliedAlpha) {
        color0 = unpremultiplyAlpha(color0);
        color1 = unpremultiplyAlpha(color1);
    }

    // Do a smooth transition with alpha-blending around the edges.
    float isInside = all(abs(layer1ProjectedCoordNdc) < 1);
    float edgeFade = 1.0; // 1.0 in center, 0.0 at edges

    if (smoothingArea) {
        float2 s = smoothstep(float2(0, 0), float2(smoothingArea, smoothingArea), layer1TexCoord) -
                   smoothstep(float2(1, 1) - float2(smoothingArea, smoothingArea), float2(1, 1), layer1TexCoord);
        edgeFade = s.x * s.y;

        // Remove the max(0.5, ...) floor that caused a hard visibility edge.
        color1.a = isInside * edgeFade;

        // Dither the blend alpha in the transition zone to mask the resolution boundary.
        // Interleaved Gradient Noise — no texture lookup, ~3 ALU ops.
        float ign = frac(52.9829189 * frac(dot(position.xy, float2(0.06711056, 0.00583715))));
        // transitionMask is 0 when alpha is 0 or 1, peaks at alpha=0.5 — limits dithering to the boundary.
        float transitionMask = saturate(color1.a * (1.0 - color1.a) * 4.0);
        color1.a = saturate(color1.a + (ign - 0.5) * 0.08 * transitionMask);
    } else {
        color1.a = isInside;
    }

    // Feather the CAS sharpening at focus view edges.
    // When sharpening is active, the focus view's increased local contrast makes the resolution
    // boundary more visible. We counteract this by applying a mild blur in the transition zone,
    // reducing the focus view's effective sharpness to better match the peripheral view.
    if (sharpenFocusView > 0 && smoothingArea > 0) {
        float blurMask = saturate(edgeFade * (1.0 - edgeFade) * 4.0);
        if (blurMask > 0.01) {
            float focusWidth, focusHeight;
            sourceFocusTexture.GetDimensions(focusWidth, focusHeight);
            float2 texel = float2(1.0 / focusWidth, 1.0 / focusHeight);

            float3 blurred = color1.rgb * 0.4;
            blurred += sourceFocusTexture.Sample(sourceSampler, layer1TexCoord + float2(texel.x, 0)).rgb * 0.15;
            blurred += sourceFocusTexture.Sample(sourceSampler, layer1TexCoord - float2(texel.x, 0)).rgb * 0.15;
            blurred += sourceFocusTexture.Sample(sourceSampler, layer1TexCoord + float2(0, texel.y)).rgb * 0.15;
            blurred += sourceFocusTexture.Sample(sourceSampler, layer1TexCoord - float2(0, texel.y)).rgb * 0.15;

            color1.rgb = lerp(color1.rgb, blurred, blurMask * sharpenFocusView);
        }
    }

    color0 = premultiplyAlpha(color0);
    color1 = premultiplyAlpha(color1);

    // Blend the two pixels.
    float4 color;
    if (!debugFocusView) {
        color = color1 + color0 * (1 - color1.a);
    } else {
        color = color1;
    }

    return float4(color.rgb, color0.a);
}
