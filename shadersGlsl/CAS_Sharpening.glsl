// Contrast Adaptive Sharpening
#define SHARPEN_STRENGTH  0.4

void main() {
    float2 inv_res = GetInvResolution();
    float4 color = Sample();
    float2 tc = GetCoordinates();

    float3 b = SampleLocation(tc + float2( 0.0,       inv_res.y)).rgb;
    float3 d = SampleLocation(tc + float2(-inv_res.x,  0.0)).rgb;
    float3 e = color.rgb;
    float3 f = SampleLocation(tc + float2( inv_res.x,  0.0)).rgb;
    float3 h = SampleLocation(tc + float2( 0.0,      -inv_res.y)).rgb;

    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mx = max(max(max(d, e), max(f, b)), h);

    float3 blur = (b + d + f + h) * 0.25;
    float3 signal = e - blur;
    float3 amp = sqrt(min(abs(mn), abs(mx)));
    float3 w = amp * SHARPEN_STRENGTH;
    signal = clamp(abs(signal) - w, 0.0, 1.0) * sign(signal);
    float3 result = lerp(e, signal + blur, SHARPEN_STRENGTH);
    result = clamp(result, mn, mx);

    SetOutput(float4(result, color.a));
}
