// LumaSharpen - adaptive sharpening
#define STRENGTH  0.5
#define CLAMP_VAL 0.05

void main() {
    float2 inv_res = GetInvResolution();
    float4 color = Sample();
    float2 tc = GetCoordinates();

    float3 c = color.rgb;
    float3 blurred = SampleLocation(tc + float2( inv_res.x,  0.0)).rgb +
                     SampleLocation(tc + float2(-inv_res.x,  0.0)).rgb +
                     SampleLocation(tc + float2( 0.0,       inv_res.y)).rgb +
                     SampleLocation(tc + float2( 0.0,      -inv_res.y)).rgb;
    blurred *= 0.25;
    float3 sharp = clamp(c - blurred, -CLAMP_VAL, CLAMP_VAL);
    SetOutput(float4(c + sharp * STRENGTH * 2.0, color.a));
}
