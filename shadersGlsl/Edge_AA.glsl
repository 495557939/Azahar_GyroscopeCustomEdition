// Edge-Directed Anti-Aliasing (simplified SMAA-like)
#define EDGE_THRESHOLD  0.08
#define BLEND_STRENGTH  0.5

void main() {
    float2 inv_res = GetInvResolution();
    float4 color = Sample();
    float2 tc = GetCoordinates();

    float3 c  = color.rgb;
    float3 l  = SampleLocation(tc + float2(-inv_res.x,  0.0)).rgb;
    float3 r  = SampleLocation(tc + float2( inv_res.x,  0.0)).rgb;
    float3 t  = SampleLocation(tc + float2( 0.0,      -inv_res.y)).rgb;
    float3 b  = SampleLocation(tc + float2( 0.0,       inv_res.y)).rgb;

    float lc = dot(c, float3(0.299, 0.587, 0.114));
    float ll = dot(l, float3(0.299, 0.587, 0.114));
    float lr = dot(r, float3(0.299, 0.587, 0.114));
    float lt = dot(t, float3(0.299, 0.587, 0.114));
    float lb = dot(b, float3(0.299, 0.587, 0.114));

    float hEdge = abs(ll - lc) + abs(lc - lr);
    float vEdge = abs(lt - lc) + abs(lc - lb);

    if (hEdge > vEdge && hEdge > EDGE_THRESHOLD) {
        float blend = clamp(hEdge, 0.0, 1.0) * BLEND_STRENGTH;
        float3 horiz = (l + c + r) / 3.0;
        SetOutput(float4(lerp(c, horiz, blend * 0.5), color.a));
    } else if (vEdge > EDGE_THRESHOLD) {
        float blend = clamp(vEdge, 0.0, 1.0) * BLEND_STRENGTH;
        float3 vert = (t + c + b) / 3.0;
        SetOutput(float4(lerp(c, vert, blend * 0.5), color.a));
    } else {
        SetOutput(color);
    }
}
