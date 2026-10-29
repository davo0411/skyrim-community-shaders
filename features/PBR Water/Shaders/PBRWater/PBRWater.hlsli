#ifndef __PBR_WATER_HLSLI__
#define __PBR_WATER_HLSLI__

#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "PBRWater/Noise.hlsli"

// ============================================================================
// PBR Water - shared constants, resources and the wave model.
//
// The open water is an FFT ocean (Tessendorf 2001): four world-anchored cascades of 1000, 162, 26.3 and
// 4.27 m, each carrying one band of a JONSWAP wind sea plus swell, synthesised every frame on the GPU
// (OceanSpectrumCS / OceanFFTCS / OceanAssembleCS) into mip-mapped texture arrays. Every consumer samples
// the same textures with the same per-location weights:
//   - vertex / domain shader : displacement (current + previous frame for motion vectors), mip by vertex spacing
//   - pixel shader           : normals and LEAN-filtered roughness, mip by pixel footprint
//   - hull shader            : curvature for adaptive tessellation
// The CPU re-synthesises the long-wave cascades from the same coefficients (WaveModel.cpp), so swimming,
// buoyancy and floating objects follow the rendered surface without any GPU readback.
//
// What varies over the map (fetch, depth, river flow) cannot change a cascade's content, so it weights
// each cascade as a whole: the fetch-limited spectral peak removes long-wave cascades from sheltered water,
// young seas get the JONSWAP energy gain, and tanh(kh) calms waves in the shallows.
//
// Near the shore the long waves hand over to Fluid Flux-style coastline waves (ShoreWaves): a wave train running up a
// coastline map traced over the real depths, which shoals, steepens, breaks and runs up the beach as the swash
// (SwashTip); the water ends per pixel against the ground behind it (Water.hlsl), never where the mesh's triangles cut
// the terrain.
//
// References:
//   Tessendorf, "Simulating Ocean Water" (2001)
//   Horvath, "Empirical Directional Wave Spectra for Computer Graphics" (DigiPro 2015)
//   Bruneton, Neyret & Holzschuch, "Real-time Realistic Ocean Lighting using Seamless Transitions from
//   Geometry to BRDF" (2010) - filtered slope variance
//   Olano & Baker, "LEAN Mapping" (2010)
//   Mihelich & Tcheblokov, "Wakes, Explosions and Lighting: Interactive Water Simulation in Atlas" (GDC 2019)
//   Ang et al., "The Technical Art of Sea of Thieves" (SIGGRAPH 2018 Talks) - foam/scatter breakdown
//   Tessendorf, Reinhard & Gao, "Whitecap Phenomenology for Ocean Surface Simulation" (2023) - foam threshold
// ============================================================================

#define PBRW_CASCADES 4

namespace PBRWater
{
	cbuffer PBRWaterData : register(b7)
	{
		float4 Cascade0[PBRW_CASCADES];  // x 1 / tile size (1/unit), yz tile coordinate of the reference camera (incl. half texel), w texel size (units)
		float4 Cascade1[PBRW_CASCADES];  // x energy-weighted angular frequency (rad/s), y 1 / its Pierson-Moskowitz weight at unlimited fetch, z energy-weighted wavenumber (rad/unit), w std of its compression (open sea)
		float4 Params0;                  // x FFT ocean active, y wind speed (m/s), z peak omega at unlimited fetch, w gravity (units/s^2)
		float4 Params1;                  // xy wind direction, z displacement fade start, w displacement fade end (units)
		float4 Params2;                  // x choppiness, y unused, z displacement bound (units), w highest mip level
		float4 RefCamPos;                // xyz camera position the tile coordinates are relative to, w unused
		float4 Tess0;                    // x tessellation active for this draw, y target triangle size (px), z max factor, w pixels per unit at distance 1
		float4 Draw0;                    // x vertex spacing of this draw (units), y wireframe mode, z debug view, w unused
		float4 Fetch0;                   // xy grid origin (absolute world units), zw 1 / grid extent (units)
		float4 Fetch1;                   // x enabled, y direction count, z wind slice coordinate, w fetch outside the grid (m)
		float4 Terrain0;                 // xy heightmap uv scale, zw heightmap uv offset
		float4 Terrain1;                 // x heightmap z min, y heightmap z max, z enabled, w heightmap texel size (units)
		float4 Shore0;                   // x shore waves on (shore field ready), y blend from the previous shore field (0) to the current one (1), z wave group clock (s, wrapped), w largest run-up (units)
		float4 Shore1;                   // xy unused, z swell Hs (units), w open-sea wind sea Hs (units)
		float4 Ripple0;                  // xy simulation origin (absolute world units), z 1 / simulation extent (units), w enabled
		float4 Ripple1;                  // x displacement scale (units), y normal strength, z unused, w texel size (uv)
		float4 Ripple2;                  // xy previous frame's simulation origin, z step interpolation (now), w step interpolation (previous frame)
		float4 Light0;                   // x base roughness, y subsurface strength, z vanilla fresnel blend, w sun specular intensity
		float4 Light1;                   // x visibility scale, y unused, z refraction distortion scale, w point light specular intensity
		float4 Foam0;                    // foam simulation, fine level: xy grid corner (absolute units), z 1 / its extent (units), w running
		float4 Foam1;                    // x foam amount, y foam animation time (s, wrapped), z whitecap threshold: how far below 1 the foam Jacobian's smallest eigenvalue falls, in local spreads of its trace; w flow wave damping
		float4 Clarity0;                 // x sediment extinction (1/unit per unit turbidity), y base turbidity, z patchiness (0..1), w patch size (units)
		float4 Clarity1;                 // xyz sediment colour (gamma, like the water form colours), w shore resuspension strength
		float4 Clarity2;                 // x river turbidity, y weather turbidity (storm and rain, already scaled), z wading silt strength, w sediment layer height (units)
		float4 Optics0;                  // x scattering anisotropy g, y downwelling attenuation scale, z clarity animation time (s, wrapped), w significant wave height (units)
		float4 Surface0;                 // x wind roughness scale, y gust strength, z gust size (units), w rain intensity (0..1)
		float4 Surface1;                 // x wind slick strength, y unused, z fetch scale, w unused
		float4 Land0;                    // xy loaded terrain grid corner (absolute units), zw 1 / grid extent (units)
		float4 Land1;                    // x enabled, y vertex spacing (units)
		float4 Foam3;                    // x whitecap threshold where the footprint averages many crests (in local spreads), y the eigenvalue's spread per spread of the trace, zw bubble drift (units, wrapped)
		float4 Flow0;                    // worldspace flowmap UV = absXY * xz + yw (x = 0: no flowmap)
		float4 Cascade2[PBRW_CASCADES];  // x height std of the band (units, open sea), y deep-water phase speed g / omega of its mean frequency (units/s), zw unused
		float4 ShoreMap;                 // xy shore field corner (absolute units), zw 1 / its extent (units)
		float4 Swash0;                   // xy swash grid corner (absolute units), z 1 / its extent (units), w enabled
		float4 Shore2;                   // x wave clock (rad, wrapped), y the clock last frame, z its rate: this frame's (eased) angular frequency (rad/s), w peak period Tp (s)
		float4 Shore3;                   // x variation along the coast (0..1), y its drift clock (s, wrapped daily), z unused, w deep-water wavelength L0 (units)
		float4 Shore4;                   // x angular frequency the current shore field was traced for (rad/s), y the previous field's, zw unused
		float4 Shore6;                   // xy previous shore field corner (absolute units), zw 1 / its extent (units)
		float4 Grid0;                    // xy loaded cell grid min corner, zw max corner (absolute units); z <= x: none
		float4 Noise0;                   // xy surface noise grid corner (absolute units), z 1 / its extent (units), w baked (SurfaceNoiseCS)
		float4 Foam2;                    // foam simulation, coarse level: xy grid corner (absolute units), z 1 / its extent (units), w whitecap source width, in local spreads
	}

	Texture2D<float4> RippleTexture : register(t110);     // x height, y height one step earlier, z foam, w silt
	Texture2DArray<float> FetchTexture : register(t111);  // fetch in km, one slice per upwind direction
	Texture2D<float> TerrainHeightTexture : register(t112);
	Texture2D<float4> RipplePreviousTexture : register(t113);           // the ripple state as displayed last frame (motion vectors)
	Texture2D<float> LoadedTerrainTexture : register(t114);             // exact terrain z of the loaded cells (LAND vertex heights)
	Texture2DArray<float4> OceanDisplacement : register(t115);          // xyz displacement (units), w dDx/dy, one slice per cascade
	Texture2DArray<float4> OceanDerivatives : register(t116);           // xy height slopes, zw compression dDx/dx, dDy/dy
	Texture2DArray<float4> OceanSurface : register(t117);               // xy squared slopes, z squared trace of the compression (dDx/dx + dDy/dy)^2, w |laplacian| (1/unit)
	Texture2DArray<float4> OceanPreviousDisplacement : register(t118);  // last frame's displacement (motion vectors)
	Texture2D<float4> ShoreField0 : register(t119);                     // x travel time to the waterline tau (s; up the beach: the waterline's), y signed distance to it (units), z unused, w still water level (absolute z, < -1e6: none)
	Texture2D<float4> ShoreField1 : register(t120);                     // xy grad tau (s/m, seaward; up the beach: the waterline's), z beach slope (tan) at the waterline, w exposure to the swell
	Texture2DArray<float2> FoamSimTexture : register(t121);             // foam on the water (FoamSimCS): x thickness, y freshness; slice 0 fine, 1 coarse
	Texture2D<float4> ShoreFieldPrevious : register(t122);              // ShoreField0 of the field being replaced (Shore0.y < 1)
	Texture2D<float4> SwashWetTexture : register(t123);                 // x wet level, yz unused, w water surface height (units above the still water)
	Texture2D<float> ShoreRoomTexture : register(t124);                 // 0..1 room for the shore wave train: the width of the water against the wavelength
	Texture2D<float2> SurfaceNoiseTexture : register(t126);             // x TurbidityPatches, y WindGust around the camera (SurfaceNoiseCS)
	SamplerState LinearClampSampler : register(s12);
	SamplerState OceanSampler : register(s13);  // anisotropic, wrap

	static const float UnitsPerMetre = METRES_TO_UNITS;
	static const float MetresPerUnit = 1.0 / METRES_TO_UNITS;
	static const float GravityMetres = 9.81;  // m/s^2 (Gravity() is the same in units/s^2)

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
	/// How the shore waves reach this point: the coastline map (ShoreField in WaveModel.h).
	struct ShoreRay
	{
		float travelTime;  // time tau the waves take from here to the waterline (s; up the beach: the waterline's, ~0)
		float phaseLag;    // omega tau (rad): the phase the waves here lag behind the waterline's, at the frequency the field was traced for
		float distance;    // signed distance to the waterline (units): > 0 in the water, < 0 up the beach
		float2 direction;  // unit direction the waves travel in: towards the shore (up the beach: inland); the wind's outside the field
		float slowness;    // |grad tau| (s/unit): 1 / the waves' phase speed
		float slope;       // beach slope (tan) at the waterline
		float exposure;    // 0..1 how open this water is to the swell
		float room;        // 0..1 room for the wave train: none in a stream or pond narrower than the waves, nor where fronts meet
		float weight;      // 0 outside the field, fading in from its border
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
		ShoreRay shore;      // how the shore waves reach here
		float shoreHeight;   // significant height (units) of the sea arriving here from deep water
		float saturation;    // depth-limited breaking: share of the shoaled sea the depth can carry (1 offshore)
		float deepHeight;    // significant height the sea here has in deep water (units, fetch-limited)
		float beach;         // 0..1 surf and swash: waves break on this shore rather than surge against it
		float train;         // 0..1 share of the long waves carried by the shore wave train instead of the FFT (ShoreWaves)
		float4 cascade;      // vertical amplitude multiplier of each cascade: fetch, shoaling, breaking limit, damping, train hand-over
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

	/**
	 * BilinearLoad at `uv` and, when `withGradient`, its central differences one texel either way, `diff` = (B(+x) - B(-x),
	 * B(+y) - B(-y)): the five lookups share one 4 x 4 block of texels (12 loads and one GetDimensions instead of 20 and 5).
	 */
	float BilinearLoadGrad(Texture2D<float> t, float2 uv, bool withGradient, out float2 diff)
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
		diff = 0.0;
		[branch] if (withGradient)
		{
			int2 m = clamp(i0 - 1, 0, hi);
			int2 n = clamp(i0 + 2, 0, hi);
			float zm0 = t.Load(int3(m.x, a.y, 0)), zm1 = t.Load(int3(m.x, b.y, 0));
			float zn0 = t.Load(int3(n.x, a.y, 0)), zn1 = t.Load(int3(n.x, b.y, 0));
			float z0m = t.Load(int3(a.x, m.y, 0)), z1m = t.Load(int3(b.x, m.y, 0));
			float z0n = t.Load(int3(a.x, n.y, 0)), z1n = t.Load(int3(b.x, n.y, 0));
			diff.x = lerp(lerp(z10, zn0, f.x), lerp(z11, zn1, f.x), f.y) - lerp(lerp(zm0, z00, f.x), lerp(zm1, z01, f.x), f.y);
			diff.y = lerp(lerp(z01, z11, f.x), lerp(z0n, z1n, f.x), f.y) - lerp(lerp(z0m, z1m, f.x), lerp(z00, z10, f.x), f.y);
		}
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

	/**
	 * Share of the exact terrain grid at grid coordinate `landUV`: 1 inside, fading to 0 over its outer 2048 units where the
	 * coarse heightmap takes over (0 outside). A hard switch put a straight cliff in the depth along the grid's border:
	 * the coarse heightmap is metres off, and the waves' height and breaking jumped along the line.
	 */
	float ExactTerrainShare(float2 landUV, bool coarseCovers)
	{
		if (Land1.x < 0.5 || any(landUV <= 0.0) || any(landUV >= 1.0))
			return 0.0;
		if (!coarseCovers)
			return 1.0;
		float2 edge = min(landUV, 1.0 - landUV) / Land0.zw;  // units to the border
		return saturate(min(edge.x, edge.y) / 2048.0);
	}

	/**
	 * Terrain height (absolute z) at `absXY` and, when `withGradient`, its gradient (units/unit, uphill): every cell
	 * loaded so far (resolves beaches, rocks and shelves), faded into the coarse heightmap at the grid's border
	 * (ExactTerrainShare). False where neither covers it.
	 */
	bool SampleTerrain(float2 absXY, bool withGradient, out float z, out float2 grad)
	{
		float2 landUV = (absXY - Land0.xy) * Land0.zw;
		bool coarse = Terrain1.z > 0.5 && TerrainInside(absXY);
		float share = ExactTerrainShare(landUV, coarse);
		z = 0.0;
		grad = 0.0;
		[branch] if (share > 0.0)
		{
			// The grid's texels are its vertices, Land1.y apart: the central differences are one texel either way.
			float2 diff;
			z = BilinearLoadGrad(LoadedTerrainTexture, landUV, withGradient, diff);
			grad = diff / (2.0 * Land1.y);
		}
		[branch] if (coarse && share < 1.0)
		{
			float z0 = SampleTerrainZ(absXY);
			z = lerp(z0, z, share);
			if (withGradient) {
				float texel = Terrain1.w;
				float2 coarseGrad = float2(SampleTerrainZ(absXY + float2(texel, 0)), SampleTerrainZ(absXY + float2(0, texel))) - z0;
				grad = lerp(coarseGrad / texel, grad, share);
			}
		}
		return share > 0.0 || coarse;
	}

	/// Terrain height (absolute z) at `absXY`; -1e9 where no terrain covers it.
	float TerrainHeight(float2 absXY)
	{
		float z;
		float2 grad;
		return SampleTerrain(absXY, false, z, grad) ? z : -1e9;
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
		float U = max(Params0.y, 0.5);
		float F = max(fetchMetres, 10.0);
		float omega = 22.0 * pow(abs(GravityMetres * GravityMetres / (U * F)), 1.0 / 3.0);
		return max(omega, Params0.z);
	}

	/// Amplitude gain of a young, fetch-limited sea: its Phillips constant is larger than the open sea's
	/// (JONSWAP alpha = 0.076 (gF/U^2)^-0.22 against Pierson-Moskowitz 0.0081), so the short waves that
	/// survive the fetch carry more energy. Amplitude scales with sqrt(alpha).
	float FetchEnergyGain(float fetchMetres)
	{
		float U = max(Params0.y, 0.5);
		float chi = GravityMetres * max(fetchMetres, 10.0) / (U * U);
		float alpha = 0.076 * pow(chi, -0.22);
		return sqrt(clamp(alpha / 0.0081, 1.0, 4.0));
	}

	/// JONSWAP fetch-limited growth: Hs(F) = 0.0016 sqrt(gF) U / g relative to Hs(open sea) = 0.21 U^2 / g.
	float FetchHeightRatio(float fetchMetres)
	{
		return saturate(0.0016 / 0.21 * sqrt(GravityMetres * max(fetchMetres, 0.0)) / max(Params0.y, 0.5));
	}

	/// 1 inside a uv square, fading to 0 over its outer tenth: grids that move with the player blend in at their border.
	float BorderFade(float2 uv)
	{
		float2 e = saturate(min(uv, 1.0 - uv) * 10.0);
		return e.x * e.y;
	}

	bool ShoreFieldUV(float2 absXY, out float2 uv)
	{
		uv = (absXY - ShoreMap.xy) * ShoreMap.zw;
		return Shore0.x > 0.5 && all(uv > 0.0) && all(uv < 1.0);
	}

	ShoreRay SampleShore(float2 absXY)
	{
		ShoreRay r = (ShoreRay)0;
		r.slope = 1.0;
		r.direction = Params1.xy;
		float2 uv;
		[branch] if (ShoreFieldUV(absXY, uv))
		{
			float4 f0 = ShoreField0.SampleLevel(LinearClampSampler, uv, 0);
			float4 f1 = ShoreField1.SampleLevel(LinearClampSampler, uv, 0);
			r.weight = BorderFade(uv);
			r.travelTime = f0.x;
			r.distance = f0.y;
			// Each field's lag uses the frequency it was traced for (Shore4), never this frame's: those ease with the sea, and
			// omega(t) tau would sweep the crests by tau d(omega)/dt until the frequency settled.
			r.phaseLag = Shore4.x * f0.x;
			// A new field (the player moved on, the waves changed period) takes over from its predecessor over a few seconds.
			// Both are traced from the waterline, so for the same waves their travel times agree and the blend moves no crest;
			// traced for a new period, the crests slide onto their new spacing over the blend.
			[branch] if (Shore0.y < 1.0)
			{
				float2 previousUV = (absXY - Shore6.xy) * Shore6.zw;
				if (all(previousUV > 0.0) && all(previousUV < 1.0)) {
					float2 previous = ShoreFieldPrevious.SampleLevel(LinearClampSampler, previousUV, 0).xy;
					r.travelTime = lerp(previous.x, r.travelTime, Shore0.y);
					r.distance = lerp(previous.y, r.distance, Shore0.y);
					r.phaseLag = lerp(Shore4.y * previous.x, r.phaseLag, Shore0.y);
					r.weight = lerp(BorderFade(previousUV), r.weight, Shore0.y);
				}
			}
			// The waves travel down the travel time, at the phase speed 1 / |grad tau|.
			float2 grad = f1.xy * MetresPerUnit;
			r.slowness = length(grad);
			if (r.slowness > 1e-9)
				r.direction = -grad / r.slowness;
			r.slope = f1.z;
			r.exposure = f1.w;
			r.room = ShoreRoomTexture.SampleLevel(LinearClampSampler, uv, 0);
		}
		return r;
	}

	/// Still water level (absolute z) in the shore field, or a large negative value where there is none.
	float ShoreLevel(float2 absXY)
	{
		float2 uv;
		if (!ShoreFieldUV(absXY, uv))
			return -1e9;
		return ShoreField0.SampleLevel(LinearClampSampler, uv, 0).w;
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
		float k0 = omega * omega / GravityMetres;
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
		return sqrt((0.5 * GravityMetres / omega) / cg);
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
		float z;
		float2 grad;
		if (SampleTerrain(absPos.xy, true, z, grad)) {
			ctx.elevation = z - absPos.z;
			ctx.terrainGrad = grad;
			ctx.hasTerrain = 1.0;
		}
		ctx.depth = ctx.hasTerrain > 0.5 ? max(-ctx.elevation, 0.0) : 1e6;
		ctx.shore = SampleShore(absPos.xy);

		// Depth-limited breaking: in the surf zone the significant height of random waves stays below ~0.6 of the
		// depth (Thornton & Guza 1983). Every cascade keeps that share of its shoaled height.
		float depthMetres = ctx.depth * MetresPerUnit;
		float deepVariance = 0.0;
		float shoaledVariance = 0.0;
		float4 shoaledWeight;  // fetch weight times shoaling of each cascade
		[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
		{
			float fetch = FetchWeight(c, ctx);
			shoaledWeight[c] = fetch * (ctx.hasTerrain > 0.5 ? ShoalingCoefficient(Cascade1[c].x, depthMetres) : 1.0);
			float deep = fetch * Cascade2[c].x;
			float shoaled = shoaledWeight[c] * Cascade2[c].x;
			deepVariance += deep * deep;
			shoaledVariance += shoaled * shoaled;
		}
		ctx.deepHeight = 4.0 * sqrt(deepVariance);
		ctx.saturation = ctx.hasTerrain > 0.5 ? min(1.0, 0.6 * ctx.depth / max(4.0 * sqrt(shoaledVariance), 1e-3)) : 1.0;

		// Breaking or surging: the Iribarren number xi = tan(beta) / sqrt(H0 / L0) of the beach the ray runs up
		// (Battjes 1974) - waves spill (xi < 0.5) and plunge on gentle beaches, but surge up rock faces and
		// cliffs (xi > 3.3) without breaking, running up or leaving foam.
		float steepness = ctx.deepHeight * MetresPerUnit / (Math::TAU * GravityMetres / max(ctx.peakOmega * ctx.peakOmega, 1e-4));
		float iribarren = ctx.shore.slope * rsqrt(max(steepness, 1e-4));
		ctx.beach = smoothstep(3.3, 2.0, iribarren) * ctx.shore.weight * ctx.shore.room * ctx.hasTerrain * saturate(ctx.deepHeight - 1.0);

		// The sea arriving from deep water: the swell where this water is open to it, and the wind sea grown over the
		// open water upwind (an onshore wind brings its sea, an offshore wind leaves only the swell).
		float windSea = Shore1.w * ctx.fetchRatio;
		float swell = Shore1.z * ctx.shore.exposure;
		ctx.shoreHeight = sqrt(swell * swell + windSea * windSea);

		// Hand-over from the FFT to the shore wave train: in deep water (h > L0 / 2) the waves do not feel the bottom
		// and the FFT carries them; from about h = 0.15 L0 on they are refracted, shoaled and break, which the train
		// models along the shore field's wavefronts.
		float deepLength = max(Shore3.w * MetresPerUnit, 1e-3);
		// Narrow water (streams, ponds) has no room for the train; there the FFT sea, fetch- and depth-limited, carries all.
		ctx.train = smoothstep(0.5, 0.15, depthMetres / deepLength) * ctx.shore.weight * ctx.shore.room * ctx.hasTerrain;

		// Each cascade: the fetch-limited deep-water sea, shoaled over the bed (Green's law) and held to the depth-limited
		// breaking height. Near the shore the bands around the shore waves' peak hand their energy over to the shore wave
		// train (ShoreWaves), so the total stays about the same and the shorter chop rides on top of the train.
		[unroll] for (uint i = 0; i < PBRW_CASCADES; i++)
		{
			float overlap = saturate(1.0 - abs(log2(Cascade1[i].x / max(Shore4.x, 1e-3))));
			ctx.cascade[i] = shoaledWeight[i] * ctx.saturation * ctx.damping * (1.0 - ctx.train * overlap);
		}
		return ctx;
	}

	/// Distance fade of the displacement, so far water converges to the flat vanilla plane
	/// (and to the untessellated LOD meshes) instead of popping.
	float DisplacementDistanceFade(float distance)
	{
		return 1.0 - smoothstep(Params1.z, max(Params1.w, Params1.z + 1.0), distance);
	}

	/**
	 * 0 at the border of the loaded cells, 1 from 2048 units inside them (absolute `absXY`). There the cells'
	 * water meets the flat LOD water, whose coarse edge leaves T-junctions against the cells' vertices: any wave left
	 * on that border opened holes between the two.
	 */
	float LoadedGridFade(float2 absXY)
	{
		if (Grid0.z <= Grid0.x)
			return 1.0;
		float2 inside = min(absXY - Grid0.xy, Grid0.zw - absXY);
		return smoothstep(256.0, 2048.0, min(inside.x, inside.y));
	}

	/// DisplacementDistanceFade times LoadedGridFade at camera-relative `positionWS`.
	float DisplacementFade(float3 positionWS)
	{
		return DisplacementDistanceFade(length(positionWS)) * LoadedGridFade(positionWS.xy + FrameBuffer::CameraPosAdjust.xy);
	}

	/// Tile coordinate of camera-relative position `p` in cascade `c` (world-anchored, re-based on the
	/// reference camera so other cameras still sample the same water).
	float2 CascadeUV(float2 p, uint c)
	{
		float2 rel = p + (FrameBuffer::CameraPosAdjust.xy - RefCamPos.xy);
		return rel * Cascade0[c].x + Cascade0[c].yz;
	}

	// ------------------------------------------------------------------------
	// Shore wave train
	//
	// After Fluid Flux 2's coastline waves (Imaginary Blend; DaiZiLing's HLSL port of its MF_SurfaceFlux_Coastline, the
	// Flux2-Shoreline-Wave-HLSL repository): near the shore the sea is one train of its dominant waves running up a
	// coastline map instead of a random field. The map (ShoreField, WaveModel.cpp), built around the player from the real
	// depths, holds the time tau the waves take from each point to the waterline (eikonal |grad tau| = 1 / c(h) from the
	// waterline outwards: Fluid Flux's coastline distance, as a travel time, so the crests slow down and close up over the
	// shoals, turn towards the depth contours and wrap into bays), its gradient (where they travel, and their wavenumber
	// omega |grad tau|), the signed distance to the waterline, the beach slope, the exposure to the swell and the room for
	// the train. As in Fluid Flux:
	//   - the phase runs with time and the travel time to the shore, theta = clock + omega tau, offset along the coast and,
	//     offshore, across it by smooth noise (MF_CoastlineOffsets): each stretch of shore sees the crests at its own time
	//     and the crests bend offshore. The waves come in sets (ShoreGroup);
	//   - the wave's shape follows its phase and its life stage (Fluid Flux's wave profile animation), the stage being how
	//     close the shoaled wave is to breaking;
	//   - the horizontal motion follows the direction of travel, and offshore the waves hand over to the open sea.
	// The profile, a canned animation texture in Fluid Flux, is analytic here:
	//   - unbroken: linear (Airy) particle orbits in finite depth, horizontal semi-axis a coth(kh) (Dean & Dalrymple 1991),
	//     so the crests sharpen and the troughs flatten as the wave feels the bottom; the orbits tilt with the bed slope
	//     (Biesel 1952: the major axis along the slope) and, nearing breaking, pitch forward, so the front face steepens
	//     and the crest leans over it. The orbits are held short of looping, so the surface never folds over itself;
	//   - Green's-law shoaling (ShoalingCoefficient) up to depth-limited breaking at H = 0.78 h (McCowan 1894);
	//   - broken: the bore, a saw-tooth with a steep front and a long back, held to the depth (Svendsen 2006).
	// The CPU mirrors it for swimming, buoyancy and floating objects (WaveModel.cpp ShoreTrain).
	// ------------------------------------------------------------------------

	/// The rms height of a random sea in the surf zone stays below 0.42 h (Thornton & Guza 1982; the shore train's
	/// regular waves carry the rms height). Mirrors PBRWaterModel::BreakerIndex.
	static const float BreakerIndex = 0.42;
	/// Height of the regular waves that carry a random sea's energy: Hrms = Hs / sqrt(2). Mirrors PBRWaterModel::ShoreHeightRatio.
	static const float ShoreHeightRatio = 0.7071;
	/// The horizontal orbits are held to k ax <= this: the crests compress, but the surface never folds over itself (its
	/// Jacobian stays >= 0.3). Mirrors PBRWaterModel::ShoreOrbitLimit.
	static const float ShoreOrbitLimit = 0.7;

	/// The nearest point of the waterline (absolute units): along the direction of travel, by the signed distance.
	float2 CoastPoint(float2 absXY, ShoreRay ray)
	{
		return absXY + ray.direction * ray.distance;
	}

	/**
	 * Wave groups, as a factor on the height (mean ~1). A sea's narrow spectrum brings its waves in sets, a few big ones
	 * between smaller ones (groupiness: Longuet-Higgins 1984), and out of a short-crested sea the sets differ along the
	 * coast. A value noise over a few deep-water wavelengths of coast and over the time the waves reach the waterline
	 * (they carry their height in with them: now plus their travel time), re-drawn every ~7 periods and cross-faded.
	 * Bigger waves then shoal and break further out, run up further and leave their foam higher, each on its own.
	 * Mirrors PBRWaterModel::ShoreGroup.
	 * @param coast   the waterline the waves run to (CoastPoint), so a crest keeps its height all the way in
	 * @param arrival when they reach it (s, on the Shore0.z clock)
	 */
	float ShoreGroup(float2 coast, float arrival)
	{
		float2 q = coast / max(3.0 * Shore3.w, 1.0);
		float s = arrival / max(7.0 * Shore2.w, 1.0);
		float slice = floor(s);
		float t = frac(s);
		t = t * t * (3.0 - 2.0 * t);
		float n = lerp(ValueNoise(q + slice * 17.31), ValueNoise(q + (slice + 1.0) * 17.31), t);
		return lerp(0.45, 1.55, n);
	}

	/// ShoreGroup at one point of the coast, for arrival times from `latest` back to less than one group slice (~7 periods)
	/// before it: the noise of the three slices they can fall in, evaluated once (the swash reads six crests).
	struct ShoreGroupSlices
	{
		float2 q;      // coast point in noise cells
		float slice;   // the slice `latest` falls in
		float3 noise;  // the value noise of slices slice - 1, slice and slice + 1
	};

	ShoreGroupSlices ShoreGroupAt(float2 coast, float latest)
	{
		ShoreGroupSlices o;
		o.q = coast / max(3.0 * Shore3.w, 1.0);
		o.slice = floor(latest / max(7.0 * Shore2.w, 1.0));
		o.noise = float3(ValueNoise(o.q + (o.slice - 1.0) * 17.31), ValueNoise(o.q + o.slice * 17.31), ValueNoise(o.q + (o.slice + 1.0) * 17.31));
		return o;
	}

	/// ShoreGroup(coast, arrival) from the slices around it (ShoreGroupAt); `arrival` no later than their `latest`.
	float ShoreGroup(ShoreGroupSlices g, float arrival)
	{
		float s = arrival / max(7.0 * Shore2.w, 1.0);
		float t = frac(s);
		t = t * t * (3.0 - 2.0 * t);
		float n = floor(s) >= g.slice ? lerp(g.noise.y, g.noise.z, t) : lerp(g.noise.x, g.noise.y, t);
		return lerp(0.45, 1.55, n);
	}

	/**
	 * The shore waves' phase (rad): 0 at a crest, falling along the direction of travel. The clock plus the lag
	 * omega tau behind the waterline (the travel time over the real depths, so the crests refract, slow down and close
	 * up as they shoal), offset along the coast by noise at the nearest point of the waterline, drifting slowly (a sea's
	 * crests are short: they break at different times along a beach). Nothing is offset across the coast: that stretched
	 * and squeezed the wavelength at random along the direction of travel. Mirrors PBRWaterModel::ShorePhase.
	 * @param coast nearest point of the waterline (CoastPoint)
	 * @param clock Shore2.x now, Shore2.y last frame
	 * @param grad  its gradient (rad/unit): minus the local wave vector, offsets included, so the slopes match the mesh
	 */
	float ShorePhase(float2 absXY, float2 coast, ShoreRay ray, float clock, out float2 grad)
	{
		float L0 = max(Shore3.w, 1.0);
		float drift = Shore3.y * 0.004;
		float3 coarse = ValueNoiseGrad(coast / (4.0 * L0) + float2(drift, 0.0));
		float3 fine = ValueNoiseGrad(coast / (1.7 * L0) + 17.1);
		float along = coarse.x * 0.67 + fine.x * 0.33;
		float alongScale = Math::TAU * 1.2 * Shore3.x;
		// The nearest point of the coast moves along the coast with the point, not across it.
		float2 alongGrad = coarse.yz * (0.67 / (4.0 * L0)) + fine.yz * (0.33 / (1.7 * L0));
		alongGrad -= ray.direction * dot(alongGrad, ray.direction);
		grad = -Shore4.x * ray.slowness * ray.direction + alongScale * alongGrad;
		return clock + ray.phaseLag + alongScale * (along - 0.5);
	}

	/// ShorePhase without the gradient.
	float ShorePhase(float2 absXY, float2 coast, ShoreRay ray, float clock)
	{
		float2 grad;
		return ShorePhase(absXY, coast, ray, clock, grad);
	}

	/// The shape of the wave at one phase (metres): Lagrangian displacement along the direction of travel (x) and up (z),
	/// and their derivatives with respect to the phase.
	struct ShoreShape
	{
		float x, z;    // displacement (m)
		float dx, dz;  // d/d(phase) (m/rad)
	};

	/**
	 * How nonlinear the shore waves are, from the Ursell number Ur = 3/8 Hm0 k / (kh)^3 of the local sea (Ruessink,
	 * Ramaekers & van Rijn 2012, fitted to 30,000 surf zone observations): the total nonlinearity
	 * B = 0.857 / (1 + exp((-0.471 - log10 Ur) / 0.297)) and its phase psi = -90 + 90 tanh(0.815 / Ur^0.672) degrees,
	 * as the parameters of Abreu et al.'s (2010) wave form (Ruessink et al. 2012, eqs. 9-11): b = B sqrt(2 / (9 + 2 B^2)),
	 * r = 2b / (1 + b^2), phi = -psi - pi/2. In deep water r is 0 (a sinusoid); shoaling, the crests peak over flat
	 * troughs (skewed, phi = -pi/2); through the surf zone the waves pitch forward into the saw-tooth of a bore
	 * (asymmetric, phi = 0, its front steep). Mirrors PBRWaterModel::ShoreNonlinearity.
	 * @param a     amplitude (m): half the rms height
	 * @param k     wavenumber (rad/m)
	 * @param depth m
	 * @return x r (0..0.66), y phi (rad)
	 */
	float2 ShoreNonlinearity(float a, float k, float depth)
	{
		float kh = max(k * depth, 1e-3);
		float ursell = max(0.375 * (2.0 * 1.41421356 * a) * k / (kh * kh * kh), 1e-6);
		float B = 0.857 / (1.0 + exp((-0.471 - log10(ursell)) / 0.297));
		float psi = 0.5 * Math::PI * (tanh(0.815 / pow(ursell, 0.672)) - 1.0);
		float b = B * sqrt(2.0 / (9.0 + 2.0 * B * B));
		return float2(2.0 * b / (1.0 + b * b), -psi - 0.5 * Math::PI);
	}

	/**
	 * The shape of the shore waves at phase `theta` for amplitude `a` (m), crest at theta = 0: Abreu et al.'s (2010)
	 * nonlinear wave form, eta = a f (sin x + r sin(phi) / (1 + f)) / (1 - r cos(x + phi)), f = sqrt(1 - r^2), which spans
	 * exactly 2a with zero mean for any r and phi, moved so its crest is at theta = 0. The phase rises with time, so the
	 * fast rise ahead of the crest of an asymmetric wave is its steep front. Horizontally the Airy orbits. Mirrors
	 * PBRWaterModel::ShoreProfile.
	 * @param ax           horizontal semi-axis of the orbits (m)
	 * @param nonlinearity ShoreNonlinearity
	 */
	ShoreShape ShoreProfile(float theta, float a, float ax, float2 nonlinearity)
	{
		float r = nonlinearity.x, phi = nonlinearity.y;
		float f = sqrt(1.0 - r * r);
		float sinPhi, cosPhi;
		sincos(phi, sinPhi, cosPhi);
		float c = r * sinPhi / (1.0 + f);
		// The crest, where cos x - r cos phi - c r sin(x + phi) = 0: pi/2 for a skewed wave, acos r for an asymmetric one,
		// refined by Newton's method from between the two.
		float xc = lerp(acos(r), 0.5 * Math::PI, saturate(-phi / (0.5 * Math::PI)));
		[unroll] for (uint i = 0; i < 3; i++)
		{
			float g = cos(xc) - r * cosPhi - c * r * sin(xc + phi);
			float dg = -sin(xc) - c * r * cos(xc + phi);
			xc -= g / (abs(dg) > 1e-3 ? dg : -1e-3);
		}
		float sx, cx, sp, cp;
		sincos(theta + xc, sx, cx);
		sincos(theta + xc + phi, sp, cp);
		float D = 1.0 - r * cp;
		ShoreShape o;
		o.z = a * f * (sx + c) / D;
		o.dz = a * f * (cx * D - (sx + c) * r * sp) / (D * D);
		float s, co;
		sincos(theta, s, co);
		o.x = ax * s;
		o.dx = ax * co;
		return o;
	}

	/**
	 * How much whitewater the shore waves here make, 0..1. Breaking is decided by the height against the depth, so even
	 * ripples "break" in the last centimetres of water; but air is only entrained by real breakers, so the whitewater
	 * grows with them: a trace from a couple of centimetres of significant height (a stream's wavelets), full from 40 cm.
	 */
	float BreakerFoam(WaveContext ctx)
	{
		return smoothstep(0.02, 0.4, ctx.shoreHeight * ctx.damping * MetresPerUnit);
	}

	struct ShoreTrain
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float2 slope;                 // height gradient (units/unit)
		float3 compression;           // horizontal displacement gradient: x dDx/dx, y dDy/dy, z dDx/dy
		float breakStage;             // 0..1 the wave breaking here (pitching forward or broken), weighted by the train's share
		float saturation;             // beta: shoaled height against the breaking limit
		float whitewater;             // foam thickness the breaking crest makes here now (its roller)
		float heightVariance;         // variance of its height over a cycle (units^2)
		float slopeVariance;          // slope variance the scale filters out (for roughness)
		float curvature;              // largest curvature of the profile (1/unit), on its front (tessellation)
	};

	/**
	 * The shore wave train at undisplaced camera-relative `positionWS`, weighted by its share ctx.train. One
	 * implementation for geometry (VS/DS, the occluder grid, ShoreSimCS), shading and tessellation; mirrored on the
	 * CPU (WaveModel.cpp) for swimming, buoyancy and floating objects.
	 * @param scale    vertex spacing or pixel footprint (units): waves shorter than it carries fade out (their slope
	 *                 variance goes to the roughness) and the bore's front is softened to what it resolves; 0: exact
	 * @param previous also evaluate last frame's displacement (motion vectors)
	 */
	ShoreTrain ShoreWaves(float3 positionWS, WaveContext ctx, float scale, bool previous)
	{
		ShoreTrain o = (ShoreTrain)0;
		[branch] if (ctx.train > 0.0)
		{
			ShoreRay ray = ctx.shore;
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float2 coast = CoastPoint(absXY, ray);
			float omega = max(Shore4.x, 1e-3);
			float depth = max(ctx.depth * MetresPerUnit, 0.05);
			float2 gradTheta;  // rad/unit
			float theta = ShorePhase(absXY, coast, ray, Shore2.x, gradTheta);

			// Shoaling (energy flux, Green's law in shallow water) up to the surf zone's limit on the rms height.
			float deepHeight = ShoreHeightRatio * ctx.shoreHeight * MetresPerUnit * ShoreGroup(coast, Shore0.z + ray.travelTime) * ctx.damping;
			float shoaled = deepHeight * ShoalingCoefficient(omega, depth);
			float limit = BreakerIndex * depth;
			o.saturation = shoaled / max(limit, 1e-4);
			float a = 0.5 * min(shoaled, limit);
			// The local wavenumber: the travel time's omega / c with the offsets' compression, so the orbits match the phase.
			float k = max(length(gradTheta) * UnitsPerMetre, omega * omega / GravityMetres);
			float ax = min(a / max(SafeTanh(k * depth), 1e-3), ShoreOrbitLimit / k);
			float2 nonlinearity = ShoreNonlinearity(a, k, depth);
			ShoreShape shape = ShoreProfile(theta, a, ax, nonlinearity);

			// As the cascades' mip selection: what the mesh or the pixel cannot carry (k scale -> pi) fades out.
			float kUnits = k * MetresPerUnit;
			float fade = smoothstep(Math::PI, 0.5 * Math::PI, kUnits * scale);
			float w = ctx.train * fade;
			float units = UnitsPerMetre * w;
			float2 dir = ray.direction;
			o.displacement = float3(dir * shape.x, shape.z) * units;
			o.slope = shape.dz * units * gradTheta;
			o.compression = float3(dir.x * gradTheta.x, dir.y * gradTheta.y, dir.x * gradTheta.y) * (shape.dx * units);
			// The share of the waves breaking here: Thornton & Guza's (1982) weighting (Hrms / (gamma h))^4 of the shoaled
			// height, 1 through the saturated surf zone; none where the waves surge up a steep shore instead (ctx.beach).
			float saturation2 = o.saturation * o.saturation;
			float breaking = min(saturation2 * saturation2, 1.0) * ctx.beach;
			o.breakStage = breaking * ctx.train;
			// Whitewater: the surface roller a breaking wave carries on its front face, from the crest down to the mean
			// water level (Svendsen 1984). The front is where the surface rises towards the crest (the phase rises with
			// time); the edge at the crest is softened over a fifth of the amplitude's slope.
			float front = smoothstep(-0.2, 0.2, shape.dz / max(a, 1e-4));
			float above = saturate(shape.z / max(a, 1e-4));
			o.whitewater = breaking * front * above * ctx.train * BreakerFoam(ctx);
			// Abreu's form: its mean square slope is the sinusoid's (a k)^2 / 2 over sqrt(1 - r^2); its steepest front
			// sqrt((1 + r) / (1 - r)) times the sinusoid's (both independent of phi).
			float r = nonlinearity.x;
			float amplitudeUnits = a * UnitsPerMetre;
			o.heightVariance = 0.5 * amplitudeUnits * amplitudeUnits * w * w;
			o.slopeVariance = (1.0 - fade * fade) * ctx.train * ctx.train * 0.5 * amplitudeUnits * amplitudeUnits * kUnits * kUnits * rsqrt(1.0 - r * r);
			o.curvature = w * kUnits * kUnits * amplitudeUnits * (1.0 + k * ax) * (1.0 + r) / (1.0 - r);
			if (previous) {
				ShoreShape last = ShoreProfile(ShorePhase(absXY, coast, ray, Shore2.y), a, ax, nonlinearity);
				o.previousDisplacement = float3(dir * last.x, last.z) * units;
			}
		}
		return o;
	}

	struct WaveResult
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float3 normal;                // surface normal
		float jacobian;               // < 1 where the surface compresses, < 0 where it folds (breaking)
		float foamEigen;              // smallest eigenvalue of the foam Jacobian: the random sea that whitens (FoamCascades)
		float foamSpread;            // standard deviation of the trace of the foam cascades' Jacobian here (the local random sea's scale)
		float foamGate;              // 0..1 the local sea has waves big enough to whitecap
		float foamCrest;             // 0..1 this point of the surface is high on a crest, where whitecaps break
		float slopeVariance;          // slope variance filtered out of the pixel (for roughness)
		float whitewater;             // foam thickness the breaking shore waves make here now
		float breakStage;             // 0..1 how far the shore waves here have got with breaking (s)
	};

	/// Cascades whose crests whiten: all but the smallest, whose waves (shorter than 0.7 m) only break on the
	/// microscale, without entraining air. Mirrors PBRWaterModel::FoamCascades.
	static const uint FoamCascades = 3;
	/// Whitecaps break on the crests: the surface's height over its local standard deviation from which a compressed
	/// crest can break, and from which it fully can. Mirrors PBRWaterModel::WhitecapCrestLow/High.
	static const float2 WhitecapCrest = float2(0.8, 1.8);

	/// Running sums of the cascades' fields.
	struct WaveSums
	{
		float3 displacement;
		float3 previous;
		float dzdx, dzdy, dxdx, dydy, dxdy;
		float3 foam;           // horizontal displacement gradient of the waves that whiten: x dDx/dx, y dDy/dy, z dDx/dy
		float foamVariance;    // variance of the foam cascades' trace over the sea here
		float heightVariance;  // variance of the height over the sea here (units^2)
		float slopeVariance;
	};

	WaveSums EmptySums()
	{
		WaveSums sums;
		sums.displacement = 0.0;
		sums.previous = 0.0;
		sums.dzdx = sums.dzdy = sums.dxdx = sums.dydy = sums.dxdy = 0.0;
		sums.foam = 0.0;
		sums.foamVariance = 0.0;
		sums.heightVariance = 0.0;
		sums.slopeVariance = 0.0;
		return sums;
	}

	/// Adds cascade `c`: `vertical` scales heights and slopes, `horizontal` the choppy displacement.
	void AddCascade(inout WaveSums sums, uint c, float vertical, float horizontal, float4 displacement, float4 derivatives)
	{
		sums.displacement += float3(displacement.xy * horizontal, displacement.z * vertical);
		sums.dzdx += derivatives.x * vertical;
		sums.dzdy += derivatives.y * vertical;
		float heightSpread = vertical * Cascade2[c].x;
		sums.heightVariance += heightSpread * heightSpread;
		float3 gradient = float3(derivatives.zw, displacement.w) * horizontal;
		sums.dxdx += gradient.x;
		sums.dydy += gradient.y;
		sums.dxdy += gradient.z;
		if (c < FoamCascades) {
			sums.foam += gradient;
			float spread = horizontal * Cascade1[c].w;
			sums.foamVariance += spread * spread;
		}
	}

	/**
	 * Smallest eigenvalue of the Jacobian I + grad D of the horizontal displacement x -> x + D(x) (Tessendorf 2001,
	 * eq. 47): 1 at rest, falling towards 0 as a crest sharpens into a cusp and below 0 where it folds over.
	 * @param gradient x dDx/dx, y dDy/dy, z dDx/dy (= dDy/dx: the choppy displacement is a gradient field)
	 */
	float SmallestEigenvalue(float3 gradient)
	{
		float half = 0.5 * (gradient.x - gradient.y);
		return 1.0 + 0.5 * (gradient.x + gradient.y) - sqrt(half * half + gradient.z * gradient.z);
	}

	/// Adds the shore wave train: its surface, and its height to the crests the whitecaps break on. Its own breaking is
	/// its whitewater (ShoreWaves): its regular crests are no random sea, whose spread the whitecap threshold is
	/// measured in, and counted in the foam Jacobian they whitened every crest of the shoals.
	void AddShoreTrain(inout WaveSums sums, inout WaveResult o, ShoreTrain train)
	{
		sums.displacement += train.displacement;
		sums.previous += train.previousDisplacement;
		sums.dzdx += train.slope.x;
		sums.dzdy += train.slope.y;
		sums.dxdx += train.compression.x;
		sums.dydy += train.compression.y;
		sums.dxdy += train.compression.z;
		sums.heightVariance += train.heightVariance;
		sums.slopeVariance += train.slopeVariance;
		o.breakStage = train.breakStage;
		o.whitewater = train.whitewater;
	}

	WaveResult FinishWaves(WaveSums sums, WaveResult o, WaveContext ctx)
	{
		// Whitecaps need waves of a few tens of centimetres (Beaufort 3: crests begin to break at about 0.6 m; below,
		// wavelets with glassy crests), so a stream's or a pond's wavelets never whiten.
		o.foamGate = smoothstep(0.2, 0.6, ctx.deepHeight * ctx.saturation * ctx.damping * MetresPerUnit);
		// Normal of the parametric surface P(x, y) = (x + Dx, y + Dy, Dz)
		float3 tx = float3(1.0 + sums.dxdx, sums.dxdy, sums.dzdx);
		float3 ty = float3(sums.dxdy, 1.0 + sums.dydy, sums.dzdy);
		// tx and ty become parallel where the surface folds over itself; keep the normal defined there,
		// and facing up where it has folded past vertical (the visible side of the fold).
		float3 n = cross(tx, ty);
		o.normal = SafeNormalize(n.z < 0.0 ? -n : n, float3(0, 0, 1));
		o.jacobian = (1.0 + sums.dxdx) * (1.0 + sums.dydy) - sums.dxdy * sums.dxdy;
		o.foamEigen = SmallestEigenvalue(sums.foam);
		o.foamSpread = sqrt(sums.foamVariance);
		// Short waves break on the crests of the long ones (Banner & Phillips 1974): the whitecaps are on the tops.
		o.foamCrest = smoothstep(WhitecapCrest.x, WhitecapCrest.y, sums.displacement.z * rsqrt(max(sums.heightVariance, 1e-6)));
		o.displacement = sums.displacement;
		o.previousDisplacement = sums.previous;
		o.slopeVariance = sums.slopeVariance;
		return o;
	}

	/**
	 * Waves for geometry (vertex, domain, underwater shaders) at undisplaced camera-relative `positionWS`.
	 * Each cascade is read from the mip whose texels match the vertex spacing, so waves the mesh cannot carry
	 * are averaged out instead of aliasing; the shore wave train is filtered to the spacing the same way.
	 * @param spacing vertex spacing (units); 0 reads the full resolution
	 * @param previous also read last frame's displacement (compiled out when unused)
	 * @param foamStatistics unused (the whitecaps read the filtered Jacobian alone)
	 */
	WaveResult EvaluateWaves(float3 positionWS, WaveContext ctx, float spacing, bool previous, bool foamStatistics)
	{
		WaveResult o = (WaveResult)0;
		WaveSums sums = EmptySums();
		float steepnessScale = rcp(ctx.energyGain);
		if (OceanActive()) {
			[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
			{
				float vertical = ctx.cascade[c];
				[branch] if (vertical > 0.0)
				{
					float3 uv = float3(CascadeUV(positionWS.xy, c), c);
					float lod = spacing > 0.0 ? clamp(log2(spacing / Cascade0[c].w) - 0.5, 0.0, Params2.w) : 0.0;
					float horizontal = vertical * steepnessScale;
					float4 d = OceanDisplacement.SampleLevel(OceanSampler, uv, lod);
					float4 g = OceanDerivatives.SampleLevel(OceanSampler, uv, lod);
					AddCascade(sums, c, vertical, horizontal, d, g);
					if (previous) {
						float4 dp = OceanPreviousDisplacement.SampleLevel(OceanSampler, uv, lod);
						sums.previous += float3(dp.xy * horizontal, dp.z * vertical);
					}
				}
			}
		}
		AddShoreTrain(sums, o, ShoreWaves(positionWS, ctx, spacing, previous));
		return FinishWaves(sums, o, ctx);
	}

	/**
	 * Share of a texture's LEAN / Toksvig variance that is real at this pixel: none while the texture is magnified.
	 * Bilinear magnification interpolates the stored second moment (or the normal) linearly, which leaves a variance
	 * t (1 - t) (g1 - g0)^2 between every pair of texels that no surface has: a grid of rough lines along the texel
	 * rows, read as a maze in the gloss. It is only real once the footprint spans texels, i.e. from mip 0 on along the
	 * footprint's long axis (anisotropic taps average the short one).
	 * @param gx, gy screen derivatives of the texture coordinate, in texels
	 */
	float MinifiedShare(float2 gx, float2 gy)
	{
		return saturate(0.5 * log2(max(max(dot(gx, gx), dot(gy, gy)), 1e-8)));
	}

	/**
	 * Waves for shading at undisplaced camera-relative `positionWS`, filtered over the pixel footprint
	 * (trilinear / anisotropic). The slope variance the filter removes is recovered from the mip-filtered
	 * second moments (LEAN mapping) and returned for the roughness, as is the variance of the shore wave
	 * train where the pixel cannot carry it.
	 */
	WaveResult EvaluateWavesFiltered(float3 positionWS, WaveContext ctx)
	{
		WaveResult o = (WaveResult)0;
		WaveSums sums = EmptySums();
		float steepnessScale = rcp(ctx.energyGain);
		float footprint = max(length(ddx_fine(positionWS.xy)), length(ddy_fine(positionWS.xy)));
		if (OceanActive()) {
			[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
			{
				float vertical = ctx.cascade[c];
				float3 uv = float3(CascadeUV(positionWS.xy, c), c);
				float2 gx = ddx_fine(uv.xy);
				float2 gy = ddy_fine(uv.xy);
				// Sampled unconditionally: gradient sampling must stay in uniform control flow.
				float4 d = OceanDisplacement.SampleGrad(OceanSampler, uv, gx, gy);
				float4 g = OceanDerivatives.SampleGrad(OceanSampler, uv, gx, gy);
				float4 m = OceanSurface.SampleGrad(OceanSampler, uv, gx, gy);
				float horizontal = vertical * steepnessScale;
				AddCascade(sums, c, vertical, horizontal, d, g);
				float variance = max(m.x - g.x * g.x, 0.0) + max(m.y - g.y * g.y, 0.0);
				float texelsPerUV = rcp(Cascade0[c].x * Cascade0[c].w);
				float minified = MinifiedShare(gx * texelsPerUV, gy * texelsPerUV);
				sums.slopeVariance += variance * vertical * vertical * minified;
			}
		}
		AddShoreTrain(sums, o, ShoreWaves(positionWS, ctx, footprint, false));
		return FinishWaves(sums, o, ctx);
	}

	/**
	 * Curvature of the displaced surface along unit direction `e` (units^-1) around undisplaced position
	 * `positionWS`, at the scale `scale` (units) the tessellation is aiming for, so crests get the
	 * subdivision and broad troughs and calm water do not. Uses the mean |laplacian| of the height over
	 * that scale (mip-filtered, so short waves are not averaged away) and the peaked crest and steep front of
	 * the shore wave train, sharpened by the horizontal compression along `e`.
	 */
	float SurfaceCurvature(float3 positionWS, WaveContext ctx, float2 e, float scale)
	{
		float curvature = 0.0;
		float compression = 0.0;
		if (OceanActive()) {
			float steepnessScale = rcp(ctx.energyGain);
			[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
			{
				float vertical = ctx.cascade[c];
				[branch] if (vertical > 0.0)
				{
					float3 uv = float3(CascadeUV(positionWS.xy, c), c);
					float lod = clamp(log2(max(scale, 1e-3) / Cascade0[c].w), 0.0, Params2.w);
					float4 m = OceanSurface.SampleLevel(OceanSampler, uv, lod);
					float4 g = OceanDerivatives.SampleLevel(OceanSampler, uv, lod);
					float shear = OceanDisplacement.SampleLevel(OceanSampler, uv, lod).w;
					curvature += m.w * vertical;
					compression += (g.z * e.x * e.x + 2.0 * shear * e.x * e.y + g.w * e.y * e.y) * vertical * steepnessScale;
				}
			}
		}
		ShoreTrain train = ShoreWaves(positionWS, ctx, scale, false);
		curvature += train.curvature;
		compression += train.compression.x * e.x * e.x + 2.0 * train.compression.z * e.x * e.y + train.compression.y * e.y * e.y;
		float stretch = max(1.0 + compression, 0.2);
		return curvature / (stretch * stretch);
	}

	// ------------------------------------------------------------------------
	// Low-frequency surface noise. Drifting sediment plumes and wind gusts are tens of metres across: the vertices
	// evaluate them (WaterVertex) and the pixels interpolate, which saves their noise octaves on every pixel.
	// ------------------------------------------------------------------------
	float ClarityTime() { return Optics0.z; }

	/// Drifting, plume-shaped sediment patches in [0, 1]: 0 clearest, 1 murkiest.
	float TurbidityPatches(float2 absXY)
	{
		float scale = max(Clarity0.w, 100.0);
		float t = ClarityTime();
		// Plumes drift downwind at a few centimetres per second and slowly change shape.
		float2 q = absXY / scale - Params1.xy * (t * 0.04 * UnitsPerMetre / scale);
		float2 warp = float2(Fbm2(q * 0.5 + float2(3.1, t * 0.003)), Fbm2(q * 0.5 + float2(-7.7, -t * 0.0027))) - 0.5;
		return smoothstep(0.3, 0.72, Fbm(q + warp * 1.4));
	}

	/// Gusts: patches of stronger and weaker wind carried downwind at the wind speed, stretched along the wind.
	/// Returns the factor on the wind speed.
	float WindGust(float2 absXY)
	{
		float U = Params0.y;
		float2 along = Params1.xy;
		float2 across = float2(-along.y, along.x);
		float scale = max(Surface0.z, 100.0);
		float t = ClarityTime();
		float2 q = float2(dot(absXY, along) / (scale * 1.8), dot(absXY, across) / scale);
		q.x -= t * U * UnitsPerMetre / (scale * 1.8);
		float gusts = smoothstep(0.25, 0.75, Fbm2(q + float2(t * 0.01, 3.7)));
		return lerp(1.0, 0.35 + 1.3 * gusts, saturate(Surface0.y));
	}

	/**
	 * The low-frequency surface noise at absolute `absXY`: x TurbidityPatches, y WindGust. Around the camera it is read
	 * from this frame's bake (SurfaceNoiseCS: the noise once per texel instead of once per vertex, which with tessellation
	 * is every generated vertex in every pass); beyond it, and until the bake is ready, it is evaluated here.
	 */
	float2 SurfaceNoise(float2 absXY)
	{
		float2 noise;
		float2 uv = (absXY - Noise0.xy) * Noise0.z;
		[branch] if (Noise0.w > 0.5 && all(uv > 0.002) && all(uv < 0.998))
			noise = SurfaceNoiseTexture.SampleLevel(LinearClampSampler, uv, 0);
		else noise = float2(TurbidityPatches(absXY), WindGust(absXY));
		return noise;
	}

	// ------------------------------------------------------------------------
	// Interactive ripples (GPU wave-equation simulation centred on the camera)
	// ------------------------------------------------------------------------
	/// The ripple simulation's uv at absolute `absXY` for a grid cornered at `origin`; false outside it or when off.
	bool RippleUV(float2 absXY, float2 origin, out float2 uv)
	{
		uv = (absXY - origin) * Ripple0.z;
		return Ripple0.w > 0.5 && all(uv > 0.0) && all(uv < 1.0);
	}

	/// Displayed ripple height. The simulation runs at a fixed rate, so the two stored steps are
	/// interpolated to the render time; `previous` samples last frame's copy for motion vectors.
	float RippleHeight(float3 positionWS, bool previous)
	{
		float2 uv;
		if (!RippleUV(positionWS.xy + FrameBuffer::CameraPosAdjust.xy, previous ? Ripple2.xy : Ripple0.xy, uv))
			return 0.0;
		float2 steps = previous ? RipplePreviousTexture.SampleLevel(LinearClampSampler, uv, 0).xy :
		                          RippleTexture.SampleLevel(LinearClampSampler, uv, 0).xy;
		return lerp(steps.y, steps.x, previous ? Ripple2.w : Ripple2.z) * Ripple1.x * BorderFade(uv);
	}

	float RippleDisplayed(float2 uv)
	{
		float2 steps = RippleTexture.SampleLevel(LinearClampSampler, uv, 0).xy;
		return lerp(steps.y, steps.x, Ripple2.z);
	}

	/// Silt stirred up from the bottom by bodies wading through shallow water (ripple simulation w channel).
	float RippleSilt(float3 positionWS)
	{
		float2 uv;
		if (!RippleUV(positionWS.xy + FrameBuffer::CameraPosAdjust.xy, Ripple0.xy, uv))
			return 0.0;
		return max(RippleTexture.SampleLevel(LinearClampSampler, uv, 0).w, 0.0) * BorderFade(uv);
	}

	/// Ripples cannot be taller than the water is deep; keeps them from dipping below the shore.
	float RippleDepthFade(float depth)
	{
		return saturate(depth / max(3.0 * Ripple1.x, 1.0));
	}

	/// xy = height gradient (units/unit), z = foam
	float3 RippleSlopeFoam(float3 positionWS, float depth)
	{
		float2 uv;
		if (!RippleUV(positionWS.xy + FrameBuffer::CameraPosAdjust.xy, Ripple0.xy, uv))
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
		return float3(grad, foam) * BorderFade(uv);
	}

	// ------------------------------------------------------------------------
	// Swash: the water a breaking wave throws up the beach
	//
	// Up the beach the shore field carries the direction of the waterline the ray leaves from, so the swash is worked out
	// analytically from the crests that reached that point of the waterline: they arrive there at every whole cycle of the
	// waves' phase (ShorePhase at the waterline), so stretches of beach the crests reach at different times run up at
	// different times. Each crest runs up as far as Stockdon et al. (2006) give for its height H (its wave set's), the
	// deep-water wavelength L0 and the beach slope b:
	//     R = 1.1 (0.35 b sqrt(H L0) + 0.5 sqrt(H L0 (0.563 b^2 + 0.004)))
	// and its tip moves ballistically up the slope and back down (Shen & Meyer 1963; Peregrine & Williams 2001),
	// rising at w0 = sqrt(2 g sin^2(b) R) and decelerating vertically at g sin^2(b). On a gentle beach the next uprush
	// overruns the backwash (saturated, dissipative surf); on a steep one each drains before the next.
	// The swash is a thin sheet (SwashTop): it is drawn over the ground per pixel, never by raising the mesh.
	// ------------------------------------------------------------------------

	/**
	 * Height of the swash's water surface above the still water over a bed `elevation` above it (units), for the tip at
	 * `tip` and the lens coefficient `lens` (SwashTip). Shen & Meyer's (1963) solution of the shallow water equations
	 * for the uprush and backwash after a bore collapses at the waterline, in Peregrine & Williams' (2001) closed form:
	 * h = d^2 / (9 g t^2) along the bed d behind the tip, t after the collapse - as deep as the bore at the waterline
	 * (u0^2 / 9g) as the uprush starts, thinning to the tip and, as it drains, everywhere. Seaward of the waterline the
	 * collapsing bore keeps its depth at the waterline over the bed, so it sinks below the waves within a few
	 * centimetres of depth and they carry on. (Holding it at the still water there flattened every trough within reach
	 * of the swash into a plateau, ending in a wall where the reach ended.)
	 */
	float SwashTop(float elevation, float tip, float lens)
	{
		float d = tip - max(elevation, 0.0);
		return d > 0.0 ? elevation + lens * d * d : -1e6;
	}

	/**
	 * The swash over the ray through camera-relative `positionWS`: x the tip's height above the still water (units),
	 * y the lens coefficient 1 / (9 g t^2 sin^2 beta) (1/unit) of the uprush that raises the surface most here
	 * (SwashTop). Zero where waves do not break on the shore (ctx.beach = 0). The tip runs up the beach ballistically
	 * (Shen & Meyer 1963): from the waterline at u0 = sqrt(2 g R), decelerated by gravity along the slope, it reaches
	 * the run-up R and drains back over the same time; R is Stockdon et al.'s (2006) 2 % run-up of the deep-water
	 * significant height. Once a crest has drained its tip keeps falling below the still water instead of resting at
	 * it: the vertices interpolate the tip linearly, and a tip held at 0 next to a running one put the swash front on the
	 * triangle edges. Each crest's height carries its wave group (ShoreGroup), so every uprush reaches its own height.
	 * @param previous at last frame's clock (motion vectors)
	 */
	float2 SwashTip(float3 positionWS, WaveContext ctx, bool previous)
	{
		float2 result = 0.0;
		// Deeper than 1.5 run-ups the sheet stays under the depth-limited trough: nothing there can show the swash, so most
		// of the sea skips it.
		[branch] if (ctx.beach > 0.0 && ctx.elevation > -1.5 * max(Shore0.w, 1.0))
		{
			// Crests reach the waterline once per cycle of the clock, which runs at this frame's frequency.
			float omega = max(Shore2.z, 1e-3);
			float period = Math::TAU / omega;
			float b = clamp(ctx.shore.slope, 0.02, 0.3);
			float sin2 = b * b / (1.0 + b * b);
			float decel = GravityMetres * sin2;  // the tip's height: g sin^2(beta), m/s^2
			float deepLength = Shore3.w * MetresPerUnit;
			float runupPerHeight = 1.1 * (0.35 * b + 0.5 * sqrt(0.563 * b * b + 0.004));  // Stockdon R2 / sqrt(H0 L0)
			float elevation = ctx.elevation * MetresPerUnit;

			// The train's phase here (up the beach the waterline's, carried up the ray), and the time since the last crest
			// reached it: each uprush starts as its crest arrives.
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float2 coast = CoastPoint(absXY, ctx.shore);
			float theta = ShorePhase(absXY, coast, ctx.shore, previous ? Shore2.y : Shore2.x);
			float sinceLast = (theta - Math::TAU * floor(theta / Math::TAU)) / omega;
			// Stockdon's run-up is fitted to the deep-water significant height, not the train's rms height.
			float deepHeight = ctx.shoreHeight * MetresPerUnit * ctx.damping;
			// The crest that arrived tau ago reached the waterline (its wave set's clock) a travel time later.
			float arrival = Shore0.z + ctx.shore.travelTime;
			ShoreGroupSlices groups = ShoreGroupAt(coast, arrival - sinceLast);

			// The uprushes of the last periods (on a gentle beach each lasts several); the oldest fade out before they leave
			// the window, so the tip never pops. The newest crest's own path continues below the still water after it
			// drained. Kept: the one whose water stands highest here.
			const uint candidates = 6;
			float window = candidates * period;
			float tip = -1e9;
			float lens = 0.0;
			float surface = -1e9;
			[unroll] for (uint j = 0; j < candidates; j++)
			{
				float tau = sinceLast + j * period;
				float H = deepHeight * ShoreGroup(groups, arrival - tau);
				float runup = runupPerHeight * sqrt(H * deepLength);
				float w0 = sqrt(2.0 * decel * runup);
				float fade = saturate((window - tau) / period);
				float z = (w0 * tau - 0.5 * decel * tau * tau) * ctx.beach;
				if (z > 0.0)
					z *= fade;
				float t = max(tau, 1e-3);
				float k = 1.0 / (9.0 * GravityMetres * t * t * sin2);  // 1/m
				float s = SwashTop(elevation, z, k);
				// Where none covers the bed, the highest tip (the one the vertices around may need).
				if (s > surface || (s == surface && z > tip)) {
					surface = s;
					tip = z;
					lens = k;
				}
			}
			result = float2(tip * UnitsPerMetre, lens * MetresPerUnit);
		}
		return result;
	}

	/// Largest vertical reach of the surface (units): waves within their statistical bound (doubled for the fetch
	/// energy gain), the shore wave train (shoaled at most to about the deep-water height, its crest up to 2A above
	/// the mean), the swash up the beach and the ripples. Used to clamp, which also flushes NaN.
	float DisplacementLimit()
	{
		float shoreWaves = 2.0 * (max(Shore1.z, 0.0) + max(Shore1.w, 0.0));
		return 2.0 * max(Params2.z, 0.0) + shoreWaves + max(Shore0.w, 0.0) + 4.0 * max(Ripple1.x, 0.0) + 1.0;
	}

	/**
	 * How far above the bed the mesh is held near the waterline (units): enough for the triangles between vertices to
	 * clear the terrain's own triangles (which interpolate the same heights differently), so the pixels that decide
	 * where the water ends exist.
	 */
	float WaterlineLift(float spacing, float2 bedGrad)
	{
		return 3.0 + 0.1 * spacing * min(length(bedGrad), 1.0);
	}

	// ------------------------------------------------------------------------
	// Foam
	//
	// Foam is made where waves break, and nowhere else:
	//   - whitecaps: Tessendorf (2001, sec. 4.6) shows that the choppy displacement folds the surface where the smallest
	//     eigenvalue of its Jacobian falls to zero, and whitecaps are taken where it falls below a threshold (Tessendorf,
	//     Reinhard & Gao 2023). The Jacobian is that of all the bands of the random sea that whiten together
	//     (FoamCascades), so the foam lands on the sharpest crests of the surface as displaced, and only high on a crest
	//     (the height over its local spread, the shore train's included): short waves break on the crests of the long
	//     ones (Banner & Phillips 1974). The wind sets the threshold so that the
	//     sea whitecaps as much as Monahan & O'Muircheartaigh (1980) measured (WaveModel.h Whitecaps), measured against
	//     the local sea's spread of the Jacobian: the whitecap share follows the local wind, not how big the waves have
	//     grown (a bay or the shoals whitecap like the open sea under the same wind);
	//   - whitewater: the rollers on top of the breaking shore waves (ShoreWaves), at the same Jacobian criterion
	//     (Miche);
	//   - the swash: the uprush carries the bores' foam up the beach.
	// FoamSimCS keeps the foam where the water carries it (the grid is indexed by the undisplaced, Lagrangian position)
	// and lets it decay, so a breaking crest leaves a trail, and foam lingers at the water's edge after a wave. The water
	// shader adds the breaking now per pixel, so the crest line is as sharp as the pixels.
	// ------------------------------------------------------------------------

	/// Standard normal CDF (Bowling et al. 2009, within 1.4e-4).
	float NormalCDF(float x)
	{
		x = clamp(x, -8.0, 8.0);
		return rcp(1.0 + exp(-x * (1.5976 + 0.07056 * x * x)));
	}

	/**
	 * 0..1 the share of the pixel or texel breaking as a whitecap now: the foam Jacobian's smallest eigenvalue J
	 * (Tessendorf 2001, sec. 4.6) below the wind's threshold, high on a crest. The threshold is solved on the CPU
	 * (ComputeWhitecaps) from the sea's joint statistics of J and height so that the crests breaking cover what Monahan
	 * & O'Muircheartaigh (1980) measured for the wind: the share of the sea a fixed J (folding, or a crest's downward
	 * acceleration past g / 2) whitens depends on how much of the steepness the cascades resolve, the measured coverage
	 * does not. The filtered J alone: a share of the pixel past the threshold estimated from the variance its filtering
	 * averaged away (Dupuy & Bruneton 2012) put 1-11 % whitecaps on a mid-distance sea whose resolved J never got there
	 * (RenderDoc, debug view 4), as scattered blobs of the foam texture.
	 */
	float Whitecap(WaveResult waves)
	{
		// The foam is the compression itself, as debug view 4 shows it (red: 1 - J).
		return saturate(1.0 - waves.foamEigen) * waves.foamGate;
	}

	/**
	 * The foam the waves make here now: x thickness (1 covers the surface; thicker foam takes longer to thin out),
	 * y freshness (breaking now: brighter, and aerating the water below).
	 */
	float2 BreakingFoam(WaveResult waves)
	{
		float made = Whitecap(waves);
		return float2(made, saturate(made));
	}

	/// BreakingFoam where the waves break: in the water. Up the beach the shore train only carries the waterline's phase
	/// for the swash, which makes its own foam (SwashFoam); its rollers there turned every uprush into a solid white sheet.
	float2 BreakingFoam(WaveResult waves, WaveContext ctx)
	{
		float inWater = ctx.hasTerrain > 0.5 ? smoothstep(0.3 * UnitsPerMetre, -0.3 * UnitsPerMetre, ctx.elevation) : 1.0;
		return BreakingFoam(waves) * inWater;
	}

	/// One level of the foam simulation, bilinear from texel loads: the game re-binds the sampler slots the water shares
	/// with it, and a point sampler turned the foam into blocks of its texels.
	float2 FoamSimLoad(float2 uv, uint level)
	{
		uint w, h, levels;
		FoamSimTexture.GetDimensions(w, h, levels);
		float2 p = uv * float2(w, h) - 0.5;
		int2 i0 = int2(floor(p));
		float2 f = p - i0;
		int2 hi = int2(w, h) - 1;
		int2 a = clamp(i0, 0, hi);
		int2 b = clamp(i0 + 1, 0, hi);
		float2 s00 = FoamSimTexture.Load(int4(a.x, a.y, level, 0));
		float2 s10 = FoamSimTexture.Load(int4(b.x, a.y, level, 0));
		float2 s01 = FoamSimTexture.Load(int4(a.x, b.y, level, 0));
		float2 s11 = FoamSimTexture.Load(int4(b.x, b.y, level, 0));
		return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	}

	/// FoamSimLoad with a cubic B-spline: bilinear left the texels' diamonds in the foam's thresholded edges. Only where
	/// there is foam (the bilinear sample says so) are the 16 texels loaded.
	float2 FoamSimLoadSmooth(float2 uv, uint level)
	{
		float2 state = FoamSimLoad(uv, level);
		[branch] if (max(state.x, state.y) > 1e-3)
		{
			uint w, h, levels;
			FoamSimTexture.GetDimensions(w, h, levels);
			float2 p = uv * float2(w, h) - 0.5;
			int2 i0 = int2(floor(p)) - 1;
			float2 f = p - floor(p);
			float2 f2 = f * f, f3 = f2 * f;
			float2 wt[4] = { (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0,
				(1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, f3 / 6.0 };
			int2 hi = int2(w, h) - 1;
			state = 0.0;
			[unroll] for (int y = 0; y < 4; y++)
			{
				[unroll] for (int x = 0; x < 4; x++)
				{
					int2 t = clamp(i0 + int2(x, y), 0, hi);
					state += FoamSimTexture.Load(int4(t, level, 0)) * (wt[x].x * wt[y].y);
				}
			}
		}
		return state;
	}

	/// The foam simulation's state (x thickness, y freshness) at absolute `absXY`: the fine level, handing over to the
	/// coarse one over its outer tenth, and none past the coarse one (the waves are flat there).
	float2 SampleFoamSim(float2 absXY)
	{
		float2 state = 0.0;
		[branch] if (Foam0.w > 0.5)
		{
			float2 uv1 = (absXY - Foam2.xy) * Foam2.z;
			float2 uv0 = (absXY - Foam0.xy) * Foam0.z;
			float2 edge1 = min(uv1, 1.0 - uv1);
			float2 edge0 = min(uv0, 1.0 - uv0);
			float coarse = saturate(min(edge1.x, edge1.y) * 10.0);
			float fine = saturate(min(edge0.x, edge0.y) * 10.0);
			[branch] if (coarse > 0.0 && fine < 1.0)
				state = FoamSimLoad(uv1, 1) * coarse;
			[branch] if (fine > 0.0)
				state = lerp(state, FoamSimLoadSmooth(uv0, 0), fine);
		}
		return state;
	}

	/// The highest the water reached lately at absolute `absXY` (ShoreSimCS wet level, units above the still water), or
	/// -1e6 outside the swash grid.
	float SwashWetLevel(float2 absXY)
	{
		float level = -1e6;
		[branch] if (Swash0.w > 0.5)
		{
			float2 uv = (absXY - Swash0.xy) * Swash0.z;
			if (all(uv > 0.0) && all(uv < 1.0))
				level = SwashWetTexture.SampleLevel(LinearClampSampler, uv, 0).x;
		}
		return level;
	}

	/**
	 * True where the ground at this pixel lies above the waves and the swash sheet. Decided per pixel from the terrain
	 * heights: the swash sheet ends abruptly at its tip, so the same test on the vertices, interpolated across the
	 * triangles, put the waterline and the swash front on the triangle edges, and stepped them from vertex to vertex
	 * (vertices that also move with the camera's tessellation). Both passes test the same thing and cover the same
	 * pixels; the colour pass then finds the exact edge against the ground in the depth buffer (Water.hlsl), which the
	 * slack keeps inside: the rendered terrain's triangles differ from the bilinear heights by a few centimetres.
	 * @param positionWS camera-relative position of the displaced surface
	 * @param shoreState the vertex shader's SurfaceDisplacement::shore
	 */
	bool AboveWaterline(float3 positionWS, float4 shoreState)
	{
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		float terrainZ = TerrainHeight(absXY);
		if (terrainZ < -1e8)
			return false;
		float bed = terrainZ - (shoreState.w + FrameBuffer::CameraPosAdjust.z);
		if (bed <= max(shoreState.z, SwashTop(bed, shoreState.x, shoreState.y)) + 0.05 * UnitsPerMetre)
			return false;
		// Kept up to where the water reached lately, per pixel: the tip interpolated from the vertices runs ahead of or
		// behind the true one inside each triangle, which cut straight-edged holes into a fresh uprush. The colour pass
		// finds the true waterline there (Water.hlsl) and shows the ground where it is dry.
		return bed > SwashWetLevel(absXY) + 0.05 * UnitsPerMetre;
	}

	// ------------------------------------------------------------------------
	// Full surface displacement used by geometry (VS / DS)
	// ------------------------------------------------------------------------
	struct SurfaceDisplacement
	{
		float3 current;
		float3 previous;
		float4 shore;  // x swash tip (units above the still water), y its lens coefficient (1/unit; SwashTop), z waves without the swash (units above the still water), w still water level (camera-relative z)
	};

	SurfaceDisplacement DisplaceSurface(float3 positionWS, float flowDamping, float spacing)
	{
		SurfaceDisplacement o;
		float damping = DisplacementFade(positionWS) * flowDamping;

		WaveContext ctx = BuildWaveContext(positionWS, damping);
		WaveResult now = EvaluateWaves(positionWS, ctx, spacing, true, false);

		float rippleFade = damping * RippleDepthFade(ctx.depth);
		o.current = now.displacement + float3(0, 0, RippleHeight(positionWS, false) * rippleFade);
		o.previous = now.previousDisplacement + float3(0, 0, RippleHeight(positionWS, true) * rippleFade);

		// The swash: each breaking wave runs up the beach above the still water as a thin sheet and drains back.
		float2 tip = 0.0;
		float2 previousTip = 0.0;
		if (ctx.beach > 0.0) {
			tip = SwashTip(positionWS, ctx, false);
			previousTip = SwashTip(positionWS, ctx, true);
		}
		// The waves alone vary smoothly between vertices; the sheet ends at its tip, so the pixel shader rebuilds it over
		// its own bed (AboveWaterline).
		float waves = o.current.z;
		o.current.z = max(o.current.z, SwashTop(ctx.elevation, tip.x, tip.y));
		o.previous.z = max(o.previous.z, SwashTop(ctx.elevation, previousTip.x, previousTip.y));

		// Near the waterline the mesh never dips under the bed: where the water ends is decided per pixel against the
		// ground behind it (Water.hlsl), so the edge follows the true terrain instead of the triangles cutting through
		// it. Held up within reach of the water at the next vertex (the bed may climb that much in between), and wherever
		// the swash left foam on the sand: the mark alone falls as the crests that made it age out, while their foam stays,
		// and the triangles that sank under the sand there cut straight-edged holes into it.
		if (ctx.hasTerrain > 0.5) {
			float reach = max(0.5 * UnitsPerMetre, 2.0 * spacing * length(ctx.terrainGrad));
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			if (ctx.elevation < max(max(o.current.z, tip.x), SwashWetLevel(absXY)) + reach) {
				float lifted = ctx.elevation + WaterlineLift(spacing, ctx.terrainGrad);
				o.current.z = max(o.current.z, lifted);
				o.previous.z = max(o.previous.z, lifted);
			}
		}

		// Hard bound; clamping also flushes NaN (D3D min/max return the non-NaN operand), so a bad input can
		// never throw the mesh off-screen.
		float limit = DisplacementLimit();
		o.current = clamp(o.current, -limit, limit);
		o.previous = clamp(o.previous, -limit, limit);
		o.shore = float4(tip.x, max(tip.y, 0.0), clamp(waves, -limit, limit), positionWS.z);
		return o;
	}
}

#endif  // __PBR_WATER_HLSLI__
