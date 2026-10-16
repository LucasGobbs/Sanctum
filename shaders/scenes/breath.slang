// First scene: slow breathing gradient field. Kept minimal on purpose —
// it is the smoke test for the Slang -> goose -> SDL GPU path and the
// template for new scenes. Edit and save; the app reloads automatically.
//
// Scene convention: one file, both stages, GIS blit-style. The vertex stage
// is the same fullscreen triangle everywhere; the fragment stage is where a
// scene says something. Import "common" for the shared SceneUniforms.
import "../common";

[[vk::binding(0, 0)]]
ConstantBuffer<SceneUniforms> Uniforms;

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv;
};

[shader("vertex")]
VsOut vertMain(uint vertexIndex : SV_VertexID)
{
    VsOut o;
    // Big triangle: (-1,-1) (3,-1) (-1,3) in NDC, covering the viewport.
    float2 p = float2(
        (vertexIndex == 1) ? 3.0 : -1.0,
        (vertexIndex == 2) ? 3.0 : -1.0);
    o.pos = float4(p, 0.0, 1.0);
    // y flip: texture space expects row 0 at the top.
    o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return o;
}

struct FragmentOutput
{
    float4 color : SV_Target0;
};

float3 palette(float t)
{
    // Sanctum amber drifting into deep blue; calm, liturgical pacing.
    float3 amber = float3(0.909, 0.658, 0.341);
    float3 deep  = float3(0.031, 0.043, 0.090);
    return lerp(deep, amber, smoothstep(0.15, 0.85, t));
}

[shader("fragment")]
float4 pixelMain(VsOut input) : SV_Target0
{
    float breathe = 0.5 + 0.5 * sin(Uniforms.iTime * 0.35);
    float vign = 1.0 - 0.55 * length(input.uv - 0.5);
    float3 c = palette(0.2 + 0.6 * breathe) * vign;
    return float4(c, 1.0);
}
