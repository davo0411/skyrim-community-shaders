// ============================================================================
// PBR Water - sea spray, pass 3: the living particles, drawn after the water. Procedural: no textures.
//
// Spray is two things to the eye (Wretborn et al. and the film pipelines split whitewater the same way: spray, mist,
// foam, bubbles):
//   droplets  water thrown clear, millimetres to centimetres across. Far below a pixel at any distance, they show as the
//             streak a droplet draws over the shutter: a capsule along its velocity on screen, at least a pixel wide,
//             its coverage scaled down with its width below that (a thin line's energy is kept instead of its size).
//   puffs     the sheet breaking into ligaments and the cloud of fine spray and mist around it: a soft blob whose
//             density is a Gaussian broken up by value noise, the noise eating into it as it ages, so a puff tears
//             apart and thins instead of fading as a disc. Each puff has its own noise (its random bits) that drifts
//             as it lives.
// Both are lit as the vanilla particles are (Particle.hlsl): the sun after its shadows and the ambient light, with a
// lighting influence mixing lit and unlit. Water droplets are Mie scatterers: the sun's share is weighted by a
// Henyey-Greenstein phase function (normalised to the isotropic one), so spray against the sun glows; a puff also takes
// wrapped diffuse light on a normal bulging towards the eye, so its sunlit side reads as a volume. Soft against the
// scene's depth (read here: no depth buffer is bound), premultiplied, so an emitter sits anywhere between alpha blended
// and additive.
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
	float3 positionWS: TEXCOORD0;             // camera-relative
	float3 normalWS: TEXCOORD1;               // a puff's bulging normal
	float2 uv: TEXCOORD2;                     // -1..1 across the quad (droplets: x across, y along the streak)
	nointerpolation float4 info: TEXCOORD3;   // x alpha, y emitter, z view depth of the centre (units), w radius (units)
	nointerpolation float4 shape: TEXCOORD4;  // droplets: x streak length over width; puffs: x life (0..1), yz noise offset
};

#if defined(VSHADER)
VS_OUTPUT VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	VS_OUTPUT o = (VS_OUTPUT)0;
	PBRWaterSpray::Particle p = Particles[Alive[instanceID]];
	uint emitterIndex = PBRWaterSpray::EmitterIndex(p);
	PBRWaterSpray::Emitter em = Emitters[emitterIndex];
	bool droplet = em.Shape.x < 0.5;

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

	// The size of a pixel at the particle (units): the render height and the projection's vertical scale.
	float viewDepth = mul(FrameBuffer::CameraViewProj, float4(centre, 1.0)).w;
	float screenHeight = SharedData::BufferDim.y * FrameBuffer::DynamicResolutionParams1.y;
	float pixel = 2.0 * max(viewDepth, 1.0) / max(abs(FrameBuffer::CameraProj[1][1]) * screenHeight, 1.0);

	float2 extent = radius.xx;
	float3 axisX = right, axisY = up;
	uint bits = PBRWaterSpray::ParticleBits(p);
	if (droplet) {
		// The streak the droplet draws over the shutter, along its velocity across the view.
		float3 across = p.velocity - view * dot(p.velocity, view);
		float acrossSpeed = length(across);
		float halfWidth = max(radius, 0.75 * pixel);
		float halfLength = halfWidth + 0.5 * acrossSpeed * em.Render.y;
		if (acrossSpeed > 1e-3) {
			axisY = across / acrossSpeed;
			axisX = cross(view, axisY);
		}
		extent = float2(halfWidth, halfLength);
		// Thinner than a pixel: the coverage it has. Drawn out into a streak: its light spread along it (square root:
		// a streak stays visible where a single droplet would be).
		alpha *= saturate(radius / (0.75 * pixel)) * sqrt(saturate(halfWidth / halfLength));
		o.shape = float4(halfLength / halfWidth, 0.0, 0.0, 0.0);
	} else {
		// A puff smaller than a few pixels is only its colour: kept at two pixels, its density spread over them.
		float shown = max(radius, 2.0 * pixel);
		alpha *= saturate(radius / shown) * saturate(radius / shown);
		extent = shown.xx;
		// Drawn out along its motion across the view (the rising streaks of a splash), or turning as it drifts.
		float3 across = p.velocity - view * dot(p.velocity, view);
		float acrossSpeed = length(across);
		float stretch = 0.5 * acrossSpeed * em.Render.y;
		if (stretch > 0.05 * shown) {
			axisY = across / acrossSpeed;
			axisX = cross(view, axisY);
			extent.y += stretch;
			alpha *= sqrt(saturate(extent.x / extent.y));
		} else {
			float angle = p.rotation + p.rotationSpeed * p.age;
			float s, c;
			sincos(angle, s, c);
			axisX = right * c + up * s;
			axisY = up * c - right * s;
		}
		o.shape = float4(t, float(bits & 0x3FFu) * 0.731, float((bits >> 10) & 0x3FFu) * 0.517, 0.0);
	}

	float2 corner = float2(vertexID & 1u ? 1.0 : -1.0, vertexID & 2u ? 1.0 : -1.0);
	float3 positionWS = centre + axisX * corner.x * extent.x + axisY * corner.y * extent.y;
	o.position = mul(FrameBuffer::CameraViewProj, float4(positionWS, 1.0));
	o.positionWS = positionWS;
	o.normalWS = normalize(axisX * corner.x + axisY * corner.y - view * 0.8);
	o.uv = corner;
	o.info = float4(alpha, emitterIndex, viewDepth, radius);
	if (alpha <= 1e-4 || radius <= 0.0)
		o.position = float4(0.0, 0.0, -1.0, 1.0);  // culled
	return o;
}
#endif

#if defined(PSHADER)
Texture2D<float> SceneDepth : register(t3);
SamplerState SpraySampler : register(s0);

#	define LinearSampler SpraySampler
#	include "Common/ShadowSampling.hlsli"

#	if defined(IBL)
#		include "IBL/IBL.hlsli"
#	endif

float Hash2(float2 p)
{
	p = frac(p * float2(0.1031, 0.1030));
	p += dot(p, p.yx + 33.33);
	return frac((p.x + p.y) * p.x);
}

float ValueNoise(float2 p)
{
	float2 i = floor(p);
	float2 f = p - i;
	float2 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(Hash2(i), Hash2(i + float2(1.0, 0.0)), u.x), lerp(Hash2(i + float2(0.0, 1.0)), Hash2(i + 1.0), u.x), u.y);
}

/// Three octaves of value noise, 0..1.
float Fbm(float2 p)
{
	return 0.5333 * ValueNoise(p) + 0.2667 * ValueNoise(p * 2.03 + 17.1) + 0.2 * ValueNoise(p * 4.07 + 41.7);
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
	bool droplet = em.Shape.x < 0.5;

	// Soft against the scene, and gone behind it.
	float sceneDepth = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.position.xy, 0)));
	float depth = input.info.z;
	float soft = saturate((sceneDepth - depth) / max(em.Render.x, 1.0));
	// Close to the eye a particle fills the view: faded out over its own size.
	float near = saturate((depth - 0.5 * input.info.w) / max(2.0 * input.info.w, 1.0));
	alpha *= soft * near;
	if (alpha <= 1e-4)
		discard;

	float density;
	float sunWrap = 1.0;
	float skyShade = 1.0;
	[branch] if (droplet)
	{
		// A capsule along y: its round ends a width across, antialiased over the pixels it spans.
		float aspect = max(input.shape.x, 1.0);
		float2 q = float2(input.uv.x, max(abs(input.uv.y) * aspect - (aspect - 1.0), 0.0));
		float d = length(q);
		density = saturate((1.0 - d) / max(fwidth(d), 1e-3));
	}
	else
	{
		// A blob with a ragged edge: its outline pushed in and out by noise (domain warping), its inside broken up by the
		// same noise, which eats into it as it ages, so it tears apart and thins instead of fading as a disc.
		float life = input.shape.x;
		float detail = saturate(em.Shape.z);
		float2 q = input.uv * (2.0 + 3.0 * detail) + input.shape.yz + life * float2(0.6, -0.9);
		float n1 = Fbm(q);
		float n2 = Fbm(q * 2.13 + 7.31);
		// The outline frays at a finer scale than the body breaks up: wisps, not lumps.
		float n3 = Fbm(q * 3.7 + 3.1);
		float2 warped = input.uv + (float2(n1, n3) - 0.5) * (1.2 * detail);
		float r = length(warped);
		float body = (1.0 - smoothstep(0.25, 1.0, r)) * lerp(1.0, saturate(1.8 * n2 - 0.1), 0.6 * detail);
		float erosion = detail * (0.08 + 0.6 * life * life);
		float softness = max(em.Shape.y, 0.02);
		density = saturate((body - erosion) / softness);
		density *= density * (3.0 - 2.0 * density);
		float3 normal = normalize(input.normalWS);
		sunWrap = saturate((dot(normal, SharedData::DirLightDirection.xyz) + 0.5) / 1.5);
		// The sky lights a puff from above: its underside, facing the water, is darker.
		skyShade = 0.75 + 0.25 * saturate(0.5 + 0.5 * normal.z);
	}
	alpha *= density;
	if (alpha <= 1e-4)
		discard;

	float3 view = normalize(input.positionWS);
	float phase = ForwardScattering(em.Shade.z, dot(view, SharedData::DirLightDirection.xyz));
	float3 ambient = max(0.0, SharedData::GetAmbient(float3(0.0, 0.0, 1.0)));
#	if defined(IBL)
	if (SharedData::iblSettings.EnableIBL)
		ambient = ImageBasedLighting::GetDiffuseIBL(ambient, float3(0.0, 0.0, -1.0));
#	endif
	// Droplets take the sun through their phase alone; a puff half through it and half as wrapped diffuse light.
	float sunShare = droplet ? phase : 0.5 * phase + sunWrap;
	float3 lit = SunColor(input.positionWS) * sunShare + ambient * skyShade;
	float3 color = em.Color.rgb * lerp(1.0.xxx, lit, saturate(em.Shade.y));

#	if defined(ENABLE_LL)
	// Linear Lighting blends in linear space: the vanilla particles' alpha is gamma-shaped for it (Particle.hlsl).
	alpha = pow(saturate(alpha), TransferFunctions::GAME_GAMMA);
#	endif
	alpha = saturate(alpha);
	return float4(color * alpha, alpha * (1.0 - saturate(em.Shade.w)));
}
#endif
