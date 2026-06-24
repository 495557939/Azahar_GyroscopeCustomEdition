// FXAA x2 - Two-pass Fast Approximate Anti-Aliasing
// Applies FXAA algorithm twice for stronger edge smoothing
#define SUBPIX  0.25  // 0=sharp, 1=soft
#define EDGE    0.10  // lower = more sensitive

void main() {
    float2 inv_res = GetInvResolution();
    float2 tc = GetCoordinates();
    float4 color = Sample();

    // === Pass 1 ===
    float lumaC = dot(color.rgb, float3(0.299, 0.587, 0.114));
    float lumaD = dot(SampleLocation(tc + float2( 0.0, -inv_res.y)).rgb, float3(0.299, 0.587, 0.114));
    float lumaU = dot(SampleLocation(tc + float2( 0.0,  inv_res.y)).rgb, float3(0.299, 0.587, 0.114));
    float lumaL = dot(SampleLocation(tc + float2(-inv_res.x,  0.0)).rgb, float3(0.299, 0.587, 0.114));
    float lumaR = dot(SampleLocation(tc + float2( inv_res.x,  0.0)).rgb, float3(0.299, 0.587, 0.114));

    float lumaMin = min(lumaC, min(min(lumaD, lumaU), min(lumaL, lumaR)));
    float lumaMax = max(lumaC, max(max(lumaD, lumaU), max(lumaL, lumaR)));
    float range = lumaMax - lumaMin;
    if (range < max(EDGE, lumaMax * 0.0625)) { SetOutput(color); return; }

    float2 dir;
    dir.x = -((lumaL + lumaR) - 2.0 * lumaC);
    dir.y =  ((lumaU + lumaD) - 2.0 * lumaC);
    float dirReduce = max((lumaL + lumaR + lumaU + lumaD) * 0.0625, 0.001);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + dirReduce), -8.0, 8.0) * inv_res;

    float4 rgA = 0.5 * (SampleLocation(tc + dir * (1.0/3.0 - 0.5)) +
                         SampleLocation(tc + dir * (2.0/3.0 - 0.5)));
    float4 rgB = rgA * 0.5 + 0.25 * (SampleLocation(tc + dir * -0.5) +
                                       SampleLocation(tc + dir *  0.5));
    float lumaB1 = dot(rgB.rgb, float3(0.299, 0.587, 0.114));
    float4 pass1 = (lumaB1 < lumaMin || lumaB1 > lumaMax) ? rgA : rgB;

    // === Pass 2 (apply FXAA again on pass1 result) ===
    lumaC = dot(pass1.rgb, float3(0.299, 0.587, 0.114));
    lumaD = dot(SampleLocation(tc + float2( 0.0, -inv_res.y)).rgb, float3(0.299, 0.587, 0.114));
    lumaU = dot(SampleLocation(tc + float2( 0.0,  inv_res.y)).rgb, float3(0.299, 0.587, 0.114));
    lumaL = dot(SampleLocation(tc + float2(-inv_res.x,  0.0)).rgb, float3(0.299, 0.587, 0.114));
    lumaR = dot(SampleLocation(tc + float2( inv_res.x,  0.0)).rgb, float3(0.299, 0.587, 0.114));

    lumaMin = min(lumaC, min(min(lumaD, lumaU), min(lumaL, lumaR)));
    lumaMax = max(lumaC, max(max(lumaD, lumaU), max(lumaL, lumaR)));
    range = lumaMax - lumaMin;

    if (range < max(EDGE * 1.5, lumaMax * 0.08)) { SetOutput(pass1); return; }

    dir.x = -((lumaL + lumaR) - 2.0 * lumaC);
    dir.y =  ((lumaU + lumaD) - 2.0 * lumaC);
    dirReduce = max((lumaL + lumaR + lumaU + lumaD) * 0.0625, 0.001);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + dirReduce), -8.0, 8.0) * inv_res;

    rgA = 0.5 * (SampleLocation(tc + dir * (1.0/3.0 - 0.5)) +
                 SampleLocation(tc + dir * (2.0/3.0 - 0.5)));
    rgB = rgA * 0.5 + 0.25 * (SampleLocation(tc + dir * -0.5) +
                               SampleLocation(tc + dir *  0.5));
    float lumaB2 = dot(rgB.rgb, float3(0.299, 0.587, 0.114));
    float4 pass2 = (lumaB2 < lumaMin || lumaB2 > lumaMax) ? rgA : rgB;

    SetOutput(pass2);
}
