// Vignette + Scanlines retro look
#define VIGNETTE_STRENGTH  0.3
#define SCANLINE_STRENGTH  0.15

void main() {
    float2 tc = GetCoordinates();
    float4 color = Sample();

    float2 uv = tc - 0.5;
    float vignette = 1.0 - dot(uv, uv) * VIGNETTE_STRENGTH * 4.0;
    color.rgb *= vignette;

    float scanline = sin(tc.y * GetResolution().y * 3.14159) * SCANLINE_STRENGTH + (1.0 - SCANLINE_STRENGTH);
    color.rgb *= clamp(scanline, 0.0, 1.0);

    SetOutput(color);
}
