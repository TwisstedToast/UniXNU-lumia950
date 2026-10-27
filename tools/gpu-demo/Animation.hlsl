cbuffer Frame : register(b0)
{
    float2 resolution;
    float timeSeconds;
    float padding;
};

Texture2D<float4> hudTexture : register(t0);
SamplerState hudSampler : register(s0);

struct VertexOutput
{
    float4 position : SV_Position;
};

VertexOutput vertexMain(uint vertex : SV_VertexID)
{
    VertexOutput output;
    float2 corner = float2((vertex << 1) & 2, vertex & 2);
    output.position = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float4 pixelMain(VertexOutput input) : SV_Target
{
    float2 uv = input.position.xy / resolution;
    float2 centered = (input.position.xy - resolution * 0.5) / min(resolution.x, resolution.y);
    float phase = timeSeconds * 0.7;
    float wave = sin(uv.y * 6 + sin(uv.x * 4 + phase) + phase);
    float3 color = lerp(float3(0.015, 0.025, 0.10), float3(0.13, 0.035, 0.25), uv.y);
    color += float3(0.025, 0.10, 0.16) * (wave * 0.5 + 0.5);
    float2 orbCenter = float2(sin(phase) * 0.18, cos(phase * 0.83) * 0.14 + 0.05);
    float2 relative = centered - orbCenter;
    float radiusSquared = dot(relative, relative);
    color += float3(0.02, 0.28, 0.42) / (1 + radiusSquared * 18);
    float orb = 1 - smoothstep(0.255, 0.26, sqrt(radiusSquared));
    float depth = sqrt(saturate(1 - radiusSquared / (0.26 * 0.26)));
    float ribbon = sin(relative.x * 18 + relative.y * 11 + depth * 5 + phase * 2);
    float3 surface = lerp(float3(0.06, 0.88, 0.98), float3(0.95, 0.16, 0.48), ribbon * 0.5 + 0.5);
    surface *= 0.25 + depth * 0.8;
    float highlight = pow(saturate(dot(normalize(float3(relative / 0.26, depth)),
        normalize(float3(-0.4, -0.5, 1)))), 32);
    color = lerp(color, surface + highlight * 0.7, orb);
    float orbit = abs(length(centered - orbCenter * 0.6) - 0.38);
    color += float3(0.13, 0.45, 0.70) * (1 - smoothstep(0.001, 0.003, orbit));
    float hudWidth = min(resolution.x - 48, 1024);
    float2 hudUv = (input.position.xy - float2(24, 24)) / float2(hudWidth, hudWidth * 0.25);
    if (all(hudUv >= 0) && all(hudUv <= 1)) {
        float4 text = hudTexture.SampleLevel(hudSampler, hudUv, 0);
        color = lerp(color * 0.5, float3(0.90, 0.98, 1), text.a);
    }
    return float4(saturate(color), 1);
}
