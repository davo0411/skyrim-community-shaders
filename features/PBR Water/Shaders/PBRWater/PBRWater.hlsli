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
// coastline map traced over the real depths, which shoals, steepens, curls over and breaks. In the inner surf each
// broken wave is handed to a shallow water simulation (SwashStepCS), which carries it on as a bore, collapses it at the
// waterline and runs its water up the beach and back; the drawn surface passes from the one to the other over a fixed
// band of depths (ComposeSurf). The water ends per pixel against the ground behind it (Water.hlsl), never where the
// mesh's triangles cut the terrain.
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
		float4 Swash0;                   // xy wet grid corner (absolute units), z 1 / its extent (units), w enabled (ShoreSimCS; read by the lit terrain and objects)
		float4 Shore2;                   // x wave clock (rad, wrapped after ShoreClockCycles waves), y the clock last frame, z its rate (rad/s): the eased angular frequency, w peak period Tp (s)
		float4 Shore3;                   // x swash linger (the bed's drag on the surf simulation, times), y undular bore whelps strength, z breaker curl, w deep-water wavelength L0 (units)
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
	Texture2D<float2> ShoreRoomTexture : register(t124);                // x 0..1 room for the shore wave train: the width of the water against the wavelength, y the depth a wave's crest answers to (m; ShoreCrestDepth)
	Texture2D<float2> SurfaceNoiseTexture : register(t126);              // x TurbidityPatches, y WindGust around the camera (SurfaceNoiseCS)
	Texture2D<float4> ShoreFoamTexture : register(t127);                // SwashOutCS: x the foam the surf left on the water, y the surf simulation's surface (units above the still water; the bed where dry), z its depth (units), w its foam
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
			r.room = ShoreRoomTexture.SampleLevel(LinearClampSampler, uv, 0).x;
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

	/// Where the bore's front stands in its wave, in phase. Mirrors BoreFront in ShoreProfile.cpp.
	static const float ShoreBoreFront = 0.16;
	/// The phase of a wave at which the depth it answers to is read: where its crest stands as it breaks. Mirrors
	/// PBRWaterModel::ShoreCrestReference.
	static const float ShoreCrestReference = 0.32;
	/// The least a bore's front stands over the bed as it comes to the waterline (units): about u0^2 / 9g for uprushes of
	/// a few metres a second (Shen and Meyer 1963). Only seen where no surf simulation takes the wave over (far from the
	/// camera): run down to nothing, the bore sank away over the last metres of water.
	static const float ShoreBoreFloor = 0.25 * UnitsPerMetre;
	/// Over this height above the still water (units) the shore waves end: up the beach the water is the surf
	/// simulation's (ComposeSurf).
	static const float ShoreTrainLanding = 0.1 * UnitsPerMetre;

	/**
	 * The phase of a wave's crest at stage `stage`: it moves towards the front of the wave as the wave shoals (0.5 to 0.32)
	 * and, as it breaks, runs ahead to where a plunger's lip comes down and the bore's front forms (0.16). It only ever
	 * moves forward. Mirrors Evaluate in ShoreProfile.cpp and PBRWaterModel's ShoreCrestPhase.
	 */
	float ShoreCrestPhase(float stage)
	{
		return 0.5 - 0.18 * smoothstep(0.0, 0.5, stage) - (0.32 - ShoreBoreFront) * smoothstep(0.50, 0.72, stage);
	}

	/**
	 * How far the crests run ahead of or behind a straight front along the coast (waves): long, slowly drifting bends, so
	 * stretches of beach break at different times. A function of the waterline point and the clock only, so every point of
	 * a ray and every pass agree; each term turns over a whole number of times per wrap of the clock (1024 waves).
	 * Shore1.x scales it. Mirrors PBRWaterModel::ShoreWobble.
	 * @param coast the waterline the wave runs to (CoastPoint)
	 * @param clock the shore clock (rad)
	 */
	float ShoreWobble(float2 coast, float clock)
	{
		float2 q = coast / max(2.0 * Shore3.w, 1.0);
		static const float2 d0 = float2(1.0, 0.0);
		static const float2 d1 = float2(-0.504846, 0.863209);
		static const float2 d2 = float2(-0.400799, -0.916166);
		float w = 0.5 * sin(dot(q, d0) + clock * (37.0 / 1024.0)) +
		          0.3 * sin(1.73 * dot(q, d1) - clock * (61.0 / 1024.0)) +
		          0.2 * sin(2.91 * dot(q, d2) + clock * (89.0 / 1024.0));
		return Shore1.x * w;
	}

	/// The phase of the shore train (rad): each cycle is one wave, rising towards the sea. Mirrors the CPU's ShorePhase.
	float ShorePhase(ShoreRay ray, float2 coast, float clock)
	{
		return clock + ray.phaseLag + Math::TAU * (ray.along + ShoreWobble(coast, clock));
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

	/// The bore's crest as a share of the wave's height: the profile's collapse leaves 1 - 0.42 of it (ShoreProfile.cpp).
	static const float ShoreBoreCrest = 0.58;

	/**
	 * The share of the depth a broken wave's height saturates at in the inner surf, on a bed of slope `slope` (tan beta): about
	 * 0.4 on a gentle beach to 0.6 on a steep one (Thornton and Guza 1983; Raubenheimer, Guza and Elgar 1996).
	 */
	float ShoreSaturatedShare(float slope)
	{
		return 0.4 + 0.2 * saturate((clamp(slope, 0.01, 0.3) - 0.02) / 0.08);
	}

	/**
	 * A breaking wave's height (m) once it is a bore, from its height `height` as the breaker limit holds it. The limit
	 * (gamma h, gamma up to 1.2) holds the breaker; the bore it collapses into is lower (the profile's collapse takes 0.58 of
	 * the height at the bore's front), and in the inner surf the heights saturate at a share of the depth, H/h about 0.4 on
	 * a gentle beach to 0.6 on a steep one (Thornton and Guza 1983; Raubenheimer, Guza and Elgar 1996). The bore does not
	 * vanish with the depth, as that share does: it ends at the waterline with a front of a share of the breaker's height,
	 * whose water runs on as the swash (Shen and Meyer 1963; Peregrine and Williams 2001). The change comes in with the bore
	 * (stage 0.66 to 0.80), lowering the breaker in the inner surf and holding up its front until the last few decimetres of water. Mirrors PBRWaterModel::ShoreBoreHeight.
	 * @param ratio shoaled height over the breaker limit (> 1: broken)
	 * @param depth still water depth under the crest (m)
	 */
	float ShoreBoreHeight(float height, float stage, float ratio, float slope, float index, float depth)
	{
		[branch] if (ratio > 1.0)
		{
			float breakerHeight = index * depth * pow(abs(ratio), 0.8);  // H_b: the limit at the break point's depth
			float saturated = ShoreSaturatedShare(slope) * depth;
			// The front is held up only while there is water for it: over the last few decimetres the bore collapses into the
			// swash. Held all the way, it stood on the dry sand under the train.
			float front = max(saturated, 0.15 * breakerHeight * smoothstep(0.05, 0.4, depth)) / ShoreBoreCrest;
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
		float froude = ShoreBoreCrest * wave.height / depthA;
		float weakBore = (1.0 - smoothstep(0.25, 0.5, froude)) * smoothstep(0.7, 0.85, wave.stage);
		float barLee = smoothstep(1.0, 1.6, depthA / crestDepth) * smoothstep(0.25, 0.4, wave.stage);
		return Shore3.y * max(weakBore, barLee) * 0.3 * wave.height;
	}

	/**
	 * The whelps' height above the wave's own profile and its rate of change with the phase (both in the units of
	 * `amplitude`): crests of spacing `spacing` that start at the wave's crest (ShoreCrestPhase: the bore's front once it
	 * has broken) and die away behind it within a few spacings. Nothing ahead of it. Mirrors PBRWaterModel::ShoreWhelps.
	 * @param x         the phase on the wave
	 * @param stage     the wave's stage
	 * @param amplitude height of the first crest
	 * @param lambda    the wave's wavelength (units)
	 * @param spacing   the whelps' wavelength (units)
	 */
	float2 ShoreWhelps(float x, float stage, float amplitude, float lambda, float spacing)
	{
		float anchor = ShoreCrestPhase(stage);
		float2 o = 0.0;
		[branch] if (amplitude > 0.0 && x > anchor)
		{
			float c = lambda / spacing;  // whelps per phase
			float s = (x - anchor) * c;
			float e = exp(-s / 2.2);
			float t = saturate((x - 0.75) / 0.22);
			float tail = 1.0 - t * t * (3.0 - 2.0 * t);  // gone by the end of the wave, where the next begins
			float dTail = -6.0 * t * (1.0 - t) / 0.22;
			float b = 0.5 - 0.5 * cos(Math::TAU * s);
			float dB = Math::PI * c * sin(Math::TAU * s);
			o = amplitude * float2(e * tail * b, e * (dTail * b + tail * dB - c / 2.2 * tail * b));
		}
		return o;
	}

	/**
	 * The depth (m) a wave answers to when its crest stands at absolute `absXY` (ShoreField::crestDepth, WaveModel.cpp):
	 * the depth averaged along the crest, and never more than the least the wave crossed on its way in from the sea.
	 * A wave feels the bottom over a stretch of its crest, and breaking is not undone: a wave that broke over a bar
	 * crosses the deeper water behind it as a bore. Read from the bed under the crest instead, a wave's stage followed
	 * every crease and dip of the terrain, backwards as well as forwards, and its whole shape trembled as it came in.
	 * Mirrors ShoreField::CrestDepthAt.
	 * @param fallback the depth to use outside the shore field (m)
	 */
	float ShoreCrestDepth(float2 absXY, float fallback)
	{
		float depth = fallback;
		float2 uv;
		[branch] if (ShoreFieldUV(absXY, uv))
			depth = ShoreRoomTexture.SampleLevel(LinearClampSampler, uv, 0).y;
		return depth;
	}

	/// The wave passing absolute `absXY` at `clock`.
	ShoreWave ShoreWaveAt(float2 absXY, float2 coast, WaveContext ctx, float clock)
	{
		ShoreRay ray = ctx.shore;
		ShoreWave o;
		float cycles = ShorePhase(ray, coast, clock) / Math::TAU;
		o.index = floor(cycles);
		o.phase = cycles - o.index;
		float omega = max(Shore4.x, 1e-3);
		o.wavelength = Math::TAU / (omega * max(ray.slowness, 1e-6));

		// Everything about the wave is decided where its crest stands as it breaks, a third of a wave away at most, so all of
		// it agrees; and by a depth that only ever falls as the wave comes in, so its stage only ever advances.
		float crestDepth = max(ShoreCrestDepth(absXY + ray.direction * ((o.phase - ShoreCrestReference) * o.wavelength), ctx.depth * MetresPerUnit), 0.05);
		float deep = ShoreDeepHeight(ctx) * ShoreSet(coast, o.index);
		float shoaled = deep * ShoalingCoefficient(omega, crestDepth);
		// An onshore wind makes the wave break earlier (a lower breaker index), an offshore wind later (ShoreWindBreaking).
		float index = ShoreBreakerIndex(ctx.shore.slope, min(shoaled, ShoreBreakerLimit * crestDepth), Math::TAU / omega) * (1.0 - ShoreWindBreaking * ctx.onshoreWind);
		float limit = index * crestDepth;
		o.height = min(min(shoaled, limit), ShoreSteepestWave * o.wavelength * MetresPerUnit);
		// Rock faces and cliffs: the waves surge against them and go back, they stay short of breaking.
		o.stage = min(ShoreBreakerStage(shoaled / limit, ctx.shore.slope, index, ShoreBreakerPlunging(ShoreBreakerIribarren(ctx))), lerp(0.42, 1.0, ctx.beach));
		o.height = ShoreBoreHeight(o.height, o.stage, shoaled / limit, ctx.shore.slope, index, crestDepth);

		// The whelps trail the crest: a weak bore or a bar's lee. The depth under it, from the wave's own phase, is the same
		// for every point of the wave.
		o.whelp = 0.0;
		o.whelpLength = o.wavelength;
		[branch] if (Shore3.y > 0.0 && o.stage > 0.25)
		{
			float anchorBed = TerrainHeight(absXY + ray.direction * ((o.phase - ShoreCrestPhase(o.stage)) * o.wavelength));
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
	// The breaker's shape and its curl
	//
	// A wave is one hump through all of its life (ShoreShapeAt, ShoreHump; baked into the profile table by Evaluate in
	// ShoreProfile.cpp): it peaks as it shoals, its crest runs ahead of it as it breaks and it turns into a bore, a steep
	// front with a long back. The table holds no overturning lip: a mesh of a vertex every metre cannot carry one.
	//
	// Where the mesh is fine enough the wave curls over for real. The water above a pivot on the front face is turned
	// forward about it, the most at the crest, which becomes the lip's tip, and is thrown ahead, so the tip plunges into the
	// trough in front of the face (the shape of Longuet-Higgins' overturning jet, not its dynamics; the timing is
	// Galvin's plunge distance, ShoreBreakerStage). The surface stays one sheet, so the underside of the lip faces down and
	// the face under it is seen through the tube. The lip is turned out of the wave's own shape at that moment, so it sits
	// on the hump as that runs forward under it; once the jet has landed the tube's roof falls onto the water below it and
	// slides back along that surface to its own place in the bore. Nothing in it moves back out to sea.
	//
	// The curl is kept apart from the surface it rides on (ShoreTrain::lip): near the shore that surface is the surf
	// simulation's (ComposeSurf), and the lip is drawn over whichever water is under it.
	// Geometry only: the CPU mirror (swimming, buoyancy) rides the table's uncurled wave.
	// ------------------------------------------------------------------------
	static const float ShoreCurlAngle = 2.3;   // turn of the crest at full curl (rad)
	static const float ShoreCurlThrow = 1.0;   // how far the lip is thrown ahead of the face at full curl (wave heights)
	static const float ShoreCurlPivot = 0.45;  // the pivot's height on the front face (share of the crest's)
	/// How far behind the crest the turn runs out (wave heights): the top of the lip. Over a share of the wavelength
	/// instead, the back of the wave was turned about the pivot with a lever of several wave heights, swung up to twice
	/// the wave's height over the crest and, as it fell back into the bore, ran a wave height or more back out to sea.
	static const float ShoreCurlLipLength = 0.6;
	/// Stages of the curl (ShoreBreakerStage): the lip turns over until it lands in the trough (Galvin's plunge distance),
	/// the tube's roof falls onto the water under it, and that water settles into the bore as it forms.
	static const float2 ShoreCurlOverturn = float2(0.44, 0.64);
	static const float2 ShoreCurlImpact = float2(0.62, 0.70);
	static const float2 ShoreCurlSettle = float2(0.68, 0.80);
	/// The stages the curl is drawn over: faded in as the wave steepens, and out once the lip has settled into the bore.
	static const float4 ShoreCurlWindow = float4(0.40, 0.46, 0.80, 0.84);
	/// The stages over which the wave turns into the bore (Evaluate in ShoreProfile.cpp).
	static const float2 ShoreBoreRise = float2(0.58, 0.80);

	/// Smoothstep as ShoreProfile.cpp's: (x - a) / (b - a), clamped and eased.
	float ShoreSmooth(float a, float b, float x)
	{
		float t = saturate((x - a) / (b - a));
		return t * t * (3.0 - 2.0 * t);
	}

	/// The hump of a wave at one stage of its life (Evaluate in ShoreProfile.cpp).
	struct ShoreShape
	{
		float crest;   // the crest's phase
		float front;   // the front face's width (phase)
		float back;    // the back's width while the wave is whole (phase)
		float power;   // the narrowing of the crest
		float lean;    // how far the crest's water is carried forward (share of `forward`)
		float height;  // the crest's height (share of the wave's): the bore is lower than the wave that broke
		float bore;    // 0..1 how far the hump has turned into the bore
	};

	/**
	 * The shape of a wave at stage `v`. Mirrors Evaluate in ShoreProfile.cpp.
	 * @param forwardScale the table's scale of this stage's forward displacement (ShoreProfileAt slice 1, z): 1 for the
	 *                     shape alone
	 */
	ShoreShape ShoreShapeAt(float v, float forwardScale)
	{
		ShoreShape f;
		float s = ShoreSmooth(0.0, 0.5, v);
		f.bore = ShoreSmooth(ShoreBoreRise.x, ShoreBoreRise.y, v);
		f.crest = ShoreCrestPhase(v);
		f.front = lerp(0.5 * (1.0 - s) + 0.075 * s, 0.105, f.bore);
		f.back = 0.5 * (1.0 - s) + 0.30 * s;
		f.power = 1.0 + 1.2 * s;
		f.lean = lerp(0.10 * ShoreSmooth(0.25, 0.55, v), 0.05, f.bore) * forwardScale;
		f.height = 1.0 - (1.0 - ShoreBoreCrest) * ShoreSmooth(0.58, 0.72, v);
		return f;
	}

	/// The hump's height at phase `x` as a share of its crest's: either face falls away from the crest, a raised cosine
	/// narrowed towards it while the wave is whole, a ramp ahead and a long back once it is a bore.
	float ShoreHump(float x, ShoreShape f)
	{
		float xi = x - f.crest;
		float hump;
		if (xi < 0.0) {
			float a = saturate(-xi / f.front);
			float wave = pow(abs(0.5 * (1.0 + cos(Math::PI * a))), f.power);
			hump = lerp(wave, 1.0 - a * a * (3.0 - 2.0 * a), f.bore);
		} else {
			float wave = pow(abs(0.5 * (1.0 + cos(Math::PI * saturate(xi / f.back)))), f.power);
			float bore = exp(-xi / 0.34) * (1.0 - ShoreSmooth(0.82, 1.0, x));
			hump = lerp(wave, bore, f.bore);
		}
		return hump;
	}

	/**
	 * The uncurled wave at phase `x`: where its water stands along the travel (its rest position is -x lambda) and how
	 * high above the trough (units). The same surface as the profile table's.
	 */
	float2 ShoreCurlBackground(float x, ShoreShape f, float lambda, float height, float forward)
	{
		float hump = ShoreHump(x, f);
		return float2(-x * lambda + f.lean * hump * forward, f.height * hump * height);
	}

	/// What a curling wave's shape needs that is the same all along it: worked out once (ShoreCurlSetup) and passed to
	/// every ShoreCurlShape of the wave.
	struct ShoreCurlFrame
	{
		ShoreShape shape;  // the hump the lip is turned out of
		float pivot;       // the pivot's phase on the front face
		float2 pivotAt;    // the pivot (along the travel, up; units)
		float turn;        // the crest's turn now (rad)
		float thrown;      // how far the crest is thrown ahead now (units)
		float impact;      // 0..1 the jet has landed: the roof falls onto the water under it
		float settle;      // 0..1 the fallen roof has slid back to its own place in the bore
	};

	/**
	 * The frame of a curling wave of stage `v`, wavelength `lambda`, height `height` (units), the table's forward reach
	 * `forward` (units) and its scale for this stage `forwardScale`, and curl strength `amount` (ShoreCurlWeight).
	 */
	ShoreCurlFrame ShoreCurlSetup(float v, float lambda, float height, float forward, float forwardScale, float amount)
	{
		ShoreCurlFrame f;
		f.shape = ShoreShapeAt(v, forwardScale);
		// The pivot on the front face, where the whole wave stands ShoreCurlPivot of its crest high.
		f.pivot = f.shape.crest - acos(2.0 * pow(ShoreCurlPivot, 1.0 / f.shape.power) - 1.0) / Math::PI * f.shape.front;
		f.pivotAt = ShoreCurlBackground(f.pivot, f.shape, lambda, height, forward);
		float life = ShoreSmooth(ShoreCurlOverturn.x, ShoreCurlOverturn.y, v);
		f.turn = ShoreCurlAngle * min(amount, 1.0) * life;
		f.thrown = ShoreCurlThrow * amount * life * height;
		f.impact = ShoreSmooth(ShoreCurlImpact.x, ShoreCurlImpact.y, v);
		f.settle = ShoreSmooth(ShoreCurlSettle.x, ShoreCurlSettle.y, v);
		return f;
	}

	/**
	 * The curled wave at phase `x`: x its displacement along the direction of travel, y its height above the trough
	 * (units). Where the curl does not reach, exactly ShoreCurlBackground's surface.
	 * @param f       ShoreCurlSetup of the wave
	 * @param lambda  the wavelength (units)
	 * @param height  trough to crest (units)
	 * @param forward the table's forward reach (units)
	 */
	float2 ShoreCurlShape(float x, ShoreCurlFrame f, float lambda, float height, float forward)
	{
		float2 shape = ShoreCurlBackground(x, f.shape, lambda, height, forward);

		// How much of the turn this point takes: none below the pivot, rising to all of it at the crest, the lip's tip, and
		// running out behind it over the top of the lip.
		float g = 0.0;
		if (x > f.shape.crest) {
			float behind = (x - f.shape.crest) * lambda / max(ShoreCurlLipLength * height, 1e-3);
			g = exp(-behind * behind);
		} else if (x > f.pivot) {
			float u = (x - f.pivot) / max(f.shape.crest - f.pivot, 1e-4);
			g = u * u;
		}
		[branch] if (g > 1e-3)
		{
			float t = f.turn * g;
			float ct = cos(t);
			float st = sin(t);
			float2 d = shape - f.pivotAt;
			float2 curled = f.pivotAt + float2(d.x * ct + d.y * st, d.y * ct - d.x * st);
			curled.x += f.thrown * g;
			[branch] if (f.impact > 0.0)
			{
				// Once the jet has landed the tube's roof falls onto the water under it, the point of that surface below
				// where it stands (found in two steps: the surface leans forward), then slides back along it to its own
				// place in the bore, under the surface rather than over it. Pulled straight back to its place instead, the
				// lip ran back out to sea through the air as the bore formed.
				float under = saturate(-curled.x / max(lambda, 1e-3));
				[unroll] for (uint i = 0; i < 2; i++)
					under = saturate(under + (ShoreCurlBackground(under, f.shape, lambda, height, forward).x - curled.x) / max(lambda, 1e-3));
				curled = lerp(curled, ShoreCurlBackground(lerp(under, x, f.settle), f.shape, lambda, height, forward), f.impact);
			}
			shape = curled;
		}
		return float2(shape.x + x * lambda, shape.y);
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
	 * fades the lip in and out where there is next to none: the stage window's ends, and where the mesh first resolves the face.
	 */
	float ShoreCurlWeight(float3 positionWS, float wavelength, float stage, float iribarren, out float amount)
	{
		float weight = 0.0;
		amount = 0.0;
		[branch] if (Shore3.z > 0.0 && stage > ShoreCurlWindow.x && stage < ShoreCurlWindow.w)
		{
			// Only plungers curl (Battjes 1974): on gentle beaches waves spill, white water tumbling down the face; on the
			// steepest they collapse or surge up the beach without breaking. A wave that plunges at all throws its lip all the
			// way over into the trough: scaled by how much of a plunger it is, the curl's turn stopped part way and the
			// lip stood as a cliff that rode in until the bore caught up with it.
			float plunging = ShoreBreakerPlunging(iribarren);
			// The lip needs a few dozen vertices across its wave to hold its shape. Measured against the wave's deep-water
			// length (Shore3.w), which does not change as it comes in, so a wave keeps its curl all the way to the shore;
			// against its length here, which shrinks in the shallows, the curl unwound as the wave reached the beach.
			// Tessellation.hlsli refines the mesh by the same length.
			float lipLength = max(wavelength, Shore3.w);
			float spacing = ShoreCurlSpacing(positionWS);
			amount = Shore3.z * smoothstep(0.0, 0.3, plunging) * smoothstep(lipLength / 25.0, lipLength / 50.0, spacing);
			if (amount > 0.0)
				weight = smoothstep(lipLength / 12.0, lipLength / 25.0, spacing) * smoothstep(ShoreCurlWindow.x, ShoreCurlWindow.y, stage) *
				         (1.0 - smoothstep(ShoreCurlWindow.z, ShoreCurlWindow.w, stage));
		}
		return weight;
	}

	struct ShoreTrain
	{
		float3 displacement;          // world-space offset (units), without the lip
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float2 slope;                 // height gradient (units/unit)
		float3 compression;           // horizontal displacement gradient: x dDx/dx, y dDy/dy, z dDx/dy
		float3 lip;                   // a breaker's overturning lip: its offset from the surface it rides on (units)
		float3 previousLip;           // the same last frame
		float2 lipSlope;              // the lip's share of the height gradient
		float3 lipCompression;        // and of the horizontal displacement gradient
		float foam;                   // 0..1 white water the waves carry here
		float breakStage;             // 0..1 the wave breaking here (pitching forward or broken), weighted by the train's share
		float slopeVariance;          // slope variance the scale filters out (for roughness)
		float curl;                   // 0..1 how much of a breaker's curl is shown here: the surface may face down
		float crest;                  // units above the still water the crest of the shore wave here stands (its top)
		float bore;                   // 0..1 the wave has broken into a bore: its white water lies on a flattened surface (stage 0.7..0.85)
		float aeration;               // 0..1 air churned into the water here (ShoreAeration): the crushed tube, the splash-up and the roller
		float impact;                 // 0..1 the wave's jet has landed (stage past 0.6): its folding surface is white water from now on
		float clearFace;              // 0..1 the steepening face, the lip and the tube of a wave about to plunge: clear water (ShoreClearFace)
		// Set by ComposeSurf:
		float rawHeight;     // the train's own height here, before the surf simulation took over (units above the still water)
		float surf;          // 0..1 share of the surf simulation in the drawn surface here
		float sheetSurface;  // the simulation's surface here (units above the still water)
		float sheetDepth;    // its depth (units; 0 where dry or not drawn)
		float sheetFoam;     // 0..1 the white water it carries
	};

	static const float ShoreRollerLumps = 0.12;  // the roller's churning lumps, in wave heights at full aeration

	/**
	 * Air in the breaking wave at phase `x` and stage `v`, 0..1, behind the phase `front` (the foot of the wave's front
	 * face). Before the jet lands the water is clear (white only at the lip's tip: the profile's crown foam). When it lands, the tube collapses and the splash-up rises: the air under it and
	 * the air the jet drives in turn the whole crushed region opaque (void fractions to 0.6 in the collapsing cavity and
	 * 0.35 in the splash-up: Na et al. 2016; Lubin et al. 2011). That becomes the roller riding the bore's front, the
	 * aerated water thinning behind it into the residual foam, and weakening as the bore runs into the shallows.
	 */
	float ShoreAeration(float x, float v, float front)
	{
		// Behind the front (the splash-up rises where the lip came down, ahead of the crest): the whole crushed tube at first
		// (to about the old crest), the roller a wave height long once the bore has formed.
		float impact = ShoreSmooth(0.60, 0.68, v);
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
		float crest = ShoreCrestPhase(v);
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
	 * The shape of one wave of the train (ShoreWaves) at undisplaced camera-relative `positionWS`, added to `o`.
	 * @param wave   the wave passing here (ShoreWaveAt)
	 * @param height its height here (units)
	 * @param gate   the train's share here
	 * @param w      the same times the fade of waves the mesh or the pixel cannot carry
	 */
	void ShoreWaveShape(inout ShoreTrain o, float3 positionWS, float2 absXY, float2 coast, WaveContext ctx, ShoreWave wave, float height, float gate, float w, float scale, bool previous, bool shading)
	{
		float4 shape = ShoreProfileAt(wave.phase, wave.stage, 0.0);
		float forward = min(wave.wavelength, ShoreForwardHeights * height);
		float2 dir = ctx.shore.direction;
		float omega = max(Shore4.x, 1e-3);

		// The profile stands on its trough, so a wave of any height meets the next without a step. The sea's level comes
		// from the depth here, not the wave's, and is as continuous: about half the height while it is a sinusoid, less
		// as the wave narrows.
		float localDepth = max(ctx.depth * MetresPerUnit, 0.05);
		float localShoaled = ShoreDeepHeight(ctx) * ShoalingCoefficient(omega, localDepth);
		float localLimit = ShoreBreakerLimit * localDepth;
		float meanLevel = min(localShoaled, localLimit) * lerp(0.5, 0.22, saturate(2.0 * localShoaled / (localShoaled + localLimit))) * UnitsPerMetre;

		// Along the travel and up (units), and their rates of change with the phase.
		float along = shape.x * forward;
		float up = shape.y * height;
		float dAlong = shape.z * forward;
		float dUp = shape.w * height;

		float lastPhase = wave.phase;
		if (previous) {
			float cycles = ShorePhase(ctx.shore, coast, Shore2.y) / Math::TAU;
			lastPhase = cycles - floor(cycles);
		}

		// The curl: the lip's offset from the table's surface, and its rates of change (central differences: they carry the
		// surface around the lip and under it).
		float2 lip = 0.0;
		float2 dLip = 0.0;
		float2 lastLip = 0.0;
		float curlAmount;
		float curl = ShoreCurlWeight(positionWS, wave.wavelength, wave.stage, ShoreBreakerIribarren(ctx), curlAmount);
		[branch] if (curl > 0.0)
		{
			static const float e = 1.0 / 512.0;
			float forwardScale = ShoreProfileAt(wave.phase, wave.stage, 1.0).z;
			ShoreCurlFrame frame = ShoreCurlSetup(wave.stage, wave.wavelength, height, forward, forwardScale, curlAmount);
			float2 rest = float2(wave.wavelength, 0.0);
			float2 c = ShoreCurlShape(wave.phase, frame, wave.wavelength, height, forward) - ShoreCurlBackground(wave.phase, frame.shape, wave.wavelength, height, forward) - wave.phase * rest;
			float2 cA = ShoreCurlShape(wave.phase + e, frame, wave.wavelength, height, forward) - ShoreCurlBackground(wave.phase + e, frame.shape, wave.wavelength, height, forward) - (wave.phase + e) * rest;
			float2 cB = ShoreCurlShape(wave.phase - e, frame, wave.wavelength, height, forward) - ShoreCurlBackground(wave.phase - e, frame.shape, wave.wavelength, height, forward) - (wave.phase - e) * rest;
			lip = curl * c;
			dLip = curl * (cA - cB) / (2.0 * e);
			if (previous)
				lastLip = curl * (ShoreCurlShape(lastPhase, frame, wave.wavelength, height, forward) - ShoreCurlBackground(lastPhase, frame.shape, wave.wavelength, height, forward) - lastPhase * rest);
			o.curl = curl * w;
		}
		// The undular bore's whelps ride on the profile; ones shorter than the vertex spacing or pixel fade out.
		float whelpAmplitude = wave.whelp * UnitsPerMetre * smoothstep(Math::PI, 0.5 * Math::PI, Math::TAU / wave.whelpLength * scale);
		[branch] if (whelpAmplitude > 0.0)
		{
			float2 whelps = ShoreWhelps(wave.phase, wave.stage, whelpAmplitude, wave.wavelength, wave.whelpLength);
			up += whelps.x;
			dUp += whelps.y;
		}
		o.bore = smoothstep(0.70, 0.85, wave.stage) * gate;
		o.crest = (height - meanLevel) * w;

		// The churned water starts at the foot of the front face, where the lip came down and the splash-up rises.
		ShoreShape hump = ShoreShapeAt(wave.stage, 1.0);
		float aeration = ShoreAeration(wave.phase, wave.stage, hump.crest - 0.5 * hump.front) * BreakerStrength(ctx);
		o.aeration = aeration * gate;
		o.impact = ShoreSmooth(0.60, 0.68, wave.stage) * gate;
		o.clearFace = ShoreClearFace(wave.phase, wave.stage) * gate;

		o.displacement = float3(dir * along, up - meanLevel) * w;
		o.lip = float3(dir * lip.x, lip.y) * w;
		// The phase rises towards the sea, a wavelength per cycle.
		float2 gradPhase = -dir / wave.wavelength;
		o.slope = (dUp * w) * gradPhase;
		o.lipSlope = (dLip.y * w) * gradPhase;

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
		float3 stretch = float3(dir.x * gradPhase.x, dir.y * gradPhase.y, dir.x * gradPhase.y);
		o.compression = stretch * (dAlong * w);
		o.lipCompression = stretch * (dLip.x * w);
		o.breakStage = smoothstep(0.45, 0.6, wave.stage) * gate;
		if (shading)
			o.foam = ShoreProfileAt(wave.phase, wave.stage, 1.0).x * gate * BreakerStrength(ctx);
		if (previous) {
			float4 last = ShoreProfileAt(lastPhase, wave.stage, 0.0);
			float2 lastShape = float2(last.x * forward, last.y * height);
			lastShape.y += ShoreWhelps(lastPhase, wave.stage, whelpAmplitude, wave.wavelength, wave.whelpLength).x;
			o.previousDisplacement += float3(dir * lastShape.x, lastShape.y - meanLevel) * w;
			o.previousLip = float3(dir * lastLip.x, lastLip.y) * w;
		}
	}

	/**
	 * The shore wave train at undisplaced camera-relative `positionWS`, weighted by its share ctx.train: the train alone,
	 * as it comes in from the sea (ComposeSurf hands it to the surf simulation near the shore). One implementation for
	 * geometry (VS/DS, the occluder grid, the compute shaders), shading and tessellation; mirrored on the CPU
	 * (WaveModel.cpp) for swimming, buoyancy and floating objects.
	 * @param scale    vertex spacing or pixel footprint (units): waves shorter than it fade out (their slope variance goes
	 *                 to the roughness); 0: exact
	 * @param previous also evaluate last frame's displacement (motion vectors)
	 * @param shading  also read the foam (pixel shader)
	 */
	ShoreTrain ShoreWaves(float3 positionWS, WaveContext ctx, float scale, bool previous, bool shading)
	{
		ShoreTrain o = (ShoreTrain)0;
		// The train ends at the still waterline: up the beach the water is the surf simulation's. (Past the simulation's
		// grid nothing runs up the beach.)
		float gate = ctx.train * (1.0 - smoothstep(0.0, ShoreTrainLanding, ctx.elevation));
		[branch] if (gate > 0.0)
		{
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float2 coast = CoastPoint(absXY, ctx.shore);
			ShoreWave wave = ShoreWaveAt(absXY, coast, ctx, Shore2.x);
			float k = Math::TAU / wave.wavelength;
			float fade = smoothstep(Math::PI, 0.5 * Math::PI, k * scale);
			float w = gate * fade;
			float height = wave.height * UnitsPerMetre;
			// A broken wave answers to the water it is in: in the inner surf its height saturates at 0.4 to 0.6 of the
			// depth here (Thornton and Guza 1983). Its height is decided at its crest, which stands up to a third of a
			// wavelength out to sea: held to that, the water at the waterline stood a metre high under every bore. Down to
			// the waterline it keeps a low front (ShoreBoreFloor).
			float saturated = max(ShoreSaturatedShare(ctx.shore.slope) * max(ctx.depth, 0.0), ShoreBoreFloor) / ShoreBoreCrest;
			height = lerp(height, min(height, saturated), smoothstep(ShoreBoreRise.x, ShoreBoreRise.y, wave.stage));
			float amplitudeUnits = 0.5 * height;
			o.slopeVariance = (1.0 - fade * fade) * gate * gate * 0.5 * amplitudeUnits * amplitudeUnits * k * k;
			[branch] if (w > 0.0)
				ShoreWaveShape(o, positionWS, absXY, coast, ctx, wave, height, gate, w, scale, previous, shading);
		}
		return o;
	}

	// ------------------------------------------------------------------------
	// The surf simulation: the broken wave's own water through the inner surf and up the beach
	//
	// Each wave comes in on the train, steepens, curls and breaks. What a broken wave does next is no function of that one
	// wave: its bore runs into the backwash of the one before, collapses at the waterline, and its water runs up the beach
	// as a thin sheet, slows on the slope and drains back, overtaken by or meeting the next (Hibberd and Peregrine 1979;
	// Shen and Meyer 1963). So the inner surf and the swash are simulated: the shallow water equations with wetting and
	// drying on a camera-centred grid (ShoreFoamCS lays out what the waves bring, SwashStepCS steps it, SwashOutCS packs it
	// for the draws).
	//
	// The hand-over is fixed in space, by the depth against the waves' height (SurfSharesAt): where a wave of the sea's
	// mean height has become a bore, the simulation is given the train's waves; a little further in it runs free, and the
	// drawn surface passes from the train to the simulation across that band (ComposeSurf). Fixed weights between two
	// surfaces that agree there: nothing switches as a wave passes, and inshore of the band the simulation is the only
	// water, so each uprush is the wave that came in. Past the grid the train runs to the waterline and ends there.
	//
	// The sheet is thin and ends abruptly at its front, so its edge is found per pixel: the mesh lies on the sand wherever
	// the water can reach (DisplaceSurface), the stencil pass keeps what the water covers (AboveWaterline), and the colour
	// pass finds the exact edge against the ground (Water.hlsl).
	// ------------------------------------------------------------------------

	/// Water shallower than this (units, 3 mm) is the sand's wet sheen, not water.
	static const float SwashMinDepth = 0.003 * UnitsPerMetre;

	/// The hand-over in break ratios (ShoreBreakRatio: 1 where a wave of the sea's mean height breaks, growing as the
	/// depth^-5/4 inshore of that): the simulation is held to the train's incoming waves up to x and free from y, and it
	/// is the whole drawn surface from z. On beaches up to about 1:5 a plunger's lip has landed by a ratio of 2.
	static const float3 SurfHandOver = float3(2.0, 3.5, 5.0);

	/// How the train and the surf simulation share the water at a point.
	struct SurfShares
	{
		float incoming;  // 0..1 the simulation takes the train's incoming waves here
		float outgoing;  // 0..1 and gives up what it sends back out to sea: further out, where the train alone is the water
		float drawn;     // 0..1 share of the simulation in the drawn surface
	};

	/// The shares at the context's point: a function of the place only (the depth and the sea arriving there).
	SurfShares SurfSharesAt(WaveContext ctx)
	{
		SurfShares o;
		// Up the beach the simulation is all the water there is.
		o.incoming = 0.0;
		o.outgoing = 0.0;
		o.drawn = 1.0;
		[branch] if (ctx.elevation < 0.0)
		{
			float ratio = ShoreBreakRatio(ctx.shoreHeight * ctx.damping * MetresPerUnit, ctx.depth * MetresPerUnit);
			o.incoming = 1.0 - smoothstep(SurfHandOver.x, SurfHandOver.y, ratio);
			o.outgoing = 1.0 - smoothstep(1.0, SurfHandOver.x, ratio);
			o.drawn = smoothstep(SurfHandOver.x, SurfHandOver.z, ratio);
		}
		return o;
	}

	/// The surf simulation's grid coordinates of absolute `absXY` (Foam2), and whether they are inside it.
	bool ShoreGridUV(float2 absXY, out float2 uv)
	{
		uv = (absXY - Foam2.xy) * Foam2.z;
		return Foam2.w > 0.5 && all(uv > 0.0) && all(uv < 1.0);
	}

	/// The surf simulation at a point (SampleSurf).
	struct SurfSheet
	{
		float surface;  // its surface (units above the still water): the bed where it is dry
		float depth;    // its depth (units)
		float foam;     // 0..1 the white water it carries
		float2 slope;   // the surface's gradient (units/unit)
		float share;    // 1 inside the grid, fading to 0 over its border; 0 outside
	};

	/**
	 * The surf simulation at absolute `absXY` (SwashOutCS).
	 * @param withSlope also work out the surface's gradient (four more reads)
	 * @param spacing   vertex spacing (units; 0: exact): a mesh coarser than the grid reads the surface averaged over its
	 *                  spacing. A bore's front is two cells wide: read at a point by vertices further apart than that, it
	 *                  stepped from vertex to vertex as it moved. (The pixels read it exactly, and shade it sharp.)
	 */
	SurfSheet SampleSurf(float2 absXY, bool withSlope, float spacing)
	{
		SurfSheet o = (SurfSheet)0;
		float2 uv;
		[branch] if (ShoreGridUV(absXY, uv))
		{
			uint w, h;
			ShoreFoamTexture.GetDimensions(w, h);
			float2 texel = 1.0 / float2(w, h);
			float3 sheet = ShoreFoamTexture.SampleLevel(LinearClampSampler, uv, 0).yzw;
			float2 reach = 0.35 * spacing * Foam2.z;
			[branch] if (reach.x > 0.5 * texel.x)
			{
				sheet = 0.25 * (ShoreFoamTexture.SampleLevel(LinearClampSampler, uv + reach, 0).yzw +
								   ShoreFoamTexture.SampleLevel(LinearClampSampler, uv - reach, 0).yzw +
								   ShoreFoamTexture.SampleLevel(LinearClampSampler, uv + float2(reach.x, -reach.y), 0).yzw +
								   ShoreFoamTexture.SampleLevel(LinearClampSampler, uv + float2(-reach.x, reach.y), 0).yzw);
			}
			o.surface = sheet.x;
			o.depth = sheet.y;
			o.foam = sheet.z;
			o.share = BorderFade(uv);
			[branch] if (withSlope)
			{
				float east = ShoreFoamTexture.SampleLevel(LinearClampSampler, uv + float2(texel.x, 0.0), 0).y;
				float west = ShoreFoamTexture.SampleLevel(LinearClampSampler, uv - float2(texel.x, 0.0), 0).y;
				float north = ShoreFoamTexture.SampleLevel(LinearClampSampler, uv + float2(0.0, texel.y), 0).y;
				float south = ShoreFoamTexture.SampleLevel(LinearClampSampler, uv - float2(0.0, texel.y), 0).y;
				o.slope = float2(east - west, north - south) * (0.5 * Foam2.z / texel);
			}
		}
		return o;
	}

	/**
	 * Hands the train's surface to the surf simulation where the simulation is the water (SurfSharesAt): the heights pass
	 * to its surface and the train's forward lean fades out (the simulation is a height field), over the fixed band of
	 * the hand-over. A breaker's lip stays (ShoreTrain::lip): it is drawn over whichever water is under it.
	 * @param pixel   shading: the surface's gradient is read too, and where the simulation is dry the surface is far below
	 *                the ground (the pixel is not water). For geometry the dry surface is the bed, so the mesh lies on it.
	 * @param spacing vertex spacing of the geometry (units; 0 for shading): SampleSurf
	 */
	void ComposeSurf(inout ShoreTrain train, float3 positionWS, WaveContext ctx, bool pixel, float spacing)
	{
		train.rawHeight = train.displacement.z;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		float2 uv;
		[branch] if (ctx.hasTerrain > 0.5 && ShoreGridUV(absXY, uv))
		{
			float drawn = SurfSharesAt(ctx).drawn;
			[branch] if (drawn > 0.0)
			{
				SurfSheet sheet = SampleSurf(absXY + (1.0 - drawn) * train.displacement.xy, pixel, spacing);
				drawn *= sheet.share;
				float keep = 1.0 - drawn;
				// Dry: no water for the pixels; the geometry keeps the train's own height there and is laid on the bed by
				// DisplaceSurface, up to where water can reach (set on the simulation's bed outright, it climbed every cliff
				// in the grid).
				float surface = pixel ? (sheet.depth > SwashMinDepth ? sheet.surface : -1e6) : lerp(train.displacement.z, sheet.surface, smoothstep(0.0, 2.0 * SwashMinDepth, sheet.depth));
				train.displacement = float3(train.displacement.xy * keep, lerp(train.displacement.z, surface, drawn));
				train.previousDisplacement = float3(train.previousDisplacement.xy * keep, lerp(train.previousDisplacement.z, surface, drawn));
				train.slope = lerp(train.slope, sheet.slope, drawn);
				train.compression *= keep;
				// The train's white water gives way with its surface: the simulation carries its own.
				train.foam *= keep;
				train.bore *= keep;
				train.aeration *= keep;
				train.impact *= keep;
				train.clearFace *= keep;
				train.crest = lerp(train.crest, max(sheet.surface, 0.0), drawn);
				train.surf = drawn;
				train.sheetSurface = sheet.surface;
				train.sheetDepth = sheet.depth;
				train.sheetFoam = sheet.foam * drawn;
			}
		}
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
		float crest;                  // units above the still water the shore wave's crest stands here (ShoreTrain::crest)
		float bore;                   // 0..1 the shore wave has flattened into a bore (ShoreTrain::bore)
		float breakerFoam;            // 0..1 the breaking wave's own white water, a raw coverage like the sea's (BreakerFoam)
		float aeration;               // 0..1 churned white water (ShoreTrain::aeration): drawn solid, without the foam's pattern
		float clearFace;              // 0..1 a face about to plunge, clear of foam (ShoreTrain::clearFace)
		float heightRaw;              // units above the still water the waves stand here without the surf simulation (ShoreTrain::rawHeight and the cascades)
		float surf;                   // 0..1 share of the surf simulation in the surface here (ShoreTrain::surf)
		float sheetSurface;           // the surf simulation's surface here (units above the still water; ShoreTrain::sheetSurface)
		float sheetDepth;             // its depth (units)
		float sheetFoam;              // 0..1 the white water it carries
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
		float3 compression = train.compression + train.lipCompression;
		float jacobian = (1.0 + compression.x) * (1.0 + compression.y) - compression.z * compression.z;
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

	/// Adds the shore waves: the train or the surf simulation under it (ComposeSurf), and the breaker's lip over that.
	void AddShoreTrain(inout WaveSums sums, inout WaveResult o, ShoreTrain train)
	{
		o.heightRaw = sums.displacement.z + train.rawHeight;
		sums.displacement += train.displacement + train.lip;
		sums.previous += train.previousDisplacement + train.previousLip;
		sums.dzdx += train.slope.x + train.lipSlope.x;
		sums.dzdy += train.slope.y + train.lipSlope.y;
		sums.dxdx += train.compression.x + train.lipCompression.x;
		sums.dydy += train.compression.y + train.lipCompression.y;
		sums.dxdy += train.compression.z + train.lipCompression.z;
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
		o.surf = train.surf;
		o.sheetSurface = train.sheetSurface;
		o.sheetDepth = train.sheetDepth;
		o.sheetFoam = train.sheetFoam;
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
	 * are averaged out instead of aliasing; the shore wave train is filtered to the spacing the same way. Where the surf
	 * simulation is the water its surface replaces the train's (ComposeSurf), the bed where it is dry.
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
		ShoreTrain train = ShoreWaves(positionWS, ctx, spacing, previous, false);
		ComposeSurf(train, positionWS, ctx, false, spacing);
		AddShoreTrain(sums, o, train);
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
	 * @param surfFoam 0..1 the foam the surf left on the water here (ShoreFoamAt): on a face that is not flat it is
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
		ShoreTrain train = ShoreWaves(positionWS, ctx, footprint, false, true);
		ComposeSurf(train, positionWS, ctx, true, 0.0);
		AddShoreTrain(sums, o, train);
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
	// Surf foam
	//
	// Shore waves carry their white water in their profile (ShoreProfile.h), so it travels with the wave; what the breakers
	// leave behind on the water is simulated (ShoreFoamCS): it rides the water and decays. The surf simulation carries its
	// own foam (SurfSheet::foam).
	// ------------------------------------------------------------------------

	/**
	 * The foam the surf has left on the water at absolute `absXY` (ShoreFoamCS), 0..1. Fades out over the grid's outer
	 * tenth; zero outside it. A cubic B-spline in four bilinear reads (Sigg & Hadwiger 2005): the grid's 17 cm texels drew
	 * the foam's edges as diamonds under a bilinear read.
	 */
	float ShoreFoamAt(float2 absXY)
	{
		float foam = 0.0;
		float2 uv;
		[branch] if (ShoreGridUV(absXY, uv))
		{
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
			float s00 = ShoreFoamTexture.SampleLevel(LinearClampSampler, float2(h0.x, h0.y), 0).x;
			float s10 = ShoreFoamTexture.SampleLevel(LinearClampSampler, float2(h1.x, h0.y), 0).x;
			float s01 = ShoreFoamTexture.SampleLevel(LinearClampSampler, float2(h0.x, h1.y), 0).x;
			float s11 = ShoreFoamTexture.SampleLevel(LinearClampSampler, float2(h1.x, h1.y), 0).x;
			foam = saturate((g0.y * (g0.x * s00 + g1.x * s10) + g1.y * (g0.x * s01 + g1.x * s11)) * BorderFade(uv));
		}
		return foam;
	}

	// ------------------------------------------------------------------------
	// Waterline
	// ------------------------------------------------------------------------

	/// Largest vertical reach of the surface (units): waves within their statistical bound (doubled for the fetch
	/// energy gain), the shore wave train (shoaled at most to about the deep-water height, its crest up to 2A above
	/// the mean), the surf simulation's run-up and the ripples. Used to clamp, which also flushes NaN.
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

	/**
	 * True where the ground at this pixel lies above the water. Decided per pixel: the surf simulation is read here (its
	 * sheet ends abruptly at its front, so interpolated from the vertices it ended along the triangle edges), the waves
	 * and the simulation's share come from the vertices (they vary smoothly). Both passes test the same thing and cover
	 * the same pixels; the colour pass then finds the exact edge against the ground in the depth buffer (Water.hlsl),
	 * which the slack keeps inside: the rendered terrain's triangles differ from the bilinear heights by a few centimetres.
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
		float water = shoreState.z;
		[branch] if (shoreState.x > 0.0)
		{
			SurfSheet sheet = SampleSurf(absXY, false, 0.0);
			water = lerp(water, sheet.depth > SwashMinDepth ? sheet.surface : -1e6, shoreState.x * sheet.share);
		}
		return bed > water + 0.05 * UnitsPerMetre;
	}

	// ------------------------------------------------------------------------
	// Full surface displacement used by geometry (VS / DS)
	// ------------------------------------------------------------------------
	struct SurfaceDisplacement
	{
		float3 current;
		float3 previous;
		float4 shore;  // x share of the surf simulation in the surface (SurfSharesAt, before the grid's border fade), y unused, z the waves without it (units above the still water), w still water level (camera-relative z)
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
		float3 ripple = RippleDisplacement(positionWS, false) * rippleFade;
		o.current = now.displacement + ripple;
		o.previous = motion ? now.previousDisplacement + RippleDisplacement(positionWS, true) * rippleFade : 0.0;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		// The waves alone: AboveWaterline blends them with the surf simulation per pixel.
		float waves = now.heightRaw + ripple.z;

		// Near the waterline the mesh never dips under the bed: where the water ends is decided per pixel against the
		// ground behind it (AboveWaterline, Water.hlsl), so the edge follows the true terrain instead of the triangles
		// cutting through it. The mesh lies on the bed within reach of the water at the next vertex (the bed may climb that
		// much in between), and on the surf simulation's grid up to the highest the water can run up (Shore0.w): it is in
		// place before each sheet arrives and stays until after it has drained, so no triangle sinks into the sand under a
		// sheet the pixels still draw. Never above where any water stands: past the grid `reach` grows with the mesh
		// spacing times the slope, and in the middle distance it draped the mesh up rock and ice faces, shards standing
		// out of the water through the coarser distant terrain.
		// The bed is taken where the waves have carried the vertex (they move it along by up to a wavelength, towards the
		// beach and back), and as high as the game's terrain triangles can stand there: held above the bed under its rest
		// position, the triangles of a vertex carried up the slope sank into the sand and left pockets of ground in the water.
		if (ctx.hasTerrain > 0.5) {
			float ceiling = TerrainCeiling(absXY + o.current.xy);
			float bed = ceiling > -1e8 ? ceiling - (positionWS.z + FrameBuffer::CameraPosAdjust.z) : ctx.elevation;
			float water = o.current.z;
			float highest = max(water, max(Shore0.w, 0.0) + 0.5 * UnitsPerMetre);
			float2 uv;
			float held = ShoreGridUV(absXY, uv) ? highest : min(water + max(0.5 * UnitsPerMetre, 2.0 * spacing * length(ctx.terrainGrad)), highest);
			float lifted = min(bed, held) + WaterlineLift(spacing, ctx.terrainGrad);
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
		float2 gridUV;
		float surf = ctx.hasTerrain > 0.5 && ShoreGridUV(absXY, gridUV) ? SurfSharesAt(ctx).drawn : 0.0;
		o.shore = float4(surf, 0.0, clamp(waves, -limit, limit), positionWS.z);
		return o;
	}
}

#endif  // __PBR_WATER_HLSLI__
