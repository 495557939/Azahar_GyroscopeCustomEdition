// FXAA x4 - Four-pass edge smoothing for heavy aliasing
// Each pass detects edges and blends perpendicular to the edge direction
#define SUBPIX  0.3
#define EDGE    0.08

float4 ApplyFXAA(float4 in_color, float2 tc, float2 inv_res, float3 luma_vec) {
    float lumaC = dot(in_color.rgb, luma_vec);
    float lumaD = dot(SampleLocation(tc + float2( 0.0, -inv_res.y)).rgb, luma_vec);
    float lumaU = dot(SampleLocation(tc + float2( 0.0,  inv_res.y)).rgb, luma_vec);
    float lumaL = dot(SampleLocation(tc + float2(-inv_res.x,  0.0)).rgb, luma_vec);
    float lumaR = dot(SampleLocation(tc + float2( inv_res.x,  0.0)).rgb, luma_vec);

    float lumaMin = min(lumaC, min(min(lumaD, lumaU), min(lumaL, lumaR)));
    float lumaMax = max(lumaC, max(max(lumaD, lumaU), max(lumaL, lumaR)));
    float range = lumaMax - lumaMin;
    if (range < max(EDGE, lumaMax * 0.0625)) return in_color;

    float2 dir;
    dir.x = -((lumaL + lumaR) - 2.0 * lumaC);
    dir.y =  ((lumaU + lumaD) - 2.0 * lumaC);
    float dirReduce = max((lumaL + lumaR + lumaU + lumaD) * 0.0625, 0.001);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + dirReduce), -8.0, 8.0) * inv_res;

    float4 rgA = 0.5 * (SampleLocation(tc + dir * (1.0/3.0 - 0.5)) +
                         SampleLocation(tc + dir * (2.0/3.0 - 0.5)));
    float4 rgB = rgA * 0.5 + 0.25 * (SampleLocation(tc + dir * -0.5) +
                                       SampleLocation(tc + dir *  0.5));
    float lumaB = dot(rgB.rgb, luma_vec);
    return (lumaB < lumaMin || lumaB > lumaMax) ? rgA : rgB;
}

void main() {
    float2 inv_res = GetInvResolution();
    float2 tc = GetCoordinates();
    float3 luma = float3(0.299, 0.587, 0.114);

    float4 out_color = ApplyFXAA(Sample(), tc, inv_res, luma);
    out_color = ApplyFXAA(out_color, tc, inv_res, luma);
    out_color = ApplyFXAA(out_color, tc, inv_res, luma);
    out_color = ApplyFXAA(out_color, tc, inv_res, luma);

    SetOutput(out_color);
}
