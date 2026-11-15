// ============================================================================
// PBR Water - sea spray, pass 3: the living particles as billboards, drawn after the water.
//
// Shaded as the vanilla particles are (Particle.hlsl): the texture times the base colour, lit by the sun after its shadows and by the ambient light, with the lighting influence of
// BSEffectShaderProperty mixing lit and unlit. Spray scatters light forward (water droplets are Mie scatterers), so the
// sun's share is weighted by a Henyey-Greenstein phase function normalised to the isotropic one: spray seen against the
// sun glows. Soft against the scene's depth, and premultiplied, so an emitter can be anywhere between alpha blended and
// additive.
// ============================================================================

#include "Common/Color.hlsli"
#include "Common/ColorManagement.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"
#include "PBRWater/Spray.hlsli"

StructuredBuffer<PBRWaterSpray::Emitter> Emitters : register(t0);
StructuredBuffer<PBRWaterSpray::Particle> Particles : register(t1);
StructuredBuffer<uint> Alive : register(t2);

struct VS_OUTPUT
{
	float4 position: SV_POSITION;
	float3 positionWS: TEXCOORD0;  // camera-relative
	float2 uv: TEXCOORD1;
	nointerpolation float4 info: TEXCOORD2;  // x alpha, y emitter, z view depth of the centre (units), w radius (units)
};

#if defined(VSHADER)
VS_OUTPUT VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	VS_OUTPUT o = (VS_OUTPUT)0;
	PBRWaterSpray::Particle p = Particles[Alive[instanceID]];
	uint emitterIndex = PBRWaterSpray::EmitterIndex(p);
	PBRWaterSpray::Emitter em = Emitters[emitterIndex];

	float t = saturate(p.age / max(p.life, 1e-3));
	float grow = em.Life.z > 0.0 ? saturate(p.age / em.Life.z) : 1.0;
	float radius = p.radius * lerp(1.0, em.Size.z, t) * grow;
	float fadeIn = em.Shade.x > 0.0 ? saturate(t / em.Shade.x) : 1.0;
	float fadeOut = em.Life.w > 0.0 ? saturate((p.life - p.age) / em.Life.w) : 1.0;

	float3 centre = p.position - FrameBuffer::CameraPosAdjust.xyz;
	float distance = length(centre);
	float distanceFade = 1.0 - smoothstep(0.8 * em.Size.w, em.Size.w, distance);
	float alpha = em.Color.a * fadeIn * fadeOut * distanceFade;

	// Facing the camera, upright; looking straight down at it, the camera's own axes.
	float3 view = centre / max(distance, 1e-3);
	float3 up = float3(0.0, 0.0, 1.0) - view * view.z;
	float upLength = length(up);
	float3 right;
	if (upLength > 1e-3) {
		up /= upLength;
		right = cross(view, up);
	} else {
		right = normalize(FrameBuffer::CameraView[0].xyz);
		up = cross(right, view);
	}

	float2 extent = radius.xx;
	float3 axisX = right, axisY = up;
	// Motion stretch: drawn out along its velocity across the view, as a streak of falling water is.
	float3 across = p.velocity - view * dot(p.velocity, view);
	float acrossSpeed = length(across);
	float stretch = acrossSpeed * em.Render.y;
	if (stretch > 0.05 * radius) {
		axisY = across / acrossSpeed;
		axisX = cross(view, axisY);
		extent.y += 0.5 * stretch;
	} else {
		float angle = p.rotation + p.rotationSpeed * p.age;
		float s, c;
		sincos(angle, s, c);
		axisX = right * c + up * s;
		axisY = up * c - right * s;
	}

	float2 corner = float2(vertexID & 1u ? 1.0 : -1.0, vertexID & 2u ? 1.0 : -1.0);
	float3 positionWS = centre + axisX * corner.x * extent.x + axisY * corner.y * extent.y;
	o.position = mul(FrameBuffer::CameraViewProj, float4(positionWS, 1.0));
	o.positionWS = positionWS;

	// The atlas frame (BSPSysSubTexModifier): played over the life, or one picked at birth.
	uint columns = max((uint)em.Atlas.x, 1u);
	uint rows = max((uint)em.Atlas.y, 1u);
	uint frames = max(min((uint)em.Atlas.z, columns * rows), 1u);
	uint frame = em.Atlas.w > 0.5 ? min((uint)(t * frames), frames - 1u) : min(PBRWaterSpray::AtlasFrame(p), frames - 1u);
	float2 cell = float2(frame % columns, frame / columns);
	o.uv = (cell + float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5)) / float2(columns, rows);

	float viewDepth = mul(FrameBuffer::CameraViewProj, float4(centre, 1.0)).w;
	o.info = float4(alpha, emitterIndex, viewDepth, radius);
	if (alpha <= 0.0 || radius <= 0.0)
		o.position = float4(0.0, 0.0, -1.0, 1.0);  // culled
	return o;
}
#endif

#if defined(PSHADER)
Texture2D<float4> SprayTexture0 : register(t3);
Texture2D<float4> SprayTexture1 : register(t4);
Texture2D<float4> SprayTexture2 : register(t5);
Texture2D<float4> SprayTexture3 : register(t6);
Texture2D<float> SceneDepth : register(t7);
SamplerState SpraySampler : register(s0);

#	define LinearSampler SpraySampler
#	include "Common/ShadowSampling.hlsli"

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

float4 SampleSprayTexture(uint index, float2 uv)
{
	[branch] switch (index)
	{
	case 1:
		return SprayTexture1.Sample(SpraySampler, uv);
	case 2:
		return SprayTexture2.Sample(SpraySampler, uv);
	case 3:
		return SprayTexture3.Sample(SpraySampler, uv);
	default:
		return SprayTexture0.Sample(SpraySampler, uv);
	}
}

/// Henyey-Greenstein phase function over the isotropic one (1 / 4 pi): 1 for g = 0.
float ForwardScattering(float g, float cosTheta)
{
	float g2 = g * g;
	return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

/// The sun at `positionWS` after its shadows (Particle.hlsl GetDirLightColor).
float3 SunColor(float3 positionWS)
{
	float unusedDetailedShadow;
	float3 color = SharedData::DirLightColor.xyz * ShadowSampling::GetLightingShadow(positionWS, unusedDetailedShadow);
	return color * ShadowSampling::GetWorldShadow(positionWS, FrameBuffer::CameraPosAdjust.xyz);
}

float4 PSMain(VS_OUTPUT input) : SV_Target0
{
	PBRWaterSpray::Emitter em = Emitters[(uint)input.info.y];
	float alpha = input.info.x;

	// Soft against the scene, and gone behind it (the depth buffer is not bound: it is read here).
	float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.position.xy, 0)));
	float depth = input.info.z;
	float soft = saturate((sceneDepth - depth) / max(em.Render.x, 1.0));
	// Close to the eye a particle fills the view: faded out over its own size.
	float near = saturate((depth - 0.5 * input.info.w) / max(2.0 * input.info.w, 1.0));
	alpha *= soft * near;
	if (alpha <= 1e-3)
		discard;

	float4 source = SampleSprayTexture((uint)em.Render.z, input.uv);
	float3 baseColor = em.Color.rgb * Color::Albedo(ColorManagement::TextureToWorking(source.rgb));
	alpha *= source.a;
	if (alpha <= 1e-3)
		discard;

	float3 view = normalize(input.positionWS);
	float phase = ForwardScattering(em.Shade.z, dot(view, SharedData::DirLightDirection.xyz));
	float3 ambient = max(0.0, SharedData::GetAmbient(float3(0.0, 0.0, 1.0)));
#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL)
		ambient = ImageBasedLighting::GetDiffuseIBL(ambient, float3(0.0, 0.0, -1.0));
#	endif
	float3 lit = SunColor(input.positionWS) * phase + ambient;
	float3 color = baseColor * lerp(1.0.xxx, lit, saturate(em.Shade.y));

#	if defined(ENABLE_LL)
	// Linear Lighting blends in linear space: the vanilla particles' alpha is gamma-shaped for it (Particle.hlsl).
	alpha = pow(saturate(alpha), TransferFunctions::GAME_GAMMA);
#	endif
	alpha = saturate(alpha);
	return float4(color * alpha, alpha * (1.0 - saturate(em.Shade.w)));
}
#endif
