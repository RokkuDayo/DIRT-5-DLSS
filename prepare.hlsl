// DIRT 5 camera and motion input conversion for DLAA and DLSS.
// CBV addresses point at the original game allocations for the same dispatch.
#include "reprojection.hlsli"

cbuffer InputSettings : register(b2) {
    uint4 RenderRect;
};
Texture2D<float> SceneDepth : register(t0);
Texture2D<float4> MotionDisplacement : register(t1);
Texture2D<float3> SceneColor : register(t2);
RWTexture2D<float2> MotionPixels : register(u0);
RWTexture2D<float> DlssDepth : register(u1);
RWTexture2D<float4> DlssColor : register(u2);

[numthreads(16, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const uint2 extent = RenderRect.zw;
    if (any(thread.xy >= extent)) return;

    const uint2 pixel = thread.xy + RenderRect.xy;
    const float2 uv = (float2(thread.xy) + 0.5) / float2(extent);
    const float depth = SceneDepth.Load(int3(pixel, 0));
    const float3 displacement = MotionDisplacement.Load(int3(pixel, 0)).xyz;
    const float4 previousClip = PreviousClip(uv, InverseViewZ(depth), displacement);

    // Use unjittered motion in current-to-previous pixel units. Jitter is supplied separately to NGX.
    const float2 delta = (previousClip.xy / previousClip.w - CurrentNdc(uv)) * float2(0.5, -0.5) * float2(extent);
    const bool valid = previousClip.w > 0.0 && all(isfinite(delta)) && all(abs(delta) < 65504.0);
    MotionPixels[thread.xy] = valid ? delta : float2(0.0, 0.0);
    DlssDepth[thread.xy] = depth;
    DlssColor[thread.xy] = float4(SceneColor.Load(int3(pixel, 0)), 1.0);
}

