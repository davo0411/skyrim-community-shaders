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
// Near the shore the long waves hand over to an analytic wave train refracted from the open sea over the real depths
// (ShoreWaves): it shoals, peaks, breaks and runs up the beach as the swash (SwashTip), and the water ends per pixel
// against the ground behind it (Water.hlsl), never where the mesh's triangles cut the terrain.
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
		float4 Shore2;                   // xyz wave clock of each shore wave component (rad, wrapped), w peak period Tp (s)
		float4 Shore3;                   // xyz the clocks last frame, w deep-water wavelength L0 (units)
		float4 Shore4;                   // xyz angular frequency each component was traced for in the current shore field (rad/s), w unused
		float4 Shore5;                   // xyz clock shift applied when the current field took over (rad): the previous field's clocks are Shore2 - Shore5
		float4 Shore6;                   // xy previous shore field corner (absolute units), zw 1 / its extent (units)
		float4 Shore7;                   // xyz angular frequency each component was traced for in the previous shore field (rad/s), w unused
		float4 Grid0;                    // xy loaded cell grid min corner, zw max corner (absolute units); z <= x: none
	}

	Texture2D<float4> RippleTexture : register(t110);     // x height, y height one step earlier, z foam, w silt
	Texture2DArray<float> FetchTexture : register(t111);  // fetch in km, one slice per upwind direction
	Texture2D<float> TerrainHeightTexture : register(t112);
	Texture2D<float4> RipplePreviousTexture : register(t113);           // the ripple state as displayed last frame (motion vectors)
	Texture2D<float> LoadedTerrainTexture : register(t114);             // exact terrain z of the loaded cells (LAND vertex heights)
	Texture2DArray<float4> OceanDisplacement : register(t115);          // xyz displacement (units), w dDx/dy, one slice per cascade
	Texture2DArray<float4> OceanDerivatives : register(t116);           // xy height slopes, zw compression dDx/dx, dDy/dy
	Texture2DArray<float4> OceanSurface : register(t117);               // xy squared slopes, z unused, w |laplacian| (1/unit)
	Texture2DArray<float4> OceanPreviousDisplacement : register(t118);  // last frame's displacement (motion vectors)
	Texture2D<float4> ShoreField0 : register(t119);                     // xyz arrival time T of each shore wave component (s, field-relative; up the beach: at the waterline), w still water level (absolute z, < -1e6: none)
	Texture2D<float4> ShoreField1 : register(t120);                     // xy grad T of component 0, zw of component 1 (s/m)
	Texture2D<float4> ShoreField2 : register(t121);                     // xy grad T of component 2 (s/m), z beach slope (tan) at the waterline, w exposure to the swell
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
	/// How the shore waves reach this point (shore field, ShoreField in WaveModel.h).
	struct ShoreRay
	{
		float3 travelTime;    // arrival time T of each component's wavefronts (s, field-relative; up the beach: at the waterline)
		float3 slownessX;     // grad T of each component (s/unit), x: it travels along grad T, |grad T| = 1 / c
		float3 slownessY;     // the same, y
		float slope;          // beach slope (tan) at the waterline
		float exposure;       // 0..1 how open this water is to the swell
		float room;           // 0..1 room for the wave train: none in a stream or pond narrower than the waves
		float weight;         // 0 outside the field, fading in from its border
		float3 previousTime;  // while a new field takes over: its predecessor's arrival times (s, the predecessor's reference)
		float blend;          // 0..1 share of the current field's train; the rest is the predecessor's
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
		float2 uv;
		[branch] if (ShoreFieldUV(absXY, uv))
		{
			float4 f0 = ShoreField0.SampleLevel(LinearClampSampler, uv, 0);
			float4 f1 = ShoreField1.SampleLevel(LinearClampSampler, uv, 0);
			float4 f2 = ShoreField2.SampleLevel(LinearClampSampler, uv, 0);
			r.weight = BorderFade(uv);
			r.travelTime = f0.xyz;
			r.blend = 1.0;
			// A new field (the player moved on, the sea turned) takes over from its predecessor over a few seconds: the two
			// trains are cross-faded (SumShoreComponents), each with its own arrival times and frequencies. Blending the
			// times instead moved every crest by omega dT over the blend, so the waves raced, stalled or ran backwards.
			[branch] if (Shore0.y < 1.0)
			{
				float2 previousUV = (absXY - Shore6.xy) * Shore6.zw;
				if (all(previousUV > 0.0) && all(previousUV < 1.0)) {
					r.previousTime = ShoreFieldPrevious.SampleLevel(LinearClampSampler, previousUV, 0).xyz;
					r.blend = Shore0.y;
					r.weight = lerp(BorderFade(previousUV), r.weight, Shore0.y);
				}
			}
			r.slownessX = float3(f1.x, f1.z, f2.x) * MetresPerUnit;
			r.slownessY = float3(f1.y, f1.w, f2.y) * MetresPerUnit;
			r.slope = f2.z;
			r.exposure = f2.w;
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
	// Near the shore the waves stop being a random field: refraction turns their crests towards the depth contours,
	// shoaling raises them and the depth makes them break. The train is the sea's dominant waves as three components
	// at slightly different frequencies and directions (ShoreComponentAmplitude; WaveModel.h): they beat into wave
	// groups (sets) in time and, crossing at small angles, into crest segments that wander along the shore. Component i
	// has phase theta_i = clock_i - omega_i T_i, with T_i the arrival time of its wavefronts, which the shore field
	// solves over the real depths from the open sea (eikonal |grad T| = 1 / c(h)): the crests refract round headlands,
	// wrap into bays and reach every stretch of a beach at its own time.
	//
	// The components are summed as one complex amplitude Z = sum a_i e^(i theta_i): the local wave has height |Z| and
	// phase arg Z, and is then shaped as a whole (its own sinusoids could not steepen or break):
	//   - Green's-law shoaling (ShoalingCoefficient) up to depth-limited breaking at H = 0.78 h (McCowan 1894);
	//   - the profile is shaped in phase, so it is a pure height field and can never fold: crests peak and troughs
	//     flatten as the Ursell number grows (cnoidal waves, Korteweg & de Vries 1895; Wiegel 1960), and once the wave
	//     breaks its front steepens and its back lengthens (the saw-tooth bore, Svendsen 2006).
	// The CPU mirrors it for swimming, buoyancy and floating objects (WaveModel.cpp).
	// ------------------------------------------------------------------------

	/// Relative amplitude of each shore wave component. Mirrors PBRWaterModel::ShoreComponentAmplitude.
	static const float3 ShoreComponentAmplitude = float3(1.0, 0.75, 0.6);
	/// Amplitude per unit significant height: sum a_i^2 / 2 = (Hs / 4)^2. Mirrors PBRWaterModel::ShoreAmplitudeScale.
	static const float ShoreAmplitudeScale = 0.255;

	/// Run-up over Stockdon et al.'s (2006) 2 % value. Mirrors PBRWaterModel::ShoreRunupScale.
	static const float ShoreRunupScale = 1.6;

	/// Deep-water amplitude (m) of each shore wave component here.
	float3 ShoreDeepAmplitudes(WaveContext ctx)
	{
		return ShoreComponentAmplitude * (ShoreAmplitudeScale * ctx.shoreHeight * MetresPerUnit * ctx.damping);
	}

	/**
	 * Wave groups, as a factor on the height (mean ~1). A sea's narrow spectrum brings its waves in sets, a few big ones
	 * between smaller ones (groupiness: Longuet-Higgins 1984), and out of a short-crested sea the sets differ along the
	 * coast. A value noise over a few deep-water wavelengths of coast and over the time the waves left deep water (they
	 * carry their height in with them: now minus their travel time), re-drawn every ~7 periods and cross-faded. Bigger
	 * waves then shoal and break further out, run up further and leave their foam higher, each on its own.
	 * Mirrors PBRWaterModel::ShoreGroup.
	 * @param departure when the waves here left deep water (s, on the Shore0.z clock)
	 */
	float ShoreGroup(float2 absXY, float departure)
	{
		float2 q = absXY / max(3.0 * Shore3.w, 1.0);
		float s = departure / max(7.0 * Shore2.w, 1.0);
		float slice = floor(s);
		float t = frac(s);
		t = t * t * (3.0 - 2.0 * t);
		float n = lerp(ValueNoise(q + slice * 17.31), ValueNoise(q + (slice + 1.0) * 17.31), t);
		return lerp(0.45, 1.55, n);
	}

	/// The shore wave components summed at one point and instant (metres, radians, units).
	struct ShoreSum
	{
		float amplitude;       // |Z| (m)
		float phase;           // arg Z: 0 at a crest, falling along the direction of travel
		float2 phaseGrad;      // grad arg Z (rad/unit): minus the local wave vector
		float2 amplitudeGrad;  // grad |Z| (m/unit)
	};

	/**
	 * Sums the components with deep-water amplitudes `amplitude` at the clocks `clock` (Shore2.xyz, or Shore3.xyz for
	 * last frame). d(arg Z) = sum Re(conj(Z) c_i) d(theta_i) / |Z|^2 and d|Z| = -sum Im(conj(Z) c_i) d(theta_i) / |Z|,
	 * with grad theta_i = -omega_i grad T_i. Each field's spatial phase uses the frequencies it was traced for (Shore4,
	 * Shore7), never this frame's: those ease with the sea, and omega(t) T would sweep the crests by T d(omega)/dt. While
	 * a new field takes over, the predecessor's components are added with the remaining share (its clocks are Shore5
	 * behind; its directions are taken from the current field's).
	 */
	ShoreSum SumShoreComponents(ShoreRay ray, float3 clock, float3 amplitude)
	{
		ShoreSum o;
		float3 s, c;
		sincos(clock - Shore4.xyz * ray.travelTime, s, c);
		float3 a = amplitude * ray.blend;
		float3 re = a * c;
		float3 im = a * s;
		float3 rePrevious = 0.0;
		float3 imPrevious = 0.0;
		[branch] if (ray.blend < 1.0)
		{
			sincos(clock - Shore5.xyz - Shore7.xyz * ray.previousTime, s, c);
			float3 aPrevious = amplitude * (1.0 - ray.blend);
			rePrevious = aPrevious * c;
			imPrevious = aPrevious * s;
		}
		float2 z = float2(dot(re + rePrevious, 1.0.xxx), dot(im + imPrevious, 1.0.xxx));
		float zz = dot(z, z);
		o.amplitude = sqrt(zz);
		o.phase = atan2(z.y, z.x);
		float3 along = z.x * re + z.y * im;
		float3 across = z.x * im - z.y * re;
		float3 alongPrevious = z.x * rePrevious + z.y * imPrevious;
		float3 acrossPrevious = z.x * imPrevious - z.y * rePrevious;
		// Where the components cancel the phase turns fast; the floor keeps it bounded where the height is gone anyway.
		float total = amplitude.x + amplitude.y + amplitude.z;
		float norm = max(zz, 1e-4 * total * total + 1e-12);
		float3 gx = -Shore4.xyz * ray.slownessX;
		float3 gy = -Shore4.xyz * ray.slownessY;
		float3 gxPrevious = -Shore7.xyz * ray.slownessX;
		float3 gyPrevious = -Shore7.xyz * ray.slownessY;
		o.phaseGrad = float2(dot(along, gx) + dot(alongPrevious, gxPrevious), dot(along, gy) + dot(alongPrevious, gyPrevious)) / norm;
		o.amplitudeGrad = -float2(dot(across, gx) + dot(acrossPrevious, gxPrevious), dot(across, gy) + dot(acrossPrevious, gyPrevious)) * rsqrt(norm);
		return o;
	}

	/// The shaped shore wave at one point and one instant (metres and radians).
	struct ShoreProfile
	{
		float height;      // surface height (m)
		float dHeight;     // dz / d(arg Z) (m/rad)
		float shape;       // dz / dA at a fixed phase
		float cap;         // A per unit |Z|: shoaling, held to the breaking limit
		float orbital;     // horizontal orbital displacement along the direction of travel (m)
		float stretch;     // d orbital / d(arg Z) (m/rad)
		float amplitude;   // A (m): half the crest-to-trough height
		float k;           // wavenumber (rad/m)
		float breakStage;  // 0..1 s: the wave is breaking (and on this shore breaks rather than surges)
		float saturation;  // beta: the shoaled amplitude against the breaking limit (>= 1 in the surf zone)
		float phase;       // remapped phase v': 0 at the crest, pi in the trough, the front just below 2 pi
		float frontWidth;  // wf: phase the front spans (rad)
		float peakedness;  // p: cnoidal peakedness of the crests
	};

	/**
	 * Shapes the summed wave.
	 * @param scale size (units) of what has to carry the front (vertex spacing, pixel footprint); 0: exact
	 */
	ShoreProfile EvaluateShoreProfile(WaveContext ctx, ShoreSum sum, float scale)
	{
		ShoreProfile o;
		float omega = max(Shore4.x, 1e-3);
		// Shoaling (Green's law) up to the depth-limited height: a wave breaks at H = 0.78 h (McCowan 1894); in the surf
		// zone the broken bores keep that height against the depth.
		float depth = ctx.depth * MetresPerUnit;
		float h = max(depth, 0.15);
		float shoaled = sum.amplitude * ShoalingCoefficient(omega, depth);
		float breakingAmplitude = 0.5 * 0.78 * depth;
		o.saturation = shoaled / max(breakingAmplitude, 1e-4);
		o.amplitude = min(shoaled, breakingAmplitude);
		o.cap = o.amplitude / max(sum.amplitude, 1e-6);
		o.breakStage = smoothstep(0.75, 1.0, o.saturation) * ctx.beach;
		o.k = Wavenumber(omega, h);

		// The wave travels down the phase: v = 0 is the crest, the front (ahead of the crest) lies just below 2 pi, the
		// back just above 0. Breaking narrows the front to a fifth of the cycle; the phase is remapped so the front takes
		// wf and the back the rest. The front never gets narrower than two samples of what has to carry it, so a coarse
		// mesh gets a softer front instead of aliasing.
		float v = sum.phase - Math::TAU * floor(sum.phase / Math::TAU);
		o.frontWidth = clamp(Math::PI * (1.0 - 0.8 * o.breakStage), 2.0 * o.k * scale * MetresPerUnit, Math::PI);
		float backWidth = Math::TAU - o.frontWidth;
		bool back = v < backWidth;
		float rate = Math::PI / (back ? backWidth : o.frontWidth);  // dv'/dv
		o.phase = back ? v * rate : Math::PI + (v - backWidth) * rate;

		// Cnoidal peakedness from the Ursell number Ur = H L^2 / h^3: sinusoidal below ~10, then the crests peak and the
		// troughs flatten. P = ((1 + cos v') / 2)^p keeps the crest-to-trough height at 2A; its mean
		// Gamma(p + 1/2) / (sqrt(pi) Gamma(p + 1)) ~ 1 / sqrt(pi (p + 1/4)) is removed to keep the mean level.
		float wavelength = Math::TAU / o.k;
		float ursell = 2.0 * o.amplitude * wavelength * wavelength / (h * h * h);
		o.peakedness = 1.0 + 3.0 * saturate((ursell - 10.0) / 50.0);
		float c = max(0.5 + 0.5 * cos(o.phase), 1e-6);
		float P = pow(c, o.peakedness);
		o.shape = 2.0 * (P - rsqrt(Math::PI * (o.peakedness + 0.25)));
		o.height = o.amplitude * o.shape;
		o.dHeight = -o.amplitude * o.peakedness * pow(c, o.peakedness - 1.0) * sin(o.phase) * rate;

		// A small orbital motion along the direction of travel (Q = 1), held to A k <= 0.5 so the Jacobian stays >= 0.5.
		float f = min(1.0, 0.5 / max(o.amplitude * o.k, 1e-6));
		o.orbital = o.amplitude * f * sin(sum.phase);
		o.stretch = o.amplitude * f * cos(sum.phase);
		return o;
	}

	struct ShoreTrain
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float2 slope;                 // height gradient (units/unit)
		float3 compression;           // horizontal displacement gradient: x dDx/dx, y dDy/dy, z dDx/dy
		float2 direction;             // the direction the waves travel in here (unit)
		float breakStage;             // 0..1 the wave breaking here (s), weighted by the train's share
		float saturation;             // beta: shoaled height against the breaking limit
		float crestFront;             // 0..1 the crest and the front face
		float foamTrail;              // 0..1 just behind a crest that passed: the foam a breaking crest leaves
		float resolved;               // 0..1 how much of the train the mesh or pixel carries (its slopes are only there)
		float slopeVariance;          // slope variance the scale filters out (for roughness)
		float curvature;              // largest curvature of the profile (1/unit), on its front (tessellation)
		float amplitude;              // A (units), weighted
		float k;                      // wavenumber (rad/unit)
	};

	/**
	 * The shore wave train at undisplaced camera-relative `positionWS`, weighted by its share ctx.train. One
	 * implementation for geometry (VS/DS, the occluder grid, ShoreSimCS), shading and tessellation; mirrored on the
	 * CPU (WaveModel.cpp) for swimming, buoyancy and floating objects.
	 * @param scale    vertex spacing or pixel footprint (units): waves shorter than it carries fade out (their slope
	 *                 variance goes to the roughness) and the front is softened to what it resolves; 0: exact
	 * @param previous also evaluate last frame's displacement (motion vectors)
	 */
	ShoreTrain ShoreWaves(float3 positionWS, WaveContext ctx, float scale, bool previous)
	{
		ShoreTrain o = (ShoreTrain)0;
		[branch] if (ctx.train > 0.0)
		{
			float3 amplitude = ShoreDeepAmplitudes(ctx) * ShoreGroup(positionWS.xy + FrameBuffer::CameraPosAdjust.xy, Shore0.z - ctx.shore.travelTime.x);
			ShoreSum sum = SumShoreComponents(ctx.shore, Shore2.xyz, amplitude);
			ShoreProfile p = EvaluateShoreProfile(ctx, sum, scale);
			float k = p.k * MetresPerUnit;
			float amplitudeUnits = p.amplitude * UnitsPerMetre;
			// As the cascades' mip selection: what the mesh or the pixel cannot carry (k scale -> pi) fades out.
			float fade = smoothstep(Math::PI, 0.5 * Math::PI, k * scale);
			float w = ctx.train * fade;
			float waveNumber = length(sum.phaseGrad);
			float2 dir = waveNumber > 1e-8 ? -sum.phaseGrad / waveNumber : 0.0;
			o.displacement = float3(dir * p.orbital, p.height) * (UnitsPerMetre * w);
			o.slope = (p.dHeight * sum.phaseGrad + p.shape * p.cap * sum.amplitudeGrad) * (UnitsPerMetre * w);
			o.compression = float3(dir.x * dir.x, dir.y * dir.y, dir.x * dir.y) * (-p.stretch * waveNumber * UnitsPerMetre * w);
			o.direction = dir;
			o.breakStage = p.breakStage * ctx.train;
			o.saturation = p.saturation;
			// The front just ahead of the crest, and the crest itself (about a fifth of the cycle); behind it the foam it
			// leaves, thinning over a third of a cycle.
			o.crestFront = smoothstep(0.55, 0.95, cos(p.phase + 0.4));
			o.foamTrail = p.phase < Math::PI ? exp(-5.0 * p.phase / Math::PI) : 0.0;
			o.resolved = fade;
			// Mean square slope: (A k)^2 / 2 for a sinusoid; the remap steepens the front by pi / wf over its share
			// wf / 2pi of the cycle, and flattens the back alike.
			float backWidth = Math::TAU - p.frontWidth;
			float meanSquareSlope = 0.25 * amplitudeUnits * amplitudeUnits * k * k * (Math::PI / p.frontWidth + Math::PI / backWidth);
			o.slopeVariance = (1.0 - fade * fade) * ctx.train * ctx.train * meanSquareSlope;
			// Peaked crest: z ~ 2A (1 - p v'^2 / 4), with v' = k s pi / wf on the front.
			float frontRate = Math::PI / p.frontWidth;
			o.curvature = w * k * k * amplitudeUnits * p.peakedness * frontRate * frontRate;
			o.amplitude = amplitudeUnits * w;
			o.k = k;
			if (previous) {
				ShoreSum lastSum = SumShoreComponents(ctx.shore, Shore3.xyz, amplitude);
				ShoreProfile last = EvaluateShoreProfile(ctx, lastSum, scale);
				float lastNumber = length(lastSum.phaseGrad);
				float2 lastDir = lastNumber > 1e-8 ? -lastSum.phaseGrad / lastNumber : 0.0;
				o.previousDisplacement = float3(lastDir * last.orbital, last.height) * (UnitsPerMetre * w);
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
	};

	WaveSums EmptySums()
	{
		WaveSums sums;
		sums.displacement = 0.0;
		sums.previous = 0.0;
		sums.dzdx = sums.dzdy = sums.dxdx = sums.dydy = sums.dxdy = 0.0;
		sums.slopeVariance = 0.0;
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
				AddCascade(sums, vertical, horizontal, d, g);
				float variance = max(m.x - g.x * g.x, 0.0) + max(m.y - g.y * g.y, 0.0);
				float texelsPerUV = rcp(Cascade0[c].x * Cascade0[c].w);
				sums.slopeVariance += variance * vertical * vertical * MinifiedShare(gx * texelsPerUV, gy * texelsPerUV);
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
	// Up the beach the shore field carries the arrival times of the waterline the ray leaves from, so the swash is
	// worked out analytically from the crests that reached that stretch of waterline, and stretches of beach the crests
	// reach at different times run up at different times. Each crest runs up as far as Stockdon et al. (2006) give for
	// its height H (2 |Z| when it arrived), the deep-water wavelength L0 and the beach slope b:
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
	 * @param previous  at last frame's clocks (motion vectors)
	 * @param elevation bed above the still water (units) for the foam left behind; -1e9: not needed
	 */
	float3 SwashCrests(float3 positionWS, WaveContext ctx, bool previous, float elevation)
	{
		float3 result = 0.0;
		// Deeper than 1.5 run-ups the sheet stays under the depth-limited trough (>= -0.39 h): nothing there can show the
		// swash, so most of the sea skips it.
		[branch] if (ctx.beach > 0.0 && ctx.elevation > -1.5 * max(Shore0.w, 1.0))
		{
			float omega = max(Shore4.x, 1e-3);
			float period = Math::TAU / omega;
			float b = clamp(ctx.shore.slope, 0.02, 0.3);
			float decel = GravityMetres * b * b / (1.0 + b * b);  // g sin^2(b), m/s^2
			float deepLength = Shore3.w * MetresPerUnit;
			float runupPerHeight = ShoreRunupScale * 1.1 * (0.35 * b + 0.5 * sqrt(0.563 * b * b + 0.004));  // R / sqrt(H L0)
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float departure = Shore0.z - ctx.shore.travelTime.x;
			float bed = elevation * MetresPerUnit;

			// The components at the waterline now (the current field's and, while it takes over, its predecessor's, as in
			// SumShoreComponents), and the time since the last crest reached it (arg Z rises at about omega).
			float3 amplitude = ShoreDeepAmplitudes(ctx);
			float3 a = amplitude * ctx.shore.blend;
			float3 aPrevious = amplitude * (1.0 - ctx.shore.blend);
			float3 clock = previous ? Shore3.xyz : Shore2.xyz;
			float3 s, c, sPrevious, cPrevious;
			sincos(clock - Shore4.xyz * ctx.shore.travelTime, s, c);
			sincos(clock - Shore5.xyz - Shore7.xyz * ctx.shore.previousTime, sPrevious, cPrevious);
			float phase = atan2(dot(a, s) + dot(aPrevious, sPrevious), dot(a, c) + dot(aPrevious, cPrevious));
			float sinceLast = (phase - Math::TAU * floor(phase / Math::TAU)) / omega;
			// Each component turned back to that crest, then one period further for each older crest: complex rotations,
			// so the candidates need no more trigonometry.
			float3 bs, bc, ps, pc, bsPrevious, bcPrevious, psPrevious, pcPrevious;
			sincos(-Shore4.xyz * sinceLast, bs, bc);
			sincos(-Shore4.xyz * period, ps, pc);
			sincos(-Shore7.xyz * sinceLast, bsPrevious, bcPrevious);
			sincos(-Shore7.xyz * period, psPrevious, pcPrevious);
			float3 re = a * (c * bc - s * bs);
			float3 im = a * (s * bc + c * bs);
			float3 rePrevious = aPrevious * (cPrevious * bcPrevious - sPrevious * bsPrevious);
			float3 imPrevious = aPrevious * (sPrevious * bcPrevious + cPrevious * bsPrevious);

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
				float H = 2.0 * length(float2(dot(re + rePrevious, 1.0.xxx), dot(im + imPrevious, 1.0.xxx))) * ShoreGroup(absXY, departure - tau);
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
				float3 nextRe = re * pc - im * ps;
				im = re * ps + im * pc;
				re = nextRe;
				nextRe = rePrevious * pcPrevious - imPrevious * psPrevious;
				imPrevious = rePrevious * psPrevious + imPrevious * pcPrevious;
				rePrevious = nextRe;
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
