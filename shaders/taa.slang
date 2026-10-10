// Temporal anti-aliasing resolve (full screen): reprojects the history with the motion of the closest surface in
// the 3 x 3 neighbourhood, samples it with Catmull-Rom, clips it to the current neighbourhood's colour distribution
// (variance clipping in YCoCg) and blends. Writes the display image and the next history.
#include "common.slang"

[shader("vertex")]
float4 vs_taa(uint vid : SV_VertexID) : SV_Position
{
    float2 ndc = float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0;
    return float4(ndc, 0.0, 1.0);
}

float3 rgb_to_ycocg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 ycocg_to_rgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// Bicubic Catmull-Rom from 9 bilinear taps (Jimenez, "Filmic SMAA").
float3 sample_catmull_rom(Texture2D tex, SamplerState s, float2 uv, float2 size, float2 inv)
{
    float2 p = uv * size;
    float2 t1 = floor(p - 0.5) + 0.5;
    float2 f = p - t1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (t1 - 1.0) * inv, t3 = (t1 + 2.0) * inv, t12 = (t1 + w2 / w12) * inv;
    float3 r = float3(0, 0, 0);
    r += tex.SampleLevel(s, float2(t0.x, t0.y), 0).rgb * w0.x * w0.y;
    r += tex.SampleLevel(s, float2(t12.x, t0.y), 0).rgb * w12.x * w0.y;
    r += tex.SampleLevel(s, float2(t3.x, t0.y), 0).rgb * w3.x * w0.y;
    r += tex.SampleLevel(s, float2(t0.x, t12.y), 0).rgb * w0.x * w12.y;
    r += tex.SampleLevel(s, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y;
    r += tex.SampleLevel(s, float2(t3.x, t12.y), 0).rgb * w3.x * w12.y;
    r += tex.SampleLevel(s, float2(t0.x, t3.y), 0).rgb * w0.x * w3.y;
    r += tex.SampleLevel(s, float2(t12.x, t3.y), 0).rgb * w12.x * w3.y;
    r += tex.SampleLevel(s, float2(t3.x, t3.y), 0).rgb * w3.x * w3.y;
    return max(r, 0.0);
}

struct TaaOut {
    float4 display : SV_Target0;
    float4 history : SV_Target1;
};

[shader("fragment")]
TaaOut fs_taa(float4 pos : SV_Position)
{
    FrameConstants f = *pc.frame;
    Texture2D scene = g_textures[TEX_SCENE];
    int2 px = int2(pos.xy), size = int2(f.screen.xy);
    float3 cur = scene.Load(int3(px, 0)).rgb;
    float3 m1 = float3(0, 0, 0), m2 = float3(0, 0, 0);
    float closest = -1.0;
    int2 closest_px = px;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            int2 q = clamp(px + int2(i, j), int2(0, 0), size - 1);
            float3 y = rgb_to_ycocg(scene.Load(int3(q, 0)).rgb);
            m1 += y;
            m2 += y * y;
            float d = g_textures[TEX_DEPTH].Load(int3(q, 0)).x;           // reversed Z: larger = closer
            if (d > closest) { closest = d; closest_px = q; }
        }
    float2 motion = g_textures[TEX_MOTION].Load(int3(closest_px, 0)).xy;
    float2 uv = pos.xy * f.screen.zw;
    float2 huv = uv - motion;

    float w = f.taa.y;
    float3 result = cur;
    if (w < 1.0 && all(huv > 0.0) && all(huv < 1.0)) {
        float3 hist = sample_catmull_rom(g_textures[f.history_tex], g_samplers[SAMPLER_LINEAR_CLAMP], huv, f.screen.xy, f.screen.zw);
        float3 mean = m1 / 9.0;
        float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
        float3 lo = mean - 1.25 * sigma, hi = mean + 1.25 * sigma;
        // Clip towards the mean (not clamp per channel): fewer colour shifts.
        float3 h = rgb_to_ycocg(hist);
        float3 d = h - mean, e = 0.5 * (hi - lo) + 1e-5;
        float3 r = abs(d / e);
        float m = max(r.x, max(r.y, r.z));
        if (m > 1.0) h = mean + d / m;
        // Faster response under motion: less smearing of moving detail.
        float speed = length(motion * f.screen.xy);
        w = lerp(w, 0.3, saturate(speed / 16.0));
        result = lerp(ycocg_to_rgb(h), cur, w);
    }
    TaaOut o;
    o.display = float4(result, 1.0);
    o.history = float4(result, 1.0);
    return o;
}
