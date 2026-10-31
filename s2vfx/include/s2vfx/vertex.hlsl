// The vertex stage of a model shader: the object's place from the engine's transform texture (one rigid matrix, or
// up to four bones per vertex), the flex (morph) deltas and the wrinkle amount, the tangent frame (normal, tangent and
// bitangent, plain or packed in four bytes), the uv, and the object's tint and id. A shader's VS block includes this
// and calls VertexMain, or writes its own MainVs around it.
//
// The engine picks the dynamic combos from the mesh: D_BLEND_WEIGHT_COUNT 0 to 4, D_COMPRESSED_NORMALS_AND_TANGENTS,
// D_MORPH (a mesh with flexes); D_MULTIVIEW_INSTANCING (common.hlsl, one view only). Carried over from the game's
// vr_simple and vr_complex vertex shaders.

DynamicCombo(D_BLEND_WEIGHT_COUNT, 0..4, Sys(ALL));
DynamicCombo(D_COMPRESSED_NORMALS_AND_TANGENTS, 0..1, Sys(ALL));
DynamicCombo(D_MORPH, 0..1, Sys(ALL));

Texture2D g_tTransformTexture;
SamplerState g_sTransformTexture < Filter(MIN_MAG_MIP_POINT); >;
#if D_MORPH
// The engine composites this each frame from the flex weights: per vertex, the position's delta in the left half
// and the normal's in the right half (its w is the wrinkle amount).
Texture2D g_tCompositeMorphTextureAtlas;
#endif

struct VertexInput
{
    float3 vPositionOs : POSITION0 < Semantic(PosXyz); >;
    float2 vTexCoord : TEXCOORD0 < Semantic(LowPrecisionUv); >;
#if S_SECONDARY_UV
    // A shader with F_SECONDARY_UV declares StaticCombo(S_SECONDARY_UV, F_SECONDARY_UV, Sys(ALL)) in its VS block:
    // the second uv is read only from meshes whose material asks for it (as vr_complex has it).
    float2 vTexCoord2 : TEXCOORD1 < Semantic(LowPrecisionUv1); >;
#endif
    // Packed: the normal in bytes 0 and 1, the tangent in 2 and 3 (its handedness the sign of byte 3). Plain: the
    // normal here and the tangent with its handedness in w below.
    float4 vNormalOs : NORMAL0 < Semantic(OptionallyCompressedTangentFrame); >;
#if !D_COMPRESSED_NORMALS_AND_TANGENTS
    float4 vTangentUOs_flTangentVSign : TANGENT0 < Semantic(TangentU_SignV); >;
#endif
#if D_BLEND_WEIGHT_COUNT >= 1
    uint4 vBlendIndices : BLENDINDICES0 < Semantic(BlendIndices); >;
#endif
#if D_BLEND_WEIGHT_COUNT >= 2
    float4 vBlendWeight : BLENDWEIGHT0 < Semantic(BlendWeight); >;
#endif
#if D_MORPH
    float flVertexIndex : TEXCOORD14 < Semantic(MorphIndex); >;
#endif
    float2 vTransformTextureUV : TEXCOORD13 < Semantic(InstanceTransformUv); >;
#if D_MULTIVIEW_INSTANCING
    uint nInstanceID : SV_InstanceID < Semantic(None); >;
#endif
};

// The texel n places after the object's first one, wrapped onto the next row.
float2 TexelUv(uint n, float2 objectUv)
{
    const float x = (float(n) + 0.5) * g_vInvGBufferSize.z + objectUv.x;
    const float row = floor(x);
    return float2(x - row, objectUv.y + row * g_vInvGBufferSize.w);
}

// Three rows of a matrix stored as three texels from uv on.
float3x4 RowsAt(float2 uv)
{
    return float3x4(g_tTransformTexture.SampleLevel(g_sTransformTexture, uv, 0),
                    g_tTransformTexture.SampleLevel(g_sTransformTexture, uv, 0, int2(1, 0)),
                    g_tTransformTexture.SampleLevel(g_sTransformTexture, uv, 0, int2(2, 0)));
}

// A bone's first texel: 8 texels past the object's own, 4 per bone.
float2 BoneUv(uint bone, float2 objectUv)
{
    return TexelUv(bone * 4 + 8, objectUv);
}

// A unit vector packed in two bytes: per byte a six bit magnitude and two sign bits, folded twice (octahedral). The
// first byte's top sign is the z sign.
float3 UnpackOctahedral(float2 packed)
{
    float2 v = packed * 255.0 - 128.0;
    const float zSign = v.x < 0.0 ? -1.0 : 1.0;
    v = abs(v) - (v < 0.0 ? 1.0 : 0.0);
    v = v - 64.0;
    const float2 s2 = v < 0.0 ? -1.0 : 1.0;
    v = abs(v) - (v < 0.0 ? 1.0 : 0.0);
    v /= 63.0;
    const float3 n = normalize(float3(v, 1.0 - v.x - v.y));
    return float3(s2 * n.xy, zSign * n.z);
}

float3 UnpackNormal(float2 packed)
{
    return UnpackOctahedral(packed);
}

#if D_MORPH
// The vertex's flex deltas: the object's texel 4 holds the atlas row width (w) and x offset (z), texel 5 the y offset
// (w), all as floats. A row width of zero is an object with no flex data. The normal delta's w is the wrinkle amount.
void MorphDeltas(float vertexIndex, float2 objectUv, out float3 position, out float4 normal)
{
    position = 0.0;
    normal = 0.0;
    const float4 t4 = g_tTransformTexture.SampleLevel(g_sTransformTexture, TexelUv(4, objectUv), 0);
    const float4 t5 = g_tTransformTexture.SampleLevel(g_sTransformTexture, TexelUv(5, objectUv), 0);
    const uint width = (uint)t4.w;
    if (width == 0)
        return;
    const uint id = (uint)vertexIndex;
    const int2 texel = int2(id % width + (uint)t4.z, id / width + (uint)t5.w);
    uint atlasWidth, atlasHeight;
    g_tCompositeMorphTextureAtlas.GetDimensions(atlasWidth, atlasHeight);
    position = g_tCompositeMorphTextureAtlas.Load(int3(texel, 0)).xyz;
    normal = g_tCompositeMorphTextureAtlas.Load(int3(texel.x + atlasWidth / 2, texel.y, 0));
}
#endif

// The inverse transpose of a matrix's 3x3 part: what turns normals (and, as Valve's skinned shaders do, tangents)
// when a bone also scales.
float3x3 InverseTranspose(float3x4 m)
{
    const float3 r0 = m[0].xyz, r1 = m[1].xyz, r2 = m[2].xyz;
    const float3x3 cofactor = float3x3(cross(r1, r2), cross(r2, r0), cross(r0, r1));
    return cofactor / dot(r0, cross(r1, r2));
}

PixelInput VertexMain(VertexInput i)
{
    const float2 objectUv = i.vTransformTextureUV;
    float3 position = i.vPositionOs;
#if D_COMPRESSED_NORMALS_AND_TANGENTS
    float3 normal = UnpackOctahedral(i.vNormalOs.xy);
    const float3 tangent = UnpackOctahedral(i.vNormalOs.zw);
    const float handedness = i.vNormalOs.w * 255.0 - 128.0 < 0.0 ? -1.0 : 1.0;
#else
    float3 normal = i.vNormalOs.xyz;
    const float3 tangent = i.vTangentUOs_flTangentVSign.xyz;
    const float handedness = i.vTangentUOs_flTangentVSign.w;
#endif
    float wrinkle = 0.0;
#if D_MORPH
    // The flexes move the bind pose, before anything else touches it; the tangent is left as it is.
    float3 morphPosition;
    float4 morphNormal;
    MorphDeltas(i.flVertexIndex, objectUv, morphPosition, morphNormal);
    position += morphPosition;
    const float3 movedNormal = normal + morphNormal.xyz;
    if (dot(movedNormal, movedNormal) > 1e-12)
        normal = normalize(movedNormal);
    wrinkle = morphNormal.w;
#endif
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
#if S2VFX_DISPLACE
    // A shader that moves the surface itself defines S2VFX_DISPLACE and a Displace function before including this:
    // the position and normal as the mesh has them (after the flexes), the matrix that places them, and four values
    // of its own for its pixel stage (PixelInput.vExtra).
    float4 displaced;
    Displace(position, normal, m, displaced);
#endif
    // The tools variant of Valve's shaders (the one S2FM draws with) turns the normal and the tangent by the inverse
    // transpose, rigid or skinned: right when the matrix scales one axis more than another.
    const float3x3 frame = InverseTranspose(m);
    const float3 positionWs = mul(m, float4(position, 1.0));

    PixelInput o;
    o.vPositionWs = positionWs - g_vHighPrecisionLightingOffsetWs.xyz;
    // The frame the pixel shaders rebuild a normal map's normal in: the tangent made square to the normal
    // (Gram-Schmidt), the bitangent their cross product turned by the handedness.
    const float3 normalWs = normalize(mul(frame, normal));
    float3 tangentWs = mul(frame, tangent);
    tangentWs = normalize(tangentWs - normalWs * dot(tangentWs, normalWs));
    o.vNormalWs = float4(normalWs, wrinkle);
    o.vTangentWs = tangentWs;
    o.vBitangentWs = cross(normalWs, tangentWs) * handedness;
    o.vTexCoord = i.vTexCoord;
#if S_SECONDARY_UV
    o.vTexCoord2 = i.vTexCoord2;
#else
    o.vTexCoord2 = i.vTexCoord;
#endif
#if S2VFX_DISPLACE
    o.vExtra = displaced;
#endif
#if S2VFX_OBJECT_MATRIX
    o.vObjectRow0 = m[0];
    o.vObjectRow1 = m[1];
    o.vObjectRow2 = m[2];
#endif
    // The fourth texel: x a value of the object's own, y its tint as bytes, z its id.
    const float4 extra = g_tTransformTexture.SampleLevel(g_sTransformTexture, objectUv, 0, int2(3, 0));
    const uint tint = asuint(extra.y);
    o.vTint = float4(float3((tint >> 16) & 255, (tint >> 8) & 255, tint & 255) / 255.0, extra.x);
#if S2VFX_DEBUG_FRAME
    // Test only: red the normals arrive packed, green the handedness is positive, blue the mesh is skinned.
    o.vTint = float4(D_COMPRESSED_NORMALS_AND_TANGENTS ? 1.0 : 0.0, handedness > 0.0 ? 1.0 : 0.0,
                     D_BLEND_WEIGHT_COUNT > 0 ? 1.0 : 0.0, 1.0);
#endif
#if D_MORPH && S2VFX_DEBUG_MORPH
    // Test only: the flex's position delta, as a colour the debug pixel shader shows.
    o.vTint = float4(saturate(abs(morphPosition) * S2VFX_DEBUG_MORPH), 1.0);
#endif
#if D_MULTIVIEW_INSTANCING
    o.nObjectId = uint2(asuint(extra.z) & 0xFFFF, i.nInstanceID);
    o.flClip = 1.0;
#else
    o.nObjectId = asuint(extra.z) & 0xFFFF;
#endif
    o.vPositionPs = mul(g_matWorldToProjectionMultiview[0], float4(positionWs, 1.0) + g_vWorldToCameraOffsetMultiview[0]);
    return o;
}
