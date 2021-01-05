#define rootsig \
    "StaticSampler(s0, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_ANISOTROPIC, maxAnisotropy = 8, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \
    "StaticSampler(s1, visibility = SHADER_VISIBILITY_PIXEL, filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, comparisonFunc = COMPARISON_LESS_EQUAL, addressU = TEXTURE_ADDRESS_BORDER, addressV = TEXTURE_ADDRESS_BORDER, borderColor = STATIC_BORDER_COLOR_OPAQUE_WHITE), " \
    "RootConstants(b0, num32BitConstants=20, visibility = SHADER_VISIBILITY_VERTEX)," \
    "CBV(b1, space = 0, visibility = SHADER_VISIBILITY_PIXEL"), \
    "CBV(b2, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "CBV(b3, space = 0, visibility = SHADER_VISIBILITY_ALL"), \
    "DescriptorTable(SRV(t0, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL), " \
    "DescriptorTable(SRV(t1, space = 0, numDescriptors = 1000), visibility = SHADER_VISIBILITY_VERTEX), " \
    "DescriptorTable(SRV(t2, space = 1, numDescriptors = 1000), visibility = SHADER_VISIBILITY_PIXEL) "

struct FrameCbLayout
{
	float4x4 sceneRotation;
	uint sceneIndexBufferBindlessIndex;
	uint scenePositionBufferBindlessIndex;
	uint sceneNormalBufferBindlessIndex;
	uint sceneUvBufferBindlessIndex;
	int envmapTextureIndex;
};

struct ViewCbLayout
{
	float4x4 viewTransform;
	float4x4 projectionTransform;
};

struct MeshCbLayout
{
	float4x4 localToWorld;
	uint indexOffset;
	uint positionOffset;
	uint normalOffset;
	uint uvOffset;
};

struct MaterialCbLayout
{
	float3 emissiveFactor;
	float metallicFactor;
	float3 baseColorFactor;
	float roughnessFactor;
	int baseColorTextureIndex;
	int metallicRoughnessTextureIndex;
	int normalTextureIndex;
	int baseColorSamplerIndex;
	int metallicRoughnessSamplerIndex;
	int normalSamplerIndex;
};

SamplerState g_anisoSampler : register(s0);
ConstantBuffer<MeshCbLayout> g_meshConstants : register(b0);
ConstantBuffer<MaterialCbLayout> g_materialConstants : register(b1);
ConstantBuffer<ViewCbLayout> g_viewConstants : register(b2);
ConstantBuffer<FrameCbLayout> g_frameConstants : register(b3);
Texture2D g_bindless2DTextures[] : register(t0, space0);
ByteAddressBuffer g_bindlessBuffers[] : register(t1, space0);
TextureCube g_bindlessCubeTextures[] : register(t2, space1);

struct vs_to_ps
{
	float4 pos : SV_POSITION;
	float4 normal : INTERPOLATED_WORLD_NORMAL;
	float2 uv : INTERPOLATED_UV_0;
};

vs_to_ps vs_main(uint vertexId : SV_VertexID)
{
	vs_to_ps o;

	// size of 4 for 32 bit indices
	uint index = g_bindlessBuffers[g_frameConstants.sceneIndexBufferBindlessIndex].Load(4*(vertexId + g_meshConstants.indexOffset));

	// size of 12 for float3 positions
	float3 position = g_bindlessBuffers[g_frameConstants.scenePositionBufferBindlessIndex].Load<float3>(12 * (index + g_meshConstants.positionOffset));

	// size of 12 for float3 normals
	float3 normal = g_bindlessBuffers[g_frameConstants.sceneNormalBufferBindlessIndex].Load<float3>(12 * (index + g_meshConstants.normalOffset));

	// size of 8 for float2 uv's
	float2 uv = g_bindlessBuffers[g_frameConstants.sceneUvBufferBindlessIndex].Load<float2>(8 * (index + g_meshConstants.uvOffset));

	float4x4 localToWorld = mul(g_meshConstants.localToWorld, g_frameConstants.sceneRotation);
	float4 worldPos = mul(float4(position, 1.f), localToWorld);
	float4x4 viewProjTransform = mul(g_viewConstants.viewTransform, g_viewConstants.projectionTransform);
	o.pos = mul(worldPos, viewProjTransform);
	o.normal = mul(float4(normal, 0.f), g_meshConstants.localToWorld);
	o.uv = uv;

	return o;
}

float4 ps_main(vs_to_ps input) : SV_Target
{
	float4 normal = normalize(input.normal);
	float4 lightDir = float4(1, 1, -1, 0);
	float3 baseColor = g_bindless2DTextures[g_materialConstants.baseColorTextureIndex].Sample(g_anisoSampler, input.uv).rgb;
	baseColor *= g_bindlessCubeTextures[g_frameConstants.envmapTextureIndex].Sample(g_anisoSampler, normal.xyz).rgb;

	return saturate(dot(lightDir, normal)) * float4(baseColor.rgb, 0.f);
}