cbuffer DrawConstants : register(b0) {
    uint PackedInstance;
};

float4 main(float4 position : SV_Position, float3 displacement : TEXCOORD16) : SV_Target0 {
    const uint instance = PackedInstance & 0xfffff;
    // Encode the draw's base instance as an exact, finite half. Zero is the
    // background clear and one is the stock shader's unlabelled geometry.
    const uint sign = (instance & 1) << 15;
    const float id = position.w > 0.0 && instance < 30720 ? f16tof32(0x4000 + (instance >> 1) + sign) : 1.0;
    return float4(displacement, id);
}
