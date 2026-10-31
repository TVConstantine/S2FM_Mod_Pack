// The vertex stage of a model shader: the object's place from the engine's transform texture (one rigid matrix, or
// up to four bones per vertex), the normal (plain or packed in two bytes), the uv, and the per object tint and id.
// Its outputs are those every Alyx model pixel shader reads (S2FM draws models in the VrForward mode).
//
// Combos: D_BLEND_WEIGHT_COUNT 0 to 4, D_COMPRESSED_NORMALS_AND_TANGENTS, D_MORPH (the input is declared, the morph
// itself is not applied yet), D_MULTIVIEW_INSTANCING (one view only).
#include "engine_cbuffers.hlsl"

Texture2D g_tTransformTexture : register(t11);
SamplerState g_sTransformTexture : register(s0);

struct VertexInput
{
    float3 vPositionOs : POSITION0;
    float2 vTexCoord : TEXCOORD0;
    float4 vNormalOs : NORMAL0;
#if D_BLEND_WEIGHT_COUNT >= 1
    uint4 vBlendIndices : BLENDINDICES0;
#endif
#if D_BLEND_WEIGHT_COUNT >= 2
    float4 vBlendWeight : BLENDWEIGHT0;
#endif
#if D_MORPH
    float flVertexIndex : TEXCOORD14;
#endif
    float2 vTransformTextureUV : TEXCOORD13;
#if D_MULTIVIEW_INSTANCING
    uint nInstanceID : SV_InstanceID;
#endif
};

struct VertexOutput
{
    float3 vPositionWs : TEXCOORD0;    // less the lighting offset
    float3 vNormalWs : TEXCOORD1;
    float2 vTexCoord : TEXCOORD2;
    float4 vTint : TEXCOORD4;
#if D_MULTIVIEW_INSTANCING
    uint2 nObjectId : TEXCOORD12;
#else
    uint nObjectId : TEXCOORD12;
#endif
    float4 vPositionPs : SV_Position;
#if D_MULTIVIEW_INSTANCING
    float flClip : SV_ClipDistance0;
#endif
};

// Three rows of a matrix stored as three texels from uv on.
float3x4 RowsAt(float2 uv)
{
    return float3x4(g_tTransformTexture.SampleLevel(g_sTransformTexture, uv, 0),
                    g_tTransformTexture.SampleLevel(g_sTransformTexture, uv, 0, int2(1, 0)),
                    g_tTransformTexture.SampleLevel(g_sTransformTexture, uv, 0, int2(2, 0)));
}

// A bone's first texel: 8 texels past the object's own, 4 per bone, wrapped onto the next row.
float2 BoneUv(uint bone, float2 objectUv)
{
    const float x = (float(bone * 4 + 8) + 0.5) * g_vInvGBufferSize.z + objectUv.x;
    const float row = floor(x);
    return float2(x - row, objectUv.y + row * g_vInvGBufferSize.w);
}

// A normal packed in two bytes: per byte a six bit magnitude and two sign bits, folded twice (octahedral).
float3 UnpackNormal(float2 packed)
{
    float2 v = packed * 255.0 - 128.0;
    const float2 s1 = v < 0.0 ? -1.0 : 1.0;
    const float zSign = v.x < 0.0 ? -1.0 : 1.0;
    v = abs(v) - (v < 0.0 ? 1.0 : 0.0);
    v = v - 64.0;
    const float2 s2 = v < 0.0 ? -1.0 : 1.0;
    v = abs(v) - (v < 0.0 ? 1.0 : 0.0);
    v /= 63.0;
    float3 n = float3(v, 1.0 - v.x - v.y);
    n = normalize(n);
    return float3(s2 * n.xy, zSign * n.z);
}

VertexOutput MainVs(VertexInput i)
{
    const float2 objectUv = i.vTransformTextureUV;
    float3 position = i.vPositionOs;
#if D_BLEND_WEIGHT_COUNT >= 1
    // Skinned: the object's first texel holds a scale in z; the bones are blended by weight.
    position *= g_tTransformTexture.SampleLevel(g_sTransformTexture, objectUv, 0).z;
  #if D_BLEND_WEIGHT_COUNT == 1
    float3x4 m = RowsAt(BoneUv(i.vBlendIndices.x, objectUv));
  #else
    float3x4 m = RowsAt(BoneUv(i.vBlendIndices.x, objectUv)) * i.vBlendWeight.x;
    m += RowsAt(BoneUv(i.vBlendIndices.y, objectUv)) * i.vBlendWeight.y;
    #if D_BLEND_WEIGHT_COUNT >= 3
    m += RowsAt(BoneUv(i.vBlendIndices.z, objectUv)) * i.vBlendWeight.z;
    #endif
    #if D_BLEND_WEIGHT_COUNT >= 4
    m += RowsAt(BoneUv(i.vBlendIndices.w, objectUv)) * i.vBlendWeight.w;
    #endif
  #endif
#else
    const float3x4 m = RowsAt(objectUv);
#endif
#if D_COMPRESSED_NORMALS_AND_TANGENTS
    const float3 normal = UnpackNormal(i.vNormalOs.xy);
#else
    const float3 normal = i.vNormalOs.xyz;
#endif
    float3 positionWs = mul(m, float4(position, 1.0));
#ifdef S2VFX_TEST_LIFT
    positionWs.z += S2VFX_TEST_LIFT;    // tests only: shows that this code is the one drawing
#endif

    VertexOutput o;
    o.vPositionWs = positionWs - g_vHighPrecisionLightingOffsetWs.xyz;
    o.vNormalWs = normalize(mul((float3x3)m, normal));
    o.vTexCoord = i.vTexCoord;
    // The fourth texel: x a value of the object's own, y its tint as bytes, z its id.
    const float4 extra = g_tTransformTexture.SampleLevel(g_sTransformTexture, objectUv, 0, int2(3, 0));
    const uint tint = asuint(extra.y);
    o.vTint = float4(float3((tint >> 16) & 255, (tint >> 8) & 255, tint & 255) / 255.0, extra.x);
#if D_MULTIVIEW_INSTANCING
    o.nObjectId = uint2(asuint(extra.z) & 0xFFFF, i.nInstanceID);
    o.flClip = 1.0;
#else
    o.nObjectId = asuint(extra.z) & 0xFFFF;
#endif
    o.vPositionPs = mul(g_matWorldToProjectionMultiview[0], float4(positionWs, 1.0) + g_vWorldToCameraOffsetMultiview[0]);
    return o;
}
