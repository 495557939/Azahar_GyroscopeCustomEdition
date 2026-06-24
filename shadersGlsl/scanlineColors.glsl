/*	
[OptionRangeFloat]
GUIName = Contrast Threshold
OptionName = CONTRAST_THRESHOLD
MinValue = 0.10
MaxValue = 1.00
StepAmount = 0.01
DefaultValue = 0.50

[OptionRangeFloat]
GUIName = Edge Darkening
OptionName = EDGE_DARKENING
MinValue = 0.00
MaxValue = 0.50
StepAmount = 0.01
DefaultValue = 0.15
*/

#define lumCoeff float3(0.2126729, 0.7151522, 0.0721750)

float AvgLuminance(float3 color)
{
    return sqrt(dot(color * color, lumCoeff));
}

#ifndef CONTRAST_THRESHOLD
#define CONTRAST_THRESHOLD 0.50
#endif

#ifndef EDGE_DARKENING
#define EDGE_DARKENING 0.15
#endif

float4 ColorGradePass(float4 color)
{
    // 屏幕边缘暗化程度，利用横坐标
    float level = (4.0 - GetCoordinates().x) * EDGE_DARKENING;
    
    // 对比度增强
    float4 base = smoothstep(0.2, CONTRAST_THRESHOLD, color);
    
    // 色彩方向向量，安全归一化（防止零向量导致斑点）
    float lum = AvgLuminance(color.rgb);
    float4 dirVec = float4(color.rgb, lum);
    float len = length(dirVec);
    float4 normDir = (len > 1e-6) ? (dirVec / len) : float4(0.0, 0.0, 0.0, 0.0);
    
    float4 intensity = base + normDir;
    
    // 混合调色与原图
    float4 result = intensity * (0.5 - level) + color * 1.1;
    return clamp(result, 0.0, 1.0);
}

void main()
{
    float4 color = Sample();
    color = ColorGradePass(color);
    SetOutput(color);
}