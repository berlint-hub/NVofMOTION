// NVOF Motion vectors - debug visualization (phase 2c).
// Shows the flow field the addon injects into the effect texture
// "motion_vectors_tex" (R16G16F, grid dims, px units, +x right, +y down).
// Set the addon's "Target texture" field to exactly: motion_vectors_tex
// Dims track the backbuffer ((W+3)/4 = addon grid for any resolution);
// if the addon reports mismatch, the session grid changed (resize) -
// just Reload effects.
// Copy this file next to your other ReShade .fx files and enable it.
// No #includes needed (self-contained).

texture motion_vectors_tex
{
    Width = (BUFFER_WIDTH + 3) / 4;
    Height = (BUFFER_HEIGHT + 3) / 4;
    Format = RG16F;
};
sampler NVOFMotionSampler
{
    Texture = motion_vectors_tex;
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
    float2 flow = tex2D(NVOFMotionSampler, uv).xy;
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
