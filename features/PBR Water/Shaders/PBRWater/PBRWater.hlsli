#ifndef __PBR_WATER_HLSLI__
#define __PBR_WATER_HLSLI__

#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/Math.hlsli"

// ============================================================================
// PBR Water - shared constants, resources and the analytic wave model.
//
// The surface is a sum of Gerstner waves whose parameters are generated on the CPU from a
// JONSWAP spectrum (see PBRWater::WaveSpectrum). Every consumer evaluates the *same* function:
//   - vertex / domain shader : displacement (current + previous frame for motion vectors)
//   - pixel shader           : per-pixel normals, crest foam and filtered roughness
//   - CPU                    : water height queries for swimming and Havok buoyancy
// so geometry, shading, motion vectors and gameplay agree without any GPU readback.
//
// Per-wave phases are wrapped on the CPU in double precision relative to RefCamPos, so the GPU only
// ever works with camera-relative coordinates and the waves never "swim" far from the origin.
//
// References:
//   Tessendorf, "Simulating Ocean Water" (2001)
//   Fréchot, "Realistic simulation of ocean surface using wave spectra" (2006) - JONSWAP / PM spectra
//   GPU Gems ch.1, "Effective Water Simulation from Physical Models" - Gerstner sums
//   Ang, "The Technical Art of Sea of Thieves" (SIGGRAPH 2018 Talks) - foam/scatter breakdown
//   Pleasant & Ross, "Wakes, Explosions and Lighting: Interactive Water Simulation in Atlas" (GDC 2019)
// ============================================================================

#define PBRW_MAX_WAVES 16

namespace PBRWater
{
	cbuffer PBRWaterData : register(b7)
	{
		float4 WaveDirK[PBRW_MAX_WAVES];   // xy direction, z wavenumber k (rad/unit), w angular frequency (rad/s)
		float4 WaveAmp[PBRW_MAX_WAVES];    // x amplitude (units), y steepness Q, z phase offset (now), w phase offset (previous frame)
		float4 WaveExtra[PBRW_MAX_WAVES];  // x PM low-frequency weight at unlimited fetch, y wavelength (units), zw unused
		float4 Params0;                    // x wave count, y wind speed (m/s), z peak omega at unlimited fetch, w gravity (units/s^2)
		float4 Params1;                    // xy wind direction, z displacement fade start, w displacement fade end (units)
		float4 Params2;                    // x choppiness, y whitecap coverage, z amplitude sum (units), w shortest displaced wavelength (units)
		float4 RefCamPos;                  // xyz camera position the phase offsets are relative to, w wave time (s)
		float4 Tess0;                      // x tessellation active for this draw, y target triangle size (px), z max factor, w pixels per unit at distance 1
		float4 Draw0;                      // x vertex spacing of this draw (units), y wireframe mode, z debug view, w unused
		float4 Fetch0;                     // xy grid origin (absolute world units), zw 1 / grid extent (units)
		float4 Fetch1;                     // x enabled, y direction count, z wind slice coordinate, w fetch outside the grid (m)
		float4 Terrain0;                   // xy heightmap uv scale, zw heightmap uv offset
		float4 Terrain1;                   // x heightmap z min, y heightmap z max, z enabled, w heightmap texel size (units)
		float4 Shore0;                     // x amplitude (units), y angular frequency (rad/s), z nominal beach slope, w onset depth (units)
		float4 Shore1;                     // x steepness, y foam strength, z phase (now), w phase (previous frame)
		float4 Ripple0;                    // xy simulation origin (absolute world units), z 1 / simulation extent (units), w enabled
		float4 Ripple1;                    // x displacement scale (units), y normal strength, z unused, w texel size (uv)
		float4 Ripple2;                    // xy previous frame's simulation origin, z step interpolation (now), w step interpolation (previous frame)
		float4 Light0;                     // x base roughness, y subsurface strength, z vanilla fresnel blend, w sun specular intensity
		float4 Light1;                     // x visibility scale, y foam albedo, z refraction distortion scale, w point light specular intensity
		float4 Foam0;                      // x shore foam width (units), y crest foam threshold, z foam amount, w foam pattern scale (units)
		float4 Foam1;                      // x crest foam persistence (s), y foam animation time (s, wrapped), z wake foam strength, w flow wave damping
	}

	Texture2D<float4> RippleTexture : register(t110);     // x height, y height one step earlier, z foam, w unused
	Texture2DArray<float> FetchTexture : register(t111);  // fetch in km, one slice per upwind direction
	Texture2D<float> TerrainHeightTexture : register(t112);
	Texture2D<float4> RipplePreviousTexture : register(t113);  // the ripple state as displayed last frame (motion vectors)
	SamplerState LinearClampSampler : register(s12);

	static const float UnitsPerMetre = METRES_TO_UNITS;
	static const float MetresPerUnit = 1.0 / METRES_TO_UNITS;

	/// Normalises `v`, or returns `fallback` for a zero-length vector (normalize() would return NaN).
	float3 SafeNormalize(float3 v, float3 fallback)
	{
		float lengthSq = dot(v, v);
		return lengthSq > 1e-20 ? v * rsqrt(lengthSq) : fallback;
	}

	uint WaveCount() { return min((uint)Params0.x, PBRW_MAX_WAVES); }
	float Gravity() { return Params0.w; }
	float WaveTime() { return RefCamPos.w; }

	// ------------------------------------------------------------------------
	// Spatial context: everything that makes the spectrum vary over the map.
	// ------------------------------------------------------------------------
	struct WaveContext
	{
		float peakOmega;     // local spectral peak (rad/s); larger than the open-sea peak where fetch is short
		float fetchRatio;    // local significant wave height relative to the open sea (fetch-limited growth)
		float depth;         // water depth below the undisplaced surface (units)
		float damping;       // overall amplitude multiplier (distance fade, flow, ...)
		float spacing;       // local vertex spacing (units) for the Nyquist fade; 0 disables it
		float2 terrainGrad;  // terrain height gradient (units/unit), points uphill i.e. towards the shore
		float hasTerrain;    // 1 when depth and gradient come from the heightmap
	};

	float SampleTerrainZ(float2 absXY)
	{
		float2 uv = absXY * Terrain0.xy + Terrain0.zw;
		float h = TerrainHeightTexture.SampleLevel(LinearClampSampler, uv, 0);
		return lerp(Terrain1.x, Terrain1.y, h);
	}

	bool TerrainInside(float2 absXY)
	{
		float2 uv = absXY * Terrain0.xy + Terrain0.zw;
		return all(uv > 0.0) && all(uv < 1.0);
	}

	/// Fetch (open water distance upwind) in metres, interpolated between the two nearest
	/// precomputed directions. Short fetch means a young, short-wavelength sea.
	float SampleFetchMetres(float2 absXY)
	{
		if (Fetch1.x < 0.5)
			return Fetch1.w;
		float2 uv = (absXY - Fetch0.xy) * Fetch0.zw;
		if (any(uv < 0.0) || any(uv > 1.0))
			return Fetch1.w;
		float slices = Fetch1.y;
		float s = Fetch1.z;
		float s0 = floor(s);
		float t = s - s0;
		float s1 = fmod(s0 + 1.0, slices);
		float f0 = FetchTexture.SampleLevel(LinearClampSampler, float3(uv, s0), 0);
		float f1 = FetchTexture.SampleLevel(LinearClampSampler, float3(uv, s1), 0);
		return lerp(f0, f1, t) * 1000.0;
	}

	/// JONSWAP fetch-limited peak angular frequency (Hasselmann et al. 1973), clamped to the
	/// fully developed Pierson-Moskowitz peak.
	float FetchPeakOmega(float fetchMetres)
	{
		const float g = 9.81;
		float U = max(Params0.y, 0.5);
		float F = max(fetchMetres, 10.0);
		float omega = 22.0 * pow(abs(g * g / (U * F)), 1.0 / 3.0);
		return max(omega, Params0.z);
	}

	/// JONSWAP fetch-limited growth: Hs(F) = 0.0016 sqrt(gF) U / g relative to Hs(open sea) = 0.21 U^2 / g.
	float FetchHeightRatio(float fetchMetres)
	{
		return saturate(0.0016 / 0.21 * sqrt(9.81 * max(fetchMetres, 0.0)) / max(Params0.y, 0.5));
	}

	WaveContext BuildWaveContext(float3 positionWS, float damping, float spacing)
	{
		WaveContext ctx;
		float3 absPos = positionWS + FrameBuffer::CameraPosAdjust.xyz;
		float fetchMetres = SampleFetchMetres(absPos.xy);
		ctx.peakOmega = FetchPeakOmega(fetchMetres);
		ctx.fetchRatio = FetchHeightRatio(fetchMetres);
		ctx.depth = 1e6;
		ctx.terrainGrad = 0.0;
		ctx.hasTerrain = 0.0;
		ctx.damping = damping;
		ctx.spacing = spacing;

		if (Terrain1.z > 0.5 && TerrainInside(absPos.xy)) {
			float texel = Terrain1.w;
			float z0 = SampleTerrainZ(absPos.xy);
			float zx = SampleTerrainZ(absPos.xy + float2(texel, 0));
			float zy = SampleTerrainZ(absPos.xy + float2(0, texel));
			// The undisplaced surface point is the water plane: its own z is the water height.
			ctx.depth = max(absPos.z - z0, 0.0);
			ctx.terrainGrad = float2(zx - z0, zy - z0) / texel;
			ctx.hasTerrain = 1.0;
		}
		return ctx;
	}

	/// Distance fade of the displacement, so far water converges to the flat vanilla plane
	/// (and to the untessellated LOD meshes) instead of popping.
	float DisplacementDistanceFade(float distance)
	{
		return 1.0 - smoothstep(Params1.z, max(Params1.w, Params1.z + 1.0), distance);
	}

	/// tanh for x >= 0 that cannot overflow. FXC lowers the intrinsic to (e^2x - 1) / (e^2x + 1),
	/// which is inf / inf = NaN once kh exceeds ~44, i.e. for any deep water.
	float SafeTanh(float x)
	{
		return 1.0 - 2.0 / (exp(2.0 * min(max(x, 0.0), 20.0)) + 1.0);
	}

	/// Amplitude multiplier for one spectral component at this location.
	float WaveWeight(uint i, WaveContext ctx)
	{
		float omega = WaveDirK[i].w;
		float k = WaveDirK[i].z;
		float w = ctx.damping;

		// Fetch: ratio of the local Pierson-Moskowitz low-frequency cut-off to the open-sea one.
		// Long swells cannot exist on a pond, short chop is unaffected.
		float r = ctx.peakOmega / omega;
		float rLocal = exp(-1.25 * r * r * r * r);
		w *= saturate(rLocal / max(WaveExtra[i].x, 1e-4));

		// Depth: orbital motion is limited by the bottom (tanh(kh) from linear wave theory).
		w *= SafeTanh(k * ctx.depth);

		// Nyquist: never displace geometry with waves the vertex grid cannot represent.
		if (ctx.spacing > 0.0)
			w *= saturate(WaveExtra[i].y / max(ctx.spacing, 1e-4) * 0.5 - 1.0);

		return w;
	}

	struct WaveResult
	{
		float3 displacement;  // world-space offset (units)
		float3 normal;        // surface normal
		float jacobian;       // < 1 where the surface compresses, < 0 where it folds (breaking)
		float slopeVariance;  // filtered-out slope variance (for roughness)
		float shoreBreak;     // 0..1 breaking intensity of the shore waves
		float shoreMask;      // 0..1 presence of the shore waves
		float shoreCrest;     // 0..1 how close this point is to a shore wave crest (swash foam)
	};

	/// Nominal shoaling travel time from depth h to the shoreline on a beach of slope s:
	/// integral of dx / sqrt(g h) = 2 sqrt(h) / (s sqrt(g)). Its gradient gives wavefronts that
	/// follow the depth contours and shorten towards the beach, i.e. refraction and shoaling.
	float ShoreTravelTime(float depthUnits)
	{
		float hm = max(depthUnits * MetresPerUnit, 0.02);
		return 2.0 * sqrt(hm) / (max(Shore0.z, 0.005) * sqrt(9.81));
	}

	/**
	 * Evaluates the wave field at undisplaced camera-relative position `positionWS`.
	 * @param previous   evaluate at the previous frame's time (motion vectors)
	 * @param filterSize world-space footprint of a pixel (0 for geometry); components smaller than
	 *                   the footprint are faded out and their slope variance is returned instead
	 */
	WaveResult EvaluateWaves(float3 positionWS, WaveContext ctx, bool previous, float filterSize)
	{
		WaveResult o;
		o.displacement = 0.0;
		o.slopeVariance = 0.0;
		o.shoreBreak = 0.0;
		o.shoreMask = 0.0;
		o.shoreCrest = 0.0;

		// Camera-relative position re-based on the phase reference camera. For the main camera this
		// offset is zero; other cameras (local map, cubemaps) still get correct, if less precise, waves.
		float2 p = positionWS.xy + (FrameBuffer::CameraPosAdjust.xy - RefCamPos.xy);

		float dxdx = 0.0, dydy = 0.0, dxdy = 0.0;
		float dzdx = 0.0, dzdy = 0.0;

		uint count = WaveCount();
		[loop] for (uint i = 0; i < count; i++)
		{
			float4 dk = WaveDirK[i];
			float4 amp = WaveAmp[i];
			float weight = WaveWeight(i, ctx);

			if (filterSize > 0.0) {
				// Fade components whose wavelength is below ~2 pixels; their slopes become roughness.
				float fade = saturate(WaveExtra[i].y / max(filterSize, 1e-4) * 0.5 - 1.0);
				float slope = dk.z * amp.x * weight;
				o.slopeVariance += 0.5 * slope * slope * (1.0 - fade * fade);
				weight *= fade;
			}

			float A = amp.x * weight;
			if (A <= 0.0)
				continue;

			float theta = dk.z * dot(dk.xy, p) + (previous ? amp.w : amp.z);
			float s, c;
			sincos(theta, s, c);

			float QA = amp.y * A;
			o.displacement.xy += dk.xy * (QA * c);
			o.displacement.z += A * s;

			float WA = dk.z * A;
			float QWA = amp.y * WA;
			dzdx += dk.x * WA * c;
			dzdy += dk.y * WA * c;
			dxdx -= dk.x * dk.x * QWA * s;
			dydy -= dk.y * dk.y * QWA * s;
			dxdy -= dk.x * dk.y * QWA * s;
		}

		// Shoreline waves: a separate train that travels up the depth gradient and breaks.
		if (ctx.hasTerrain > 0.5 && Shore0.x > 0.0 && ctx.depth < Shore0.w) {
			float gradLen = length(ctx.terrainGrad);
			if (gradLen > 1e-4) {
				float2 dir = ctx.terrainGrad / gradLen;
				float h = max(ctx.depth, 1.0);
				float onset = Shore0.w;

				// Wave height grows with Green's law (h^-1/4) and is capped by depth-limited breaking (H <= 0.78 h).
				float A0 = Shore0.x * ctx.fetchRatio * ctx.damping;
				float A = A0 * sqrt(sqrt(onset / h));
				float Amax = 0.39 * h;
				o.shoreBreak = saturate(A / Amax - 1.0);
				A = min(A, Amax);

				// Fade in as the bottom is felt, fade out in the last few centimetres of the swash zone.
				float envelope = smoothstep(onset, onset * 0.5, h) * smoothstep(0.05 * UnitsPerMetre, 0.25 * UnitsPerMetre, h);
				o.shoreMask = envelope;
				A *= envelope;

				float omega = Shore0.y;
				float theta = -omega * ShoreTravelTime(h) - (previous ? Shore1.w : Shore1.z);
				float s, c;
				sincos(theta, s, c);
				o.shoreCrest = envelope * saturate(s);

				// d(theta)/dx: theta' (h) * dh/dx, with dh/dx = -terrainGrad
				float hm = max(h * MetresPerUnit, 0.02);
				float dTdh = (1.0 / (max(Shore0.z, 0.005) * sqrt(9.81 * hm))) * MetresPerUnit;
				float2 dTheta = omega * dTdh * ctx.terrainGrad;
				float kLocal = length(dTheta);

				// Breaking waves pitch forward; keep Q*k*A < 1 so the surface never loops.
				float Q = saturate(Shore1.x + o.shoreBreak * 0.5);
				float QA = min(Q * A, 0.9 / max(kLocal, 1e-5));

				o.displacement.xy += dir * (QA * c);
				o.displacement.z += A * s;
				dzdx += dTheta.x * A * c;
				dzdy += dTheta.y * A * c;
				float QK = QA * kLocal;
				dxdx -= dir.x * dir.x * QK * s;
				dydy -= dir.y * dir.y * QK * s;
				dxdy -= dir.x * dir.y * QK * s;
			}
		}

		// Normal of the parametric surface P(x, y) = (x + Dx, y + Dy, Dz)
		float3 tx = float3(1.0 + dxdx, dxdy, dzdx);
		float3 ty = float3(dxdy, 1.0 + dydy, dzdy);
		// tx and ty become parallel where the surface folds over itself; keep the normal defined there.
		o.normal = SafeNormalize(cross(tx, ty), float3(0, 0, 1));
		o.jacobian = (1.0 + dxdx) * (1.0 + dydy) - dxdy * dxdy;
		return o;
	}

	/**
	 * Jacobian of the open-sea waves `pastSeconds` ago at undisplaced position `positionWS`. Because the
	 * position is Lagrangian, this is where the water at this point was folding a moment earlier:
	 * sampling a few past instants lets crest foam linger and thin out behind the crest instead of popping.
	 */
	float WaveJacobian(float3 positionWS, WaveContext ctx, float pastSeconds, float filterSize)
	{
		float2 p = positionWS.xy + (FrameBuffer::CameraPosAdjust.xy - RefCamPos.xy);
		float dxdx = 0.0, dydy = 0.0, dxdy = 0.0;
		uint count = WaveCount();
		[loop] for (uint i = 0; i < count; i++)
		{
			float4 dk = WaveDirK[i];
			float4 amp = WaveAmp[i];
			float weight = WaveWeight(i, ctx) * saturate(WaveExtra[i].y / max(filterSize, 1e-4) * 0.5 - 1.0);
			float QWA = amp.y * dk.z * amp.x * weight;
			if (QWA <= 0.0)
				continue;
			// Going back in time advances the phase by omega * dt.
			float s = sin(dk.z * dot(dk.xy, p) + amp.z + dk.w * pastSeconds);
			dxdx -= dk.x * dk.x * QWA * s;
			dydy -= dk.y * dk.y * QWA * s;
			dxdy -= dk.x * dk.y * QWA * s;
		}
		return (1.0 + dxdx) * (1.0 + dydy) - dxdy * dxdy;
	}

	// ------------------------------------------------------------------------
	// Interactive ripples (GPU wave-equation simulation centred on the camera)
	// ------------------------------------------------------------------------
	float RippleEdgeFade(float2 uv)
	{
		float2 e = saturate(min(uv, 1.0 - uv) * 10.0);
		return e.x * e.y;
	}

	/// Displayed ripple height. The simulation runs at a fixed rate, so the two stored steps are
	/// interpolated to the render time; `previous` samples last frame's copy for motion vectors.
	float RippleHeight(float3 positionWS, bool previous)
	{
		if (Ripple0.w < 0.5)
			return 0.0;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		float2 uv = (absXY - (previous ? Ripple2.xy : Ripple0.xy)) * Ripple0.z;
		if (any(uv <= 0.0) || any(uv >= 1.0))
			return 0.0;
		float2 steps = previous ? RipplePreviousTexture.SampleLevel(LinearClampSampler, uv, 0).xy :
		                          RippleTexture.SampleLevel(LinearClampSampler, uv, 0).xy;
		return lerp(steps.y, steps.x, previous ? Ripple2.w : Ripple2.z) * Ripple1.x * RippleEdgeFade(uv);
	}

	float RippleDisplayed(float2 uv)
	{
		float2 steps = RippleTexture.SampleLevel(LinearClampSampler, uv, 0).xy;
		return lerp(steps.y, steps.x, Ripple2.z);
	}

	/// Ripples cannot be taller than the water is deep; keeps them from dipping below the shore.
	float RippleDepthFade(float depth)
	{
		return saturate(depth / max(3.0 * Ripple1.x, 1.0));
	}

	/// xy = height gradient (units/unit), z = foam
	float3 RippleSlopeFoam(float3 positionWS, float depth)
	{
		if (Ripple0.w < 0.5)
			return 0.0;
		float2 uv = (positionWS.xy + FrameBuffer::CameraPosAdjust.xy - Ripple0.xy) * Ripple0.z;
		if (any(uv <= 0.0) || any(uv >= 1.0))
			return 0.0;
		float t = Ripple1.w;
		float hL = RippleDisplayed(uv - float2(t, 0));
		float hR = RippleDisplayed(uv + float2(t, 0));
		float hD = RippleDisplayed(uv - float2(0, t));
		float hU = RippleDisplayed(uv + float2(0, t));
		// Show foam on the crests only: it thins away as the ripple passes and the surface drops.
		float crest = saturate(RippleDisplayed(uv) * 6.0 + 0.15);
		float foam = RippleTexture.SampleLevel(LinearClampSampler, uv, 0).z * crest;
		float texelUnits = t / Ripple0.z;
		float2 grad = float2(hR - hL, hU - hD) / (2.0 * texelUnits) * Ripple1.x * Ripple1.y * RippleDepthFade(depth);
		return float3(grad, foam) * RippleEdgeFade(uv);
	}

	// ------------------------------------------------------------------------
	// Full surface displacement used by geometry (VS / DS)
	// ------------------------------------------------------------------------
	struct SurfaceDisplacement
	{
		float3 current;
		float3 previous;
		float3 waveNormal;
		float jacobian;
		float shoreBreak;
		float depth;
	};

	SurfaceDisplacement DisplaceSurface(float3 positionWS, float flowDamping, float spacing)
	{
		SurfaceDisplacement o;
		float distance = length(positionWS);
		float damping = DisplacementDistanceFade(distance) * flowDamping;

		WaveContext ctx = BuildWaveContext(positionWS, damping, spacing);
		WaveResult now = EvaluateWaves(positionWS, ctx, false, 0.0);
		WaveResult prev = EvaluateWaves(positionWS, ctx, true, 0.0);

		float rippleFade = damping * RippleDepthFade(ctx.depth);
		o.current = now.displacement + float3(0, 0, RippleHeight(positionWS, false) * rippleFade);
		o.previous = prev.displacement + float3(0, 0, RippleHeight(positionWS, true) * rippleFade);

		// Hard bound: waves stay within their amplitude sum (doubled for shoaling) and the ripple sim stays within a few
		// times its scale. Clamping also flushes NaN (D3D min/max return the non-NaN operand), so a
		// bad input can never throw the mesh off-screen.
		float limit = 2.0 * max(Params2.z, 0.0) + 4.0 * max(Ripple1.x, 0.0) + 1.0;
		o.current = clamp(o.current, -limit, limit);
		o.previous = clamp(o.previous, -limit, limit);
		o.waveNormal = now.normal;
		o.jacobian = now.jacobian;
		o.shoreBreak = now.shoreBreak;
		o.depth = ctx.depth;
		return o;
	}
}

#endif  // __PBR_WATER_HLSLI__
