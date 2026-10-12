#ifndef __PBR_WATER_HLSLI__
#define __PBR_WATER_HLSLI__

#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/Math.hlsli"

// ============================================================================
// PBR Water - shared constants, resources and the wave model.
//
// The open water is an FFT ocean (Tessendorf 2001): four world-anchored cascades of 1000, 162, 26.3 and
// 4.27 m, each carrying one band of a JONSWAP wind sea plus swell, synthesised every frame on the GPU
// (OceanSpectrumCS / OceanFFTCS / OceanAssembleCS) into mip-mapped texture arrays. Every consumer samples
// the same textures with the same per-location weights:
//   - vertex / domain shader : displacement (current + previous frame for motion vectors), mip by vertex spacing
//   - pixel shader           : normals, crest foam trails and LEAN-filtered roughness, mip by pixel footprint
//   - hull shader            : curvature for adaptive tessellation
// The CPU re-synthesises the long-wave cascades from the same coefficients (WaveModel.cpp), so swimming,
// buoyancy and floating objects follow the rendered surface without any GPU readback.
//
// What varies over the map (fetch, depth, river flow) cannot change a cascade's content, so it weights
// each cascade as a whole: the fetch-limited spectral peak removes long-wave cascades from sheltered water,
// young seas get the JONSWAP energy gain, and tanh(kh) calms waves in the shallows.
//
// References:
//   Tessendorf, "Simulating Ocean Water" (2001)
//   Horvath, "Empirical Directional Wave Spectra for Computer Graphics" (DigiPro 2015)
//   Bruneton, Neyret & Holzschuch, "Real-time Realistic Ocean Lighting using Seamless Transitions from
//   Geometry to BRDF" (2010) - filtered slope variance
//   Olano & Baker, "LEAN Mapping" (2010)
//   Mihelich & Tcheblokov, "Wakes, Explosions and Lighting: Interactive Water Simulation in Atlas" (GDC 2019)
//   Ang et al., "The Technical Art of Sea of Thieves" (SIGGRAPH 2018 Talks) - foam/scatter breakdown
// ============================================================================

#define PBRW_CASCADES 4
#define PBRW_HULL_MASKS 8

namespace PBRWater
{
	cbuffer PBRWaterData : register(b7)
	{
		float4 Cascade0[PBRW_CASCADES];  // x 1 / tile size (1/unit), yz tile coordinate of the reference camera (incl. half texel), w texel size (units)
		float4 Cascade1[PBRW_CASCADES];  // x energy-weighted angular frequency (rad/s), y 1 / its Pierson-Moskowitz weight at unlimited fetch, z energy-weighted wavenumber (rad/unit), w std of its compression (open sea)
		float4 Params0;                  // x FFT ocean active, y wind speed (m/s), z peak omega at unlimited fetch, w gravity (units/s^2)
		float4 Params1;                  // xy wind direction, z displacement fade start, w displacement fade end (units)
		float4 Params2;                  // x choppiness, y whitecap coverage, z displacement bound (units), w highest mip level
		float4 RefCamPos;                // xyz camera position the tile coordinates are relative to, w unused
		float4 Tess0;                    // x tessellation active for this draw, y target triangle size (px), z max factor, w pixels per unit at distance 1
		float4 Draw0;                    // x vertex spacing of this draw (units), y wireframe mode, z debug view, w hull mask count
		float4 Fetch0;                   // xy grid origin (absolute world units), zw 1 / grid extent (units)
		float4 Fetch1;                   // x enabled, y direction count, z wind slice coordinate, w fetch outside the grid (m)
		float4 Terrain0;                 // xy heightmap uv scale, zw heightmap uv offset
		float4 Terrain1;                 // x heightmap z min, y heightmap z max, z enabled, w heightmap texel size (units)
		float4 Shore0;                   // x shore waves on (shore field ready), y angular frequency the rays were traced for (rad/s), z run-up scale, w largest run-up (units)
		float4 Shore1;                   // x breaking (surf) foam strength, y swash foam strength, zw unused
		float4 Ripple0;                  // xy simulation origin (absolute world units), z 1 / simulation extent (units), w enabled
		float4 Ripple1;                  // x displacement scale (units), y normal strength, z unused, w texel size (uv)
		float4 Ripple2;                  // xy previous frame's simulation origin, z step interpolation (now), w step interpolation (previous frame)
		float4 Light0;                   // x base roughness, y subsurface strength, z vanilla fresnel blend, w sun specular intensity
		float4 Light1;                   // x visibility scale, y foam albedo, z refraction distortion scale, w point light specular intensity
		float4 Foam0;                    // x shore foam width (units), y unused, z foam amount, w foam pattern scale (units)
		float4 Foam1;                    // x crest foam persistence (s), y foam animation time (s, wrapped), z wake foam strength, w flow wave damping
		float4 Clarity0;                 // x sediment extinction (1/unit per unit turbidity), y base turbidity, z patchiness (0..1), w patch size (units)
		float4 Clarity1;                 // xyz sediment colour (gamma, like the water form colours), w shore resuspension strength
		float4 Clarity2;                 // x river turbidity, y weather turbidity (storm and rain, already scaled), z wading silt strength, w sediment layer height (units)
		float4 Optics0;                  // x scattering anisotropy g, y downwelling attenuation scale, z clarity animation time (s, wrapped), w significant wave height (units)
		float4 Surface0;                 // x wind roughness scale, y gust strength, z gust size (units), w rain intensity (0..1)
		float4 Surface1;                 // x wind streak strength, y unused, z fetch scale, w unused
		float4 Land0;                    // xy loaded terrain grid corner (absolute units), zw 1 / grid extent (units)
		float4 Land1;                    // x enabled, y vertex spacing (units)
		float4 Foam2;                    // x whitecap amount, y whitecap pattern scale (units), z whitecap streak stretch, w bubble amount
		float4 Foam3;                    // xy foam drift offset (units, wrapped), zw extra bubble drift (units, wrapped)
		float4 Flow0;                    // worldspace flowmap UV = absXY * xz + yw (x = 0: no flowmap)
		float4 HullA[PBRW_HULL_MASKS];   // xy centre (absolute units), z floor (absolute units), w unused
		float4 HullB[PBRW_HULL_MASKS];   // xy length axis (unit), z half length, w half width (units)
		float4 Cascade2[PBRW_CASCADES];  // x height std of the band (units, open sea), y deep-water phase speed g / omega of its mean frequency (units/s), zw unused
		float4 ShoreMap;                 // xy shore field corner (absolute units), zw 1 / its extent (units)
		float4 Swash0;                   // xy swash grid corner (absolute units), z 1 / its extent (units), w enabled
		float4 Swash1;                   // xy last frame's swash grid corner, zw unused
	}

	Texture2D<float4> RippleTexture : register(t110);     // x height, y height one step earlier, z foam, w silt
	Texture2DArray<float> FetchTexture : register(t111);  // fetch in km, one slice per upwind direction
	Texture2D<float> TerrainHeightTexture : register(t112);
	Texture2D<float4> RipplePreviousTexture : register(t113);           // the ripple state as displayed last frame (motion vectors)
	Texture2D<float> LoadedTerrainTexture : register(t114);             // exact terrain z of the loaded cells (LAND vertex heights)
	Texture2DArray<float4> OceanDisplacement : register(t115);          // xyz displacement (units), w dDx/dy, one slice per cascade
	Texture2DArray<float4> OceanDerivatives : register(t116);           // xy height slopes, zw compression dDx/dx, dDy/dy
	Texture2DArray<float4> OceanSurface : register(t117);               // xy squared slopes, z crest foam trail, w |laplacian| (1/unit)
	Texture2DArray<float4> OceanPreviousDisplacement : register(t118);  // last frame's displacement (motion vectors)
	Texture2D<float4> ShoreFieldTexture : register(t119);               // x shoaling delay (s), y signed distance to the waterline (units, > 0 inland), zw ray direction
	Texture2D<float2> ShoreLevelTexture : register(t120);               // x beach slope (tan) where the ray meets the waterline, y still water level (absolute z, < -1e8: none)
	Texture2D<float4> SwashTexture : register(t121);                    // x run-up level (units above the still water), y its rise speed (units/s), z level of the last peak, w s since the bed here was under water
	Texture2D<float4> SwashPreviousTexture : register(t122);            // the run-up as displayed last frame (motion vectors)
	Texture2D<float4> SwashWetTexture : register(t123);                 // x wet level, y foam the backwash left on the bed, z its deposit, w water surface height (units above the still water)
	SamplerState LinearClampSampler : register(s12);
	SamplerState OceanSampler : register(s13);  // anisotropic, wrap

	static const float UnitsPerMetre = METRES_TO_UNITS;
	static const float MetresPerUnit = 1.0 / METRES_TO_UNITS;

	/// Normalises `v`, or returns `fallback` for a zero-length vector (normalize() would return NaN).
	float3 SafeNormalize(float3 v, float3 fallback)
	{
		float lengthSq = dot(v, v);
		return lengthSq > 1e-20 ? v * rsqrt(lengthSq) : fallback;
	}

	bool OceanActive() { return Params0.x > 0.5; }
	float Gravity() { return Params0.w; }

	// ------------------------------------------------------------------------
	// Spatial context: everything that makes the spectrum vary over the map.
	// ------------------------------------------------------------------------
	/// Where waves travel to the shore here (shore field, ShoreField in WaveModel.h).
	struct ShoreRay
	{
		float delay;     // shoaling delay (s) accumulated along the ray up to here
		float distance;  // signed distance to the waterline (units): > 0 inland, < 0 offshore
		float2 dir;      // unit ray direction, towards the waterline and on up the beach
		float slope;     // beach slope (tan) where the ray meets the waterline
		float weight;    // 0 outside the field, fading in from its border
	};

	struct WaveContext
	{
		float fetchMetres;   // open water upwind (m)
		float peakOmega;     // local spectral peak (rad/s); larger than the open-sea peak where fetch is short
		float energyGain;    // amplitude gain of the fetch-limited spectrum (FetchEnergyGain)
		float fetchRatio;    // local significant wave height relative to the open sea (fetch-limited growth)
		float depth;         // water depth below the undisplaced surface (units)
		float elevation;     // bed height above the still water (units): -depth under water, > 0 on the beach
		float damping;       // overall amplitude multiplier (distance fade, flow, ...)
		float2 terrainGrad;  // terrain height gradient (units/unit), points uphill i.e. towards the shore
		float hasTerrain;    // 1 when depth and gradient come from the heightmap
		ShoreRay shore;      // wave ray through here
		float saturation;    // depth-limited breaking: share of the shoaled sea the depth can carry (1 offshore)
		float deepHeight;    // significant height the sea here has in deep water (units, fetch-limited)
		float beach;         // 0..1 surf and swash: waves break on this shore rather than surge against it
	};

	/// Bilinear lookup from texel loads, clamped to the edge. The terrain heights must never depend on the
	/// sampler bound to the slot: the game re-binds sampler slots behind our back, and a point sampler
	/// turns the depth into blocks (visible as pixelated shore waves and foam).
	float BilinearLoad(Texture2D<float> t, float2 uv)
	{
		uint w, h;
		t.GetDimensions(w, h);
		float2 p = uv * float2(w, h) - 0.5;
		int2 i0 = int2(floor(p));
		float2 f = p - i0;
		int2 hi = int2(w, h) - 1;
		int2 a = clamp(i0, 0, hi);
		int2 b = clamp(i0 + 1, 0, hi);
		float z00 = t.Load(int3(a.x, a.y, 0));
		float z10 = t.Load(int3(b.x, a.y, 0));
		float z01 = t.Load(int3(a.x, b.y, 0));
		float z11 = t.Load(int3(b.x, b.y, 0));
		return lerp(lerp(z00, z10, f.x), lerp(z01, z11, f.x), f.y);
	}

	float SampleTerrainZ(float2 absXY)
	{
		float2 uv = absXY * Terrain0.xy + Terrain0.zw;
		float h = BilinearLoad(TerrainHeightTexture, uv);
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
		// Skyrim's distances are compressed: the baked open water counts Surface1.z times over.
		return lerp(f0, f1, t) * 1000.0 * max(Surface1.z, 1.0);
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

	float4 BilinearLoad4(Texture2D<float4> t, float2 uv)
	{
		uint w, h;
		t.GetDimensions(w, h);
		float2 p = uv * float2(w, h) - 0.5;
		int2 i0 = int2(floor(p));
		float2 f = p - i0;
		int2 hi = int2(w, h) - 1;
		int2 a = clamp(i0, 0, hi);
		int2 b = clamp(i0 + 1, 0, hi);
		return lerp(lerp(t.Load(int3(a.x, a.y, 0)), t.Load(int3(b.x, a.y, 0)), f.x), lerp(t.Load(int3(a.x, b.y, 0)), t.Load(int3(b.x, b.y, 0)), f.x), f.y);
	}

	float2 BilinearLoad2(Texture2D<float2> t, float2 uv)
	{
		uint w, h;
		t.GetDimensions(w, h);
		float2 p = uv * float2(w, h) - 0.5;
		int2 i0 = int2(floor(p));
		float2 f = p - i0;
		int2 hi = int2(w, h) - 1;
		int2 a = clamp(i0, 0, hi);
		int2 b = clamp(i0 + 1, 0, hi);
		return lerp(lerp(t.Load(int3(a.x, a.y, 0)), t.Load(int3(b.x, a.y, 0)), f.x), lerp(t.Load(int3(a.x, b.y, 0)), t.Load(int3(b.x, b.y, 0)), f.x), f.y);
	}

	bool ShoreFieldUV(float2 absXY, out float2 uv)
	{
		uv = (absXY - ShoreMap.xy) * ShoreMap.zw;
		return Shore0.x > 0.5 && all(uv > 0.0) && all(uv < 1.0);
	}

	ShoreRay SampleShore(float2 absXY)
	{
		ShoreRay r;
		r.delay = 0.0;
		r.distance = -1e6;
		r.dir = 0.0;
		r.slope = 1.0;
		r.weight = 0.0;
		float2 uv;
		if (!ShoreFieldUV(absXY, uv))
			return r;
		float4 field = BilinearLoad4(ShoreFieldTexture, uv);
		float len = length(field.zw);
		r.delay = field.x;
		r.distance = field.y;
		r.dir = len > 1e-3 ? field.zw / len : 0.0;
		r.slope = BilinearLoad2(ShoreLevelTexture, uv).x;
		// Fade out towards the field's border (it moves with the player) and where rays meet head on.
		float2 e = saturate(min(uv, 1.0 - uv) * 10.0);
		r.weight = e.x * e.y * saturate(len * 2.0);
		return r;
	}

	/// Still water level (absolute z) in the shore field, or a large negative value where there is none.
	float ShoreLevel(float2 absXY)
	{
		float2 uv;
		if (!ShoreFieldUV(absXY, uv))
			return -1e9;
		return BilinearLoad2(ShoreLevelTexture, uv).y;
	}

	/// tanh for x >= 0 that cannot overflow. FXC lowers the intrinsic to (e^2x - 1) / (e^2x + 1),
	/// which is inf / inf = NaN once kh exceeds ~44, i.e. for any deep water.
	float SafeTanh(float x)
	{
		return 1.0 - 2.0 / (exp(2.0 * min(max(x, 0.0), 20.0)) + 1.0);
	}

	/// Wavenumber (rad/m) of waves of angular frequency `omega` in `depthMetres` of water: Eckart (1952),
	/// k = k0 / sqrt(tanh(k0 h)), within a few percent of omega^2 = g k tanh(k h). Mirrors WaveModel.cpp.
	float Wavenumber(float omega, float depthMetres)
	{
		float k0 = omega * omega / 9.81;
		return k0 * rsqrt(max(SafeTanh(k0 * max(depthMetres, 1e-3)), 1e-6));
	}

	/// Phase speed (m/s). Mirrors PBRWaterModel::PhaseSpeed.
	float PhaseSpeed(float omega, float depthMetres)
	{
		omega = max(omega, 1e-3);
		return omega / Wavenumber(omega, depthMetres);
	}

	/// Shoaling coefficient sqrt(cg0 / cg): energy flux is conserved as waves slow down over the shoaling bed
	/// (Green's law, H ~ h^-1/4, in shallow water). Mirrors PBRWaterModel::ShoalingCoefficient.
	float ShoalingCoefficient(float omega, float depthMetres)
	{
		omega = max(omega, 1e-3);
		float d = max(depthMetres, 1e-3);
		float k = Wavenumber(omega, d);
		float kh = min(k * d, 20.0);
		float n = kh < 1e-2 ? 1.0 : 0.5 * (1.0 + 2.0 * kh / sinh(2.0 * kh));
		float cg = n * omega / k;
		return sqrt((0.5 * 9.81 / omega) / cg);
	}

	/// Fetch-limited deep-water amplitude multiplier of cascade `c`: the ratio of the local Pierson-Moskowitz
	/// low-frequency cut-off to the open-sea one at the cascade's mean frequency (so long swells cannot exist on a
	/// pond while short chop is unaffected), times the young sea's energy gain.
	float FetchWeight(uint c, WaveContext ctx)
	{
		float r = ctx.peakOmega / Cascade1[c].x;
		float r2 = r * r;
		return saturate(exp(-1.25 * r2 * r2) * Cascade1[c].y) * ctx.energyGain;
	}

	WaveContext BuildWaveContext(float3 positionWS, float damping)
	{
		WaveContext ctx;
		float3 absPos = positionWS + FrameBuffer::CameraPosAdjust.xyz;
		ctx.fetchMetres = SampleFetchMetres(absPos.xy);
		ctx.peakOmega = FetchPeakOmega(ctx.fetchMetres);
		ctx.energyGain = FetchEnergyGain(ctx.fetchMetres);
		ctx.fetchRatio = FetchHeightRatio(ctx.fetchMetres);
		ctx.depth = 1e6;
		ctx.elevation = -1e6;
		ctx.terrainGrad = 0.0;
		ctx.hasTerrain = 0.0;
		ctx.damping = damping;

		// The undisplaced surface point is the water plane: its own z is the water height.
		float2 landUV = (absPos.xy - Land0.xy) * Land0.zw;
		if (Land1.x > 0.5 && all(landUV > 0.0) && all(landUV < 1.0)) {
			// Exact terrain of the loaded cells: resolves beaches, rocks and shelves.
			float2 t = Land1.y * Land0.zw;
			float z0 = BilinearLoad(LoadedTerrainTexture, landUV);
			float zx = BilinearLoad(LoadedTerrainTexture, landUV + float2(t.x, 0)) - BilinearLoad(LoadedTerrainTexture, landUV - float2(t.x, 0));
			float zy = BilinearLoad(LoadedTerrainTexture, landUV + float2(0, t.y)) - BilinearLoad(LoadedTerrainTexture, landUV - float2(0, t.y));
			ctx.elevation = z0 - absPos.z;
			ctx.terrainGrad = float2(zx, zy) / (2.0 * Land1.y);
			ctx.hasTerrain = 1.0;
		} else if (Terrain1.z > 0.5 && TerrainInside(absPos.xy)) {
			float texel = Terrain1.w;
			float z0 = SampleTerrainZ(absPos.xy);
			float zx = SampleTerrainZ(absPos.xy + float2(texel, 0));
			float zy = SampleTerrainZ(absPos.xy + float2(0, texel));
			ctx.elevation = z0 - absPos.z;
			ctx.terrainGrad = float2(zx - z0, zy - z0) / texel;
			ctx.hasTerrain = 1.0;
		}
		ctx.depth = ctx.hasTerrain > 0.5 ? max(-ctx.elevation, 0.0) : 1e6;
		ctx.shore = SampleShore(absPos.xy);

		// Depth-limited breaking: in the surf zone the significant height of random waves stays below ~0.6 of the
		// depth (Thornton & Guza 1983). Every cascade keeps that share of its shoaled height.
		float depthMetres = ctx.depth * MetresPerUnit;
		float deepVariance = 0.0;
		float shoaledVariance = 0.0;
		[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
		{
			float w = FetchWeight(c, ctx) * Cascade2[c].x;
			deepVariance += w * w;
			w *= ctx.hasTerrain > 0.5 ? ShoalingCoefficient(Cascade1[c].x, depthMetres) : 1.0;
			shoaledVariance += w * w;
		}
		ctx.deepHeight = 4.0 * sqrt(deepVariance);
		ctx.saturation = ctx.hasTerrain > 0.5 ? min(1.0, 0.6 * ctx.depth / max(4.0 * sqrt(shoaledVariance), 1e-3)) : 1.0;

		// Breaking or surging: the Iribarren number xi = tan(beta) / sqrt(H0 / L0) of the beach the ray runs up
		// (Battjes 1974) - waves spill (xi < 0.5) and plunge on gentle beaches, but surge up rock faces and
		// cliffs (xi > 3.3) without breaking, running up or leaving foam.
		float steepness = ctx.deepHeight * MetresPerUnit / (6.2831853 * 9.81 / max(ctx.peakOmega * ctx.peakOmega, 1e-4));
		float iribarren = ctx.shore.slope * rsqrt(max(steepness, 1e-4));
		ctx.beach = smoothstep(3.3, 2.0, iribarren) * ctx.shore.weight * ctx.hasTerrain * saturate(ctx.deepHeight - 1.0);
		return ctx;
	}

	/// Distance fade of the displacement, so far water converges to the flat vanilla plane
	/// (and to the untessellated LOD meshes) instead of popping.
	float DisplacementDistanceFade(float distance)
	{
		return 1.0 - smoothstep(Params1.z, max(Params1.w, Params1.z + 1.0), distance);
	}

	/**
	 * Vertical amplitude multiplier of cascade `c` at this location: the fetch-limited deep-water sea, shoaled
	 * over the bed (Green's law) and held to the depth-limited breaking height.
	 */
	float CascadeWeight(uint c, WaveContext ctx)
	{
		float w = FetchWeight(c, ctx);
		if (ctx.hasTerrain > 0.5)
			w *= ShoalingCoefficient(Cascade1[c].x, ctx.depth * MetresPerUnit);
		return w * ctx.saturation * ctx.damping;
	}

	/**
	 * Shore warp: the sea that arrives at a point near the shore left deep water c0 Delta behind it along its ray
	 * (Delta: the shoaling delay). Sampling the deep-water cascade there gives the same waves, slowed, shortened by
	 * c0 / c and turned towards the shore by the depth. Each band shifts by its own deep-water speed.
	 */
	float2 ShoreShift(WaveContext ctx, uint c)
	{
		return ctx.shore.dir * (Cascade2[c].y * ctx.shore.delay * ctx.shore.weight);
	}

	/// How much the shore warp shortens cascade `c` across the depth contours: c0 / c for the waves the rays were
	/// traced for, scaled to the band (shorter waves feel the bottom later).
	float ShoreCompression(WaveContext ctx, uint c)
	{
		if (ctx.shore.weight <= 0.0 || ctx.hasTerrain < 0.5)
			return 1.0;
		float omega = max(Shore0.y, 1e-3);
		float ratio = (9.81 / omega) / PhaseSpeed(omega, max(ctx.depth * MetresPerUnit, 0.15));
		return 1.0 + max(ratio - 1.0, 0.0) * (omega / Cascade1[c].x) * ctx.shore.weight;
	}

	/**
	 * How much cascade `c` carries the waves that make whitecaps here: the band holding the local spectral peak
	 * and, fading out, the next shorter one. Shorter waves break too, but their small crests only sprinkle the
	 * sea with flecks; whitecaps are the big breakers (Banner, Babanin & Young 2000: dominant-wave breaking).
	 */
	float BreakingDominance(uint c, float peakOmega)
	{
		if (c + 1 >= PBRW_CASCADES)
			return 0.0;
		float r = Cascade1[c].x / max(peakOmega, 1e-3);
		return saturate(1.0 - 0.5 * log2(max(r, 1.0)));
	}

	/// Tile coordinate of camera-relative position `p` in cascade `c` (world-anchored, re-based on the
	/// reference camera so other cameras still sample the same water).
	float2 CascadeUV(float2 p, uint c)
	{
		float2 rel = p + (FrameBuffer::CameraPosAdjust.xy - RefCamPos.xy);
		return rel * Cascade0[c].x + Cascade0[c].yz;
	}

	struct WaveResult
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float3 normal;                // surface normal
		float jacobian;               // < 1 where the surface compresses, < 0 where it folds (breaking)
		float slopeVariance;          // slope variance filtered out of the pixel (for roughness)
		float crestTrail;             // 0..1 breaking crests of the dominant waves and the foam they left behind
		float shoreBreak;             // 0..1 a shore wave breaking here now (crest high against the depth)
		float shoreMask;              // 0..1 inner surf zone: the depth holds the waves down
		float shoreCrest;             // 0..1 how high the shore waves stand here against the depth
		float3 dominant;              // x height, yz slope of the dominant waves (EvaluateWavesFiltered only: sun shadows)
		float dominantK;              // their energy-weighted wavenumber, shortened by the shore warp (1/unit)
	};

	/// Running sums of the cascades' fields.
	struct WaveSums
	{
		float3 displacement;
		float3 previous;
		float dzdx, dzdy, dxdx, dydy, dxdy;
		float slopeVariance;
		float crestTrail;
	};

	WaveSums EmptySums()
	{
		WaveSums sums;
		sums.displacement = 0.0;
		sums.previous = 0.0;
		sums.dzdx = sums.dzdy = sums.dxdx = sums.dydy = sums.dxdy = 0.0;
		sums.slopeVariance = 0.0;
		sums.crestTrail = 0.0;
		return sums;
	}

	/// Horizontal motion sampled through the shore warp, back in world space: across the depth contours it
	/// shrinks by 1 / kappa with the wavelength, so the crests compress no more than in deep water.
	float2 UnwarpHorizontal(float2 d, float2 n, float kappa)
	{
		return d - (1.0 - 1.0 / kappa) * n * dot(n, d);
	}

	/**
	 * Adds one cascade: `vertical` scales heights and slopes, `horizontal` the choppy displacement. The fields were
	 * sampled through the shore warp, which shortens the waves by `kappa` along the ray `n`: slopes steepen by
	 * kappa across the contours (J^T), the horizontal motion shrinks by 1 / kappa, and the compression tensor
	 * M becomes (I - (1 - 1/kappa) n n^T) M (I + (kappa - 1) n n^T).
	 */
	void AddCascade(inout WaveSums sums, float vertical, float horizontal, float4 displacement, float4 derivatives, float2 n, float kappa)
	{
		float2 slope = derivatives.xy + (kappa - 1.0) * n * dot(n, derivatives.xy);
		float2 d = UnwarpHorizontal(displacement.xy, n, kappa);
		float alpha = 1.0 - 1.0 / kappa;
		float beta = kappa - 1.0;
		float2x2 P = float2x2(n.x * n.x, n.x * n.y, n.x * n.y, n.y * n.y);
		float2x2 I = float2x2(1.0, 0.0, 0.0, 1.0);
		float2x2 M = float2x2(derivatives.z, displacement.w, displacement.w, derivatives.w);
		float2x2 W = mul(mul(I - alpha * P, M), I + beta * P);
		sums.displacement += float3(d * horizontal, displacement.z * vertical);
		sums.dzdx += slope.x * vertical;
		sums.dzdy += slope.y * vertical;
		sums.dxdx += W[0][0] * horizontal;
		sums.dydy += W[1][1] * horizontal;
		sums.dxdy += 0.5 * (W[0][1] + W[1][0]) * horizontal;
	}

	/**
	 * Shore waves breaking: in the surf zone (where the depth holds the shoaled sea down) the crests that stand
	 * highest against the depth are breaking (a single wave breaks at H = 0.78 h, its crest about 0.4 h up).
	 * `heave` is the vertical displacement of the warped long-wave cascades.
	 */
	void ShoreBreaking(inout WaveResult o, WaveContext ctx, float heave)
	{
		o.shoreBreak = 0.0;
		o.shoreMask = 0.0;
		o.shoreCrest = 0.0;
		if (ctx.beach <= 0.0)
			return;
		float relative = heave / max(ctx.depth, 1.0);
		o.shoreCrest = ctx.beach * saturate(relative / 0.4);
		o.shoreBreak = ctx.beach * smoothstep(0.25, 0.42, relative) * saturate(ctx.damping * 4.0);
		o.shoreMask = ctx.beach * saturate((1.0 - ctx.saturation) * 3.0);
	}

	WaveResult FinishWaves(WaveSums sums, WaveResult o)
	{
		// Normal of the parametric surface P(x, y) = (x + Dx, y + Dy, Dz)
		float3 tx = float3(1.0 + sums.dxdx, sums.dxdy, sums.dzdx);
		float3 ty = float3(sums.dxdy, 1.0 + sums.dydy, sums.dzdy);
		// tx and ty become parallel where the surface folds over itself; keep the normal defined there,
		// and facing up where it has folded past vertical (the visible side of the fold).
		float3 n = cross(tx, ty);
		o.normal = SafeNormalize(n.z < 0.0 ? -n : n, float3(0, 0, 1));
		o.jacobian = (1.0 + sums.dxdx) * (1.0 + sums.dydy) - sums.dxdy * sums.dxdy;
		o.displacement = sums.displacement;
		o.previousDisplacement = sums.previous;
		o.slopeVariance = sums.slopeVariance;
		o.crestTrail = sums.crestTrail;
		return o;
	}

	/**
	 * Waves for geometry (vertex, domain, underwater shaders) at undisplaced camera-relative `positionWS`.
	 * Each cascade is read from the mip whose texels match the vertex spacing (finer where the shore warp
	 * shortens the waves), so waves the mesh cannot carry are averaged out instead of aliasing.
	 * @param spacing vertex spacing (units); 0 reads the full resolution
	 * @param previous also read last frame's displacement (compiled out when unused)
	 */
	WaveResult EvaluateWaves(float3 positionWS, WaveContext ctx, float spacing, bool previous)
	{
		WaveResult o = (WaveResult)0;
		WaveSums sums = EmptySums();
		float steepnessScale = rcp(ctx.energyGain);
		float heave = 0.0;
		if (OceanActive()) {
			[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
			{
				float vertical = CascadeWeight(c, ctx);
				[branch] if (vertical > 0.0)
				{
					float kappa = ShoreCompression(ctx, c);
					float3 uv = float3(CascadeUV(positionWS.xy - ShoreShift(ctx, c), c), c);
					float lod = spacing > 0.0 ? clamp(log2(spacing * kappa / Cascade0[c].w) - 0.5, 0.0, Params2.w) : 0.0;
					float horizontal = vertical * steepnessScale;
					float4 d = OceanDisplacement.SampleLevel(OceanSampler, uv, lod);
					float4 g = OceanDerivatives.SampleLevel(OceanSampler, uv, lod);
					AddCascade(sums, vertical, horizontal, d, g, ctx.shore.dir, kappa);
					if (c + 1 < PBRW_CASCADES)
						heave += d.z * vertical;
					if (previous) {
						float4 dp = OceanPreviousDisplacement.SampleLevel(OceanSampler, uv, lod);
						sums.previous += float3(UnwarpHorizontal(dp.xy, ctx.shore.dir, kappa) * horizontal, dp.z * vertical);
					}
				}
			}
		}
		ShoreBreaking(o, ctx, heave);
		return FinishWaves(sums, o);
	}

	/**
	 * Waves for shading at undisplaced camera-relative `positionWS`, filtered over the pixel footprint
	 * (trilinear / anisotropic). The slope variance the filter removes is recovered from the mip-filtered
	 * second moments (LEAN mapping) and returned for the roughness; foam trails come from the cascades
	 * that carry the waves at this location.
	 */
	WaveResult EvaluateWavesFiltered(float3 positionWS, WaveContext ctx)
	{
		WaveResult o = (WaveResult)0;
		WaveSums sums = EmptySums();
		float steepnessScale = rcp(ctx.energyGain);
		float heave = 0.0;
		float dominantVariance = 0.0;
		float dominantK2 = 0.0;
		if (OceanActive()) {
			[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
			{
				float vertical = CascadeWeight(c, ctx);
				float kappa = ShoreCompression(ctx, c);
				float dominance = BreakingDominance(c, ctx.peakOmega);
				float3 uv = float3(CascadeUV(positionWS.xy - ShoreShift(ctx, c), c), c);
				// The footprint in the tile includes the warp's compression.
				float2 gx = ddx(uv.xy);
				float2 gy = ddy(uv.xy);
				// Sampled unconditionally: gradient sampling must stay in uniform control flow.
				float4 d = OceanDisplacement.SampleGrad(OceanSampler, uv, gx, gy);
				float4 g = OceanDerivatives.SampleGrad(OceanSampler, uv, gx, gy);
				float4 m = OceanSurface.SampleGrad(OceanSampler, uv, gx, gy);
				float horizontal = vertical * steepnessScale;
				AddCascade(sums, vertical, horizontal, d, g, ctx.shore.dir, kappa);
				if (c + 1 < PBRW_CASCADES)
					heave += d.z * vertical;
				float variance = max(m.x - g.x * g.x, 0.0) + max(m.y - g.y * g.y, 0.0);
				sums.slopeVariance += variance * vertical * vertical * kappa * kappa;
				// The trail was laid down by the open-sea cascade; where fetch or depth calm it, it breaks less, and
				// only the bands with the dominant waves here make whitecaps.
				float presence = saturate(horizontal);
				sums.crestTrail = max(sums.crestTrail, m.z * presence * presence * dominance);
				// The dominant waves alone, for their sun shadows: height, slope (through the warp) and wavenumber.
				float2 slope = g.xy + (kappa - 1.0) * ctx.shore.dir * dot(ctx.shore.dir, g.xy);
				o.dominant += float3(d.z, slope) * (vertical * dominance);
				float sigma = Cascade2[c].x * vertical * dominance;
				float k = Cascade1[c].z * kappa;
				dominantVariance += sigma * sigma;
				dominantK2 += sigma * sigma * k * k;
			}
		}
		o.dominantK = dominantVariance > 1e-6 ? sqrt(dominantK2 / dominantVariance) : 0.0;
		ShoreBreaking(o, ctx, heave);
		return FinishWaves(sums, o);
	}

	/**
	 * Curvature of the displaced surface along unit direction `e` (units^-1) around undisplaced position
	 * `positionWS`, at the scale `scale` (units) the tessellation is aiming for, so crests get the
	 * subdivision and broad troughs and calm water do not. Uses the mean |laplacian| of the height over
	 * that scale (mip-filtered, so short waves are not averaged away), sharpened by the horizontal
	 * compression along `e` as on Gerstner-like crests, and by the shore warp's shortening.
	 */
	float SurfaceCurvature(float3 positionWS, WaveContext ctx, float2 e, float scale)
	{
		if (!OceanActive())
			return 0.0;
		float curvature = 0.0;
		float compression = 0.0;
		float steepnessScale = rcp(ctx.energyGain);
		[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
		{
			float vertical = CascadeWeight(c, ctx);
			[branch] if (vertical > 0.0)
			{
				float kappa = ShoreCompression(ctx, c);
				float3 uv = float3(CascadeUV(positionWS.xy - ShoreShift(ctx, c), c), c);
				float lod = clamp(log2(max(scale * kappa, 1e-3) / Cascade0[c].w), 0.0, Params2.w);
				float4 m = OceanSurface.SampleLevel(OceanSampler, uv, lod);
				float4 g = OceanDerivatives.SampleLevel(OceanSampler, uv, lod);
				float shear = OceanDisplacement.SampleLevel(OceanSampler, uv, lod).w;
				curvature += m.w * vertical * kappa * kappa;
				compression += (g.z * e.x * e.x + 2.0 * shear * e.x * e.y + g.w * e.y * e.y) * vertical * steepnessScale;
			}
		}
		float stretch = max(1.0 + compression, 0.2);
		return curvature / (stretch * stretch);
	}

	/**
	 * True inside a small open hull (rowboat, raft, a boat lying in the shallows) above its floor: a wave
	 * rising over the floor must not show inside the boat. The footprint is a rounded rectangle (superellipse)
	 * inside the hull's walls, so the bow and stern taper and the water around the hull is untouched.
	 * @param positionWS camera-relative position of the displaced surface
	 */
	bool InsideDryHull(float3 positionWS)
	{
		uint count = (uint)Draw0.w;
		float3 absPos = positionWS + FrameBuffer::CameraPosAdjust.xyz;
		[loop] for (uint i = 0; i < PBRW_HULL_MASKS; i++)
		{
			if (i >= count)
				break;
			float2 d = absPos.xy - HullA[i].xy;
			float2 axis = HullB[i].xy;
			float2 local = float2(dot(d, axis), dot(d, float2(-axis.y, axis.x))) / max(HullB[i].zw, 1.0);
			float2 a = abs(local);
			if (pow(a.x, 2.5) + pow(a.y, 2.5) < 1.0 && absPos.z > HullA[i].z)
				return true;
		}
		return false;
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
		// Foam churned up by bodies and breaking ripples; the simulation carries it with the surface drift
		// and lets it thin out, so it stays where the water was disturbed instead of flickering with crests.
		float foam = RippleTexture.SampleLevel(LinearClampSampler, uv, 0).z;
		float texelUnits = t / Ripple0.z;
		float2 grad = float2(hR - hL, hU - hD) / (2.0 * texelUnits) * Ripple1.x * Ripple1.y * RippleDepthFade(depth);
		return float3(grad, foam) * RippleEdgeFade(uv);
	}

	// ------------------------------------------------------------------------
	// Swash: the water a breaking wave throws up the beach (state kept by ShoreSimCS)
	// ------------------------------------------------------------------------

	/// The swash lens: the water over the beach thins to nothing at the run-up tip; its surface rises with this
	/// share of the beach slope (the lens of Peregrine & Williams 2001 is a thin wedge).
	static const float SwashSurfaceSlope = 0.4;

	bool SwashUV(float2 absXY, bool previous, out float2 uv)
	{
		uv = (absXY - (previous ? Swash1.xy : Swash0.xy)) * Swash0.z;
		return Swash0.w > 0.5 && all(uv > 0.0) && all(uv < 1.0);
	}

	/// Swash state at absolute `absXY`: x run-up level (units above the still water), y its rise speed (units/s),
	/// z level the last uprush reached, w s since the bed here was last under water.
	float4 SwashState(float2 absXY, bool previous)
	{
		float2 uv;
		if (!SwashUV(absXY, previous, uv))
			return float4(0.0, 0.0, 0.0, 600.0);
		float4 s = previous ? BilinearLoad4(SwashPreviousTexture, uv) : BilinearLoad4(SwashTexture, uv);
		// The grid scrolls with the camera: its edge fades out so newly covered ground never pops.
		float2 e = saturate(min(uv, 1.0 - uv) * 20.0);
		s.xz *= e.x * e.y;
		return s;
	}

	/**
	 * Height of the swash surface above the still water over a bed `elevation` above it, with the run-up at
	 * `level`: the lens rises from the bed to the tip, and beyond the tip drops steeply under the beach, where the
	 * terrain hides the mesh (and where its triangles differ from the bilinear bed, keeps it well under).
	 */
	float SwashSurface(float elevation, float level)
	{
		float z = lerp(level, elevation, SwashSurfaceSlope);
		float beyond = elevation - level - 10.0;
		return beyond > 0.0 ? z - beyond * 4.0 : z;
	}

	/**
	 * Height of the deep-water sea (fetch-limited, before shoaling) that the shore warp brings to camera-relative
	 * `xy` along ray `dir` with shoaling delay `delay`, low-passed at mip `lod`: the waves arriving there.
	 */
	float DeepSeaHeight(float2 xy, WaveContext ctx, float2 dir, float delay, float lod)
	{
		if (!OceanActive())
			return 0.0;
		float height = 0.0;
		[unroll] for (uint c = 0; c + 1 < PBRW_CASCADES; c++)
		{
			float3 uv = float3(CascadeUV(xy - dir * (Cascade2[c].y * delay), c), c);
			height += OceanDisplacement.SampleLevel(OceanSampler, uv, lod).z * FetchWeight(c, ctx);
		}
		return height;
	}

	/// Largest vertical reach of the surface (units): waves within their statistical bound (doubled for the fetch
	/// energy gain), the swash up the beach and the ripples. Used to clamp, which also flushes NaN.
	float DisplacementLimit()
	{
		return 2.0 * max(Params2.z, 0.0) + max(Shore0.w, 0.0) + 4.0 * max(Ripple1.x, 0.0) + 1.0;
	}

	// ------------------------------------------------------------------------
	// Full surface displacement used by geometry (VS / DS)
	// ------------------------------------------------------------------------
	struct SurfaceDisplacement
	{
		float3 current;
		float3 previous;
		float jacobian;
		float shoreBreak;
		float depth;
	};

	SurfaceDisplacement DisplaceSurface(float3 positionWS, float flowDamping, float spacing)
	{
		SurfaceDisplacement o;
		float distance = length(positionWS);
		float damping = DisplacementDistanceFade(distance) * flowDamping;

		WaveContext ctx = BuildWaveContext(positionWS, damping);
		WaveResult now = EvaluateWaves(positionWS, ctx, spacing, true);

		float rippleFade = damping * RippleDepthFade(ctx.depth);
		o.current = now.displacement + float3(0, 0, RippleHeight(positionWS, false) * rippleFade);
		o.previous = now.previousDisplacement + float3(0, 0, RippleHeight(positionWS, true) * rippleFade);

		// The swash: each breaking wave runs up the beach above the still water and drains back.
		if (ctx.beach > 0.0) {
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			o.current.z = max(o.current.z, SwashSurface(ctx.elevation, SwashState(absXY, false).x));
			o.previous.z = max(o.previous.z, SwashSurface(ctx.elevation, SwashState(absXY, true).x));
		}

		// Hard bound; clamping also flushes NaN (D3D min/max return the non-NaN operand), so a bad input can
		// never throw the mesh off-screen.
		float limit = DisplacementLimit();
		o.current = clamp(o.current, -limit, limit);
		o.previous = clamp(o.previous, -limit, limit);
		o.jacobian = now.jacobian;
		o.shoreBreak = now.shoreBreak;
		o.depth = ctx.depth;
		return o;
	}

	/**
	 * Height of the displaced surface (waves, swash and ripples) directly above camera-relative `xy`, for a flat
	 * water plane at camera-relative height `flatZ`. The waves also move the water sideways, so the
	 * undisplaced point that ends up above `xy` is found with a short fixed-point iteration, as
	 * WaveSnapshot::SampleAt does on the CPU.
	 */
	float SurfaceHeightAt(float2 xy, float flatZ, uint iterations)
	{
		float3 p = float3(xy, flatZ);
		WaveContext ctx = BuildWaveContext(p, DisplacementDistanceFade(length(p)));
		float2 x0 = xy;
		[unroll] for (uint i = 0; i < iterations; i++)
		{
			x0 = xy - EvaluateWaves(float3(x0, flatZ), ctx, 0.0, false).displacement.xy;
		}
		float3 undisplaced = float3(x0, flatZ);
		float height = EvaluateWaves(undisplaced, ctx, 0.0, false).displacement.z;
		height += RippleHeight(undisplaced, false) * RippleDepthFade(ctx.depth);
		if (ctx.beach > 0.0)
			height = max(height, SwashSurface(ctx.elevation, SwashState(xy + FrameBuffer::CameraPosAdjust.xy, false).x));
		float limit = DisplacementLimit();
		return flatZ + clamp(height, -limit, limit);
	}
}

#endif  // __PBR_WATER_HLSLI__
