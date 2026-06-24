// Vibrance - smarter saturation booster (boosts muted colors more than already-vivid ones)
#define VIBRANCE  0.3   // 0.0=off, 1.0=heavy

void main() {
    float4 color = Sample();
    float gray = dot(color.rgb, float3(0.299, 0.587, 0.114));
    float maxChan = max(max(color.r, color.g), color.b);
    float minChan = min(min(color.r, color.g), color.b);
    float sat = maxChan - minChan;  // current saturation
    float target = sat + (1.0 - sat) * VIBRANCE;  // boost low-saturation more
    float factor = (sat > 0.001) ? target / sat : 1.0;
    color.rgb = gray + (color.rgb - gray) * factor;
    SetOutput(color);
}
