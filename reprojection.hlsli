// DIRT 5 camera constants from the original temporal dispatch.
cbuffer Batch : register(b0) {
    float4 CurrentNdcCorrection : packoffset(c4);
};
cbuffer View : register(b1) {
    float4 CurrentClipRows[3] : packoffset(c16);
    float4 CameraOrigin : packoffset(c35);
    float4 PreviousClipRows[4] : packoffset(c44);
    float4 DeviceDepthCoefficients : packoffset(c78);
    float4 ViewRays[3] : packoffset(c80);
    float4 WorldOrigin : packoffset(c89);
};

float2 CurrentNdc(float2 uv) {
    return float2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y) + CurrentNdcCorrection.zw;
}

float InverseViewZ(float depth) {
    return mad(depth, DeviceDepthCoefficients.x, DeviceDepthCoefficients.y);
}

float3 ViewOffset(float2 uv, float inverseZ) {
    const float3 ray = ViewRays[0].xyz + uv.x * ViewRays[1].xyz + uv.y * ViewRays[2].xyz;
    return -(ray - CameraOrigin.xyz) / inverseZ;
}

float4 PreviousClip(float2 uv, float inverseZ, float3 displacement) {
    const float3 position = CameraOrigin.xyz + ViewOffset(uv, inverseZ) - displacement;
    return position.x * PreviousClipRows[0] + position.y * PreviousClipRows[1] +
        position.z * PreviousClipRows[2] + PreviousClipRows[3];
}

float3 ProjectCurrentOffset(float3 offset) {
    float3 clip = (offset.x * CurrentClipRows[0] + offset.y * CurrentClipRows[1] +
                   offset.z * CurrentClipRows[2]).xyw;
    clip.xy += CurrentNdcCorrection.zw * clip.z;
    return clip;
}
