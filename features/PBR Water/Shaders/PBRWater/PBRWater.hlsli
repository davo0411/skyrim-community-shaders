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
//   GarrettGunnell/Water (Acerola, MIT, Copyright (c) 2023 Garrett Gunnell) - the open-sea foam: per-cascade Jacobian
//     accumulated in OceanAssembleCS, summed and drawn as below (FftFoam)
// ============================================================================

#define PBRW_CASCADES 4

namespace PBRWater
{
	cbuffer PBRWaterData : register(b7)
	{
		float4 Cascade0[PBRW_CASCADES];  // x 1 / tile size (1/unit), yz tile coordinate of the reference camera (incl. half texel), w texel size (units)
		float4 Cascade1[PBRW_CASCADES];  // x energy-weighted angular frequency (rad/s), y 1 / its Pierson-Moskowitz weight at unlimited fetch, z energy-weighted wavenumber (rad/unit), w unused
		float4 Params0;                  // x FFT ocean active, y wind speed (m/s), z peak omega at unlimited fetch, w gravity (units/s^2)
		float4 Params1;                  // xy wind direction, z displacement fade start, w displacement fade end (units)
		float4 Params2;                  // x choppiness, y unused, z displacement bound (units), w highest mip level
		float4 RefCamPos;                // xyz camera position the tile coordinates are relative to, w unused
		float4 Tess0;                    // x tessellation active for this draw, y target triangle size (px), z max factor, w pixels per unit at distance 1
		float4 Draw0;                    // x vertex spacing of this draw (units), y wireframe mode, z debug view, w unused
		float4 Fetch0;                   // xy grid origin (absolute world units), zw 1 / grid extent (units)
		float4 Fetch1;                   // x enabled, y direction count, z wind slice coordinate, w fetch outside the grid (m)
		float4 Terrain0;                 // xy terrain uv scale, zw terrain uv offset
		float4 Terrain1;                 // x terrain z at texel value 0, y at 1, z enabled, w vertex spacing (units)
		float4 Shore0;                   // x shore waves on (shore field ready), y blend from the previous shore field (0) to the current one (1), z unused, w largest run-up (units)
		float4 Shore1;                   // xy unused, z swell Hs (units), w open-sea wind sea Hs (units)
		float4 Ripple0;                  // xy simulation origin (absolute world units), z 1 / simulation extent (units), w enabled
		float4 Ripple1;                  // x displacement scale (units), y normal strength, z unused, w texel size (uv)
		float4 Ripple2;                  // xy previous frame's simulation origin, z step interpolation (now), w step interpolation (previous frame)
		float4 Light0;                   // x base roughness, y subsurface strength, z vanilla fresnel blend, w sun specular intensity
		float4 Light1;                   // x visibility scale, y unused, z refraction distortion scale, w point light specular intensity
		float4 Foam1;                    // xyz unused, w flow wave damping
		float4 Clarity0;                 // x sediment extinction (1/unit per unit turbidity), y base turbidity, z patchiness (0..1), w patch size (units)
		float4 Clarity1;                 // xyz sediment colour (gamma, like the water form colours), w shore resuspension strength
		float4 Clarity2;                 // x river turbidity, y weather turbidity (storm and rain, already scaled), z wading silt strength, w sediment layer height (units)
		float4 Optics0;                  // x scattering anisotropy g, y downwelling attenuation scale, z clarity animation time (s, wrapped), w significant wave height (units)
		float4 Surface0;                 // x wind roughness scale, y gust strength, z gust size (units), w rain intensity (0..1)
		float4 Surface1;                 // x wind slick strength, y unused, z fetch scale, w unused
		float4 Flow0;                    // worldspace flowmap UV = absXY * xz + yw (x = 0: no flowmap)
		float4 Cascade2[PBRW_CASCADES];  // x height std of the band (units, open sea), y deep-water phase speed g / omega of its mean frequency (units/s), zw unused
		float4 ShoreMap;                 // xy shore field corner (absolute units), zw 1 / its extent (units)
		float4 Swash0;                   // xy swash grid corner (absolute units), z 1 / its extent (units), w enabled
		float4 Shore2;                   // x wave clock (rad, wrapped after ShoreClockCycles waves), y the clock last frame, z its rate (rad/s): the eased angular frequency, w peak period Tp (s)
		float4 Shore3;                   // x swash linger, y undular bore whelps strength, z breaker curl, w deep-water wavelength L0 (units)
		float4 Shore4;                   // x angular frequency the current shore field was traced for (rad/s), y the previous field's, z the sea's own deep-water wavelength L0 (units; run-up), w this frame's time step (s; foam motion vectors)
		float4 Shore6;                   // xy previous shore field corner (absolute units), zw 1 / its extent (units)
		float4 Grid0;                    // xy loaded cell grid min corner, zw max corner (absolute units); z <= x: none
		float4 Noise0;                   // xy surface noise grid corner (absolute units), z 1 / its extent (units), w baked (SurfaceNoiseCS)
		float4 Tune0;                    // x river current speed at full flow strength (m/s), y flow period per metre of a cascade's texel (s/m), z shortest flow period (s), w longest (s)
		float4 Tune1;                    // foam offset of each cascade, longest waves first
		float4 Tune2;                    // x share of a cascade's amplitude that counts fully in the foam, y exponent of the foam's distance falloff, z distance at which it has faded out (m), w roughness added per unit of foam
		float4 Tune3;                    // x ripple horizontal displacement strength, y its clamp (simulation texels), z shore train reach (waves), w coverage of the surf's lingering foam
		float4 Tune4;                    // x variation of the shore waves' heights (0 all alike, 1 Rayleigh-distributed in groups), y mean crest length (deep wavelengths), z detail normal strength in calm air, w in a gale
		float4 Tune5;                    // x point light emitter radius (units), y foam feather, z foam bubble size (m), w shore foam lifetime on the water (s)
		float4 Foam2;                    // xy shore foam grid corner (absolute units), z 1 / its extent (units), w enabled (ShoreFoamCS)
		float4 SunMap0;                  // xy wave heightfield centre (absolute units), z 1 / its extent (units), w drawn this frame
		float4 SunMap1;                  // x its reference level (absolute z), y height range either side of it (units), zw unused
	}

	Texture2D<float4> RippleTexture : register(t110);                   // x height, y height one step earlier, w silt
	Texture2DArray<float> FetchTexture : register(t111);                // fetch in km, one slice per upwind direction
	Texture2D<float> TerrainTexture : register(t112);                   // the worldspace's terrain: its LAND vertices, normalised to Terrain1.xy
	Texture2D<float4> RipplePreviousTexture : register(t113);           // the ripple state as displayed last frame (motion vectors)
	Texture2DArray<float4> OceanDisplacement : register(t115);          // xyz displacement (units), w dDx/dy, one slice per cascade
	Texture2DArray<float4> OceanDerivatives : register(t116);           // xy height slopes, zw compression dDx/dx, dDy/dy
	Texture2DArray<float2> OceanSurface : register(t117);               // squared slopes (LEAN moments)
	Texture2DArray<float4> OceanPreviousDisplacement : register(t118);  // last frame's displacement (motion vectors)
	Texture2DArray<float4> ShoreProfileTexture : register(t114);        // the shore wave through its life (ShoreProfile.h): slice 0 forward, up, their d/dphase; slice 1 foam
	Texture2DArray<float> OceanFoam : register(t121);                   // the FFT foam, one persistent slice per cascade (OceanAssembleCS)
	Texture2D<float4> ShoreField0 : register(t119);                     // x travel time to the waterline tau (s; up the beach: the waterline's), y signed distance to it (units), z offset of the crests along the coast (wave cycles), w still water level (absolute z, < -1e6: none)
	Texture2D<float4> ShoreField1 : register(t120);                     // xy grad tau (s/m, seaward; up the beach: the waterline's), z beach slope (tan) at the waterline, w exposure to the swell
	Texture2D<float4> ShoreFieldPrevious : register(t122);              // ShoreField0 of the field being replaced (Shore0.y < 1)
	Texture2D<float4> SwashWetTexture : register(t123);                 // x wet level, y reach, z unused, w water surface height (units above the still water)
	Texture2D<float> ShoreRoomTexture : register(t124);                 // 0..1 room for the shore wave train: the width of the water against the wavelength
	Texture2D<float2> SurfaceNoiseTexture : register(t126);
	Texture2D<float4> ShoreFoamTexture : register(t127);  // x foam the surf left on the water (ShoreFoamCS); the swash (SwashOutCS): y its surface, z its depth (units above the still water), w its foam             // x TurbidityPatches, y WindGust around the camera (SurfaceNoiseCS)
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
		float along;       // the crests' offset here along the coast (wave cycles)
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
		float waterZ;        // the still water here (absolute z, units)
		float depth;         // water depth below the undisplaced surface (units)
		float elevation;     // bed height above the still water (units): -depth under water, > 0 on the beach
		float damping;       // overall amplitude multiplier (distance fade, flow, ...)
		float2 flow;         // the river current here (units/s, FlowVelocity); 0 on still water: the waves are carried along it (FlowPhases)
		float2 terrainGrad;  // terrain height gradient (units/unit), points uphill i.e. towards the shore
		float hasTerrain;    // 1 when depth and gradient come from the terrain
		ShoreRay shore;      // how the shore waves reach here
		float shoreHeight;   // significant height (units) of the sea arriving here from deep water
		float saturation;    // depth-limited breaking: share of the shoaled sea the depth can carry (1 offshore)
		float deepHeight;    // significant height the sea here has in deep water (units, fetch-limited)
		float beach;         // 0..1 surf and swash: waves break on this shore rather than surge against it
		float iribarren;     // surf similarity tan(beta) / sqrt(H0 / L0) of the beach here (Battjes 1974): spilling < 0.5, plunging to 3.3, surging beyond
		float onshoreWind;   // -1..1 the wind along the waves' travel (ShoreOnshoreWind): > 0 onshore, makes waves spill and break early; < 0 offshore, they plunge and break late
		float train;         // 0..1 share of the long waves carried by the shore wave train instead of the FFT (ShoreWaves)
		float4 cascade;      // vertical amplitude multiplier of each cascade: fetch, shoaling, breaking limit, damping, train hand-over
		float4 sea;          // the same before the hand-over: the sea here, whether the FFT or the shore train carries it
	};

	/**
	 * Bilinear lookup from texel loads, clamped to the edge, and, when `withGradient`, its central differences one texel
	 * either way, `diff` = (B(+x) - B(-x), B(+y) - B(-y)). The terrain heights must never depend on the sampler bound to the
	 * slot: the game re-binds sampler slots behind our back, and a point sampler turns the depth into blocks (visible as
	 * pixelated shore waves and foam). The five lookups share one 4 x 4 block of texels (12 loads and one GetDimensions
	 * instead of 20 and 5).
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

	/**
	 * Terrain height (absolute z) at `absXY` and, when `withGradient`, its gradient (units/unit, uphill), from the
	 * worldspace's LAND vertices. False outside them.
	 */
	bool SampleTerrain(float2 absXY, bool withGradient, out float z, out float2 grad)
	{
		z = 0.0;
		grad = 0.0;
		float2 uv = absXY * Terrain0.xy + Terrain0.zw;
		if (Terrain1.z < 0.5 || any(uv <= 0.0) || any(uv >= 1.0))
			return false;
		// The texels are the vertices, Terrain1.w apart: the central differences are one texel either way.
		float2 diff;
		z = lerp(Terrain1.x, Terrain1.y, BilinearLoadGrad(TerrainTexture, uv, withGradient, diff));
		grad = diff * (Terrain1.y - Terrain1.x) / (2.0 * Terrain1.w);
		return true;
	}

	/// Terrain height (absolute z) at `absXY`; -1e9 where no terrain covers it.
	float TerrainHeight(float2 absXY)
	{
		float z;
		float2 grad;
		return SampleTerrain(absXY, false, z, grad) ? z : -1e9;
	}

	/**
	 * The highest the rendered terrain can stand at `absXY` (absolute z); -1e9 where no terrain covers it. The game draws
	 * each quad of LAND vertices as two triangles, which differ from the bilinear height (TerrainHeight) by up to a
	 * quarter of the quad's twist |z00 + z11 - z10 - z01| at its centre and not at all on its edges, whichever diagonal
	 * it splits along.
	 */
	float TerrainCeiling(float2 absXY)
	{
		float2 uv = absXY * Terrain0.xy + Terrain0.zw;
		float ceiling = -1e9;
		[branch] if (Terrain1.z > 0.5 && all(uv > 0.0) && all(uv < 1.0))
		{
			uint w, h;
			TerrainTexture.GetDimensions(w, h);
			float2 p = uv * float2(w, h) - 0.5;
			int2 i0 = int2(floor(p));
			float2 f = p - i0;
			int2 hi = int2(w, h) - 1;
			int2 a = clamp(i0, 0, hi);
			int2 b = clamp(i0 + 1, 0, hi);
			float z00 = TerrainTexture.Load(int3(a.x, a.y, 0));
			float z10 = TerrainTexture.Load(int3(b.x, a.y, 0));
			float z01 = TerrainTexture.Load(int3(a.x, b.y, 0));
			float z11 = TerrainTexture.Load(int3(b.x, b.y, 0));
			float bilinear = lerp(lerp(z00, z10, f.x), lerp(z01, z11, f.x), f.y);
			// Each triangle stands off the bilinear surface by twist * min(u, v) * (1 - max(u, v)) (u, v measured from its
			// diagonal's corner): nothing on the quad's edges, a quarter of the twist at its centre. The quarter taken over
			// the whole quad stepped at every quad edge, where the twist changes, and stood the mesh held above the bed in
			// straight-edged, right-angled terraces along the LAND grid.
			float twist = z00 + z11 - z10 - z01;
			float2 g = float2(1.0 - f.x, f.y);
			float alongMain = min(f.x, f.y) * (1.0 - max(f.x, f.y));  // split along 00-11
			float alongCross = min(g.x, g.y) * (1.0 - max(g.x, g.y));  // split along 10-01
			ceiling = lerp(Terrain1.x, Terrain1.y, bilinear + max(twist * alongMain, -twist * alongCross));
		}
		return ceiling;
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

	/// No wave is steeper than this height over wavelength (Miche 1944). Mirrors PBRWaterModel::ShoreSteepestWave.
	static const float ShoreSteepestWave = 0.142;
	/// How many waves out from the shore the train reaches, fading in over the outer half (Tune3.z). Mirrors PBRWaterModel::ShoreTuning::trainCycles.
	/// Waves break when their height reaches this share of the depth under the crest (McCowan 1894). Mirrors
	/// PBRWaterModel::ShoreBreakerLimit.
	static const float ShoreBreakerLimit = 0.78;
	/// Height of the regular waves that carry a random sea's energy: Hrms = Hs / sqrt(2). Mirrors PBRWaterModel::ShoreHeightRatio.
	static const float ShoreHeightRatio = 0.7071;
	/// Coverage of the foam that lingers in the surf between the breakers (lace, through the foam texture's black point): Tune3.w.

	/// The share of a cascade of mean frequency `omega` that the shore train takes over: all of it up to twice the train's
	/// frequency, none from four times it. Mirrors PBRWaterModel::ShoreHandOver.
	float ShoreHandOver(float omega)
	{
		return 1.0 - smoothstep(1.0, 2.0, log2(omega / max(Shore4.x, 1e-3)));
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
			r.along = f0.z;
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
					float3 previous = ShoreFieldPrevious.SampleLevel(LinearClampSampler, previousUV, 0).xyz;
					r.travelTime = lerp(previous.x, r.travelTime, Shore0.y);
					r.distance = lerp(previous.y, r.distance, Shore0.y);
					r.along = lerp(previous.z, r.along, Shore0.y);
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

	/**
	 * The shore train's shoaled height here against the breaker limit of the depth: 1 where its waves break, more
	 * inshore of that (the surf). PBRWaterModel::ShoreSurfShare works out the same ratio.
	 * @param shoreHeightMetres significant height of the sea arriving from deep water (m)
	 * @param depthMetres       the depth here (m)
	 */
	float ShoreBreakRatio(float shoreHeightMetres, float depthMetres)
	{
		float depth = max(depthMetres, 0.05);
		float shoaled = ShoreHeightRatio * shoreHeightMetres * ShoalingCoefficient(max(Shore4.x, 1e-3), depth);
		return shoaled / (ShoreBreakerLimit * depth);
	}

	/**
	 * 0..1 how close the shore train is to breaking on the bed here: its shoaled height against the breaker limit.
	 * Offshore the FFT sea and the train ride together; only in the surf, where the train's waves break, does it take
	 * the FFT's long waves over (ShoreHandOver). Mirrors PBRWaterModel::ShoreSurfShare.
	 * @param shoreHeightMetres significant height of the sea arriving from deep water (m)
	 * @param depthMetres       the depth here (m)
	 */
	float ShoreSurfShare(float shoreHeightMetres, float depthMetres)
	{
		return smoothstep(0.3, 0.8, ShoreBreakRatio(shoreHeightMetres, depthMetres));
	}

	/// No wave is higher than this multiple of the sea's height (ShoreSet). Mirrors PBRWaterModel::ShoreSetCeiling.
	static const float ShoreSetCeiling = 2.5;
	/// How far a full onshore (+1) or offshore (-1) wind moves the breaker index: onshore wind makes waves break earlier, in
	/// deeper water, and spill; offshore wind holds them up until the water is shallower and they plunge (Douglass 1990:
	/// some 10 % at 10 m/s; King and Baker 1996; Feddersen and Veron 2005). Mirrors PBRWaterModel::ShoreWindBreaking.
	static const float ShoreWindBreaking = 0.12;

	/**
	 * The signed share of the wind that reaches the waves here, -1..1: the wind's strength (a breeze below 2 m/s does
	 * nothing, a strong breeze at 12 m/s all it can) times its component along the waves' travel. Positive: onshore.
	 * Mirrors PBRWaterModel::ShoreOnshoreWind.
	 * @param direction the waves' direction of travel (ShoreRay::direction)
	 */
	float ShoreOnshoreWind(float2 direction)
	{
		return smoothstep(2.0, 12.0, Params0.y) * dot(Params1.xy, direction);
	}

	WaveContext BuildWaveContext(float3 positionWS, float damping)
	{
		WaveContext ctx;
		float3 absPos = positionWS + FrameBuffer::CameraPosAdjust.xyz;
		ctx.waterZ = absPos.z;
		ctx.fetchMetres = SampleFetchMetres(absPos.xy);
		ctx.peakOmega = FetchPeakOmega(ctx.fetchMetres);
		ctx.energyGain = FetchEnergyGain(ctx.fetchMetres);
		ctx.fetchRatio = FetchHeightRatio(ctx.fetchMetres);
		ctx.depth = 1e6;
		ctx.elevation = -1e6;
		ctx.terrainGrad = 0.0;
		ctx.hasTerrain = 0.0;
		ctx.damping = damping;
		ctx.flow = 0.0;

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
		ctx.onshoreWind = ShoreOnshoreWind(ctx.shore.direction);

		// Depth-limited breaking: in the surf zone the significant height of random waves stays below ~0.6 of the
		// depth (Thornton & Guza 1983). Every cascade keeps that share of its shoaled height.
		float depthMetres = ctx.depth * MetresPerUnit;
		float deepVariance = 0.0;
		float shoaledVariance = 0.0;
		float4 shoaledWeight;  // fetch weight times shoaling of each cascade
		[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
		{
			float fetch = FetchWeight(c, ctx);
			// Waves only feel a bottom shallower than about half their wavelength (k h < pi): deeper, Green's law leaves them be.
			shoaledWeight[c] = fetch * (Cascade1[c].z * ctx.depth < Math::PI ? ShoalingCoefficient(Cascade1[c].x, depthMetres) : 1.0);
			float deep = fetch * Cascade2[c].x;
			float shoaled = shoaledWeight[c] * Cascade2[c].x;
			deepVariance += deep * deep;
			shoaledVariance += shoaled * shoaled;
		}
		ctx.deepHeight = 4.0 * sqrt(deepVariance);
		ctx.saturation = ctx.hasTerrain > 0.5 ? min(1.0, 0.6 * ctx.depth / max(4.0 * sqrt(shoaledVariance), 1e-3)) : 1.0;

		// The sea arriving from deep water: the swell where this water is open to it, and the wind sea grown over the
		// open water upwind (an onshore wind brings its sea, an offshore wind leaves only the swell).
		float windSea = Shore1.w * ctx.fetchRatio;
		float swell = Shore1.z * ctx.shore.exposure;
		ctx.shoreHeight = sqrt(swell * swell + windSea * windSea);

		// Breaking or surging: the Iribarren number xi = tan(beta) / sqrt(H0 / L0) of the beach the ray runs up
		// (Battjes 1974) - waves spill (xi < 0.5) and plunge on gentle beaches, but surge up rock faces and
		// cliffs (xi > 3.3) without breaking, running up or leaving foam. H0 and L0 are the waves that break here, the shore
		// train's (its deep-water height, ShoreDeepHeight, and its own period). Taken from the whole sea's height (swell and
		// all) over the local wind sea's peak, a light breeze's short period read the waves as steep, and the small ones
		// spilled in without a curl, where on a beach their size makes them plunge as a shore break.
		float breakerHeight = ShoreHeightRatio * ctx.shoreHeight * MetresPerUnit;
		float breakerOmega = Shore4.x;
		if (breakerHeight <= 0.0 || breakerOmega <= 1e-3) {
			breakerHeight = ctx.deepHeight * MetresPerUnit;
			breakerOmega = ctx.peakOmega;
		}
		float steepness = breakerHeight / (Math::TAU * GravityMetres / max(breakerOmega * breakerOmega, 1e-4));
		float iribarren = ctx.shore.slope * rsqrt(max(steepness, 1e-4));
		ctx.iribarren = iribarren;
		ctx.beach = smoothstep(3.3, 2.0, iribarren) * ctx.shore.weight * ctx.shore.room * ctx.hasTerrain * saturate(ctx.deepHeight - 1.0);

		// Hand-over from the FFT to the shore wave train, counted in waves from the shore: Skyrim's beds fall away within a
		// wave or two of the waterline, so a limit on the depth left room for one wave and never a train of them.
		// Narrow water (streams, ponds) has no room for the train; there the FFT sea, fetch- and depth-limited, carries all.
		float shoreCycles = ctx.shore.travelTime * Shore4.x / Math::TAU;
		ctx.train = smoothstep(Tune3.z, 0.5 * Tune3.z, shoreCycles) * ctx.shore.weight * ctx.shore.room * ctx.hasTerrain;

		// Each cascade: the fetch-limited deep-water sea, shoaled over the bed (Green's law) and held to the depth-limited
		// breaking height. Offshore the train rides on the FFT sea, which keeps its waves and their chop. In the surf the
		// train's breakers take the energy from the random sea, longer and shorter, up to twice the train's frequency,
		// and only the short chop rides on top (ShoreHandOver). Handing over wherever the train reached replaced a stormy
		// FFT sea with the smooth train far out from the shore. Mirrors WaveModel's BuildContext.
		float handOver = ctx.train * ShoreSurfShare(ctx.shoreHeight * MetresPerUnit, depthMetres);
		[unroll] for (uint i = 0; i < PBRW_CASCADES; i++)
		{
			ctx.sea[i] = shoaledWeight[i] * ctx.saturation * ctx.damping;
			ctx.cascade[i] = ctx.sea[i] * (1.0 - handOver * ShoreHandOver(Cascade1[i].x));
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
	// River currents
	//
	// After Crest 4 (wave-harmonic/crest, MIT, ShapeCombine.compute Flow() and its _FLOW_ON branch). The wave tiles cannot
	// be advected (they are periodic textures), so a cascade is read twice, the position pushed back along the current by
	// the two phases of a sawtooth clock half a period apart, and the reads are cross-faded with triangle weights: each
	// copy is at its most displaced where its weight is 0 and the jump back goes unseen, so the waves travel downstream
	// without stretching. A function of position and time only: every pass reads the same blend.
	// ------------------------------------------------------------------------
	// The current's speed, the flow clock's period per metre of texel and its shortest and longest periods are settings (Tune0).
	static const float FlowFrameTime = 1.0 / 60.0;  // frame time assumed for last frame's flow offsets (motion vectors; the shader has no frame time) (s)

	/// The current (units/s) from a sample of the worldspace flowmap (RG direction, B strength, A mask: as the damping reads it).
	float2 FlowVelocity(float4 flowSample)
	{
		float2 direction = -(flowSample.xy * 2.0 - 1.0);  // the water's travel direction, as vanilla's world-space flow decodes it
		float lengthSq = dot(direction, direction);
		direction = lengthSq > 1e-4 ? direction * rsqrt(lengthSq) : 0.0;
		return direction * (saturate(flowSample.z * flowSample.w) * Tune0.x * UnitsPerMetre);
	}

	/// The clock the flow phases run on (s, wraps at 86400; the game timer, so it stops with the game).
	float FlowClock() { return Optics0.z; }

	/// The two phases of a cascade's flow clock: read the cascade at `position - offset0` with weight `weight0`, and at `position - offset1` with `weight1`.
	struct FlowBlend
	{
		float2 offset0;  // how far the first read lies back along the current (units)
		float2 offset1;
		float weight0;  // triangle weights: sum to 1
		float weight1;
	};

	/**
	 * Crest's Flow(): the period grows with the cascade's texel, the offsets are the clock modulo the period and half a
	 * period later, and the weight of each falls to 0 as its offset wraps. Use the result for every field of the cascade
	 * (displacement, derivatives, moments, foam) so they agree.
	 * @param velocity the current (units/s); FlowVelocity
	 * @param c        the cascade
	 * @param time     the flow clock, FlowClock() (minus FlowFrameTime for last frame)
	 */
	FlowBlend FlowPhases(float2 velocity, uint c, float time)
	{
		float texelMetres = Cascade0[c].w * MetresPerUnit;
		float period = exp2(round(log2(clamp(Tune0.y * texelMetres, Tune0.z, Tune0.w))));
		float phase = frac(time / period);  // 0..1
		FlowBlend b;
		b.offset0 = velocity * (phase * period);
		b.offset1 = velocity * (frac(phase + 0.5) * period);
		b.weight0 = 1.0 - abs(2.0 * phase - 1.0);  // 0 as the first offset wraps, 1 half a period later
		b.weight1 = 1.0 - b.weight0;
		return b;
	}

	// ------------------------------------------------------------------------
	// Shore wave train
	//
	// After Fluid Flux 2's coastline waves (Imaginary Blend; DaiZiLing's HLSL port of its MF_SurfaceFlux_Coastline, the
	// Flux2-Shoreline-Wave-HLSL repository). Near the shore the sea is one train of its dominant waves running up a
	// coastline map instead of a random field. The map (ShoreField, WaveModel.cpp), built from the real depths, holds the
	// time tau the waves take from each point to the waterline (eikonal |grad tau| = 1 / c(h)), so the crests slow down,
	// close up over the shoals, turn towards the depth contours and wrap into bays.
	//
	// As in Fluid Flux the phase is cut into whole waves, and each wave lives its own life: its shape comes from one
	// lookup of a profile (ShoreProfile.h) by its phase and its stage. What a stage is differs from Fluid Flux, where it
	// follows the distance to the shore: here it is the wave's height against the depth under its own crest, so a wave
	// peaks where it shoals, pitches over and collapses into a bore where it reaches its limit (McCowan 1894), on a
	// sandbar as well as on the beach, sooner if it is one of the big ones of a set. All of one wave shares that crest, so
	// several waves at different stages stand along a ray at once. Where one wave ends the profile is flat, so the next
	// can differ in height and stage without a step.
	//
	// The CPU mirrors it for swimming, buoyancy and floating objects (WaveModel.cpp ShoreTrain).
	// ------------------------------------------------------------------------

	/// A wave's water is carried forward by its wavelength times the profile, but by no more than this many wave heights:
	/// small waves over a long period compress, they do not fold. Mirrors PBRWaterModel::ShoreForwardHeights.
	static const float ShoreForwardHeights = 16.0;

	/// The nearest point of the waterline (absolute units): along the direction of travel, by the signed distance.
	float2 CoastPoint(float2 absXY, ShoreRay ray)
	{
		return absXY + ray.direction * ray.distance;
	}

	/**
	 * One wave's height as a factor on the sea's (mean ~1). Random waves have no single height: the heights of a narrow-band
	 * sea follow a Rayleigh distribution (Longuet-Higgins 1952), and the big ones come in groups of some five to ten
	 * (Longuet-Higgins 1984). So the factor is a slow group envelope that runs with the wave's number, times a Rayleigh
	 * draw of its own that has no memory from one wave to the next. Waves of different heights break at different depths
	 * (the biggest furthest out: Thornton and Guza 1983), which is what spreads the break points over the surf zone.
	 *
	 * The draw varies along the coast over a crest length (ShoreCrestLength: short crests where the sea is turbulent,
	 * after Baker et al. 2023, Nuss et al. 2023), so one wave stands taller on some stretches, breaks further out there
	 * and shows segments of a crest breaking at different distances from the shore. A function of the wave's number and the
	 * waterline point only: each wave keeps its height all the way in, and no step shows between stretches.
	 * Mirrors PBRWaterModel::ShoreSet.
	 * @param coast the waterline the wave runs to (CoastPoint)
	 * @param index the wave's number (ShoreWaveAt)
	 */
	float ShoreSet(float2 coast, float index)
	{
		float crest = max(Tune4.y * Shore3.w, 1.0);
		float group = ValueNoise(coast / (3.0 * crest) + index * float2(0.13, 0.07));
		float2 seed = float2(Hash12(float2(index, 3.7)), Hash12(float2(index, 11.3))) * 61.0;
		// Value noise bunches around its middle: stretch it back towards a uniform draw before the Rayleigh transform
		// sqrt(-ln(1 - u)), whose mean over a uniform u is Gamma(3/2) = 0.8862.
		float u = clamp(0.5 + 1.7 * (ValueNoise(coast / crest + seed) - 0.5), 0.03, 0.97);
		float rayleigh = sqrt(-log(1.0 - u)) * 1.128379;
		return clamp(1.0 + Tune4.x * (lerp(0.6, 1.4, group) * rayleigh - 1.0), 0.1, ShoreSetCeiling);
	}

	// The shore train: the sea's dominant waves. Its higher harmonics are not trains of their own but trailing crests in the
	// main wave's phase (ShoreWhelps). The train index `j` the shore functions take is always 0.
	static const uint ShoreTrainCount = 1;
	static const float ShoreTrainMultiple[ShoreTrainCount] = { 1.0 };
	/// Where the bore's front stands in its wave, in phase: the swash starts as it reaches the waterline. Mirrors BoreFront in ShoreProfile.cpp.
	static const float ShoreBoreFront = 0.16;

	/**
	 * How far the crests of train `j` run ahead of or behind a straight front along the coast (waves): long, slowly
	 * drifting bends, so stretches of beach break at different times and the trains' crests cross. A function of the
	 * waterline point and the clock only, so every point of a ray and every pass agree; each term turns over a whole
	 * number of times per wrap of the clock (1024 waves). Shore1.x scales it. Mirrors PBRWaterModel::ShoreWobble.
	 * @param coast the waterline the wave runs to (CoastPoint)
	 * @param clock the shore clock (rad)
	 */
	float ShoreWobble(float2 coast, float clock, uint j)
	{
		float2 q = coast / max(2.0 * Shore3.w, 1.0);
		float seed = 2.39996 * (float)j;
		float2 d0 = float2(cos(seed), sin(seed));
		float2 d1 = float2(cos(seed + 2.1), sin(seed + 2.1));
		float2 d2 = float2(cos(seed + 4.3), sin(seed + 4.3));
		float w = 0.5 * sin(dot(q, d0) + clock * (37.0 / 1024.0) + seed) +
		          0.3 * sin(1.73 * dot(q, d1) - clock * (61.0 / 1024.0) + 1.3 * seed) +
		          0.2 * sin(2.91 * dot(q, d2) + clock * (89.0 / 1024.0) + 2.7 * seed);
		return Shore1.x * w;
	}

	/// The phase of shore train `j` (rad): each cycle is one wave, rising towards the sea. Mirrors the CPU's ShorePhase.
	float ShorePhase(ShoreRay ray, float2 coast, float clock, uint j)
	{
		return ShoreTrainMultiple[j] * (clock + ray.phaseLag + Math::TAU * ray.along) + Math::TAU * ShoreWobble(coast, clock, j);
	}

	/// One lookup of the wave profile. Mirrors PBRWaterModel::ShoreProfileTable::At.
	float4 ShoreProfileAt(float phase, float stage, float slice)
	{
		static const float2 size = float2(256.0, 64.0);  // ShoreProfileTable::Phases, Stages
		float2 uv = (float2(phase, stage) * (size - 1.0) + 0.5) / size;
		return ShoreProfileTexture.SampleLevel(LinearClampSampler, float3(uv, slice), 0);
	}

	/**
	 * Weggel's (1972) breaker index: the height over depth at which waves break on a plane slope `slope` (tan beta), for
	 * a breaker of `heightMetres` and period `period` (s): gamma = b(m) - a(m) Hb / (g T^2), a = 43.75 (1 - e^-19m),
	 * b = 1.56 / (1 + e^-19.5m). McCowan's 0.78 on a flat bed; steeper beaches hold higher waves before they break, and
	 * steeper waves break sooner. Fitted for slopes up to 1:10. Mirrors PBRWaterModel::ShoreBreakerIndex.
	 */
	float ShoreBreakerIndex(float slope, float heightMetres, float period)
	{
		float m = clamp(slope, 0.0, 0.1);
		float a = 43.75 * (1.0 - exp(-19.0 * m));
		float b = 1.56 / (1.0 + exp(-19.5 * m));
		return clamp(b - a * heightMetres / (GravityMetres * period * period), 0.6, 1.2);
	}

	/**
	 * A wave's stage (ShoreProfile.h) from its shoaled height over its breaker limit under its crest, `ratio`. Up to the
	 * limit the stage follows the ratio (the wave peaks as it shoals). Past it breaking runs on its own clock, the
	 * distance the wave has travelled since its break point, in breaker heights: the depth then is h_b ratio^-4/5 (Green's
	 * law), so the wave is (1 - ratio^-4/5) / (gamma m) breaker heights past it on a slope m. A plunger's crest reaches the
	 * trough ahead (Galvin 1969: x_p = (4.0 - 9.25 m) H_b, plunging waves travel 4 to 8 breaker heights while they break).
	 * The jet's impact throws the splash-up ahead of it and the tube is gone within about half a plunge length more: the
	 * outer region of the surf zone, where the breaker turns into a bore, is a few breaker heights long (Svendsen 1984;
	 * Peregrine 1983), not a second plunge length. A spilling breaker has no tube to bring down (Battjes 1974): its crest
	 * goes straight to a roller, so its plunge length shrinks with the share of plungers, `plunging`. The bore then runs
	 * in to the waterline. Driven by the ratio alone, the curl lasted while the depth fell to a third of the breaking
	 * depth, tens of metres on a gentle beach.
	 * Mirrors PBRWaterModel::ShoreBreakerStage.
	 * @param index    the breaker index gamma (ShoreBreakerIndex)
	 * @param plunging 0..1 how much of a plunger the breaker is (spilling 0, plunging 1): ShoreBreakerPlunging
	 */
	float ShoreBreakerStage(float ratio, float slope, float index, float plunging)
	{
		float stage = ratio / (1.0 + ratio);
		[branch] if (ratio > 1.0)
		{
			float m = clamp(slope, 0.01, 0.3);
			float depthShare = pow(abs(ratio), -0.8);  // h / h_b
			float travelled = (1.0 - depthShare) / (index * m);
			float plunge = max(4.0 - 9.25 * m, 2.0) * lerp(0.35, 1.0, plunging);
			float transition = 0.5 * plunge;
			// Where the bore has formed, as a share of the breaking depth, and the stage running to 1 at the waterline.
			float formed = 1.0 - (plunge + transition) * index * m;
			if (travelled < plunge)
				stage = 0.5 + 0.16 * travelled / plunge;
			else if (travelled < plunge + transition)
				stage = 0.66 + 0.14 * (travelled - plunge) / transition;
			else
				stage = 0.80 + 0.20 * saturate(1.0 - depthShare / max(formed, 1e-3));
		}
		return stage;
	}

	/**
	 * How much of a plunger a breaker is on a beach of surf similarity `iribarren` (Battjes 1974: spilling below 0.5,
	 * plunging above it, surging beyond 3.3): a spiller rolls over without a tube, a plunger throws one, and by 2 the waves
	 * collapse or surge instead. The change is centred on Battjes' 0.5: ramped from there to 1, a beach's breakers were
	 * half spillers well inside the plunging range and curled their lips only up to a cliff. Mirrors
	 * PBRWaterModel::ShoreBreakerPlunging.
	 */
	float ShoreBreakerPlunging(float iribarren)
	{
		return smoothstep(0.35, 0.6, iribarren) * (1.0 - smoothstep(2.0, 3.3, iribarren));
	}

	/// How far a full onshore (+1) or offshore (-1) wind moves the breaker type, as a share of the surf similarity. Mirrors PBRWaterModel::ShoreWindPlunge.
	static const float ShoreWindPlunge = 0.3;

	/**
	 * The surf similarity the breakers here take their type from: the beach's, moved by the wind. An onshore wind pushes the
	 * crest over early into a spiller and flattens the overturn; an offshore wind holds the face up into a plunger (Douglass
	 * 1990; Feddersen and Fincham 2023: the overturn shrinks as the onshore wind grows against the wave's speed). Only the
	 * breaker type: whether waves break on this shore at all (WaveContext::beach) stays the beach's.
	 * Mirrors PBRWaterModel::ShoreBreakerIribarren.
	 */
	float ShoreBreakerIribarren(WaveContext ctx)
	{
		return ctx.iribarren * (1.0 - ShoreWindPlunge * ctx.onshoreWind);
	}

	/**
	 * A breaking wave's height (m) once it is a bore, from its height `height` as the breaker limit holds it. The limit
	 * (gamma h, gamma up to 1.2) holds the breaker; the bore it collapses into is lower (the profile's collapse takes 0.58 of
	 * the height at the bore's front), and in the inner surf the heights saturate at a share of the depth, H/h about 0.4 on
	 * a gentle beach to 0.6 on a steep one (Thornton and Guza 1983; Raubenheimer, Guza and Elgar 1996). The bore does not
	 * vanish with the depth, as that share does: it ends at the waterline with a front of a share of the breaker's height
	 * that the swash carries on (Shen and Meyer 1963; Peregrine and Williams 2001). The change comes in with the bore
	 * (stage 0.66 to 0.80), lowering the breaker in the inner surf and holding up its front until the last few decimetres of water. Mirrors PBRWaterModel::ShoreBoreHeight.
	 * @param ratio shoaled height over the breaker limit (> 1: broken)
	 * @param depth still water depth under the crest (m)
	 */
	float ShoreBoreHeight(float height, float stage, float ratio, float slope, float index, float depth)
	{
		[branch] if (ratio > 1.0)
		{
			float m = clamp(slope, 0.01, 0.3);
			float breakerHeight = index * depth * pow(abs(ratio), 0.8);  // H_b: the limit at the break point's depth
			float saturated = (0.4 + 0.2 * saturate((m - 0.02) / 0.08)) * depth;
			// The front is held up only while there is water for it: over the last few decimetres the bore collapses into the
			// swash, which carries its water on (SwashStepCS). Held all the way, it stood on the dry sand under the train.
			float front = max(saturated, 0.15 * breakerHeight * smoothstep(0.05, 0.4, depth)) / 0.58;
			height = lerp(height, front, smoothstep(0.66, 0.80, stage));
		}
		return height;
	}

	/// One wave of a train, as a point on it sees it.
	struct ShoreWave
	{
		float phase;       // where this point is on the wave: 0 the trough ahead of it, 1 the trough behind
		float index;       // the wave's number
		float stage;       // its life, 0 deep water, 0.5 reaching its limit, 1 the bore at the shore
		float height;      // trough to crest (m)
		float wavelength;  // here (units)
		float whelp;       // height (m) of the crests trailing the wave's front (ShoreWhelps); 0 where it has none
		float whelpLength; // their wavelength (units)
	};

	/// Height (m) the sea's dominant waves arrive with from deep water, before shoaling. Mirrors the CPU's ShoreDeepHeight.
	float ShoreDeepHeight(WaveContext ctx)
	{
		return ShoreHeightRatio * ctx.shoreHeight * MetresPerUnit * ctx.damping;
	}

	/**
	 * The whelps of an undular bore: a bore weak against the depth it runs in (Froude number below about 1.3, a bore
	 * height under 0.4 of the depth) does not break into a turbulent front but rides on a train of smaller crests that trail
	 * it at a spacing of a few depths and shrink away behind it (Favre 1935; Chanson 2009). Water that gets deeper inshore
	 * of a bar frees the wave's higher harmonics in the same way, and they trail the crest (Beji and Battjes 1993). Both
	 * live in the main wave's own phase, so they can never get ahead of it. Mirrors PBRWaterModel::ShoreWhelpAmplitude.
	 * @param wave   the wave, with phase, stage and height set
	 * @param depthA the depth under the point the whelps trail (m)
	 * @param crestDepth the depth under the wave's crest (m)
	 * @return the height (m) of the first crest behind the front
	 */
	float ShoreWhelpAmplitude(ShoreWave wave, float depthA, float crestDepth)
	{
		// The bore's front stands about 0.58 of the wave's height (the profile's bore).
		float froude = 0.58 * wave.height / depthA;
		float weakBore = (1.0 - smoothstep(0.25, 0.5, froude)) * smoothstep(0.7, 0.85, wave.stage);
		float barLee = smoothstep(1.0, 1.6, depthA / crestDepth) * smoothstep(0.25, 0.4, wave.stage);
		return Shore3.y * max(weakBore, barLee) * 0.3 * wave.height;
	}

	/**
	 * The whelps' height above the wave's own profile and its rate of change with the phase (both in the units of
	 * `amplitude`): crests of spacing `spacing` that start where the front stands (ShoreBoreFront, or the crest while the
	 * wave has not yet broken) and die away behind it within a few spacings. Nothing ahead of the front. Mirrors
	 * PBRWaterModel::ShoreWhelps.
	 * @param x         the phase on the wave
	 * @param stage     the wave's stage
	 * @param amplitude height of the first crest
	 * @param lambda    the wave's wavelength (units)
	 * @param spacing   the whelps' wavelength (units)
	 */
	float2 ShoreWhelps(float x, float stage, float amplitude, float lambda, float spacing)
	{
		float anchor = lerp(0.5, ShoreBoreFront, smoothstep(0.5, 0.8, stage));
		[branch] if (amplitude <= 0.0 || x <= anchor) return 0.0;
		float c = lambda / spacing;  // whelps per phase
		float s = (x - anchor) * c;
		float e = exp(-s / 2.2);
		float t = saturate((x - 0.75) / 0.22);
		float tail = 1.0 - t * t * (3.0 - 2.0 * t);  // gone by the end of the wave, where the next begins
		float dTail = -6.0 * t * (1.0 - t) / 0.22;
		float b = 0.5 - 0.5 * cos(Math::TAU * s);
		float dB = Math::PI * c * sin(Math::TAU * s);
		return amplitude * float2(e * tail * b, e * (dTail * b + tail * dB - c / 2.2 * tail * b));
	}

	/// Half the stretch of crest the bed under a breaking wave is averaged over (units): a LAND quad at least, a quarter
	/// wave at most. Mirrors PBRWaterModel::ShoreCrestBedHalfWidth.
	float ShoreCrestBedHalfWidth(float wavelength)
	{
		return max(128.0, 0.25 * wavelength);
	}

	/**
	 * The bed (absolute z) under a wave's crest at absolute `crest`, averaged along the crest (`along`, unit) over
	 * ShoreCrestBedHalfWidth either way, or -1e9 where no terrain is there. A wave answers to the bottom over a stretch of its
	 * crest, not to the point under it: read at one point, the stage followed every crease of the terrain's 128-unit facets
	 * (the distance a breaker has run multiplies a depth difference by 1 / (gamma m), twenty to fifty times on a gentle
	 * beach), and neighbouring stretches of one crest stood at the curl and at the landed lip a few units apart, joined by a
	 * right-angled wall. Mirrors PBRWaterModel::ShoreCrestBed.
	 */
	float ShoreCrestBed(float2 crest, float2 along, float halfWidth)
	{
		static const float Offsets[5] = { -1.0, -0.5, 0.0, 0.5, 1.0 };
		static const float Weights[5] = { 1.0, 2.0, 3.0, 2.0, 1.0 };
		float sum = 0.0;
		float weight = 0.0;
		[unroll] for (uint i = 0; i < 5; i++)
		{
			float z = TerrainHeight(crest + along * (Offsets[i] * halfWidth));
			if (z > -1e8) {
				sum += z * Weights[i];
				weight += Weights[i];
			}
		}
		return weight > 0.0 ? sum / weight : -1e9;
	}

	/// The wave of train `j` (always 0) passing absolute `absXY` at `clock`.
	ShoreWave ShoreWaveAt(float2 absXY, float2 coast, WaveContext ctx, float clock, uint j)
	{
		ShoreRay ray = ctx.shore;
		ShoreWave o;
		float cycles = ShorePhase(ray, coast, clock, j) / Math::TAU;
		o.index = floor(cycles);
		o.phase = cycles - o.index;
		float omega = max(Shore4.x, 1e-3) * ShoreTrainMultiple[j];
		o.wavelength = Math::TAU / (omega * max(ray.slowness, 1e-6));

		// Everything about the wave is decided at its crest, half a wave away at most, so all of it agrees.
		float crestBed = ShoreCrestBed(absXY + ray.direction * ((o.phase - 0.5) * o.wavelength), float2(-ray.direction.y, ray.direction.x), ShoreCrestBedHalfWidth(o.wavelength));
		float crestDepth = max((ctx.waterZ - crestBed) * MetresPerUnit, 0.05);
		float deep = ShoreDeepHeight(ctx) * ShoreSet(coast, o.index);
		float shoaled = deep * ShoalingCoefficient(omega, crestDepth);
		// An onshore wind makes the wave break earlier (a lower breaker index), an offshore wind later (ShoreWindBreaking).
		float index = ShoreBreakerIndex(ctx.shore.slope, min(shoaled, ShoreBreakerLimit * crestDepth), Math::TAU / omega) * (1.0 - ShoreWindBreaking * ctx.onshoreWind);
		float limit = index * crestDepth;
		o.height = min(min(shoaled, limit), ShoreSteepestWave * o.wavelength * MetresPerUnit);
		// Rock faces and cliffs: the waves surge against them and go back, they stay short of breaking.
		o.stage = min(ShoreBreakerStage(shoaled / limit, ctx.shore.slope, index, ShoreBreakerPlunging(ShoreBreakerIribarren(ctx))), lerp(0.42, 1.0, ctx.beach));
		o.height = ShoreBoreHeight(o.height, o.stage, shoaled / limit, ctx.shore.slope, index, crestDepth);

		// The whelps trail the front (or, before the wave breaks, the crest): a weak bore or a bar's lee. The depth under
		// that point, from the wave's own phase, is the same for every point of it.
		o.whelp = 0.0;
		o.whelpLength = o.wavelength;
		[branch] if (Shore3.y > 0.0 && o.stage > 0.25)
		{
			float anchor = lerp(0.5, ShoreBoreFront, smoothstep(0.5, 0.8, o.stage));
			float anchorBed = TerrainHeight(absXY + ray.direction * ((o.phase - anchor) * o.wavelength));
			float anchorDepth = max((ctx.waterZ - anchorBed) * MetresPerUnit, 0.05);
			o.whelp = ShoreWhelpAmplitude(o, anchorDepth, crestDepth);
			// Spaced at a few depths, from a crest in a few dozen to a couple of whelps per wave.
			o.whelpLength = clamp(8.0 * anchorDepth * UnitsPerMetre, o.wavelength / 24.0, o.wavelength / 2.5);
		}
		return o;
	}

	/**
	 * How real the shore waves' breaking is here, 0..1. Breaking is decided by the height against the depth, so even
	 * ripples "break" in the last centimetres of water; but only real breakers pitch over and entrain air: a trace from a
	 * couple of centimetres of significant height (a stream's wavelets), full from 40 cm.
	 */
	float BreakerStrength(WaveContext ctx)
	{
		return smoothstep(0.02, 0.4, ctx.shoreHeight * ctx.damping * MetresPerUnit);
	}

	// ------------------------------------------------------------------------
	// Breaker curl
	//
	// The profile table holds the plunging lip only as far as the mesh can carry it unfolded (ShoreProfile.cpp MinStretch).
	// Where the mesh is fine enough the wave curls over for real: the water above a pivot on the front face is turned
	// forward about it, the most at the crest, which becomes the lip's tip, and less behind it, which becomes the top of the
	// lip, and is thrown ahead, so the tip plunges into the trough in front of the face (the shape of Longuet-Higgins'
	// overturning jet, not its dynamics). The surface stays one sheet, so the underside of the lip faces down and the face
	// under it is seen through the tube. The turn depends on the wave's aspect (wavelength over height), so it is worked
	// out here rather than baked. The shapes are the profile's (Evaluate in ShoreProfile.cpp) shoaling wave and bore; the
	// curl has gone by the time the bore has formed, and outside its stages the table is used as it is.
	// Geometry only: the CPU mirror (swimming, buoyancy) rides the table's uncurled wave.
	// ------------------------------------------------------------------------
	static const float ShoreCurlAngle = 2.3;   // turn of the crest at full curl (rad)
	static const float ShoreCurlThrow = 1.2;   // how far the lip is thrown ahead of the face at full curl (wave heights)
	static const float ShoreCurlPivot = 0.45;  // the pivot's height on the front face (share of the crest's)

	/// Smoothstep as ShoreProfile.cpp's: (x - a) / (b - a), clamped and eased.
	float ShoreSmooth(float a, float b, float x)
	{
		float t = saturate((x - a) / (b - a));
		return t * t * (3.0 - 2.0 * t);
	}

	/**
	 * Where a curling wave's lip comes down (phase; ShoreCurlShape's plunge point), for a wave of stage `v`, wavelength
	 * `lambda`, height `height` (units), the table's forward scale `forward` and curl strength `amount`.
	 */
	float ShoreCurlLanding(float v, float lambda, float height, float forward, float amount)
	{
		float s = ShoreSmooth(0.0, 0.5, v);
		float crest = 0.5 - 0.18 * s;
		float front = 0.5 * (1.0 - s) + 0.075 * s;
		float n = 1.0 + 1.2 * s;
		float lean = 0.10 * ShoreSmooth(0.25, 0.55, v);
		float ap = acos(2.0 * pow(ShoreCurlPivot, 1.0 / n) - 1.0) / Math::PI;
		float xp = crest - ap * front;
		float sp = -xp * lambda + lean * ShoreCurlPivot * forward;
		float zp = ShoreCurlPivot * height;
		float T = ShoreCurlAngle * min(amount, 1.0);
		float crestS = -crest * lambda + lean * forward - sp;
		float crestZ = height - zp;
		float plungeS = sp + crestS * cos(T) + crestZ * sin(T) + ShoreCurlThrow * amount * height;
		return -plungeS / max(lambda, 1e-3);
	}

	/**
	 * The phase of a curled wave's bore front: where its lip came down as the bore rises out of the splash-up, easing to
	 * the table's (ShoreBoreFront) by the time the bore has formed (0.80), where ShoreCurlWeight hands back. At the table's
	 * front from the start, the bore and its white water ran out ahead of a lip that landed behind it, as if the wave the
	 * curl rode on had gone on without it.
	 */
	float ShoreCurlFront(float v, float lambda, float height, float forward, float amount)
	{
		float landing = clamp(ShoreCurlLanding(v, lambda, height, forward, amount), 0.04, 0.45);
		return lerp(landing, ShoreBoreFront, ShoreSmooth(0.68, 0.80, v));
	}

	/**
	 * The curled wave at phase `x` and stage `v`: x its displacement along the direction of travel, y its height above
	 * the trough (units).
	 * @param lambda  the wavelength (units)
	 * @param height  trough to crest (units)
	 * @param forward the table's forward scale (units): AddShoreWaves' `forward`
	 * @param amount  curl strength (Shore3.z)
	 */
	float2 ShoreCurlShape(float x, float v, float lambda, float height, float forward, float amount)
	{
		float s = ShoreSmooth(0.0, 0.5, v);
		float crest = 0.5 - 0.18 * s;
		float front = 0.5 * (1.0 - s) + 0.075 * s;
		float back = 0.5 * (1.0 - s) + 0.30 * s;
		float xi = x - crest;
		float a = xi < 0.0 ? saturate(-xi / front) : saturate(xi / back);
		float n = 1.0 + 1.2 * s;
		float wave = pow(0.5 * (1.0 + cos(Math::PI * a)), n);
		float lean = 0.10 * ShoreSmooth(0.25, 0.55, v);

		// The shoaling wave, along the travel (rest position -x lambda) and up.
		float S = -x * lambda + lean * wave * forward;
		float Z = wave * height;

		// The pivot on the front face, where the wave stands ShoreCurlPivot of its crest high.
		float ap = acos(2.0 * pow(ShoreCurlPivot, 1.0 / n) - 1.0) / Math::PI;
		float xp = crest - ap * front;
		float sp = -xp * lambda + lean * ShoreCurlPivot * forward;
		float zp = ShoreCurlPivot * height;

		// The curl grows until the tip reaches the trough ahead (stage 0.64, Galvin's plunge distance: ShoreBreakerStage).
		float life = amount * ShoreSmooth(0.44, 0.64, v);
		// Behind the crest the turn falls off over the top of the lip. The water there sits about a wave height from the
		// pivot, so the falloff has to be longer than ~2.5 height/wavelength or the turn carries each point further than the
		// surface runs between them: over 0.035 of a wave the back of the wave folded over itself just behind the crest, and
		// a strip of its underside showed along the top of every curl.
		float behind = (x - crest) / clamp(2.5 * height / max(lambda, 1e-3), 0.05, 0.15);
		float g = x <= xp ? 0.0 : (x <= crest ? ((x - xp) * (x - xp)) / max((crest - xp) * (crest - xp), 1e-8) : exp(-behind * behind));
		// The lip turns at most ShoreCurlAngle: a stronger curl (Shore3.z above 1) throws it further, it does not turn it
		// further. Turned by the strength as well, the lip went past pointing down and folded back under itself, its
		// underside showing over the crest.
		float turn = min(amount, 1.0);
		float t = ShoreCurlAngle * turn * ShoreSmooth(0.44, 0.64, v) * g;
		float ct = cos(t);
		float st = sin(t);
		float dS = S - sp;
		float dZ = Z - zp;
		float Sc = sp + dS * ct + dZ * st + ShoreCurlThrow * life * g * height;
		float Zc = zp - dS * st + dZ * ct;

		// Then the tube is crushed: the jet has hit the trough, the air under it is trapped and the lip falls into the
		// plunge point (where the crest landed, on the trough), the splash-up ahead of it becoming the bore's front. Turning
		// the lip back instead raised it into a wall of glassy water.
		float plungeS = -ShoreCurlLanding(v, lambda, height, forward, amount) * lambda;
		float crush = ShoreSmooth(0.62, 0.70, v) * saturate(3.0 * g);
		Sc = lerp(Sc, plungeS, crush);
		Zc = lerp(Zc, 0.0, crush);

		// The bore the wave collapses into: it rises out of the splash-up once the lip has landed (0.64), its front where
		// the lip came down (ShoreCurlFront). Blended in from 0.58 as the table's is, a low rolling hump rose under the lip
		// while it was still in the air and took over from it. It has formed by 0.80, as the table's has: from there the two
		// shapes are the same bore (ShoreCurlWeight hands back).
		float b = ShoreSmooth(0.64, 0.80, v);
		float frontWidth = 0.035 + 0.02 * (1.0 - b);
		float boreFront = ShoreCurlFront(v, lambda, height, forward, amount);
		float bore = x < boreFront ? ShoreSmooth(boreFront - 3.0 * frontWidth, boreFront, x) :
		                             exp(-(x - boreFront) / 0.34) * (1.0 - ShoreSmooth(0.82, 1.0, x));
		float collapse = 1.0 - 0.42 * ShoreSmooth(0.64, 0.76, v);
		float Sb = -x * lambda + 0.05 * bore * forward;
		float Zb = bore * collapse * height;

		return float2(lerp(Sc, Sb, b) + x * lambda, lerp(Zc, Zb, b));
	}

	/**
	 * Mesh spacing (units) at camera-relative `positionWS` where the shore waves can curl, or 1e6 where the draw is not
	 * tessellated (the curl needs the extra triangles the hull shader adds there, Shore1.y). A function of the position
	 * and the draw only, so the vertices and the pixels of one draw agree on it.
	 */
	float ShoreCurlSpacing(float3 positionWS)
	{
		if (Tess0.x < 0.5)
			return 1e6;
		float target = max(Tess0.y, 1.0) * max(length(positionWS), 1.0) / max(Tess0.w, 1e-3) / max(Shore1.y, 1.0);
		return max(target, Draw0.x / max(Tess0.z, 1.0));
	}

	/**
	 * Where a wave of `wavelength` (units) at stage `stage` takes the curl's shape at camera-relative `positionWS` on a
	 * beach of surf similarity `iribarren`. Returns the share of ShoreCurlShape over the table's shape (0..1), and sets
	 * `amount` to the curl's strength to pass it (Shore3.z at most). A weaker plunger, or a mesh too coarse for the whole
	 * lip, curls less: blending a share of the full curl into the table's wave instead moved the vertices little while the
	 * curl's steep turn about the lip took over the normal, so a plain standing face was lit as an overhang. The share only
	 * blends shapes without a lip: the stage window's ends, and the analytic wave where the mesh first resolves its face.
	 */
	float ShoreCurlWeight(float3 positionWS, float wavelength, float stage, float iribarren, out float amount)
	{
		float weight = 0.0;
		amount = 0.0;
		[branch] if (Shore3.z > 0.0 && stage > 0.40 && stage < 0.88)
		{
			// Only plungers curl (Battjes 1974): on gentle beaches waves spill, white water tumbling down the face; on the
			// steepest they collapse or surge up the beach without breaking. A wave that plunges at all throws its lip all the
			// way over into the trough: scaled by how much of a plunger it is, the curl's turn stopped part way and the
			// lip stood as a cliff that rode in until the bore caught up with it.
			float plunging = ShoreBreakerPlunging(iribarren);
			// The lip is fed from about a tenth of the wave, which needs a few dozen vertices across it to hold its shape.
			float spacing = ShoreCurlSpacing(positionWS);
			amount = Shore3.z * smoothstep(0.0, 0.3, plunging) * smoothstep(wavelength / 25.0, wavelength / 50.0, spacing);
			if (amount > 0.0)
				// Handed back to the table once both are the same bore (ShoreCurlShape has formed it by 0.80, collapsed by
				// 0.76, as the table has). Handed back at 0.72 to 0.78, the crushed lip lying out at the plunge point was
				// blended straight into the table's bore at the old crest: along the crest, where the stage passed through
				// those values, the blend stood up as flat walls meeting at right angles.
				weight = smoothstep(wavelength / 12.0, wavelength / 25.0, spacing) * smoothstep(0.40, 0.46, stage) * (1.0 - smoothstep(0.82, 0.88, stage));
		}
		return weight;
	}

	struct ShoreTrain
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float2 slope;                 // height gradient (units/unit)
		float3 compression;           // horizontal displacement gradient: x dDx/dx, y dDy/dy, z dDx/dy
		float foam;                   // 0..1 white water the waves carry here
		float breakStage;             // 0..1 the wave breaking here (pitching forward or broken), weighted by the train's share
		float slopeVariance;          // slope variance the scale filters out (for roughness)
		float curl;                   // 0..1 how much of a breaker's curl is shown here: the surface may face down
		float crest;                  // units above the still water the crest of the shore wave here stands (its top)
		float bore;                   // 0..1 the wave has broken into a bore: its white water lies on a flattened surface (stage 0.7..0.85)
		float aeration;               // 0..1 air churned into the water here (ShoreAeration): the crushed tube, the splash-up and the roller
		float impact;                 // 0..1 the wave's jet has landed (stage past 0.6): its folding surface is white water from now on
		float clearFace;              // 0..1 the steepening face, the lip and the tube of a wave about to plunge: clear water (ShoreClearFace)
	};

	static const float ShoreRollerLumps = 0.12;  // the roller's churning lumps, in wave heights at full aeration

	/**
	 * Air in the breaking wave at phase `x` and stage `v`, 0..1. Before the jet lands the water is clear (white only at the
	 * lip's tip: the profile's crown foam). When it lands, the tube collapses and the splash-up rises: the air under it and
	 * the air the jet drives in turn the whole crushed region opaque (void fractions to 0.6 in the collapsing cavity and
	 * 0.35 in the splash-up: Na et al. 2016; Lubin et al. 2011). That becomes the roller riding the bore's front, the
	 * aerated water thinning behind it into the residual foam, and weakening as the bore runs into the shallows.
	 */
	float ShoreAeration(float x, float v, float front)
	{
		float impact = ShoreSmooth(0.60, 0.68, v);
		[branch] if (impact <= 0.0) return 0.0;
		// Behind the front (ShoreCurlFront where the wave curled: the splash-up rises where the lip came down): the whole
		// crushed tube at first (to about the old crest), the roller a wave height long once the bore has formed.
		float tail = lerp(0.28, 0.08, ShoreSmooth(0.70, 0.88, v));
		float ahead = ShoreSmooth(front - 0.04, front, x);
		float behind = x <= front ? 1.0 : exp(-(x - front) / tail);
		float roller = ahead * behind * (1.0 - ShoreSmooth(0.82, 1.0, x));
		return impact * roller * (1.0 - 0.5 * ShoreSmooth(0.88, 1.0, v));
	}

	/**
	 * The part of a wave about to plunge that is clear water, 0..1: its face from the trough ahead up over the crest, the
	 * lip and the tube under it, from when it steepens until the jet lands. The foam the surf left on the water and the sea's
	 * own foam are drawn off it (BreakerFoam keeps the lip's crown): drawn on, they showed through the tube as a texture
	 * where a real tube is glassy water.
	 */
	float ShoreClearFace(float x, float v)
	{
		float window = ShoreSmooth(0.38, 0.46, v) * (1.0 - ShoreSmooth(0.64, 0.72, v));
		float crest = 0.5 - 0.18 * ShoreSmooth(0.0, 0.5, v);
		return window * (1.0 - ShoreSmooth(crest + 0.04, crest + 0.12, x));
	}

	/**
	 * The roller's churning lumps at `q` (the position in lump lengths), boiling at `clock`: x the height (-0.5..0.5),
	 * yz its gradient in lump lengths.
	 */
	float3 ShoreRollerChurn(float2 q, float clock)
	{
		float3 a = ValueNoiseGrad(q + float2(0.31, -0.17) * clock);
		float3 b = ValueNoiseGrad(q * 2.1 + float2(-0.23, 0.29) * clock + 5.3);
		return float3(0.65 * (a.x - 0.5) + 0.35 * (b.x - 0.5), 0.65 * a.yz + 0.35 * 2.1 * b.yz);
	}

	/**
	 * Adds train `j` to the shore waves at undisplaced camera-relative `positionWS` (absolute `absXY`, running to the
	 * waterline point `coast`), with the share `gate`.
	 */
	void AddShoreWaves(inout ShoreTrain o, float3 positionWS, float2 absXY, float2 coast, WaveContext ctx, float scale, bool previous, bool shading, uint j, float gate, float yieldTo)
	{
		ShoreWave wave = ShoreWaveAt(absXY, coast, ctx, Shore2.x, j);
		// Inshore of the hand-over the swash simulation carries the bore (DrawnShoreWaves). Only the bore gives way: a wave
		// still curling keeps its lip whatever the depth under each vertex, which differs across the face and along the crest
		// and took the curl away in pieces, a gap wherever the bed came up under it.
		gate *= lerp(1.0, yieldTo, smoothstep(0.66, 0.80, wave.stage));
		float k = Math::TAU / wave.wavelength;
		float fade = smoothstep(Math::PI, 0.5 * Math::PI, k * scale);
		float w = gate * fade;
		float height = wave.height * UnitsPerMetre;
		float amplitudeUnits = 0.5 * height;
		o.slopeVariance += (1.0 - fade * fade) * gate * gate * 0.5 * amplitudeUnits * amplitudeUnits * k * k;
		[branch] if (w <= 0.0) return;

		float4 shape = ShoreProfileAt(wave.phase, wave.stage, 0.0);
		float forward = min(wave.wavelength, ShoreForwardHeights * height);
		float2 dir = ctx.shore.direction;
		float omega = max(Shore4.x, 1e-3) * ShoreTrainMultiple[j];

		// The profile stands on its trough, so a wave of any height meets the next without a step. The sea's level comes
		// from the depth here, not the wave's, and is as continuous: about half the height while it is a sinusoid, less
		// as the wave narrows.
		float localDepth = max(ctx.depth * MetresPerUnit, 0.05);
		float localDeep = ShoreDeepHeight(ctx);
		float localShoaled = localDeep * ShoalingCoefficient(omega, localDepth);
		float localLimit = ShoreBreakerLimit * localDepth;
		float meanLevel = min(localShoaled, localLimit) * lerp(0.5, 0.22, saturate(2.0 * localShoaled / (localShoaled + localLimit))) * UnitsPerMetre;

		// Along the travel and up (units), and their rates of change with the phase.
		float along = shape.x * forward;
		float up = shape.y * height;
		float dAlong = shape.z * forward;
		float dUp = shape.w * height;
		float curlAmount;
		float curl = ShoreCurlWeight(positionWS, wave.wavelength, wave.stage, ShoreBreakerIribarren(ctx), curlAmount);
		[branch] if (curl > 0.0)
		{
			// Central differences: the curl's rates of change carry the surface around the lip and under it.
			static const float e = 1.0 / 512.0;
			float2 c = ShoreCurlShape(wave.phase, wave.stage, wave.wavelength, height, forward, curlAmount);
			float2 cA = ShoreCurlShape(wave.phase + e, wave.stage, wave.wavelength, height, forward, curlAmount);
			float2 cB = ShoreCurlShape(wave.phase - e, wave.stage, wave.wavelength, height, forward, curlAmount);
			float2 dc = (cA - cB) / (2.0 * e);
			along = lerp(along, c.x, curl);
			up = lerp(up, c.y, curl);
			dAlong = lerp(dAlong, dc.x, curl);
			dUp = lerp(dUp, dc.y, curl);
			o.curl = max(o.curl, curl * w);
		}
		// The undular bore's whelps ride on the profile; ones shorter than the vertex spacing or pixel fade out.
		float whelpAmplitude = wave.whelp * UnitsPerMetre * smoothstep(Math::PI, 0.5 * Math::PI, Math::TAU / wave.whelpLength * scale);
		[branch] if (whelpAmplitude > 0.0)
		{
			float2 whelps = ShoreWhelps(wave.phase, wave.stage, whelpAmplitude, wave.wavelength, wave.whelpLength);
			up += whelps.x;
			dUp += whelps.y;
		}
		o.bore = max(o.bore, smoothstep(0.70, 0.85, wave.stage) * gate);
		o.crest = max(o.crest, (height - meanLevel) * w);

		float front = curl > 0.0 ? lerp(ShoreBoreFront, ShoreCurlFront(wave.stage, wave.wavelength, height, forward, curlAmount), curl) : ShoreBoreFront;
		float aeration = ShoreAeration(wave.phase, wave.stage, front) * BreakerStrength(ctx);
		o.aeration = max(o.aeration, aeration * gate);
		o.impact = max(o.impact, ShoreSmooth(0.60, 0.68, wave.stage) * gate);
		o.clearFace = max(o.clearFace, ShoreClearFace(wave.phase, wave.stage) * gate);

		o.displacement += float3(dir * along, up - meanLevel) * w;
		// The phase rises towards the sea, a wavelength per cycle.
		float2 gradPhase = -dir / wave.wavelength;
		o.slope += (dUp * w) * gradPhase;

		// The aerated water churns: the roller is a heap of boiling lumps about half a wave height across, not a smooth
		// sheet with foam on it, and catches the light unevenly. Lumps shorter than the vertex spacing or pixel fade out.
		float lumpLength = max(0.6 * height, 8.0);
		float lumps = ShoreRollerLumps * height * aeration * smoothstep(Math::PI, 0.5 * Math::PI, Math::TAU / lumpLength * scale);
		[branch] if (lumps > 0.0)
		{
			float2 q = absXY / lumpLength;
			float3 churn = ShoreRollerChurn(q, Shore2.x);
			o.displacement.z += lumps * churn.x * w;
			o.slope += (lumps * w / lumpLength) * churn.yz;
			if (previous)
				o.previousDisplacement.z += lumps * ShoreRollerChurn(q, Shore2.y).x * w;
		}
		o.compression += float3(dir.x * gradPhase.x, dir.y * gradPhase.y, dir.x * gradPhase.y) * (dAlong * w);
		o.breakStage = max(o.breakStage, smoothstep(0.45, 0.6, wave.stage) * gate);
		if (shading)
			o.foam = max(o.foam, ShoreProfileAt(wave.phase, wave.stage, 1.0).x * gate * BreakerStrength(ctx));
		if (previous) {
			float cycles = ShorePhase(ctx.shore, coast, Shore2.y, j) / Math::TAU;
			float lastPhase = cycles - floor(cycles);
			float4 last = ShoreProfileAt(lastPhase, wave.stage, 0.0);
			float2 lastShape = float2(last.x * forward, last.y * height);
			if (curl > 0.0)
				lastShape = lerp(lastShape, ShoreCurlShape(lastPhase, wave.stage, wave.wavelength, height, forward, curlAmount), curl);
			lastShape.y += ShoreWhelps(lastPhase, wave.stage, whelpAmplitude, wave.wavelength, wave.whelpLength).x;
			o.previousDisplacement += float3(dir * lastShape.x, lastShape.y - meanLevel) * w;
		}
	}

	/**
	 * The shore wave trains at undisplaced camera-relative `positionWS`, weighted by their share ctx.train. One
	 * implementation for geometry (VS/DS, the occluder grid, ShoreSimCS), shading and tessellation; mirrored on the
	 * CPU (WaveModel.cpp) for swimming, buoyancy and floating objects.
	 * @param scale    vertex spacing or pixel footprint (units): waves shorter than it fade out (their slope variance goes
	 *                 to the roughness); 0: exact
	 * @param previous also evaluate last frame's displacement (motion vectors)
	 * @param shading  also read the foam (pixel shader)
	 */
	ShoreTrain ShoreWaves(float3 positionWS, WaveContext ctx, float scale, bool previous, bool shading, float yieldTo = 1.0)
	{
		ShoreTrain o = (ShoreTrain)0;
		[branch] if (ctx.train > 0.0)
		{
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float2 coast = CoastPoint(absXY, ctx.shore);
			AddShoreWaves(o, positionWS, absXY, coast, ctx, scale, previous, shading, 0, ctx.train, yieldTo);
		}
		return o;
	}

	/**
	 * How much of the water the shore waves still hold for the swash simulation (ShoreFoamCS relaxes it to them there):
	 * 1 offshore, 0 inshore where the simulation runs free. The hand-over runs while the wave is a bore - from about 0.7 of
	 * its height deep, inside its breaking depth (about 1.3 of its height) - and is done before the drawn bore's front
	 * collapses over the last 0.4 m (ShoreBoreHeight), so the simulation takes the bore's water at its full height.
	 */
	float SwashHandOver(WaveContext ctx)
	{
		float depthMetres = -ctx.elevation * MetresPerUnit;
		if (ctx.train <= 0.0)
			return depthMetres > 0.05 ? 1.0 : 0.0;
		float waveHeight = clamp(ctx.shoreHeight * MetresPerUnit * ctx.damping, 0.05, 3.0);
		float start = clamp(0.7 * waveHeight, 0.1, 1.2);
		return smoothstep(min(0.4, 0.6 * start), start, depthMetres);
	}

	/**
	 * The shore waves as the surface draws them: inshore of the hand-over the swash simulation carries the water (SwashSheet),
	 * and the analytic bore gives way to it (AddShoreWaves). Both drawn there, the train's crest (at its phase speed) and the
	 * simulation's (at the bore's) drifted apart, and small waves, whose drawn front stays up to the waterline, ran in as
	 * two: a second little crest appeared and jumped at the edge of the water.
	 */
	ShoreTrain DrawnShoreWaves(float3 positionWS, WaveContext ctx, float scale, bool previous, bool shading)
	{
		float simulated = 0.0;
		[branch] if (Foam2.w > 0.5)
		{
			float2 uv = (positionWS.xy + FrameBuffer::CameraPosAdjust.xy - Foam2.xy) * Foam2.z;
			if (all(uv > 0.0) && all(uv < 1.0))
				simulated = BorderFade(uv);
		}
		return ShoreWaves(positionWS, ctx, scale, previous, shading, lerp(1.0, SwashHandOver(ctx), simulated));
	}

	struct WaveResult
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float3 normal;                // surface normal
		float fftFoam;                // 0..1 the open sea's foam here: the cascades' accumulated foam (FftFoam), already faded with distance
		float slopeVariance;          // slope variance filtered out of the pixel (for roughness)
		float breakStage;             // 0..1 how far the shore waves here have got with breaking (s)
		float shoreFoam;              // 0..1 white water the shore waves carry here
		float curl;                   // 0..1 a breaker's curl shown here (ShoreTrain::curl): the normal may face down
		float rise;                   // units/s the water climbs a breaker's front face here (ShoreTrain::rise)
		float crest;                  // units above the still water the shore wave's crest stands here (ShoreTrain::crest)
		float bore;                   // 0..1 the shore wave has flattened into a bore (ShoreTrain::bore)
		float breakerFoam;            // 0..1 the breaking wave's own white water, a raw coverage like the sea's (BreakerFoam)
		float aeration;               // 0..1 churned white water (ShoreTrain::aeration): drawn solid, without the foam's pattern
		float clearFace;              // 0..1 a face about to plunge, clear of foam (ShoreTrain::clearFace)
	};

	// The open sea's foam settings (Tune1, Tune2), after the sample scene's _FoamSubtract0..3, the cascade weighting (ours:
	// Acerola counts all), _FoamDepthAttenuation, Acerola's depth 1 - Linear01Depth and the roughness modifier.

	/**
	 * The open sea's foam, 0..1: the weighted sum of the cascades' foam plus their offsets, clamped, and faded with the
	 * camera distance: lerp(0, saturate(sum), pow(depth, FoamDepthFalloff)) of FFTWater.shader.
	 * @param sum      the cascades' weighted foam and offsets
	 * @param distance camera distance of the pixel (units)
	 */
	float FftFoam(float sum, float distance)
	{
		float depth = 1.0 - saturate(distance * MetresPerUnit / max(Tune2.z, 1.0));
		return saturate(sum) * pow(depth, Tune2.y);
	}

	/// Running sums of the cascades' fields.
	struct WaveSums
	{
		float3 displacement;
		float3 previous;
		float dzdx, dzdy, dxdx, dydy, dxdy;
		float foam;  // the cascades' foam so far, weighted and offset (FftFoam)
		float seaFoam;        // the same with the sea's weights before the shore train's hand-over (BreakerFoamDrawn)
		float seaFoamWeight;  // the sum of those weights
		float slopeVariance;
		float curl;  // ShoreTrain::curl
	};

	WaveSums EmptySums()
	{
		WaveSums sums;
		sums.displacement = 0.0;
		sums.previous = 0.0;
		sums.dzdx = sums.dzdy = sums.dxdx = sums.dydy = sums.dxdy = 0.0;
		sums.foam = 0.0;
		sums.seaFoam = 0.0;
		sums.seaFoamWeight = 0.0;
		sums.slopeVariance = 0.0;
		sums.curl = 0.0;
		return sums;
	}

	/// Adds cascade `c`: `vertical` scales heights and slopes, `horizontal` the choppy displacement.
	void AddCascade(inout WaveSums sums, uint c, float vertical, float horizontal, float4 displacement, float4 derivatives)
	{
		sums.displacement += float3(displacement.xy * horizontal, displacement.z * vertical);
		sums.dzdx += derivatives.x * vertical;
		sums.dzdy += derivatives.y * vertical;
		float3 gradient = float3(derivatives.zw, displacement.w) * horizontal;
		sums.dxdx += gradient.x;
		sums.dydy += gradient.y;
		sums.dxdy += gradient.z;
	}

	/**
	 * The white water of a breaking shore wave, 0..1, made the way the open sea's is (OceanAssembleCS after
	 * GarrettGunnell/Water): from the folding of the wave's own surface. The surface P(x) = x + D(x) folds over itself where
	 * the Jacobian of its horizontal displacement, (1 + dDx/dx)(1 + dDy/dy) - (dDx/dy)^2, falls to zero: the water piles into
	 * the crest and tumbles down the face. The Jacobian is read from the train's compression (the displacement gradient the
	 * normal is built from) and thresholded as the sea's is, a coverage from where it falls below FoldStart; it counts only
	 * where the wave is breaking. The profile's own foam slice (ShoreProfileAt, slice 1) supplies the white water the fold
	 * has not yet reached, the lip's tip and the spill. Like the sea's foam it is blended in raw: the foam texture is kept
	 * for the flattened bore (Water.hlsl).
	 */
	float BreakerFoam(ShoreTrain train)
	{
		static const float FoldStart = 0.6;  // Jacobian at which the surface starts to count as folding (the sea's bias)
		float jacobian = (1.0 + train.compression.x) * (1.0 + train.compression.y) - train.compression.z * train.compression.z;
		float fold = saturate((FoldStart - jacobian) / FoldStart);
		// The fold counts once the jet has landed: before that the overturning face is clear water, white only at the lip's
		// crown (the profile's foam). The churned water (ShoreAeration) is white throughout.
		return saturate(max(max(train.foam, train.aeration), fold * train.breakStage * train.impact));
	}

	/**
	 * The breaker's white water as it is drawn, 0..1, from its coverage `coverage` (BreakerFoam), made of the sea's own
	 * foam: the cascades' persistent Jacobian foam (OceanAssembleCS) summed with their offsets as FftFoam does, with the
	 * offsets lowered by the coverage (ShoreBreakerFoamLift per cascade). Where the breaker begins, the sea's lace and
	 * streaks thicken; where it is whitest every texel passes and the water is solid white; it has the sea's pattern, scale
	 * and distance fade throughout, and rides the water with it. The sea's weights before the shore train's hand-over
	 * (ctx.sea) count, so the pattern is there in the surf, where the train carries the long waves; the cascades the
	 * hand-over turned off entirely are not read, and where none is (no FFT sea), the coverage is drawn as it is.
	 * A coverage of its own, smooth or broken up by a noise, drew every breaker as white blobs unlike the sea's foam.
	 * @param distance camera distance of the pixel (units)
	 */
	float BreakerFoamDrawn(float coverage, WaveSums sums, float distance)
	{
		static const float ShoreBreakerFoamLift = 1.2;  // offset added to each cascade's at full coverage: every texel passes
		float drawn = 0.0;
		[branch] if (coverage > 0.0)
		{
			float sea = FftFoam(sums.seaFoam + ShoreBreakerFoamLift * coverage * sums.seaFoamWeight, distance);
			drawn = lerp(coverage, sea, saturate(sums.seaFoamWeight / 0.5));
		}
		return drawn;
	}

	/// Adds the shore wave train.
	void AddShoreTrain(inout WaveSums sums, inout WaveResult o, ShoreTrain train)
	{
		sums.displacement += train.displacement;
		sums.previous += train.previousDisplacement;
		sums.dzdx += train.slope.x;
		sums.dzdy += train.slope.y;
		sums.dxdx += train.compression.x;
		sums.dydy += train.compression.y;
		sums.dxdy += train.compression.z;
		sums.slopeVariance += train.slopeVariance;
		sums.curl = train.curl;
		o.breakStage = train.breakStage;
		o.shoreFoam = train.foam;
		o.curl = train.curl;
		o.crest = train.crest;
		o.bore = train.bore;
		o.breakerFoam = BreakerFoam(train);
		o.aeration = train.aeration;
		o.clearFace = train.clearFace;
	}

	WaveResult FinishWaves(WaveSums sums, WaveResult o)
	{
		// Normal of the parametric surface P(x, y) = (x + Dx, y + Dy, Dz)
		float3 tx = float3(1.0 + sums.dxdx, sums.dxdy, sums.dzdx);
		float3 ty = float3(sums.dxdy, 1.0 + sums.dydy, sums.dzdy);
		// tx and ty become parallel where the surface folds over itself; keep the normal defined there,
		// and facing up where it has folded past vertical (the visible side of the fold). A breaker's curl is no fold
		// but one sheet turning over: the underside of its lip faces down.
		float3 n = cross(tx, ty);
		o.normal = SafeNormalize(n.z < 0.0 && sums.curl <= 0.0 ? -n : n, float3(0, 0, 1));
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
					float4 d, g, dp = 0.0;
					[branch] if (any(ctx.flow))
					{
						// On a river the cascade is read twice, carried along the current (FlowPhases).
						FlowBlend b = FlowPhases(ctx.flow, c, FlowClock());
						float3 uv0 = float3(CascadeUV(positionWS.xy - b.offset0, c), c);
						float3 uv1 = float3(CascadeUV(positionWS.xy - b.offset1, c), c);
						d = b.weight0 * OceanDisplacement.SampleLevel(OceanSampler, uv0, lod) + b.weight1 * OceanDisplacement.SampleLevel(OceanSampler, uv1, lod);
						g = b.weight0 * OceanDerivatives.SampleLevel(OceanSampler, uv0, lod) + b.weight1 * OceanDerivatives.SampleLevel(OceanSampler, uv1, lod);
						if (previous) {
							FlowBlend pb = FlowPhases(ctx.flow, c, FlowClock() - FlowFrameTime);
							float3 puv0 = float3(CascadeUV(positionWS.xy - pb.offset0, c), c);
							float3 puv1 = float3(CascadeUV(positionWS.xy - pb.offset1, c), c);
							dp = pb.weight0 * OceanPreviousDisplacement.SampleLevel(OceanSampler, puv0, lod) + pb.weight1 * OceanPreviousDisplacement.SampleLevel(OceanSampler, puv1, lod);
						}
					}
					else
					{
						d = OceanDisplacement.SampleLevel(OceanSampler, uv, lod);
						g = OceanDerivatives.SampleLevel(OceanSampler, uv, lod);
						if (previous)
							dp = OceanPreviousDisplacement.SampleLevel(OceanSampler, uv, lod);
					}
					AddCascade(sums, c, vertical, horizontal, d, g);
					if (previous)
						sums.previous += float3(dp.xy * horizontal, dp.z * vertical);
				}
			}
		}
		AddShoreTrain(sums, o, DrawnShoreWaves(positionWS, ctx, spacing, previous, false));
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
	 * @param surfFoam 0..1 the foam the surf left on the water here (ShoreFoamBoth().x): on a face that is not flat it is
	 *                 drawn with the breaker's white water, through the sea's foam
	 */
	WaveResult EvaluateWavesFiltered(float3 positionWS, WaveContext ctx, float surfFoam)
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
				// The screen derivatives need uniform control flow; sampling with them as explicit gradients does not, so a
				// cascade the context has turned off (the shore train's, the open sea's in a pond) costs no texture reads.
				float2 gx = ddx_fine(uv.xy);
				float2 gy = ddy_fine(uv.xy);
				[branch] if (vertical > 0.0)
				{
					float4 d, g;
					float2 m;
					float foam;
					[branch] if (any(ctx.flow))
					{
						// The same two reads as the geometry's (FlowPhases), so the normals and moments match the mesh.
						FlowBlend b = FlowPhases(ctx.flow, c, FlowClock());
						float3 uv0 = float3(CascadeUV(positionWS.xy - b.offset0, c), c);
						float3 uv1 = float3(CascadeUV(positionWS.xy - b.offset1, c), c);
						d = b.weight0 * OceanDisplacement.SampleGrad(OceanSampler, uv0, gx, gy) + b.weight1 * OceanDisplacement.SampleGrad(OceanSampler, uv1, gx, gy);
						g = b.weight0 * OceanDerivatives.SampleGrad(OceanSampler, uv0, gx, gy) + b.weight1 * OceanDerivatives.SampleGrad(OceanSampler, uv1, gx, gy);
						m = b.weight0 * OceanSurface.SampleGrad(OceanSampler, uv0, gx, gy) + b.weight1 * OceanSurface.SampleGrad(OceanSampler, uv1, gx, gy);
						foam = b.weight0 * OceanFoam.SampleGrad(OceanSampler, uv0, gx, gy) + b.weight1 * OceanFoam.SampleGrad(OceanSampler, uv1, gx, gy);
					}
					else
					{
						d = OceanDisplacement.SampleGrad(OceanSampler, uv, gx, gy);
						g = OceanDerivatives.SampleGrad(OceanSampler, uv, gx, gy);
						m = OceanSurface.SampleGrad(OceanSampler, uv, gx, gy);
						foam = OceanFoam.SampleGrad(OceanSampler, uv, gx, gy);
					}
					float horizontal = vertical * steepnessScale;
					AddCascade(sums, c, vertical, horizontal, d, g);
					sums.foam += saturate(vertical / max(Tune2.x, 0.01)) * (foam + Tune1[c]);
					float seaShare = saturate(ctx.sea[c] / max(Tune2.x, 0.01));
					sums.seaFoam += seaShare * (foam + Tune1[c]);
					sums.seaFoamWeight += seaShare;
					float variance = max(m.x - g.x * g.x, 0.0) + max(m.y - g.y * g.y, 0.0);
					float texelsPerUV = rcp(Cascade0[c].x * Cascade0[c].w);
					float minified = MinifiedShare(gx * texelsPerUV, gy * texelsPerUV);
					sums.slopeVariance += variance * vertical * vertical * minified;
				}
			}
		}
		AddShoreTrain(sums, o, DrawnShoreWaves(positionWS, ctx, footprint, false, true));
		o = FinishWaves(sums, o);
		// A face about to plunge is clear water: the sea's foam and what the surf left on it are drawn off it (ShoreClearFace).
		float clear = 1.0 - o.clearFace;
		o.fftFoam = FftFoam(sums.foam, length(positionWS)) * clear;
		// The breaker's own white water, drawn with the sea's foam; once the bore lies flat the foam texture takes over.
		// The surf's leftover foam rides the water up every rising face: there it is drawn with the sea's foam too, as
		// thickened lace. Added raw, as a coverage with no pattern, it laid a flat grey veil over every breaker's face.
		float flatness = smoothstep(0.80, 0.95, o.normal.z);
		float boreFlat = o.bore * flatness;
		o.breakerFoam = BreakerFoamDrawn(max(o.breakerFoam * (1.0 - boreFlat), saturate(surfFoam) * clear * (1.0 - flatness)), sums, length(positionWS));
		// Where the water is churned through (the crushed tube, the splash-up, the roller) and at the lip's crown it is
		// opaque white, with no pattern: drawn as the sea's lace thickened, the impact showed the foam texture's holes. The lace
		// stays where the aeration thins, behind the roller and around it.
		o.breakerFoam = max(o.breakerFoam, smoothstep(0.40, 0.80, max(o.aeration, o.shoreFoam * (1.0 - boreFlat))));
		return o;
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

	// Horizontal ripple displacement (Crest 4 ShapeCombine, _DYNAMIC_WAVE_SIM_ON): the surface points shift along the
	// ripples' gradient like those of a Gerstner wave, which sharpens the crests.
	// The strength (Tune3.x, Crest default 3) and the largest displacement in simulation texels (Tune3.y, Crest default 0.3) are settings.

	/// The displayed state (height in simulation units) at `uv`, of this frame (or last frame's copy for motion vectors).
	float RippleDisplayedAt(float2 uv, bool previous)
	{
		float2 steps = previous ? RipplePreviousTexture.SampleLevel(LinearClampSampler, uv, 0).xy :
		                          RippleTexture.SampleLevel(LinearClampSampler, uv, 0).xy;
		return lerp(steps.y, steps.x, previous ? Ripple2.w : Ripple2.z);
	}

	/**
	 * How the ripples displace the surface at the undisplaced `positionWS`: xy horizontally, z the height (units).
	 * Functions of the position only, so the stencil and colour passes (and last frame's, for motion vectors) agree.
	 * The horizontal part is Crest's HorizDisplace * grad(h) / k with k = 2 pi / (3 texels), clamped to a share of a texel.
	 */
	float3 RippleDisplacement(float3 positionWS, bool previous)
	{
		float2 uv;
		if (!RippleUV(positionWS.xy + FrameBuffer::CameraPosAdjust.xy, previous ? Ripple2.xy : Ripple0.xy, uv))
			return 0.0;
		float scale = Ripple1.x * BorderFade(uv);
		float t = Ripple1.w;
		float texelUnits = t / Ripple0.z;
		float hL = RippleDisplayedAt(uv - float2(t, 0), previous);
		float hR = RippleDisplayedAt(uv + float2(t, 0), previous);
		float hD = RippleDisplayedAt(uv - float2(0, t), previous);
		float hU = RippleDisplayedAt(uv + float2(0, t), previous);
		float2 grad = float2(hR - hL, hU - hD) / (2.0 * texelUnits) * scale;
		float wavenumber = Math::TAU / (3.0 * texelUnits);
		float maxDisplacement = Tune3.y * texelUnits;
		float2 horizontal = clamp(Tune3.x * grad / wavenumber, -maxDisplacement, maxDisplacement);
		return float3(horizontal, RippleDisplayedAt(uv, previous) * scale);
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

	/// Height gradient of the ripples (units/unit).
	float2 RippleSlope(float3 positionWS, float depth)
	{
		float2 uv;
		if (!RippleUV(positionWS.xy + FrameBuffer::CameraPosAdjust.xy, Ripple0.xy, uv))
			return 0.0;
		float t = Ripple1.w;
		float hL = RippleDisplayed(uv - float2(t, 0));
		float hR = RippleDisplayed(uv + float2(t, 0));
		float hD = RippleDisplayed(uv - float2(0, t));
		float hU = RippleDisplayed(uv + float2(0, t));
		float texelUnits = t / Ripple0.z;
		float2 grad = float2(hR - hL, hU - hD) / (2.0 * texelUnits) * Ripple1.x * Ripple1.y * RippleDepthFade(depth);
		return grad * BorderFade(uv);
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
	 * the run-up R and drains back over Swash Linger (Shore3.x) times that; R is Stockdon et al.'s (2006) 2 % run-up of the deep-water
	 * significant height. Once a crest has drained its tip keeps falling below the still water instead of resting at
	 * it: the vertices interpolate the tip linearly, and a tip held at 0 next to a running one put the swash front on the
	 * triangle edges. Each crest carries its own height (ShoreSet), so every uprush reaches its own height.
	 * @param previous at last frame's clock (motion vectors)
	 */
	float2 SwashTip(float3 positionWS, WaveContext ctx, bool previous, out float rate)
	{
		float2 result = 0.0;
		rate = 0.0;
		// Deeper than 1.5 run-ups the sheet stays under the depth-limited trough: nothing there can show the swash, so most
		// of the sea skips it.
		[branch] if (ctx.beach > 0.0 && ctx.elevation > -1.5 * max(Shore0.w, 1.0))
		{
			// Crests reach the waterline once per cycle of the clock, at its rate this frame.
			float omega = max(Shore2.z, 1e-3);
			float period = Math::TAU / omega;
			float b = clamp(ctx.shore.slope, 0.02, 0.3);
			float sin2 = b * b / (1.0 + b * b);
			float decel = GravityMetres * sin2;  // the tip's height: g sin^2(beta), m/s^2
			// Stockdon's L0 is the real sea's (Shore4.z), not the shore train's held-short period: sqrt(H0 L0) gave a third of the run-up.
			float deepLength = (Shore4.z > 0.0 ? Shore4.z : Shore3.w) * MetresPerUnit;
			float runupPerHeight = 1.1 * (0.35 * b + 0.5 * sqrt(0.563 * b * b + 0.004));  // Stockdon R2 / sqrt(H0 L0)
			float elevation = ctx.elevation * MetresPerUnit;

			// The train's phase here (up the beach the waterline's, carried up the ray), and the time since the last crest
			// reached it: each uprush starts as its bore's front arrives (ShoreBoreFront into its cycle).
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float2 coast = CoastPoint(absXY, ctx.shore);
			float cycles = ShorePhase(ctx.shore, coast, previous ? Shore2.y : Shore2.x, 0) / Math::TAU - ShoreBoreFront;
			float wave = floor(cycles);
			float sinceLast = (cycles - wave) * period;
			// Stockdon's run-up is fitted to the deep-water significant height, not the train's rms height.
			float deepHeight = ctx.shoreHeight * MetresPerUnit * ctx.damping;

			// The uprushes of the last periods; the oldest fades out before it leaves the window, so the tip never pops. The
			// newest crest's own path continues below the still water after it drained. Kept: the one whose water stands
			// highest here. The backwash takes Shore3.x times as long as the uprush (Swash Linger): the water soaks into the
			// sand and drags on it on the way down, so the sheet and its foam stay on the beach for a while.
			const uint candidates = 4;
			float window = candidates * period;
			float linger = max(Shore3.x, 1.0);
			float tip = -1e9;
			float lens = 0.0;
			float surface = -1e9;
			float tipRate = 0.0;
			[unroll] for (uint j = 0; j < candidates; j++)
			{
				float tau = sinceLast + j * period;
				float H = deepHeight * ShoreSet(coast, wave - j);
				float runup = runupPerHeight * sqrt(H * deepLength);
				float w0 = sqrt(2.0 * decel * runup);
				float fade = saturate((window - tau) / period);
				// Up ballistically to the run-up at tPeak, then down at decel / linger^2: the drain takes linger times as long.
				float tPeak = w0 / max(decel, 1e-4);
				float after = max(tau - tPeak, 0.0);
				float rise = min(tau, tPeak);
				float z = (w0 * rise - 0.5 * decel * rise * rise - 0.5 * decel * after * after / (linger * linger)) * ctx.beach;
				// dz/dt (m/s): rising at w0 - decel t, draining at decel t' / linger^2.
				float zRate = (tau < tPeak ? w0 - decel * tau : -decel * after / (linger * linger)) * ctx.beach;
				if (z > 0.0) {
					z *= fade;
					zRate *= fade;
				}
				// The sheet thins with the time its water has had to spread, which the slower drain stretches too.
				float t = max(rise + after / linger, 1e-3);
				float k = 1.0 / (9.0 * GravityMetres * t * t * sin2);  // 1/m
				float s = SwashTop(elevation, z, k);
				// Where none covers the bed, the highest tip (the one the vertices around may need).
				if (s > surface || (s == surface && z > tip)) {
					surface = s;
					tip = z;
					lens = k;
					tipRate = zRate;
				}
			}
			result = float2(tip * UnitsPerMetre, lens * MetresPerUnit);
			rate = tipRate * UnitsPerMetre;
		}
		return result;
	}

	/// SwashTip without the tip's rate.
	float2 SwashTip(float3 positionWS, WaveContext ctx, bool previous)
	{
		float rate;
		return SwashTip(positionWS, ctx, previous, rate);
	}

	/**
	 * The foam at absolute `absXY`, 0..1: x what the surf has left on the water (ShoreFoamCS), y what the swash carries
	 * (SwashStepCS). Fades out over the grid's outer tenth; zero outside it.
	 */
	float2 ShoreFoamBoth(float2 absXY)
	{
		float2 foam = 0.0;
		[branch] if (Foam2.w > 0.5)
		{
			float2 uv = (absXY - Foam2.xy) * Foam2.z;
			if (all(uv > 0.0) && all(uv < 1.0)) {
				// Cubic B-spline in four bilinear reads (Sigg & Hadwiger 2005): the grid's 35 cm texels drew the foam's edges
				// as diamonds under a bilinear read.
				uint w, h;
				ShoreFoamTexture.GetDimensions(w, h);
				float2 size = float2(w, h);
				float2 q = uv * size - 0.5;
				float2 base = floor(q);
				float2 f = q - base;
				float2 f2 = f * f;
				float2 f3 = f2 * f;
				float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
				float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
				float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
				float2 w3 = f3 / 6.0;
				float2 g0 = w0 + w1;
				float2 g1 = w2 + w3;
				// Texel b sits at uv (b + 0.5) / size: the two reads fall between texels base-1 and base, and base+1 and base+2.
				float2 h0 = (base - 0.5 + w1 / g0) / size;
				float2 h1 = (base + 1.5 + w3 / g1) / size;
				float2 s00 = ShoreFoamTexture.SampleLevel(LinearClampSampler, float2(h0.x, h0.y), 0).xw;
				float2 s10 = ShoreFoamTexture.SampleLevel(LinearClampSampler, float2(h1.x, h0.y), 0).xw;
				float2 s01 = ShoreFoamTexture.SampleLevel(LinearClampSampler, float2(h0.x, h1.y), 0).xw;
				float2 s11 = ShoreFoamTexture.SampleLevel(LinearClampSampler, float2(h1.x, h1.y), 0).xw;
				foam = saturate((g0.y * (g0.x * s00 + g1.x * s10) + g1.y * (g0.x * s01 + g1.x * s11)) * BorderFade(uv));
			}
		}
		return foam;
	}

	/// The foam the surf has left on the water at absolute `absXY` (ShoreFoamCS), 0..1.
	float ShoreFoamAt(float2 absXY)
	{
		return ShoreFoamBoth(absXY).x;
	}

	/// Swash shallower than this (units, 3 mm) is the sand's wet sheen, not water.
	static const float SwashMinDepth = 0.003 * UnitsPerMetre;

	/**
	 * Height of the swash's surface above the still water over a bed `elevation` above it (units), for the sheet's
	 * `surface` and `depth` (SwashSheet): -1e6 where the sheet does not cover the bed.
	 */
	float SwashSurface(float elevation, float surface, float depth)
	{
		return depth > SwashMinDepth && surface > elevation ? surface : -1e6;
	}

	/**
	 * The swash at undisplaced camera-relative `positionWS`: x its surface, y its depth (units; depth 0 where dry). Near
	 * the camera the simulated sheet (SwashStepCS): every bore runs up the beach as water of its own, in its own direction,
	 * and meets the backwash and the uprushes beside it. Past the simulation's grid, where it fades out, the analytic
	 * run-up of the crests (SwashTip).
	 */
	float2 SwashSheet(float3 positionWS, WaveContext ctx)
	{
		float2 sheet = 0.0;
		float simulated = 0.0;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		[branch] if (Foam2.w > 0.5)
		{
			float2 uv = (absXY - Foam2.xy) * Foam2.z;
			if (all(uv > 0.0) && all(uv < 1.0)) {
				simulated = BorderFade(uv);
				sheet = ShoreFoamTexture.SampleLevel(LinearClampSampler, uv, 0).yz;
			}
		}
		[branch] if (simulated < 1.0 && ctx.beach > 0.0)
		{
			float2 tip = SwashTip(positionWS, ctx, false);
			float surface = SwashTop(ctx.elevation, tip.x, tip.y);
			float2 analytic = surface > -1e5 ? float2(surface, surface - max(ctx.elevation, 0.0)) : float2(ctx.elevation, 0.0);
			sheet = lerp(analytic, sheet, simulated);
		}
		return sheet;
	}

	/// Largest vertical reach of the surface (units): waves within their statistical bound (doubled for the fetch
	/// energy gain), the shore wave train (shoaled at most to about the deep-water height, its crest up to 2A above
	/// the mean), the swash up the beach and the ripples. Used to clamp, which also flushes NaN.
	float DisplacementLimit()
	{
		float shoreWaves = 2.0 * (max(Shore1.z, 0.0) + max(Shore1.w, 0.0));
		return 2.0 * max(Params2.z, 0.0) + shoreWaves * max(1.0, Tune4.y / 1.55) + max(Shore0.w, 0.0) + 4.0 * max(Ripple1.x, 0.0) + 1.0;
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
	// Open-sea foam is a direct port of GarrettGunnell/Water (Acerola; MIT License, Copyright (c) 2023 Garrett Gunnell,
	// https://github.com/GarrettGunnell/Water). OceanAssembleCS keeps one foam value per texel of each cascade, decayed
	// and topped up from the Jacobian of that cascade's own displacement. Here the cascades' foam is summed with a
	// per-cascade offset, as FFTWater.shader does, faded with distance, and drawn as a flat blend towards the foam colour.
	// The one change: each cascade counts by its local share (WaveContext::cascade), so ponds, sheltered water and the
	// surf, where the shore train has taken the FFT's waves over, get no FFT foam.
	//
	// Shore waves carry their white water in their profile (ShoreProfile.h), so it travels with the wave; what the breakers
	// leave behind on the water is simulated (ShoreFoamCS, ShoreFoamAt): it rides the water and the swash and decays.
	// ------------------------------------------------------------------------

	/// Vertical speed the swash reach sinks at (units/s): about as fast as a backwash drains down a beach.
	static const float ReachDrainRate = 1.0 * UnitsPerMetre;

	/// The highest the water reached in the last moments at absolute `absXY` (ShoreSimCS reach, units above the still
	/// water), or -1e6 outside the swash grid. It drains with the backwash, unlike the wet level the sand dries by.
	float SwashReach(float2 absXY)
	{
		float level = -1e6;
		[branch] if (Swash0.w > 0.5)
		{
			float2 uv = (absXY - Swash0.xy) * Swash0.z;
			if (all(uv > 0.0) && all(uv < 1.0))
				level = SwashWetTexture.SampleLevel(LinearClampSampler, uv, 0).y;
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
	/// The swash's foam (ShoreFoamBoth().y) above which what it stranded on the sand is drawn there.
	static const float StrandedFoamShown = 0.03;

	bool AboveWaterline(float3 positionWS, float4 shoreState)
	{
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		float terrainZ = TerrainHeight(absXY);
		if (terrainZ < -1e8)
			return false;
		float bed = terrainZ - (shoreState.w + FrameBuffer::CameraPosAdjust.z);
		// Any sheet counts, however thin: the colour pass fades a film of a few millimetres into the ground anyway. Cut at
		// SwashMinDepth, the depth interpolated over the triangles crossed it along their edges as a draining film
		// thinned, and cut straight-edged, right-angled holes into the water.
		float film = shoreState.y > 0.0 ? shoreState.x : -1e6;
		if (bed <= max(shoreState.z, film) + 0.05 * UnitsPerMetre)
			return false;
		// Foam the swash stranded on the sand is drawn on the ground there (Water.hlsl).
		if (ShoreFoamBoth(absXY).y > StrandedFoamShown)
			return false;
		// Kept up to where the water reached in the last moments, per pixel: the tip interpolated from the vertices runs ahead of or
		// behind the true one inside each triangle, which cut straight-edged holes into a fresh uprush. The colour pass
		// finds the true waterline there (Water.hlsl) and shows the ground where it is dry.
		return bed > SwashReach(absXY) + 0.05 * UnitsPerMetre;
	}

	// ------------------------------------------------------------------------
	// Full surface displacement used by geometry (VS / DS)
	// ------------------------------------------------------------------------
	struct SurfaceDisplacement
	{
		float3 current;
		float3 previous;
		float4 shore;  // x swash surface, y swash depth (units; SwashSheet), z waves without the swash (units above the still water), w still water level (camera-relative z)
	};

	/// @param motion also work out last frame's surface: only the stencil pass writes motion vectors
	SurfaceDisplacement DisplaceSurface(float3 positionWS, float flowDamping, float2 flowVelocity, float spacing, bool motion)
	{
		SurfaceDisplacement o;
		float damping = DisplacementFade(positionWS) * flowDamping;

		WaveContext ctx = BuildWaveContext(positionWS, damping);
		ctx.flow = flowVelocity;
		WaveResult now = EvaluateWaves(positionWS, ctx, spacing, motion);

		float rippleFade = damping * RippleDepthFade(ctx.depth);
		o.current = now.displacement + RippleDisplacement(positionWS, false) * rippleFade;
		o.previous = motion ? now.previousDisplacement + RippleDisplacement(positionWS, true) * rippleFade : 0.0;

		// The swash: each breaking wave runs up the beach above the still water as a thin sheet and drains back.
		float2 sheet = SwashSheet(positionWS, ctx);
		float swashTop = SwashSurface(ctx.elevation, sheet.x, sheet.y);
		// The waves alone vary smoothly between vertices; the sheet ends at its front, so the pixel shader rebuilds it over
		// its own bed (AboveWaterline).
		float waves = o.current.z;
		o.current.z = max(o.current.z, swashTop);
		o.previous.z = max(o.previous.z, swashTop);

		// Near the waterline the mesh never dips under the bed: where the water ends is decided per pixel against the
		// ground behind it (Water.hlsl), so the edge follows the true terrain instead of the triangles cutting through
		// it. Held up within reach of the water at the next vertex (the bed may climb that much in between), and wherever
		// the swash reached in the last moments (SwashReach, which drains with the backwash): the triangles that sank under
		// the sand there cut straight-edged holes into its sheet.
		// The bed is taken where the waves have carried the vertex (they move it along by up to a wavelength, towards the
		// beach and back), and as high as the game's terrain triangles can stand there: held above the bed under its rest
		// position, the triangles of a vertex carried up the slope sank into the sand and left pockets of ground in the water.
		if (ctx.hasTerrain > 0.5) {
			float reach = max(0.5 * UnitsPerMetre, 2.0 * spacing * length(ctx.terrainGrad));
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float ceiling = TerrainCeiling(absXY + o.current.xy);
			float bed = ceiling > -1e8 ? ceiling - (positionWS.z + FrameBuffer::CameraPosAdjust.z) : ctx.elevation;
			// Any sheet holds it, however thin, as AboveWaterline keeps it: a vertex let down under a film the pixels
			// still draw stood a wall of triangles up from the sand. The film's level comes in over its first few
			// millimetres, from a metre below the reach: switched on at any depth, it stepped along the crest of a wave
			// whose water the simulation was just taking over, and lifted part of a curling lip into a right-angled jog.
			float dry = 1.0 - saturate(sheet.y / SwashMinDepth);
			float film = sheet.x - dry * (reach + UnitsPerMetre);
			float level = max(max(o.current.z, film), SwashReach(absXY));
			// Held on the sand under the foam the swash stranded there too, so its pixels exist (AboveWaterline); eased in
			// the same way.
			float stranded = saturate((ShoreFoamBoth(absXY).y - StrandedFoamShown) / 0.05);
			level = max(level, bed - (1.0 - stranded) * (reach + UnitsPerMetre));
			// Held at the bed up to `reach` above the water, and no higher than that past it, so the vertex slides into the
			// sand continuously as the bed rises or the water drains. Held or dropped back to the waves, neighbouring
			// vertices on either side of the cut stood a wall of triangles up along a straight row of the grid.
			float lift = WaterlineLift(spacing, ctx.terrainGrad);
			float lifted = min(bed, level + reach) + lift;
			// Not a breaker's lip: thrown ahead over the trough it is measured against the ground where it lands, and was
			// lifted onto a ledge it plunges into. The ground stands in front of it there.
			float hold = 1.0 - saturate(now.curl);
			o.current.z = max(o.current.z, lerp(o.current.z, lifted, hold));
			o.previous.z = max(o.previous.z, lerp(o.previous.z, lifted, hold));
		}

		// Hard bound. Clamping also flushes NaN (D3D min/max return the non-NaN operand), so a bad input can never throw
		// the mesh off-screen, but it lands on -limit: a hole in the water, not a harmless flat spot.
		float limit = DisplacementLimit();
		o.current = clamp(o.current, -limit, limit);
		o.previous = clamp(o.previous, -limit, limit);
		o.shore = float4(clamp(sheet.x, -limit, limit), clamp(sheet.y, 0.0, limit), clamp(waves, -limit, limit), positionWS.z);
		return o;
	}
}

#endif  // __PBR_WATER_HLSLI__
