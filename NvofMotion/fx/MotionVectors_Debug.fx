// NVOF Motion vectors - debug visualization (phase 2c).
// The nvof_motion addon publishes its R16G16F flow field (grid4 by default,
// values in pixels, +x right, +y down) under the "MotionVectors" SEMANTIC
// (via effect_runtime::update_texture_bindings every executed frame).
// Textures with a semantic are bound by the runtime, not loaded from files.
// Copy this file next to your other ReShade .fx files and enable it.
// No #includes needed (self-contained).
// If the addon is idle/missing, the view stays black.

texture MotionTex : MotionVectors;
sampler MotionSampler
{
    Texture = MotionTex;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = POINT;
    AddressU = CLAMP;
    AddressV = CLAMP;
};

uniform float FlowVisScale <
    ui_type = "drag";
    ui_min = 0.0;
    ui_max = 0.5;
    ui_step = 0.001;
    ui_label = "Flow visualization scale (1/px)";
    ui_tooltip = "Magnitude multiplier before color mapping. 0.02 suits fast scenes, 0.1 slow ones.";
> = 0.02;

void VS_Fullscreen(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 HsvToRgb(float3 c)
{
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);
}

// Middlebury-style: direction -> hue, magnitude -> brightness/saturation.
float4 PS_FlowVis(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float2 flow = tex2D(MotionSampler, uv).xy;
    float mag = length(flow);
    float hue = frac(atan2(flow.y, flow.x) / 6.2831853 + 1.0);
    float sat = saturate(mag * FlowVisScale * 4.0);
    float val = saturate(mag * FlowVisScale);
    return float4(HsvToRgb(float3(hue, sat, val)), 1.0);
}

technique MotionVectorsVis < ui_label = "NVOF Motion Vectors (debug)"; >
{
    pass
    {
        VertexShader = VS_Fullscreen;
        PixelShader = PS_FlowVis;
    }
}
