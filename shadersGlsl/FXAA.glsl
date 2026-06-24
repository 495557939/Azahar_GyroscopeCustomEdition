// FXAA 3.11 with tunable parameters
// Edit values below:  Subpix=softness(0-1)  EdgeThresh=sensitivity  Quality=10-39
#define FXAA_SUBPIX       0.25
#define FXAA_EDGE_THRESH  0.125
#define FXAA_QUALITY      15

void main() {
    float2 inv_res = GetInvResolution();
    float4 color = Sample();
    float2 tc = GetCoordinates();

    float lumaC = dot(color.rgb, float3(0.299, 0.587, 0.114));
    float lumaD = dot(SampleLocation(tc + float2( 0.0, -inv_res.y)).rgb, float3(0.299, 0.587, 0.114));
    float lumaU = dot(SampleLocation(tc + float2( 0.0,  inv_res.y)).rgb, float3(0.299, 0.587, 0.114));
    float lumaL = dot(SampleLocation(tc + float2(-inv_res.x,  0.0)).rgb, float3(0.299, 0.587, 0.114));
    float lumaR = dot(SampleLocation(tc + float2( inv_res.x,  0.0)).rgb, float3(0.299, 0.587, 0.114));

    float lumaMin = min(lumaC, min(min(lumaD, lumaU), min(lumaL, lumaR)));
    float lumaMax = max(lumaC, max(max(lumaD, lumaU), max(lumaL, lumaR)));
    float range = lumaMax - lumaMin;
    if (range < max(FXAA_EDGE_THRESH, lumaMax * 0.0625)) { SetOutput(color); return; }

    float2 dir;
    dir.x = -((lumaL + lumaR) - 2.0 * lumaC);
    dir.y =  ((lumaU + lumaD) - 2.0 * lumaC);
    float dirReduce = max((lumaL + lumaR + lumaU + lumaD) * 0.0625, 0.001);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -8.0, 8.0) * inv_res;

    float4 rgA = 0.5 * (SampleLocation(tc + dir * (1.0/3.0 - 0.5)) +
                         SampleLocation(tc + dir * (2.0/3.0 - 0.5)));
    float4 rgB = rgA * 0.5 + 0.25 * (SampleLocation(tc + dir * -0.5) +
                                       SampleLocation(tc + dir *  0.5));
    float lumaB = dot(rgB.rgb, float3(0.299, 0.587, 0.114));
    SetOutput((lumaB < lumaMin || lumaB > lumaMax) ? rgA : rgB);
}
