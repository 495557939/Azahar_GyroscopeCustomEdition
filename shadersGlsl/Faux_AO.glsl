// Faux AO - Color-based ambient occlusion approximation
// Uses luminance gradients to estimate depth and adds shading in "corners"
#define AO_STRENGTH  0.3   // 0.0=off, 1.0=heavy
#define AO_RADIUS    3.0   // Sample radius in pixels

void main() {
    float2 inv_res = GetInvResolution();
    float2 tc = GetCoordinates();
    float4 color = Sample();
    float center_luma = dot(color.rgb, float3(0.299, 0.587, 0.114));

    float occlusion = 0.0;
    int samples = 4;
    for (int i = 0; i < samples; i++) {
        float angle = (float(i) / float(samples)) * 6.28318;
        float2 dir = float2(cos(angle), sin(angle)) * inv_res * AO_RADIUS;
        float sample_luma = dot(SampleLocation(tc + dir).rgb, float3(0.299, 0.587, 0.114));
        // Pixels that are darker than center = deeper → more occlusion
        float depth_diff = center_luma - sample_luma;
        occlusion += max(depth_diff, 0.0) * 0.25;
    }

    occlusion = clamp(occlusion * AO_STRENGTH, 0.0, 0.5);
    color.rgb *= (1.0 - occlusion);

    SetOutput(color);
}
