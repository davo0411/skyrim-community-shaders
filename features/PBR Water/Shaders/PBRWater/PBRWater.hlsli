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
		float4 Shore1;                   // x breaking foam strength, y swash foam strength, z swell Hs (units), w open-sea wind sea Hs (units)
		float4 Ripple0;                  // xy simulation origin (absolute world units), z 1 / simulation extent (units), w enabled
		float4 Ripple1;                  // x displacement scale (units), y normal strength, z unused, w texel size (uv)
		float4 Ripple2;                  // xy previous frame's simulation origin, z step interpolation (now), w step interpolation (previous frame)
		float4 Light0;                   // x base roughness, y subsurface strength, z vanilla fresnel blend, w sun specular intensity
		float4 Light1;                   // x visibility scale, y foam albedo, z refraction distortion scale, w point light specular intensity
		float4 Foam0;                    // x edge foam, y bubble amount, z foam amount, w unused
		float4 Foam1;                    // x unused, y foam animation time (s, wrapped), z wake foam strength, w flow wave damping
		float4 Clarity0;                 // x sediment extinction (1/unit per unit turbidity), y base turbidity, z patchiness (0..1), w patch size (units)
		float4 Clarity1;                 // xyz sediment colour (gamma, like the water form colours), w shore resuspension strength
		float4 Clarity2;                 // x river turbidity, y weather turbidity (storm and rain, already scaled), z wading silt strength, w sediment layer height (units)
		float4 Optics0;                  // x scattering anisotropy g, y downwelling attenuation scale, z clarity animation time (s, wrapped), w significant wave height (units)
		float4 Surface0;                 // x wind roughness scale, y gust strength, z gust size (units), w rain intensity (0..1)
		float4 Surface1;                 // x wind slick strength, y unused, z fetch scale, w unused
		float4 Land0;                    // xy loaded terrain grid corner (absolute units), zw 1 / grid extent (units)
		float4 Land1;                    // x enabled, y vertex spacing (units)
		float4 Foam3;                    // xy foam drift offset (units, wrapped), zw extra bubble drift (units, wrapped)
		float4 Flow0;                    // worldspace flowmap UV = absXY * xz + yw (x = 0: no flowmap)
		float4 Cascade2[PBRW_CASCADES];  // x height std of the band (units, open sea), y deep-water phase speed g / omega of its mean frequency (units/s), zw unused
		float4 ShoreMap;                 // xy shore field corner (absolute units), zw 1 / its extent (units)
		float4 Swash0;                   // xy swash grid corner (absolute units), z 1 / its extent (units), w enabled
		float4 Shore2;                   // x wave clock (rad, wrapped), y the clock last frame, z its rate: this frame's (eased) angular frequency (rad/s), w peak period Tp (s)
		float4 Shore3;                   // x variation along the coast (0..1), y its drift clock (s, wrapped daily), z unused, w deep-water wavelength L0 (units)
		float4 Shore4;                   // x angular frequency the current shore field was traced for (rad/s), y the previous field's, zw unused
		float4 Shore6;                   // xy previous shore field corner (absolute units), zw 1 / its extent (units)
		float4 Grid0;                    // xy loaded cell grid min corner, zw max corner (absolute units); z <= x: none
	}

	Texture2D<float4> RippleTexture : register(t110);     // x height, y height one step earlier, z foam, w silt
	Texture2DArray<float> FetchTexture : register(t111);  // fetch in km, one slice per upwind direction
	Texture2D<float> TerrainHeightTexture : register(t112);
	Texture2D<float4> RipplePreviousTexture : register(t113);           // the ripple state as displayed last frame (motion vectors)
	Texture2D<float> LoadedTerrainTexture : register(t114);             // exact terrain z of the loaded cells (LAND vertex heights)
	Texture2DArray<float4> OceanDisplacement : register(t115);          // xyz displacement (units), w dDx/dy, one slice per cascade
	Texture2DArray<float4> OceanDerivatives : register(t116);           // xy height slopes, zw compression dDx/dx, dDy/dy
	Texture2DArray<float4> OceanSurface : register(t117);               // xy squared slopes, z whitecap foam, w |laplacian| (1/unit)
	Texture2DArray<float4> OceanPreviousDisplacement : register(t118);  // last frame's displacement (motion vectors)
	Texture2D<float4> ShoreField0 : register(t119);                     // x travel time to the waterline tau (s; up the beach: the waterline's), y signed distance to it (units), z unused, w still water level (absolute z, < -1e6: none)
	Texture2D<float4> ShoreField1 : register(t120);                     // xy grad tau (s/m, seaward; up the beach: the waterline's), z beach slope (tan) at the waterline, w exposure to the swell
	Texture2D<float4> ShoreFieldPrevious : register(t122);              // ShoreField0 of the field being replaced (Shore0.y < 1)
	Texture2D<float4> SwashWetTexture : register(t123);                 // x wet level, yz unused, w water surface height (units above the still water)
	Texture2D<float> ShoreRoomTexture : register(t124);                 // 0..1 room for the shore wave train: the width of the water against the wavelength
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
			z = BilinearLoad(LoadedTerrainTexture, landUV);
			if (withGradient) {
				float2 t = Land1.y * Land0.zw;
				float zx = BilinearLoad(LoadedTerrainTexture, landUV + float2(t.x, 0)) - BilinearLoad(LoadedTerrainTexture, landUV - float2(t.x, 0));
				float zy = BilinearLoad(LoadedTerrainTexture, landUV + float2(0, t.y)) - BilinearLoad(LoadedTerrainTexture, landUV - float2(0, t.y));
				grad = float2(zx, zy) / (2.0 * Land1.y);
			}
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

	/**
	 * How much cascade `c` carries the dominant waves here, for its whitecaps: Ocean-URP's per-cascade foam weights
	 * (gasgiant/Ocean-URP, MIT, ~1 : 1/3 : 1/10 from the longest band), placed by the local spectral peak instead of
	 * fixed per cascade. The band holding the peak (and any longer one) counts fully, each band shorter a third as much
	 * as the one before: short waves break too, but their small crests only sprinkle the sea with flecks. The smallest
	 * band never counts (Crest's minimum waves LOD for foam).
	 */
	float DominanceWeight(uint c, float peakOmega)
	{
		if (c + 1 >= PBRW_CASCADES)
			return 0.0;
		float r = Cascade1[c].x / max(peakOmega, 1e-3);
		return pow(max(r, 1.0), -1.2);
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

	/// Waves break at H = 0.78 h (McCowan 1894). Mirrors PBRWaterModel::BreakerIndex.
	static const float BreakerIndex = 0.78;
	/// Height of the regular waves that carry a random sea's energy: Hrms = Hs / sqrt(2). Mirrors PBRWaterModel::ShoreHeightRatio.
	static const float ShoreHeightRatio = 0.7071;
	/// Run-up over Stockdon et al.'s (2006) 2 % value. Mirrors PBRWaterModel::ShoreRunupScale.
	static const float ShoreRunupScale = 1.6;
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

	/**
	 * The shore waves' phase (rad): 0 at a crest, falling along the direction of travel. Fluid Flux's
	 * MF_CoastlineProfileUV with its MF_CoastlineOffsets: the clock plus the lag omega tau behind the waterline, offset
	 * along the coast (noise at the nearest point of the waterline, drifting slowly) and, from a few wavelengths offshore,
	 * across it. Mirrors PBRWaterModel::ShorePhase.
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
		float3 across = ValueNoiseGrad(absXY / (1.3 * L0) + float2(5.3, drift * 1.5));
		float along = coarse.x * 0.67 + fine.x * 0.33;
		float offshoreRamp = ray.distance / L0 * 0.35 - 0.05;
		float offshore = saturate(offshoreRamp);
		float alongScale = Math::TAU * 1.2 * Shore3.x;
		float acrossScale = Math::TAU * 0.35;
		// The nearest point of the coast moves along the coast with the point, not across it.
		float2 alongGrad = coarse.yz * (0.67 / (4.0 * L0)) + fine.yz * (0.33 / (1.7 * L0));
		alongGrad -= ray.direction * dot(alongGrad, ray.direction);
		// The signed distance falls along the direction of travel.
		float2 offshoreGrad = offshoreRamp > 0.0 && offshoreRamp < 1.0 ? -ray.direction * (0.35 / L0) : 0.0;
		grad = -Shore4.x * ray.slowness * ray.direction + alongScale * alongGrad + acrossScale * (offshore * across.yz / (1.3 * L0) + (across.x - 0.5) * offshoreGrad);
		return clock + ray.phaseLag + alongScale * (along - 0.5) + acrossScale * offshore * (across.x - 0.5);
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
	 * Fluid Flux's wave profile: the shape at phase `theta` for amplitude `a` (m). Mirrors PBRWaterModel::ShoreProfile.
	 * @param ax         horizontal semi-axis of the orbits (m)
	 * @param tilt       forward tilt of the orbits (rad): the bed slope, and the crest pitching forward to break
	 * @param bore       0..1 broken
	 * @param frontWidth share of the cycle the bore's front takes
	 */
	ShoreShape ShoreProfile(float theta, float a, float ax, float tilt, float bore, float frontWidth)
	{
		ShoreShape o;
		float s, c, st, ct;
		sincos(theta, s, c);
		sincos(tilt, st, ct);
		// Airy orbits, an ellipse rotated forward by the tilt: at the crest the water leans towards the shore.
		float X = ax * s, Z = a * c;
		float dX = ax * c, dZ = -a * s;
		o.x = X * ct + Z * st;
		o.z = Z * ct - X * st;
		o.dx = dX * ct + dZ * st;
		o.dz = dZ * ct - dX * st;
		[branch] if (bore > 0.0)
		{
			// The phase runs up from a crest (v = 0) along its back to the trough, then up the next crest's front.
			float v = theta / Math::TAU;
			v -= floor(v);
			float back = 1.0 - frontWidth;
			bool onBack = v < back;
			float rate = onBack ? 1.0 / back : 1.0 / frontWidth;  // du / dv
			float u = onBack ? v * rate : 1.0 + (v - back) * rate;
			float su, cu;
			sincos(Math::PI * u, su, cu);
			// Saw-tooth with rounded corners and zero mean: cos(pi u) over the long back and the short front; the broken
			// water keeps a little of its forward surge.
			o.x = lerp(o.x, 0.3 * ax * s, bore);
			o.z = lerp(o.z, a * cu, bore);
			o.dx = lerp(o.dx, 0.3 * ax * c, bore);
			o.dz = lerp(o.dz, -a * Math::PI * su * rate / Math::TAU, bore);
		}
		return o;
	}

	struct ShoreTrain
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float2 slope;                 // height gradient (units/unit)
		float3 compression;           // horizontal displacement gradient: x dDx/dx, y dDy/dy, z dDx/dy
		float2 direction;             // the direction the waves travel in here (unit)
		float breakStage;             // 0..1 the wave breaking here (pitching forward or broken), weighted by the train's share
		float saturation;             // beta: shoaled height against the breaking limit
		float crestFront;             // 0..1 the crest and the front face
		float foamTrail;              // 0..1 just behind a crest that passed: the foam a breaking crest leaves
		float resolved;               // 0..1 how much of the train the mesh or pixel carries (its slopes are only there)
		float slopeVariance;          // slope variance the scale filters out (for roughness)
		float curvature;              // largest curvature of the profile (1/unit), on its front (tessellation)
		float amplitude;              // a (units), weighted
		float k;                      // wavenumber (rad/unit)
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

			// Shoaling up to the breaking limit; the stage is the shoaled height against it (> 1: broken).
			float deepHeight = ShoreHeightRatio * ctx.shoreHeight * MetresPerUnit * ShoreGroup(coast, Shore0.z + ray.travelTime) * ctx.damping;
			float shoaled = deepHeight * ShoalingCoefficient(omega, depth);
			float limit = BreakerIndex * depth;
			o.saturation = shoaled / max(limit, 1e-4);
			float a = 0.5 * min(shoaled, limit);
			// The local wavenumber: the travel time's omega / c with the offsets' compression, so the orbits match the phase.
			float k = max(length(gradTheta) * UnitsPerMetre, omega * omega / GravityMetres);
			float breaking = smoothstep(0.75, 1.0, o.saturation) * ctx.beach;
			float bore = smoothstep(1.0, 1.4, o.saturation) * ctx.beach;
			float lean = breaking * (1.0 - bore);
			float ax = min(a / max(SafeTanh(k * depth), 1e-3), ShoreOrbitLimit / k);
			float tilt = 0.5 * atan(ray.slope) + 0.6 * lean;
			// The bore's front takes a fifth of the cycle, never less than two samples of what has to carry it.
			float frontWidth = clamp(2.0 * k * scale * MetresPerUnit / Math::TAU, 0.2, 0.5);
			ShoreShape shape = ShoreProfile(theta, a, ax, tilt, bore, frontWidth);

			// As the cascades' mip selection: what the mesh or the pixel cannot carry (k scale -> pi) fades out.
			float kUnits = k * MetresPerUnit;
			float fade = smoothstep(Math::PI, 0.5 * Math::PI, kUnits * scale);
			float w = ctx.train * fade;
			float units = UnitsPerMetre * w;
			float2 dir = ray.direction;
			o.displacement = float3(dir * shape.x, shape.z) * units;
			o.slope = shape.dz * units * gradTheta;
			o.compression = float3(dir.x * gradTheta.x, dir.y * gradTheta.y, dir.x * gradTheta.y) * (shape.dx * units);
			o.direction = dir;
			o.breakStage = breaking * ctx.train;
			// Where on the cycle the crest and its foam are: the front (ahead of the crest, just below 2 pi) narrows from
			// half the cycle to the bore's as the wave breaks; v' is 0 at the crest, pi in the trough.
			float v = theta / Math::TAU;
			v -= floor(v);
			float frontShare = lerp(0.5, frontWidth, breaking);
			float remapped = v < 1.0 - frontShare ? v * Math::PI / (1.0 - frontShare) : Math::PI + (v - 1.0 + frontShare) * Math::PI / frontShare;
			// The front just ahead of the crest, and the crest itself (about a fifth of the cycle); behind it the foam it
			// leaves, thinning over a third of a cycle.
			o.crestFront = smoothstep(0.55, 0.95, cos(remapped + 0.4));
			o.foamTrail = remapped < Math::PI ? exp(-5.0 * remapped / Math::PI) : 0.0;
			o.resolved = fade;
			// Mean square slope (a k)^2 / 2 of a sinusoid, steepened by the bore's front over its share of the cycle.
			float steepening = lerp(1.0, 0.5 / frontWidth + 0.5 / (1.0 - frontWidth), bore);
			float amplitudeUnits = a * UnitsPerMetre;
			o.slopeVariance = (1.0 - fade * fade) * ctx.train * ctx.train * 0.5 * amplitudeUnits * amplitudeUnits * kUnits * kUnits * steepening;
			// The sharpened crest (k ax), the pitching front and the bore's front.
			o.curvature = w * kUnits * kUnits * amplitudeUnits * (1.0 + k * ax) * (1.0 + 2.0 * lean + bore / frontWidth);
			o.amplitude = amplitudeUnits * w;
			o.k = kUnits;
			if (previous) {
				ShoreShape last = ShoreProfile(ShorePhase(absXY, coast, ray, Shore2.y), a, ax, tilt, bore, frontWidth);
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
		float slopeVariance;          // slope variance filtered out of the pixel (for roughness)
		float crestTrail;             // whitecap foam coverage: crests whitening now and the foam they left (EvaluateWavesFiltered only)
		float shoreBreak;             // 0..1 a shore wave breaking here now: its crest and front face
		float breakStage;             // 0..1 how far the shore waves here have got with breaking (s)
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

	/// Adds one cascade: `vertical` scales heights and slopes, `horizontal` the choppy displacement.
	void AddCascade(inout WaveSums sums, float vertical, float horizontal, float4 displacement, float4 derivatives)
	{
		sums.displacement += float3(displacement.xy * horizontal, displacement.z * vertical);
		sums.dzdx += derivatives.x * vertical;
		sums.dzdy += derivatives.y * vertical;
		sums.dxdx += derivatives.z * horizontal;
		sums.dydy += derivatives.w * horizontal;
		sums.dxdy += displacement.w * horizontal;
	}

	/**
	 * How much whitewater the shore waves here make, 0..1. Breaking is decided by the height against the depth, so even
	 * ripples "break" in the last centimetres of water; whitewater grows with the breakers: a trace from a couple of
	 * centimetres of significant height (a stream's wavelets), full from 40 cm.
	 */
	float BreakerFoam(WaveContext ctx)
	{
		return smoothstep(0.02, 0.4, ctx.shoreHeight * ctx.damping * MetresPerUnit);
	}

	/// Adds the shore wave train and its foam signals.
	void AddShoreTrain(inout WaveSums sums, inout WaveResult o, ShoreTrain train, WaveContext ctx)
	{
		sums.displacement += train.displacement;
		sums.previous += train.previousDisplacement;
		sums.dzdx += train.slope.x;
		sums.dzdy += train.slope.y;
		sums.dxdx += train.compression.x;
		sums.dydy += train.compression.y;
		sums.dxdy += train.compression.z;
		sums.slopeVariance += train.slopeVariance;
		// Whitewater where the wave really is breaking, read off the surface: the crest and the steep upper part of its
		// front. A crest spills once its front is steeper than about 0.3 (the limiting Stokes crest has 30 degree faces,
		// slope 0.58; Longuet-Higgins & Fox 1977), so the foam comes on with the slope of the whole surface along the
		// direction of travel, in the upper part of the wave. Where the pixel cannot carry the train's slopes (far off)
		// the crest phase stands in. Behind the crest the foam it left thins out.
		float frontSlope = -dot(float2(sums.dzdx, sums.dzdy), train.direction);
		float crestHeight = train.displacement.z / max(2.0 * train.amplitude, 1e-3);  // ~ -0.5 trough .. 0.5-0.7 crest
		float lip = smoothstep(0.15, 0.45, frontSlope) * smoothstep(0.0, 0.35, crestHeight);
		lip = lerp(train.crestFront, lip, train.resolved);
		o.breakStage = train.breakStage;
		o.shoreBreak = train.breakStage * max(lip, 0.35 * train.foamTrail) * BreakerFoam(ctx);
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
	 * Each cascade is read from the mip whose texels match the vertex spacing, so waves the mesh cannot carry
	 * are averaged out instead of aliasing; the shore wave train is filtered to the spacing the same way.
	 * @param spacing vertex spacing (units); 0 reads the full resolution
	 * @param previous also read last frame's displacement (compiled out when unused)
	 */
	WaveResult EvaluateWaves(float3 positionWS, WaveContext ctx, float spacing, bool previous)
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
					AddCascade(sums, vertical, horizontal, d, g);
					if (previous) {
						float4 dp = OceanPreviousDisplacement.SampleLevel(OceanSampler, uv, lod);
						sums.previous += float3(dp.xy * horizontal, dp.z * vertical);
					}
				}
			}
		}
		AddShoreTrain(sums, o, ShoreWaves(positionWS, ctx, spacing, previous), ctx);
		return FinishWaves(sums, o);
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
	 * train where the pixel cannot carry it; whitecaps come from the cascades that carry the waves here.
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
				AddCascade(sums, vertical, horizontal, d, g);
				float variance = max(m.x - g.x * g.x, 0.0) + max(m.y - g.y * g.y, 0.0);
				float texelsPerUV = rcp(Cascade0[c].x * Cascade0[c].w);
				sums.slopeVariance += variance * vertical * vertical * MinifiedShare(gx * texelsPerUV, gy * texelsPerUV);
				// Jacobian foam (OceanAssembleCS), summed over the bands as in GodotOceanWaves. It was laid down by the
				// open-sea band; where fetch or depth calm the band its crests compress less and whiten less, and near the
				// shore the train takes the band's waves (and their foam) over.
				float presence = saturate(horizontal);
				sums.crestTrail += m.z * presence * presence * DominanceWeight(c, ctx.peakOmega);
			}
		}
		AddShoreTrain(sums, o, ShoreWaves(positionWS, ctx, footprint, false), ctx);
		return FinishWaves(sums, o);
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
	 * The swash over the ray through camera-relative `positionWS`: x the tip's height above the still water (units),
	 * y the swash mark - the highest a crest that has turned back reached (units) - and z the foam the crests left on
	 * a bed `elevation` above the still water (0..1). Zero where waves do not break on the shore (ctx.beach = 0). Once
	 * a crest has drained the tip keeps falling below the still water instead of resting at it: the vertices
	 * interpolate the tip linearly, and a tip held at 0 next to a running one put the swash front on the triangle edges.
	 * Each crest's height carries its wave group (ShoreGroup), so every uprush reaches its own height.
	 * @param previous  at last frame's clock (motion vectors)
	 * @param elevation bed above the still water (units) for the foam left behind; -1e9: not needed
	 */
	float3 SwashCrests(float3 positionWS, WaveContext ctx, bool previous, float elevation)
	{
		float3 result = 0.0;
		// Deeper than 1.5 run-ups the sheet stays under the depth-limited trough (>= -0.39 h): nothing there can show the
		// swash, so most of the sea skips it.
		[branch] if (ctx.beach > 0.0 && ctx.elevation > -1.5 * max(Shore0.w, 1.0))
		{
			// Crests reach the waterline once per cycle of the clock, which runs at this frame's frequency.
			float omega = max(Shore2.z, 1e-3);
			float period = Math::TAU / omega;
			float b = clamp(ctx.shore.slope, 0.02, 0.3);
			float decel = GravityMetres * b * b / (1.0 + b * b);  // g sin^2(b), m/s^2
			float deepLength = Shore3.w * MetresPerUnit;
			float runupPerHeight = ShoreRunupScale * 1.1 * (0.35 * b + 0.5 * sqrt(0.563 * b * b + 0.004));  // R / sqrt(H L0)
			float bed = elevation * MetresPerUnit;

			// The train's phase here (up the beach the waterline's, carried up the ray), and the time since the last crest
			// reached it: each uprush starts as its crest arrives.
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float2 coast = CoastPoint(absXY, ctx.shore);
			float theta = ShorePhase(absXY, coast, ctx.shore, previous ? Shore2.y : Shore2.x);
			float sinceLast = (theta - Math::TAU * floor(theta / Math::TAU)) / omega;
			float deepHeight = ShoreHeightRatio * ctx.shoreHeight * MetresPerUnit * ctx.damping;
			// The crest that arrived tau ago reached the waterline (its wave set's clock) a travel time later.
			float arrival = Shore0.z + ctx.shore.travelTime;

			// The crests of the last periods; the oldest fade out before they leave the window, so the tip never pops. The
			// newest crest's own path continues below the still water after it drained.
			const uint candidates = 6;
			float window = candidates * period;
			float tip = -1e9;
			float mark = 0.0;
			float residue = 0.0;
			[unroll] for (uint j = 0; j < candidates; j++)
			{
				float tau = sinceLast + j * period;
				float H = deepHeight * ShoreGroup(coast, arrival - tau);
				float runup = runupPerHeight * sqrt(H * deepLength);
				float w0 = sqrt(2.0 * decel * runup);
				float fade = saturate((window - tau) / period);
				float z = w0 * tau - 0.5 * decel * tau * tau;
				if (z > 0.0)
					z *= fade;
				tip = max(tip, z);
				// Once it has turned back the crest leaves its foam: a line along the highest it reached and a thin lace
				// over the sand below, until the next uprush covers it (the newest crests count most) and slowly drying out.
				float peakTime = w0 / max(decel, 1e-4);
				if (tau > peakTime) {
					float reach = runup * fade;
					mark = max(mark, reach);
					if (bed < reach) {
						float markLine = exp(-(reach - bed) / 0.2);
						residue = max(residue, max(markLine, 0.3) * exp(-(tau - peakTime) / (3.0 * period)) * fade);
					}
				}
			}
			result = float3(float2(tip, mark) * (UnitsPerMetre * ctx.beach), residue * ctx.beach);
		}
		return result;
	}

	/// SwashCrests without the foam left behind: x the tip, y the swash mark (units above the still water).
	float2 SwashTip(float3 positionWS, WaveContext ctx, bool previous)
	{
		return SwashCrests(positionWS, ctx, previous, -1e9).xy;
	}

	/**
	 * Height of the swash's water surface above the still water over a bed `elevation` above it, with the tip at `tip`
	 * (units): a thin sheet thickening behind the tip by 15 % of the height it lies below it, to at most 30 cm, and
	 * nothing beyond the tip.
	 */
	float SwashTop(float elevation, float tip)
	{
		return elevation < tip ? elevation + min(0.15 * (tip - elevation), 0.3 * UnitsPerMetre) : -1e6;
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
		float terrainZ = TerrainHeight(positionWS.xy + FrameBuffer::CameraPosAdjust.xy);
		if (terrainZ < -1e8)
			return false;
		float bed = terrainZ - (shoreState.w + FrameBuffer::CameraPosAdjust.z);
		// Up to the swash mark the colour pass draws the foam the crests left on the sand.
		return bed > max(max(shoreState.z, SwashTop(bed, shoreState.x)), shoreState.y) + 0.05 * UnitsPerMetre;
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
	// Full surface displacement used by geometry (VS / DS)
	// ------------------------------------------------------------------------
	struct SurfaceDisplacement
	{
		float3 current;
		float3 previous;
		float4 shore;  // x swash tip, y swash mark (units above the still water), z waves without the swash (units above the still water), w still water level (camera-relative z)
	};

	SurfaceDisplacement DisplaceSurface(float3 positionWS, float flowDamping, float spacing)
	{
		SurfaceDisplacement o;
		float damping = DisplacementFade(positionWS) * flowDamping;

		WaveContext ctx = BuildWaveContext(positionWS, damping);
		WaveResult now = EvaluateWaves(positionWS, ctx, spacing, true);

		float rippleFade = damping * RippleDepthFade(ctx.depth);
		o.current = now.displacement + float3(0, 0, RippleHeight(positionWS, false) * rippleFade);
		o.previous = now.previousDisplacement + float3(0, 0, RippleHeight(positionWS, true) * rippleFade);

		// The swash: each breaking wave runs up the beach above the still water as a thin sheet and drains back.
		float2 tip = 0.0;
		float previousTip = 0.0;
		if (ctx.beach > 0.0) {
			tip = SwashTip(positionWS, ctx, false);
			previousTip = SwashTip(positionWS, ctx, true).x;
		}
		// The waves alone vary smoothly between vertices; the sheet ends at its tip, so the pixel shader rebuilds it over
		// its own bed (AboveWaterline).
		float waves = o.current.z;
		o.current.z = max(o.current.z, SwashTop(ctx.elevation, tip.x));
		o.previous.z = max(o.previous.z, SwashTop(ctx.elevation, previousTip));

		// Near the waterline the mesh never dips under the bed: where the water ends is decided per pixel against the
		// ground behind it (Water.hlsl), so the edge follows the true terrain instead of the triangles cutting through
		// it. Held up within reach of the water at the next vertex (the bed may climb that much in between).
		if (ctx.hasTerrain > 0.5) {
			float reach = max(0.5 * UnitsPerMetre, 2.0 * spacing * length(ctx.terrainGrad));
			if (ctx.elevation < max(o.current.z + reach, tip.y)) {
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
		o.shore = float4(tip.x, clamp(tip.y, 0.0, limit), clamp(waves, -limit, limit), positionWS.z);
		return o;
	}
}

#endif  // __PBR_WATER_HLSLI__
