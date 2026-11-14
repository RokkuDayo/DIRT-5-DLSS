Texture2D<float4> DlssOutput : register(t0);
Texture2D<float2> MotionPixels : register(t1);
Texture2D<float> SceneDepth : register(t2);
RWTexture2D<float3> HistoryColor : register(u0);
SamplerState LinearClamp : register(s0);
cbuffer PackSettings : register(b0) {
    uint SharpeningPercent;
    uint HistoryX;
    uint HistoryY;
    uint HistoryWidth;
    uint HistoryHeight;
    uint MotionRange;
    uint RenderWidth;
    uint RenderHeight;
    float BlurScale;
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

float RotationBlurScale(int2 center, int2 renderSize, float centerDepth, float depthTolerance) {
    // Opposing cross-axis gradients indicate local rotation. A uniform pan,
    // radial expansion or one-axis shear does not satisfy this test.
    const int radius = 2;
    if (any(center < radius) || any(center + radius >= renderSize)) return 1.0;
    const int2 left = center - int2(radius, 0);
    const int2 right = center + int2(radius, 0);
    const int2 up = center - int2(0, radius);
    const int2 down = center + int2(0, radius);
    const float4 depths = float4(SceneDepth.Load(int3(left, 0)), SceneDepth.Load(int3(right, 0)),
                                 SceneDepth.Load(int3(up, 0)), SceneDepth.Load(int3(down, 0)));
    if (any(abs(depths - centerDepth) > depthTolerance)) return 1.0;

    const float verticalChange = MotionPixels.Load(int3(right, 0)).y - MotionPixels.Load(int3(left, 0)).y;
    const float horizontalChange = MotionPixels.Load(int3(down, 0)).x - MotionPixels.Load(int3(up, 0)).x;
    if (verticalChange * horizontalChange >= 0.0) return 1.0;
    const float rotation = min(abs(verticalChange), abs(horizontalChange)) * BlurScale / (2 * radius);
    return lerp(1.0, 0.35, smoothstep(0.025, 0.12, rotation));
}

float3 BlurScene(float2 uv, uint2 renderSize, float3 centerColor) {
    const uint2 center = min(uint2(uv * renderSize), renderSize - 1);
    const float2 motion = MotionPixels.Load(int3(center, 0));
    float2 direction = motion * BlurScale / float2(renderSize);
    const float span = length(direction);
    if (span < 0.001) return centerColor;
    direction *= min(1.0, 0.12 / span);
    const float centerDepth = SceneDepth.Load(int3(center, 0));
    const float depthTolerance = max(centerDepth * 0.08, 0.0001);
    const float rotationScale = RotationBlurScale(int2(center), int2(renderSize), centerDepth, depthTolerance);
    direction *= rotationScale;

    // Sample mostly toward the previous position, with a short tail on the
    // other side. Dither in screen space so the pattern is stable over time.
    const float jitter = frac(dot(floor(uv * float2(HistoryWidth, HistoryHeight)), float2(0.7548777, 0.5698403)));
    const float motionTolerance = max(length(motion) * 0.35, 2.0);
    float totalWeight = 0.03125;
    float3 color = centerColor * totalWeight;
    [loop]
    for (int tap = 0; tap < 12; ++tap) {
        const float2 tapUV = uv + direction * ((float(tap) - 3.0 + jitter) / 11.0);
        if (any(tapUV < 0.0) || any(tapUV >= 1.0)) continue;
        const uint2 samplePixel = min(uint2(tapUV * renderSize), renderSize - 1);
        const float tapDepth = SceneDepth.Load(int3(samplePixel, 0));
        const float2 tapMotion = MotionPixels.Load(int3(samplePixel, 0));
        const float depthWeight = saturate(1.0 - abs(tapDepth - centerDepth) / depthTolerance);
        const float motionWeight = saturate(1.0 - length(tapMotion - motion) / motionTolerance);
        const float weight = depthWeight * motionWeight;
        if (weight > 0.0) {
            color += DlssOutput.SampleLevel(LinearClamp, tapUV, 0.0).rgb * weight;
            totalWeight += weight;
        }
    }
    // Retain more of the resolved scene on strongly rotating surfaces, where
    // a straight sampling line is a poor approximation of the exposure path.
    return lerp(centerColor, color / totalWeight, rotationScale);
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
        color = BlurScene((float2(pixel) + 0.5) / float2(historySize), uint2(RenderWidth, RenderHeight), color);
    }
    HistoryColor[target] = clamp(color, 0.0, 65000.0);
}
