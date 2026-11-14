Texture2D<float4> DlssOutput : register(t0);
RWTexture2D<float3> HistoryColor : register(u0);
cbuffer PackSettings : register(b0) {
    uint SharpeningPercent;
    uint HistoryX;
    uint HistoryY;
    uint HistoryWidth;
    uint HistoryHeight;
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

[numthreads(16, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const int2 historySize = int2(HistoryWidth, HistoryHeight);
    if (any(thread.xy >= uint2(historySize))) return;

    const int2 pixel = int2(thread.xy);
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
    HistoryColor[pixel + int2(HistoryX, HistoryY)] = clamp(color, 0.0, 65000.0);
}
