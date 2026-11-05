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
		float4 Shore2;                   // x wave clock (rad, wrapped after ShoreClockCycles waves), y the clock last frame, z its rate (rad/s): the eased angular frequency times ShoreTimeScale, w peak period Tp (s)
		float4 Shore3;                   // xyz unused, w deep-water wavelength L0 (units)
		float4 Shore4;                   // x angular frequency the current shore field was traced for (rad/s), y the previous field's, zw unused
		float4 Shore6;                   // xy previous shore field corner (absolute units), zw 1 / its extent (units)
		float4 Grid0;                    // xy loaded cell grid min corner, zw max corner (absolute units); z <= x: none
		float4 Noise0;                   // xy surface noise grid corner (absolute units), z 1 / its extent (units), w baked (SurfaceNoiseCS)
		float4 Tune0;                    // x river current speed at full flow strength (m/s), y flow period per metre of a cascade's texel (s/m), z shortest flow period (s), w longest (s)
		float4 Tune1;                    // foam offset of each cascade, longest waves first
		float4 Tune2;                    // x share of a cascade's amplitude that counts fully in the foam, y exponent of the foam's distance falloff, z distance at which it has faded out (m), w roughness added per unit of foam
		float4 Tune3;                    // x ripple horizontal displacement strength, y its clamp (simulation texels), z shore train reach (waves), w coverage of the surf's lingering foam
		float4 Tune4;                    // x lowest and y highest factor of the shore wave sets, z detail normal strength in calm air, w in a gale
		float4 Tune5;                    // x point light emitter radius (units), y foam feather, z foam bubble size (m), w unused
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
	 * Wave sets, as a factor on one wave's height (mean ~1). A sea's narrow spectrum brings its waves in sets, a few big
	 * ones between smaller ones (Longuet-Higgins 1984), and out of a short-crested sea the sets differ along the coast. The
	 * noise runs slowly with the wave's number, so neighbours are alike and each wave keeps its height all the way in.
	 * Mirrors PBRWaterModel::ShoreSet.
	 * @param coast the waterline the wave runs to (CoastPoint)
	 * @param index the wave's number (ShoreWaveAt)
	 */
	float ShoreSet(float2 coast, float index)
	{
		return lerp(Tune4.x, Tune4.y, ValueNoise(coast / max(3.0 * Shore3.w, 1.0) + index * float2(0.14, 0.09)));
	}

	/// The shore waves' phase (rad): each cycle is one wave, rising towards the sea. Mirrors the CPU's ShoreTrain.
	float ShorePhase(ShoreRay ray, float clock)
	{
		return clock + ray.phaseLag + Math::TAU * ray.along;
	}

	/// One lookup of the wave profile. Mirrors PBRWaterModel::ShoreProfileTable::At.
	float4 ShoreProfileAt(float phase, float stage, float slice)
	{
		static const float2 size = float2(256.0, 64.0);  // ShoreProfileTable::Phases, Stages
		float2 uv = (float2(phase, stage) * (size - 1.0) + 0.5) / size;
		return ShoreProfileTexture.SampleLevel(LinearClampSampler, float3(uv, slice), 0);
	}

	/// One wave of the train, as a point on it sees it.
	struct ShoreWave
	{
		float phase;       // where this point is on the wave: 0 the trough ahead of it, 1 the trough behind
		float index;       // the wave's number
		float stage;       // its life, 0 deep water, 0.5 reaching its limit, 1 the bore at the shore
		float height;      // trough to crest (m)
		float wavelength;  // here (units)
	};

	/// The wave passing absolute `absXY` at `clock`.
	ShoreWave ShoreWaveAt(float2 absXY, float2 coast, WaveContext ctx, float clock)
	{
		ShoreRay ray = ctx.shore;
		ShoreWave o;
		float cycles = ShorePhase(ray, clock) / Math::TAU;
		o.index = floor(cycles);
		o.phase = cycles - o.index;
		float omega = max(Shore4.x, 1e-3);
		o.wavelength = Math::TAU / (omega * max(ray.slowness, 1e-6));

		// Everything about the wave is decided at its crest, half a wave away at most, so all of it agrees.
		float crestBed = TerrainHeight(absXY + ray.direction * ((o.phase - 0.5) * o.wavelength));
		float crestDepth = max((ctx.waterZ - crestBed) * MetresPerUnit, 0.05);
		float deep = ShoreHeightRatio * ctx.shoreHeight * MetresPerUnit * ctx.damping * ShoreSet(coast, o.index);
		float shoaled = deep * ShoalingCoefficient(omega, crestDepth);
		float limit = ShoreBreakerLimit * crestDepth;
		o.height = min(min(shoaled, limit), ShoreSteepestWave * o.wavelength * MetresPerUnit);
		float breaking = shoaled / limit;
		// Rock faces and cliffs: the waves surge against them and go back, they stay short of breaking.
		o.stage = min(breaking / (1.0 + breaking), lerp(0.42, 1.0, ctx.beach));
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

	struct ShoreTrain
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float2 slope;                 // height gradient (units/unit)
		float3 compression;           // horizontal displacement gradient: x dDx/dx, y dDy/dy, z dDx/dy
		float foam;                   // 0..1 white water the waves carry here
		float breakStage;             // 0..1 the wave breaking here (pitching forward or broken), weighted by the train's share
		float slopeVariance;          // slope variance the scale filters out (for roughness)
	};

	/**
	 * The shore wave train at undisplaced camera-relative `positionWS`, weighted by its share ctx.train. One
	 * implementation for geometry (VS/DS, the occluder grid, ShoreSimCS), shading and tessellation; mirrored on the
	 * CPU (WaveModel.cpp) for swimming, buoyancy and floating objects.
	 * @param scale    vertex spacing or pixel footprint (units): waves shorter than it fade out (their slope variance goes
	 *                 to the roughness); 0: exact
	 * @param previous also evaluate last frame's displacement (motion vectors)
	 * @param shading  also read the foam (pixel shader)
	 */
	ShoreTrain ShoreWaves(float3 positionWS, WaveContext ctx, float scale, bool previous, bool shading)
	{
		ShoreTrain o = (ShoreTrain)0;
		[branch] if (ctx.train > 0.0)
		{
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float2 coast = CoastPoint(absXY, ctx.shore);
			ShoreWave wave = ShoreWaveAt(absXY, coast, ctx, Shore2.x);
			float4 shape = ShoreProfileAt(wave.phase, wave.stage, 0.0);

			float k = Math::TAU / wave.wavelength;
			float fade = smoothstep(Math::PI, 0.5 * Math::PI, k * scale);
			float w = ctx.train * fade;
			float height = wave.height * UnitsPerMetre;
			float forward = min(wave.wavelength, ShoreForwardHeights * height);
			float2 dir = ctx.shore.direction;

			// The profile stands on its trough, so a wave of any height meets the next without a step. The sea's level comes
			// from the depth here, not the wave's, and is as continuous: about half the height while it is a sinusoid, less
			// as the wave narrows.
			float localDepth = max(ctx.depth * MetresPerUnit, 0.05);
			float localDeep = ShoreHeightRatio * ctx.shoreHeight * MetresPerUnit * ctx.damping;
			float localShoaled = localDeep * ShoalingCoefficient(max(Shore4.x, 1e-3), localDepth);
			float localLimit = ShoreBreakerLimit * localDepth;
			float meanLevel = min(localShoaled, localLimit) * lerp(0.5, 0.22, saturate(2.0 * localShoaled / (localShoaled + localLimit))) * UnitsPerMetre;

			o.displacement = float3(dir * (shape.x * forward), shape.y * height - meanLevel) * w;
			// The phase rises towards the sea, a wavelength per cycle.
			float2 gradPhase = -dir / wave.wavelength;
			o.slope = (shape.w * height * w) * gradPhase;
			o.compression = float3(dir.x * gradPhase.x, dir.y * gradPhase.y, dir.x * gradPhase.y) * (shape.z * forward * w);
			float breaking = smoothstep(0.45, 0.6, wave.stage);
			o.breakStage = breaking * ctx.train;
			float amplitudeUnits = 0.5 * height;
			o.slopeVariance = (1.0 - fade * fade) * ctx.train * ctx.train * 0.5 * amplitudeUnits * amplitudeUnits * k * k;
			if (shading) {
				// The bores' white water (the profile), over the foam the surf leaves behind: in a surf zone the water between the
				// breakers stays laced with the residue of the ones before, which outlasts the next bore. It follows the surf
				// (where the train is depth-limited), not a wave, so it stays put as the crests run through it.
				float residual = Tune3.w * smoothstep(0.8, 1.3, ShoreBreakRatio(ctx.shoreHeight * ctx.damping * MetresPerUnit, ctx.depth * MetresPerUnit));
				o.foam = max(ShoreProfileAt(wave.phase, wave.stage, 1.0).x, residual) * ctx.train * BreakerStrength(ctx);
			}
			if (previous) {
				float cycles = ShorePhase(ctx.shore, Shore2.y) / Math::TAU;
				float4 last = ShoreProfileAt(cycles - floor(cycles), wave.stage, 0.0);
				o.previousDisplacement = float3(dir * (last.x * forward), last.y * height - meanLevel) * w;
			}
		}
		return o;
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
		float slopeVariance;
	};

	WaveSums EmptySums()
	{
		WaveSums sums;
		sums.displacement = 0.0;
		sums.previous = 0.0;
		sums.dzdx = sums.dzdy = sums.dxdx = sums.dydy = sums.dxdy = 0.0;
		sums.foam = 0.0;
		sums.slopeVariance = 0.0;
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
		o.breakStage = train.breakStage;
		o.shoreFoam = train.foam;
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
		AddShoreTrain(sums, o, ShoreWaves(positionWS, ctx, spacing, previous, false));
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
					float variance = max(m.x - g.x * g.x, 0.0) + max(m.y - g.y * g.y, 0.0);
					float texelsPerUV = rcp(Cascade0[c].x * Cascade0[c].w);
					float minified = MinifiedShare(gx * texelsPerUV, gy * texelsPerUV);
					sums.slopeVariance += variance * vertical * vertical * minified;
				}
			}
		}
		AddShoreTrain(sums, o, ShoreWaves(positionWS, ctx, footprint, false, true));
		o = FinishWaves(sums, o);
		o.fftFoam = FftFoam(sums.foam, length(positionWS));
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
	 * the run-up R and drains back over the same time; R is Stockdon et al.'s (2006) 2 % run-up of the deep-water
	 * significant height. Once a crest has drained its tip keeps falling below the still water instead of resting at
	 * it: the vertices interpolate the tip linearly, and a tip held at 0 next to a running one put the swash front on the
	 * triangle edges. Each crest carries its own height (ShoreSet), so every uprush reaches its own height.
	 * @param previous at last frame's clock (motion vectors)
	 */
	float2 SwashTip(float3 positionWS, WaveContext ctx, bool previous)
	{
		float2 result = 0.0;
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
			float deepLength = Shore3.w * MetresPerUnit;
			float runupPerHeight = 1.1 * (0.35 * b + 0.5 * sqrt(0.563 * b * b + 0.004));  // Stockdon R2 / sqrt(H0 L0)
			float elevation = ctx.elevation * MetresPerUnit;

			// The train's phase here (up the beach the waterline's, carried up the ray), and the time since the last crest
			// reached it: each uprush starts as its crest arrives, half a wave into its cycle.
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			float2 coast = CoastPoint(absXY, ctx.shore);
			float cycles = ShorePhase(ctx.shore, previous ? Shore2.y : Shore2.x) / Math::TAU - 0.5;
			float wave = floor(cycles);
			float sinceLast = (cycles - wave) * period;
			// Stockdon's run-up is fitted to the deep-water significant height, not the train's rms height.
			float deepHeight = ctx.shoreHeight * MetresPerUnit * ctx.damping;

			// The uprushes of the last periods; the oldest fades out before it leaves the window, so the tip never pops. The
			// newest crest's own path continues below the still water after it drained. Kept: the one whose water stands
			// highest here.
			const uint candidates = 3;
			float window = candidates * period;
			float tip = -1e9;
			float lens = 0.0;
			float surface = -1e9;
			[unroll] for (uint j = 0; j < candidates; j++)
			{
				float tau = sinceLast + j * period;
				float H = deepHeight * ShoreSet(coast, wave - j);
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
	// Shore waves carry their foam in their profile (ShoreProfile.h), so it travels with the wave.
	// ------------------------------------------------------------------------

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
		float2 tip = 0.0;
		float2 previousTip = 0.0;
		if (ctx.beach > 0.0) {
			tip = SwashTip(positionWS, ctx, false);
			if (motion)
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
		// the swash reached lately: the triangles that sank under the sand there cut straight-edged holes into its sheet.
		if (ctx.hasTerrain > 0.5) {
			float reach = max(0.5 * UnitsPerMetre, 2.0 * spacing * length(ctx.terrainGrad));
			float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
			if (ctx.elevation < max(max(o.current.z, tip.x), SwashWetLevel(absXY)) + reach) {
				float lifted = ctx.elevation + WaterlineLift(spacing, ctx.terrainGrad);
				o.current.z = max(o.current.z, lifted);
				o.previous.z = max(o.previous.z, lifted);
			}
		}

		// Hard bound. Clamping also flushes NaN (D3D min/max return the non-NaN operand), so a bad input can never throw
		// the mesh off-screen, but it lands on -limit: a hole in the water, not a harmless flat spot.
		float limit = DisplacementLimit();
		o.current = clamp(o.current, -limit, limit);
		o.previous = clamp(o.previous, -limit, limit);
		o.shore = float4(tip.x, max(tip.y, 0.0), clamp(waves, -limit, limit), positionWS.z);
		return o;
	}
}

#endif  // __PBR_WATER_HLSLI__
