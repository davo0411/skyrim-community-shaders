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
		float4 WaveExtra[PBRW_MAX_WAVES];  // x PM low-frequency weight at unlimited fetch, y wavelength (units), z 1 / omega, w 1 / x
		float4 WavePast[PBRW_MAX_WAVES];   // xy cos/sin(omega * older foam delay), zw cos/sin(omega * oldest foam delay)
		float4 Params0;                    // x wave count, y wind speed (m/s), z peak omega at unlimited fetch, w gravity (units/s^2)
		float4 Params1;                    // xy wind direction, z displacement fade start, w displacement fade end (units)
		float4 Params2;                    // x choppiness, y whitecap coverage, z amplitude sum (units), w shortest displaced wavelength (units)
		float4 RefCamPos;                  // xyz camera position the phase offsets are relative to, w unused
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
		float4 Clarity0;                   // x sediment extinction (1/unit per unit turbidity), y base turbidity, z patchiness (0..1), w patch size (units)
		float4 Clarity1;                   // xyz sediment colour (gamma, like the water form colours), w shore resuspension strength
		float4 Clarity2;                   // x river turbidity, y weather turbidity (storm and rain, already scaled), z wading silt strength, w sediment layer height (units)
		float4 Optics0;                    // x scattering anisotropy g, y downwelling attenuation scale, z clarity animation time (s, wrapped), w significant wave height (units)
		float4 Surface0;                   // x wind roughness scale, y gust strength, z gust size (units), w rain intensity (0..1)
		float4 Surface1;                   // x wind streak strength, yzw unused
		float4 Land0;                      // xy loaded terrain grid corner (absolute units), zw 1 / grid extent (units)
		float4 Land1;                      // x enabled, y vertex spacing (units)
		float4 Foam2;                      // x whitecap amount, y whitecap pattern scale (units), z whitecap streak stretch, w bubble amount
		float4 Foam3;                      // xy foam drift offset (units, wrapped), zw extra bubble drift (units, wrapped)
		float4 Flow0;                      // worldspace flowmap UV = absXY * xz + yw (x = 0: no flowmap)
	}

	Texture2D<float4> RippleTexture : register(t110);     // x height, y height one step earlier, z foam, w silt
	Texture2DArray<float> FetchTexture : register(t111);  // fetch in km, one slice per upwind direction
	Texture2D<float> TerrainHeightTexture : register(t112);
	Texture2D<float4> RipplePreviousTexture : register(t113);  // the ripple state as displayed last frame (motion vectors)
	Texture2D<float> LoadedTerrainTexture : register(t114);    // exact terrain z of the loaded cells (LAND vertex heights)
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

	// ------------------------------------------------------------------------
	// Spatial context: everything that makes the spectrum vary over the map.
	// ------------------------------------------------------------------------
	struct WaveContext
	{
		float fetchMetres;     // open water upwind (m)
		float peakOmega;       // local spectral peak (rad/s); larger than the open-sea peak where fetch is short
		float energyGain;      // amplitude gain of the fetch-limited spectrum (FetchEnergyGain)
		float fetchRatio;      // local significant wave height relative to the open sea (fetch-limited growth)
		float depth;           // water depth below the undisplaced surface (units)
		float damping;         // overall amplitude multiplier (distance fade, flow, ...)
		float spacing;         // local vertex spacing (units) for the Nyquist fade; 0 disables it
		float halfInvSpacing;  // 0.5 / spacing
		float2 terrainGrad;    // terrain height gradient (units/unit), points uphill i.e. towards the shore
		float hasTerrain;      // 1 when depth and gradient come from the heightmap
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

	/// Amplitude gain of a young, fetch-limited sea: its Phillips constant is larger than the open sea's
	/// (JONSWAP alpha = 0.076 (gF/U^2)^-0.22 against Pierson-Moskowitz 0.0081), so the short waves that
	/// survive the fetch carry more energy. Amplitude scales with sqrt(alpha).
	float FetchEnergyGain(float fetchMetres)
	{
		float U = max(Params0.y, 0.5);
		float chi = 9.81 * max(fetchMetres, 10.0) / (U * U);
		float alpha = 0.076 * pow(chi, -0.22);
		return sqrt(clamp(alpha / 0.0081, 1.0, 4.0));
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
		ctx.fetchMetres = SampleFetchMetres(absPos.xy);
		ctx.peakOmega = FetchPeakOmega(ctx.fetchMetres);
		ctx.energyGain = FetchEnergyGain(ctx.fetchMetres);
		ctx.fetchRatio = FetchHeightRatio(ctx.fetchMetres);
		ctx.depth = 1e6;
		ctx.terrainGrad = 0.0;
		ctx.hasTerrain = 0.0;
		ctx.damping = damping;
		ctx.spacing = spacing;
		ctx.halfInvSpacing = 0.5 / max(spacing, 1e-4);

		// The undisplaced surface point is the water plane: its own z is the water height.
		float2 landUV = (absPos.xy - Land0.xy) * Land0.zw;
		if (Land1.x > 0.5 && all(landUV > 0.0) && all(landUV < 1.0)) {
			// Exact terrain of the loaded cells: resolves beaches, rocks and shelves.
			float2 t = Land1.y * Land0.zw;
			float z0 = LoadedTerrainTexture.SampleLevel(LinearClampSampler, landUV, 0);
			float zx = LoadedTerrainTexture.SampleLevel(LinearClampSampler, landUV + float2(t.x, 0), 0) -
			           LoadedTerrainTexture.SampleLevel(LinearClampSampler, landUV - float2(t.x, 0), 0);
			float zy = LoadedTerrainTexture.SampleLevel(LinearClampSampler, landUV + float2(0, t.y), 0) -
			           LoadedTerrainTexture.SampleLevel(LinearClampSampler, landUV - float2(0, t.y), 0);
			ctx.depth = max(absPos.z - z0, 0.0);
			ctx.terrainGrad = float2(zx, zy) / (2.0 * Land1.y);
			ctx.hasTerrain = 1.0;
		} else if (Terrain1.z > 0.5 && TerrainInside(absPos.xy)) {
			float texel = Terrain1.w;
			float z0 = SampleTerrainZ(absPos.xy);
			float zx = SampleTerrainZ(absPos.xy + float2(texel, 0));
			float zy = SampleTerrainZ(absPos.xy + float2(0, texel));
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
		float4 extra = WaveExtra[i];
		float w = ctx.damping;

		// Fetch: ratio of the local Pierson-Moskowitz low-frequency cut-off to the open-sea one.
		// Long swells cannot exist on a pond, short chop is unaffected.
		float r = ctx.peakOmega * extra.z;
		float r2 = r * r;
		w *= saturate(exp(-1.25 * r2 * r2) * extra.w) * ctx.energyGain;

		// Depth: orbital motion is limited by the bottom (tanh(kh) from linear wave theory).
		w *= SafeTanh(WaveDirK[i].z * ctx.depth);

		// Nyquist: never displace geometry with waves the vertex grid cannot represent.
		if (ctx.spacing > 0.0)
			w *= saturate(extra.y * ctx.halfInvSpacing - 1.0);

		return w;
	}

	struct WaveResult
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same at the previous frame's phases (motion vectors)
		float2 pastJacobian;          // jacobian of the open-sea waves at the two foam trail delays (WavePast)
		float3 normal;                // surface normal
		float jacobian;               // < 1 where the surface compresses, < 0 where it folds (breaking)
		float slopeVariance;          // filtered-out slope variance (for roughness)
		float shoreBreak;             // 0..1 breaking intensity of the shore waves
		float shoreMask;              // 0..1 presence of the shore waves
		float shoreCrest;             // 0..1 how close this point is to a shore wave crest (swash foam)
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
	 * Evaluates the wave field at undisplaced camera-relative position `positionWS`: now, at the previous
	 * frame's phases (motion vectors) and at the foam trail delays, sharing the per-wave weights. Outputs a
	 * caller does not use are compiled out.
	 * @param filterSize world-space footprint of a pixel (0 for geometry); components smaller than
	 *                   the footprint are faded out and their slope variance is returned instead
	 */
	WaveResult EvaluateWaves(float3 positionWS, WaveContext ctx, float filterSize)
	{
		WaveResult o;
		o.displacement = 0.0;
		o.previousDisplacement = 0.0;
		o.slopeVariance = 0.0;
		o.shoreBreak = 0.0;
		o.shoreMask = 0.0;
		o.shoreCrest = 0.0;

		// Camera-relative position re-based on the phase reference camera. For the main camera this
		// offset is zero; other cameras (local map, cubemaps) still get correct, if less precise, waves.
		float2 p = positionWS.xy + (FrameBuffer::CameraPosAdjust.xy - RefCamPos.xy);

		float dxdx = 0.0, dydy = 0.0, dxdy = 0.0;
		float dzdx = 0.0, dzdy = 0.0;
		float steepness = 0.0;                            // sum(Q k A): the most the open-sea waves can compress the surface
		float2 pastXX = 0.0, pastYY = 0.0, pastXY = 0.0;  // x older, y oldest foam trail delay

		// The steepness budget keeps sum(Q k A) < 1 (no loops) for the open-sea amplitudes; the fetch
		// energy gain only raises the surface, so it is taken back out of the horizontal motion.
		float steepnessScale = rcp(ctx.energyGain);
		float halfInvFilter = 0.5 / max(filterSize, 1e-4);

		uint count = WaveCount();
		[loop] for (uint i = 0; i < count; i++)
		{
			float4 dk = WaveDirK[i];
			float4 amp = WaveAmp[i];
			float weight = WaveWeight(i, ctx);

			if (filterSize > 0.0) {
				// Fade components whose wavelength is below ~2 pixels; their slopes become roughness.
				float fade = saturate(WaveExtra[i].y * halfInvFilter - 1.0);
				float slope = dk.z * amp.x * weight;
				o.slopeVariance += 0.5 * slope * slope * (1.0 - fade * fade);
				weight *= fade;
			}

			float A = amp.x * weight;
			if (A <= 0.0)
				continue;

			float spatial = dk.z * dot(dk.xy, p);
			float s, c;
			sincos(spatial + amp.z, s, c);
			float sp, cp;
			sincos(spatial + amp.w, sp, cp);

			float Q = amp.y * steepnessScale;
			float QA = Q * A;
			o.displacement.xy += dk.xy * (QA * c);
			o.displacement.z += A * s;
			o.previousDisplacement.xy += dk.xy * (QA * cp);
			o.previousDisplacement.z += A * sp;

			float WA = dk.z * A;
			float QWA = Q * WA;
			steepness += QWA;
			float3 dd = float3(dk.x * dk.x, dk.y * dk.y, dk.x * dk.y) * QWA;
			dzdx += dk.x * WA * c;
			dzdy += dk.y * WA * c;
			dxdx -= dd.x * s;
			dydy -= dd.y * s;
			dxdy -= dd.z * s;

			// Going back in time advances the phase: sin(theta + omega dt) from precomputed cos/sin(omega dt).
			float4 past = WavePast[i];
			float2 sPast = s * past.xz + c * past.yw;
			pastXX -= dd.x * sPast;
			pastYY -= dd.y * sPast;
			pastXY -= dd.z * sPast;
		}
		// Where the water at this (Lagrangian) point was folding a moment ago: lets crest foam linger.
		o.pastJacobian = (1.0 + pastXX) * (1.0 + pastYY) - pastXY * pastXY;

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

				// d(theta)/dx: theta' (h) * dh/dx, with dh/dx = -terrainGrad
				float omega = Shore0.y;
				float hm = max(h * MetresPerUnit, 0.02);
				float dTdh = (1.0 / (max(Shore0.z, 0.005) * sqrt(9.81 * hm))) * MetresPerUnit;
				float2 dTheta = omega * dTdh * ctx.terrainGrad;
				float kLocal = length(dTheta);
				// The wavelength collapses as 1 / sqrt(h) in the swash, and much faster on a steep (cliff)
				// shore: hold the slope k A to the breaking limit (H / L ~ 1/7) so the last metre of water
				// does not stand up into a bright, near-vertical seam along the waterline.
				A = min(A, 0.4 / max(kLocal, 1e-5));

				float travel = -omega * ShoreTravelTime(h);
				float s, c;
				sincos(travel - Shore1.z, s, c);
				float sp, cp;
				sincos(travel - Shore1.w, sp, cp);
				o.shoreCrest = envelope * saturate(s);

				// Breaking waves pitch forward; keep the total Q*k*A below 1 with the open-sea waves on top,
				// or crests that coincide fold the surface over itself (inside-out patches on the shore).
				float Q = saturate(Shore1.x + o.shoreBreak * 0.5);
				float QA = min(Q * A, max(0.9 - steepness, 0.0) / max(kLocal, 1e-5));

				o.displacement.xy += dir * (QA * c);
				o.displacement.z += A * s;
				o.previousDisplacement.xy += dir * (QA * cp);
				o.previousDisplacement.z += A * sp;
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
		// tx and ty become parallel where the surface folds over itself; keep the normal defined there,
		// and facing up where it has folded past vertical (the visible side of the fold).
		float3 n = cross(tx, ty);
		o.normal = SafeNormalize(n.z < 0.0 ? -n : n, float3(0, 0, 1));
		o.jacobian = (1.0 + dxdx) * (1.0 + dydy) - dxdy * dxdy;
		return o;
	}

	/**
	 * Curvature of the displaced surface along unit direction `e` (units^-1) at undisplaced position
	 * `positionWS`, for tessellation. Gerstner crests are sharpened by the horizontal compression
	 * (1 + dX/ds), so the vertical second derivative is divided by its square: crests get the
	 * subdivision, broad troughs and calm water do not.
	 */
	float SurfaceCurvature(float3 positionWS, WaveContext ctx, float2 e)
	{
		float2 p = positionWS.xy + (FrameBuffer::CameraPosAdjust.xy - RefCamPos.xy);
		float zss = 0.0;
		float xs = 0.0;
		float steepnessScale = rcp(ctx.energyGain);
		uint count = WaveCount();
		[loop] for (uint i = 0; i < count; i++)
		{
			float4 dk = WaveDirK[i];
			float A = WaveAmp[i].x * WaveWeight(i, ctx);
			if (A <= 0.0)
				continue;
			float de = dot(dk.xy, e);
			float kde2 = dk.z * dk.z * de * de;
			float s = sin(dk.z * dot(dk.xy, p) + WaveAmp[i].z);
			zss -= A * kde2 * s;
			xs -= WaveAmp[i].y * steepnessScale * A * dk.z * de * de * s;
		}
		float compression = max(1.0 + xs, 0.2);
		return abs(zss) / (compression * compression);
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

	/// Silt stirred up from the bottom by bodies wading through shallow water (ripple simulation w channel).
	float RippleSilt(float3 positionWS)
	{
		if (Ripple0.w < 0.5)
			return 0.0;
		float2 uv = (positionWS.xy + FrameBuffer::CameraPosAdjust.xy - Ripple0.xy) * Ripple0.z;
		if (any(uv <= 0.0) || any(uv >= 1.0))
			return 0.0;
		return max(RippleTexture.SampleLevel(LinearClampSampler, uv, 0).w, 0.0) * RippleEdgeFade(uv);
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
		WaveResult now = EvaluateWaves(positionWS, ctx, 0.0);

		float rippleFade = damping * RippleDepthFade(ctx.depth);
		o.current = now.displacement + float3(0, 0, RippleHeight(positionWS, false) * rippleFade);
		o.previous = now.previousDisplacement + float3(0, 0, RippleHeight(positionWS, true) * rippleFade);

		// Hard bound: waves stay within their amplitude sum (doubled for the fetch energy gain, the shore
		// waves for shoaling) and the ripple sim within a few times its scale. Clamping also flushes NaN
		// (D3D min/max return the non-NaN operand), so a bad input can never throw the mesh off-screen.
		float limit = 2.0 * max(Params2.z, 0.0) + 2.0 * max(Shore0.x, 0.0) + 4.0 * max(Ripple1.x, 0.0) + 1.0;
		o.current = clamp(o.current, -limit, limit);
		o.previous = clamp(o.previous, -limit, limit);
		o.waveNormal = now.normal;
		o.jacobian = now.jacobian;
		o.shoreBreak = now.shoreBreak;
		o.depth = ctx.depth;
		return o;
	}

	/**
	 * Height of the displaced surface (waves and ripples) directly above camera-relative `xy`, for a flat
	 * water plane at camera-relative height `flatZ`. Gerstner waves also move the water sideways, so the
	 * undisplaced point that ends up above `xy` is found with a short fixed-point iteration, as
	 * WaveSnapshot::SampleAt does on the CPU.
	 */
	float SurfaceHeightAt(float2 xy, float flatZ, uint iterations)
	{
		float3 p = float3(xy, flatZ);
		WaveContext ctx = BuildWaveContext(p, DisplacementDistanceFade(length(p)), 0.0);
		float2 x0 = xy;
		[unroll] for (uint i = 0; i < iterations; i++)
		{
			x0 = xy - EvaluateWaves(float3(x0, flatZ), ctx, 0.0).displacement.xy;
		}
		float3 undisplaced = float3(x0, flatZ);
		float height = EvaluateWaves(undisplaced, ctx, 0.0).displacement.z;
		height += RippleHeight(undisplaced, false) * RippleDepthFade(ctx.depth);
		float limit = 2.0 * max(Params2.z, 0.0) + 4.0 * max(Ripple1.x, 0.0) + 1.0;
		return flatZ + clamp(height, -limit, limit);
	}
}

#endif  // __PBR_WATER_HLSLI__
