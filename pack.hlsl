#include "reprojection.hlsli"

Texture2D<float4> DlssOutput : register(t0);
Texture2D<float2> MotionPixels : register(t1);
Texture2D<float> SceneDepth : register(t2);
Texture2D<float4> TaggedMotion : register(t3);
struct InstancePose {
    float4 currentX, currentY, currentZ;
    float4 previousX, previousY, previousZ;
    float4 unused0, unused1;
    float2 unused2;
};
StructuredBuffer<InstancePose> InstancePoses : register(t4);
RWTexture2D<float3> HistoryColor : register(u0);
SamplerState LinearClamp : register(s0);
cbuffer PackSettings : register(b2) {
    uint SharpeningPercent;
    uint HistoryX;
    uint HistoryY;
    uint HistoryWidth;
    uint HistoryHeight;
    uint MotionRange;
    uint RenderWidth;
    uint RenderHeight;
    float BlurScale;
    uint2 RenderOrigin;
};

float3 ReadColor(int2 pixel, int2 historySize, uint2 sourceSize) {
    if (all(uint2(historySize) == sourceSize)) return DlssOutput.Load(int3(pixel, 0)).rgb;

    // Average the source area covered by this history pixel in linear HDR.
    const float2 scale = float2(sourceSize) / float2(historySize);
    const float2 start = pixel * scale;
    const float2 end = min((pixel + 1) * scale, float2(sourceSize));
    const int2 limit = min(int2(ceil(end)), int2(sourceSize));
    float3 color = 0;
    for (int y = int(start.y); y < limit.y; ++y) {
        const float weightY = min(end.y, y + 1.0) - max(start.y, float(y));
        for (int x = int(start.x); x < limit.x; ++x) {
            const float weightX = min(end.x, x + 1.0) - max(start.x, float(x));
            color += (weightX * weightY) * DlssOutput.Load(int3(x, y, 0)).rgb;
        }
    }
    return color / ((end.x - start.x) * (end.y - start.y));
}

// The motion texture holds world displacement before camera reprojection.
static const float SceneryMotionSquared = 0.06 * 0.06;

float SceneryFootprint(float2 uv, float4 weights) {
    const float4 x = TaggedMotion.GatherRed(LinearClamp, uv);
    const float4 y = TaggedMotion.GatherGreen(LinearClamp, uv);
    const float4 z = TaggedMotion.GatherBlue(LinearClamp, uv);
    return dot(float4(!(x * x + y * y + z * z <= SceneryMotionSquared)), weights);
}

float SampleWeight(float2 uv, int2 renderSize, float id, float centerDepth, bool upscaled, bool scenery) {
    const int2 pixel = min(int2(uv * renderSize), renderSize - 1);
    if (upscaled) {
        // Check the render-resolution footprint beneath the reconstructed color.
        uint width, height;
        TaggedMotion.GetDimensions(width, height);
        const float2 position = clamp(uv * renderSize, 0.5, float2(renderSize) - 0.5);
        const float2 motionUV = (position + RenderOrigin) / float2(width, height);
        const float2 fraction = frac(position - 0.5);
        const float4 weights = float4((1.0 - fraction.x) * fraction.y, fraction.x * fraction.y,
                                     fraction.x * (1.0 - fraction.y), (1.0 - fraction.x) * (1.0 - fraction.y));
        [branch]
        if (scenery) {
            if (SceneryFootprint(motionUV, weights) > 0.0001) return 0.0;
        } else {
            const float4 ids = TaggedMotion.GatherAlpha(LinearClamp, motionUV);
            if (dot(float4(ids != id), weights) > 0.0001) return 0.0;
        }
    } else {
        const float4 candidate = TaggedMotion.Load(int3(pixel + int2(RenderOrigin), 0));
        if (scenery ? !(dot(candidate.xyz, candidate.xyz) <= SceneryMotionSquared) : candidate.w != id) return 0.0;
    }

    const float depth = SceneDepth.Load(int3(pixel, 0));
    if (scenery) {
        // Nearby scenery can share a streak through foliage and prop edges.
        // Fade across larger depth changes to keep distant sky and foreground apart.
        return 1.0 - smoothstep(0.04, 0.50, abs(depth - centerDepth) / max(max(depth, centerDepth), 0.0001));
    }
    return saturate(1.0 - abs(depth - centerDepth) / max(0.0001, centerDepth * 0.12));
}

struct WheelSpin {
    float3 radial;
    float3 tangent;
    float angle;
};

WheelSpin FindWheelSpin(float id, float3 position, float3 motion) {
    WheelSpin spin = (WheelSpin)0;
    if (abs(id) < 2.0) return spin;

    uint count, stride;
    InstancePoses.GetDimensions(count, stride);
    const uint halfId = f32tof16(id);
    const uint instance = ((halfId & 0x7fff) - 0x4000) * 2 + (halfId >> 15);
    if (count <= 1 || instance >= count) return spin;

    const InstancePose pose = InstancePoses[instance];
    const float3 currentCenter = float3(pose.currentX.w, pose.currentY.w, pose.currentZ.w);
    const float3 previousCenter = float3(pose.previousX.w, pose.previousY.w, pose.previousZ.w);
    const float3x3 currentBasis = float3x3(pose.currentX.xyz, pose.currentY.xyz, pose.currentZ.xyz);
    const float3x3 previousBasis = float3x3(pose.previousX.xyz, pose.previousY.xyz, pose.previousZ.xyz);
    const float3 axle = mul(currentBasis, float3(1, 0, 0));
    const float3 previousAxle = mul(previousBasis, float3(1, 0, 0));
    const float3 up = mul(currentBasis, float3(0, 1, 0));
    const float3 previousUp = mul(previousBasis, float3(0, 1, 0));
    const float3 forward = mul(currentBasis, float3(0, 0, 1));
    const float3 lengths = float3(dot(axle, axle), dot(up, up), dot(forward, forward));
    if (!all(abs(lengths - 1.0) < 0.05) || dot(axle, previousAxle) < 0.985) return spin;

    const float sine = dot(previousUp, forward);
    const float cosine = dot(previousUp, up);
    const float angle = atan2(sine, cosine);
    if (!(abs(angle) > 0.12 && abs(angle) < 2.8)) return spin;

    // Reject unrelated draws and mismatched base-instance tags. The wheel
    // mesh is centered on the X axis of a rigid transform at this scale.
    const float3 local = mul(position - (currentCenter - WorldOrigin.xyz), currentBasis);
    const float radiusSquared = dot(local.yz, local.yz);
    if (!(abs(local.x) < 0.45 && radiusSquared < 0.64)) return spin;
    const float3 predictedMotion = (currentCenter - previousCenter) +
        mul(currentBasis, local) - mul(previousBasis, local);
    const float3 error = predictedMotion - motion;
    if (!(dot(error, error) < 0.0025)) return spin;

    spin.radial = mul(currentBasis, float3(0, local.y, local.z));
    spin.tangent = mul(currentBasis, float3(0, -local.z, local.y));
    spin.angle = angle;
    return spin;
}

float2 SpinUV(float3 current, float2 currentNdc, WheelSpin spin, float angle) {
    float sine, cosine;
    sincos(angle, sine, cosine);
    const float3 offset = spin.radial * (cosine - 1.0) + spin.tangent * sine;
    const float3 clip = current + ProjectCurrentOffset(offset);
    return (clip.xy / clip.z - currentNdc) * float2(0.5, -0.5);
}
float3 BlurScene(float2 uv, int2 renderSize, float3 centerColor, bool upscaled) {
    const int2 center = min(int2(uv * renderSize), renderSize - 1);
    const float centerDepth = SceneDepth.Load(int3(center, 0));
    const float inverseZ = InverseViewZ(centerDepth);
    if (!(inverseZ < 0.0) || !isfinite(inverseZ)) return centerColor;

    const float4 motion = TaggedMotion.Load(int3(center + int2(RenderOrigin), 0));
    const float2 renderUV = (float2(center) + 0.5) / renderSize;
    const float2 currentNdc = CurrentNdc(renderUV);
    const float3 current = float3(currentNdc, 1.0) / -inverseZ;
    const float id = motion.w;
    const WheelSpin spin = FindWheelSpin(id, CameraOrigin.xyz + ViewOffset(renderUV, inverseZ), motion.xyz);
    const bool wheel = spin.angle != 0.0;
    const bool scenery = !wheel && dot(motion.xyz, motion.xyz) <= SceneryMotionSquared;
    const float exposureAngle = BlurScale * spin.angle;
    float2 direction;
    [branch]
    if (wheel) {
        direction = SpinUV(current, currentNdc, spin, exposureAngle);
    } else {
        const float3 previous = PreviousClip(renderUV, inverseZ, motion.xyz).xyw;
        // Apply the shutter to homogeneous clip positions before dividing by W.
        const float3 exposed = lerp(current, previous, BlurScale);
        if (!(exposed.z > 0.0) || !all(isfinite(exposed))) return centerColor;
        const float2 exposedNdc = exposed.xy / exposed.z;
        const float2 edgeScale = abs(1.1 - abs(currentNdc)) /
            max(abs(abs(exposedNdc) - abs(currentNdc)), 1e-6);
        const float edge = saturate(min(edgeScale.x, edgeScale.y));
        const float3 endpoint = lerp(current, exposed, edge);
        direction = (endpoint.xy / endpoint.z - currentNdc) * float2(0.5, -0.5);
    }
    const float2 referenceSize = float2(1080.0 * renderSize.x / renderSize.y, 1080.0);
    const float span = length(direction * referenceSize);
    if (!isfinite(span) || span < 1.0) return centerColor;

    if (upscaled && SampleWeight(uv, renderSize, id, centerDepth, true, scenery) == 0.0) return centerColor;
    // Keep taps close enough to avoid skipping over spokes or thin scenery.
    const float rolloff = !scenery && abs(id) >= 2.0 ? 5.0 : 14.0;
    const float extra = max(0.0, span - rolloff);
    const float limitedSpan = min(span, rolloff) + rolloff * extra / (rolloff + extra);
    const float spanScale = limitedSpan / span;
    direction *= spanScale;
    const float jitter = frac(dot(floor(uv * float2(HistoryWidth, HistoryHeight)), float2(0.7548777, 0.5698403)));
    const int taps = limitedSpan > 14.0 ? 9 : 5;
    const float stepSize = 1.0 / (taps - 1);
    float3 difference = 0.0;
    [loop]
    for (int tap = 0; tap < taps; ++tap) {
        const float offset = float(tap) * stepSize - (0.75 + jitter) * 0.25;
        float2 tapUV = uv + direction * offset;
        [branch]
        if (wheel) {
            tapUV = uv + SpinUV(current, currentNdc, spin, exposureAngle * spanScale * offset);
            if (!all(isfinite(tapUV))) continue;
        }
        if (any(tapUV < 0.0) || any(tapUV >= 1.0)) continue;
        const float weight = SampleWeight(tapUV, renderSize, id, centerDepth, upscaled, scenery);
        if (weight > 0.0) {
            const float3 sampleColor = upscaled ? DlssOutput.SampleLevel(LinearClamp, tapUV, 0.0).rgb :
                DlssOutput.Load(int3(min(int2(tapUV * renderSize), renderSize - 1), 0)).rgb;
            difference += (sampleColor - centerColor) * weight;
        }
    }
    // Rejected taps retain the center color and cannot amplify the surviving taps.
    return centerColor + difference / taps;
}

[numthreads(16, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const int2 historySize = int2(HistoryWidth, HistoryHeight);
    if (any(thread.xy >= uint2(historySize))) return;

    const int2 pixel = int2(thread.xy);
    const int2 target = pixel + int2(HistoryX, HistoryY);
    [branch]
    if (MotionRange != 0) {
        const uint2 renderSize = uint2(RenderWidth, RenderHeight);
        // Sample the active render rectangle. NGX can allocate a larger input texture.
        const uint2 source = min(uint2((float2(pixel) + 0.5) * (float2(renderSize) / historySize)),
                                 renderSize - 1);
        const float2 motion = MotionPixels.Load(int3(source, 0)) / MotionRange;
        HistoryColor[target] = float3(0.18 + 0.16 * clamp(motion, -1.0, 1.0),
                                      0.18 + 0.32 * saturate(length(motion)));
        return;
    }
    uint width, height;
    DlssOutput.GetDimensions(width, height);
    const uint2 sourceSize = uint2(width, height);
    float3 color = ReadColor(pixel, historySize, sourceSize);
    [branch]
    if (SharpeningPercent != 0) {
        const int2 left = int2(max(pixel.x - 1, 0), pixel.y);
        const int2 right = int2(min(pixel.x + 1, historySize.x - 1), pixel.y);
        const int2 up = int2(pixel.x, max(pixel.y - 1, 0));
        const int2 down = int2(pixel.x, min(pixel.y + 1, historySize.y - 1));
        const float3 l = ReadColor(left, historySize, sourceSize);
        const float3 r = ReadColor(right, historySize, sourceSize);
        const float3 u = ReadColor(up, historySize, sourceSize);
        const float3 d = ReadColor(down, historySize, sourceSize);
        const float3 low = min(color, min(min(l, r), min(u, d)));
        const float3 high = max(color, max(max(l, r), max(u, d)));
        const float3 detail = color - (l + r + u + d) * 0.25;
        // Bound the sharpened value to the local HDR color range to limit halos.
        color = clamp(color + (SharpeningPercent * 0.01) * detail, low, high);
    }
    [branch]
    if (BlurScale > 0.0) {
        const uint2 renderSize = uint2(RenderWidth, RenderHeight);
        const bool upscaled = any(sourceSize > renderSize);
        color = BlurScene((float2(pixel) + 0.5) / float2(historySize), int2(renderSize), color, upscaled);
    }
    HistoryColor[target] = clamp(color, 0.0, 65000.0);
}
