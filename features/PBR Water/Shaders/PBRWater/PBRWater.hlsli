#ifndef __PBR_WATER_HLSLI__
#define __PBR_WATER_HLSLI__

#include "Common/FrameBuffer.hlsli"
#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "PBRWater/Foam.hlsli"  // Terrain Variation's lattice, when it is installed
#include "PBRWater/Noise.hlsli"

// ============================================================================ PBR Water - shared constants, resources
// and the wave model.
//
// The open water is an FFT ocean (Tessendorf 2001): four world-anchored cascades of 1000, 162, 26.3 and 4.27 m, each
// one band of a JONSWAP wind sea plus swell, synthesised every frame on the GPU (OceanSpectrumCS, OceanFFTCS,
// OceanAssembleCS) into mip-mapped texture arrays. The geometry reads the displacement at the mip of its vertex
// spacing, the pixels the normals and LEAN-filtered roughness at the mip of their footprint. The CPU re-synthesises the
// long cascades from the same coefficients (WaveModel.cpp), so gameplay follows the rendered surface without a
// readback.
//
// Fetch and depth weight each cascade as a whole: the fetch-limited peak removes long cascades from sheltered water,
// young seas get the JONSWAP energy gain, and tanh(kh) calms waves in the shallows.
//
// Near the shore the long waves hand over to Fluid Flux-style coastline waves (ShoreWaves.hlsli) that shoal, curl and
// break; in the inner surf a shallow water simulation carries each broken wave up the beach and back (ShoreSurf.hlsli,
// SwashStepCS). The water ends per pixel against the ground behind it (Water.hlsl).
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
#define PBRW_MAX_HULLS 8       // PBRWater::MaxHullMasks
#define PBRW_HULL_STRIDE 7     // PBRWater::HullMaskStride
#define PBRW_HULL_STATIONS 12  // FloatingObjects::HullStations

namespace PBRWater
{
	cbuffer PBRWaterData : register(b7)
	{
		float4 Cascade0[PBRW_CASCADES];  // x 1 / tile size (1/unit), yz tile coordinate of the reference camera (incl. half texel), w texel size (units)
		float4 Cascade1[PBRW_CASCADES];  // x energy-weighted angular frequency (rad/s), y 1 / its Pierson-Moskowitz weight at unlimited fetch, z energy-weighted wavenumber (rad/unit), w the band's slope variance
		float4 Params0;                  // x FFT ocean active, y wind speed (m/s), z peak omega at unlimited fetch, w gravity (units/s^2)
		float4 Params1;                  // xy wind direction, z displacement fade start, w displacement fade end (units)
		float4 Params2;                  // x choppiness, y screen-space reflections on, z displacement bound (units), w highest mip level
		float4 RefCamPos;                // xyz camera position the tile coordinates are relative to, w fog on
		float4 Tess0;                    // x the draw's vertex mode (Water.hlsl WaterVertex), y grid cell size (px), z a breaker patch's cells across, w pixels per unit at distance 1
		float4 Draw0;                    // x vertex spacing of this draw (units), y wireframe mode, z debug view, w unused
		float4 Fetch0;                   // xy grid origin (absolute world units), zw 1 / grid extent (units)
		float4 Fetch1;                   // x enabled, y direction count, z wind slice coordinate, w fetch outside the grid (m)
		float4 Terrain0;                 // xy terrain uv scale, zw terrain uv offset
		float4 Terrain1;                 // x terrain z at texel value 0, y at 1, z enabled, w vertex spacing (units)
		float4 Shore0;                   // x shore waves on (shore field ready), y blend from the previous shore field (0) to the current one (1), z the surf's output slices' blend (SurfSample), w largest run-up (units)
		float4 Shore1;                   // x crest wobble (waves), y unused, z swell Hs (units), w open-sea wind sea Hs (units)
		float4 Ripple0;                  // xy simulation origin (absolute world units), z 1 / simulation extent (units), w enabled
		float4 Ripple1;                  // x displacement scale (units), y normal strength, z unused, w texel size (uv)
		float4 Ripple2;                  // xy previous frame's simulation origin, z step interpolation (now), w step interpolation (previous frame)
		float4 Light0;                   // x base roughness, y subsurface strength, z environment reflections on, w sun specular intensity
		float4 Light1;                   // x visibility scale, y foam on (Enable Foam), z refraction distortion scale, w point light specular intensity
		float4 Foam1;                    // x the foam bias (Jacobian), y the distance at which all foam has faded out (m), z gain on the shore's foam coverages (FoamAmount x FoamBias / 0.85), w body colour from the physics (Optics.hlsli DeepReflectance)
		float4 Clarity0;                 // x sediment extinction (1/unit per unit turbidity), y base turbidity, z patchiness (0..1), w patch size (units)
		float4 Clarity1;                 // xyz sediment colour (gamma, like the water form colours), w shore resuspension strength
		float4 Clarity2;                 // x unused, y weather turbidity (storm and rain, already scaled), z wading silt strength, w sediment layer height (units)
		float4 Optics0;                  // x scattering anisotropy g, y downwelling attenuation scale, z clarity animation time (s, wrapped), w significant wave height (units)
		float4 Surface0;                 // x wind roughness scale, yz unused, w rain intensity (0..1)
		float4 Surface1;                 // x 0 no waves (ocean, shore or ripples), 1 waves, 2 waves and the per-pixel waterline, y sediment on (Enable Water Clarity), z fetch scale, w vanilla normal maps skipped (not on rivers)
		float4 Cascade2[PBRW_CASCADES];  // x height std of the band (units, open sea), yzw unused
		float4 ShoreMap;                 // xy shore field corner (absolute units), zw 1 / its extent (units)
		float4 Swash0;                   // xy wet grid corner (absolute units), z 1 / its extent (units), w enabled (ShoreSimCS; read by the lit terrain and objects)
		float4 Shore2;                   // x wave clock (rad, wrapped after ShoreClockCycles waves), y the clock last frame, z the surf grid's scroll between its slices (SurfSample), w peak period Tp (s)
		float4 Shore3;                   // x swash linger (the bed's drag on the surf simulation, times), y undular bore whelps strength, z breaker curl, w deep-water wavelength L0 (units)
		float4 Shore4;                   // x angular frequency the current shore field was traced for (rad/s), y the previous field's, z sun shadows on, w skylighting on
		float4 Shore6;                   // xy previous shore field corner (absolute units), zw 1 / its extent (units)
		float4 Grid0;                    // xy loaded cell grid min corner, zw max corner (absolute units); z <= x: none
		float4 Noise0;                   // xy surface noise grid corner (absolute units), z 1 / its extent (units), w baked (SurfaceNoiseCS)
		float4 Tune3;                    // x ripple horizontal displacement strength, y its clamp (simulation texels), z shore train reach (waves), w unused
		float4 Tune4;                    // x variation of the shore waves' heights (0 all alike, 1 Rayleigh-distributed in groups), y mean crest length (deep wavelengths), z detail normal strength in calm air, w in a gale
		float4 Tune5;                    // x point light emitter radius (units), y unused, z foam bubble size (m), w unused
		float4 Foam2;                    // xy shore foam grid corner (absolute units), z 1 / its extent (units), w enabled (ShoreFoamCS)
		float4 SunMap0;                  // xy wave heightfield centre (absolute units), z 1 / its extent (units), w drawn this frame
		float4 SunMap1;                  // x its reference level (absolute z), y height range either side of it (units), zw unused
		float4 Swash1;                   // swash churn: x tallest lump (units), y share of the depth, z lump size (units), w boil speed
		float4 Proj0;                    // a projected grid's draw (ProjectedGrid): x columns, y its level (absolute z), z cells listed, w its first vertex
		float4 Proj1;                    // the cells (SeaLevels): xy first cell, zw cells across
		float4 Proj2;                    // x its first cell, y its first patch, z a patch's cells across, w unused
		float4 WakeInfo;                 // x Kelvin wakes this frame (KelvinWakes)
		float4 Wake0[8];                 // per wake: xy absolute position (units), z k0 = g / U^2 (1/unit), w peak height (units)
		float4 Wake1[8];                 // xy heading, zw unused

		float4 Hull0;                                        // x hull masks listed in HullData
		float4 HullData[PBRW_MAX_HULLS * PBRW_HULL_STRIDE];  // per hull (InsideHull): 3 rows of its frame (axis, -axis . centre, relative to RefCamPos); bottom, top, front, back (units); 3 x 4 half widths at its stations
	}

	Texture2D<float> SeaLevels : register(t28);  // the projected grids' cells: each one's still water level (absolute z), < -1e8 none (or a river's)

	Texture2D<float4> RippleTexture : register(t80);                   // x height, y height one step earlier, w silt
	Texture2DArray<float> FetchTexture : register(t81);                // fetch in km, one slice per upwind direction
	Texture2D<float> TerrainTexture : register(t82);                   // the worldspace's terrain: its LAND vertices, normalised to Terrain1.xy
	Texture2D<float4> RipplePreviousTexture : register(t83);           // the ripple state as displayed last frame (motion vectors)
	Texture2DArray<float4> OceanDisplacement : register(t85);          // xyz displacement (units), w dDx/dy, one slice per cascade
	Texture2DArray<float4> OceanDerivatives : register(t86);           // xy height slopes, zw compression dDx/dx, dDy/dy
	Texture2DArray<float4> OceanSurface : register(t87);               // x height (units), y shear dDx/dy, z |slope|^2 (LEAN), w foam (OceanAssembleCS)
	Texture2DArray<float4> OceanPreviousDisplacement : register(t88);  // last frame's displacement (motion vectors)
	Texture2DArray<float4> ShoreProfileTexture : register(t84);        // the shore wave through its life (ShoreProfile.h): slice 0 forward, up, their d/dphase; slice 1 foam
	Texture2D<float4> KelvinWakeTexture : register(t91);               // the Kelvin wake pattern (PBRWaterWake::CreatePattern): height, d/dX, d/dY in 1 / k0
	Texture2D<float4> ShoreField0 : register(t89);                     // x travel time to the waterline tau (s; up the beach: the waterline's), y signed distance to it (units), z offset of the crests along the coast (wave cycles), w still water level (absolute z, < -1e6: none)
	Texture2D<float4> ShoreField1 : register(t90);                     // xy grad tau (s/m, seaward; up the beach: the waterline's), z beach slope (tan) at the waterline, w exposure to the swell
	Texture2D<float4> ShoreFieldPrevious : register(t92);              // ShoreField0 of the field being replaced (Shore0.y < 1)
	Texture2D<float2> ShoreRoomTexture : register(t94);                // x 0..1 room for the shore wave train: the width of the water against the wavelength, y the depth a wave's crest answers to (m; ShoreCrestDepth)
	Texture2D<float2> SurfaceNoiseTexture : register(t96);             // x TurbidityPatches around the camera (SurfaceNoiseCS), y unused
	Texture2DArray<float4> ShoreFoamTexture : register(t97);           // two slices, the surf's last two outputs (SurfSample); SwashOutCS: x unused, y the surf simulation's surface (units above the still water; the bed where dry), z its depth (units), w its foam
	SamplerState LinearClampSampler : register(s14);
	SamplerState OceanSampler : register(s15);  // anisotropic, wrap

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
	bool FoamEnabled() { return Light1.y > 0.5; }
	bool ClarityEnabled() { return Surface1.y > 0.5; }
	/// Any waves at all (ocean, shore or ripples): without, the mesh stays flat and the wave program is skipped.
	bool WavesActive() { return Surface1.x > 0.5; }
	/// The mesh held on the bed near the shore and the waterline found per pixel (Per-Pixel Waterline, with waves only).
	bool WaterlineActive() { return Surface1.x > 1.5; }
	/// The surface's costly lighting inputs, each a setting (Water.hlsl): the cubemap reflection, screen-space reflections,
	/// the sun's shadow, Skylighting's probes and the fog.
	bool EnvReflectionsEnabled() { return Light0.z > 0.5; }
	bool SSREnabled() { return Params2.y > 0.5; }
	bool SunShadowsEnabled() { return Shore4.z > 0.5; }
	bool SkylightingEnabled() { return Shore4.w > 0.5; }
	bool FogEnabled() { return RefCamPos.w > 0.5; }
	/// Vanilla Normal Maps off: the water form's normal maps are not read (Water.hlsl GetWaterNormal; rivers keep theirs).
	bool SkipVanillaNormals() { return Surface1.w > 0.5; }
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
		float damping;       // overall amplitude multiplier (distance fade, ...)
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
		float lipSpacing;    // the spacing a breaker's lip is filtered to (units; < 0: the waves' own, ShoreWaves `scale`)
	};

	/// The 2 x 2 texels a bilinear lookup at `uv` reads, loaded and clamped to the edge (TexelQuadAt).
	struct TexelQuad
	{
		float z00, z10, z01, z11;
		float2 f;   // the lookup's position between them
		int2 i0;    // the lower corner's texel, unclamped
		int2 a, b;  // the clamped lower and upper texels
		int2 hi;    // the last texel
	};

	TexelQuad TexelQuadAt(Texture2D<float> t, float2 uv)
	{
		TexelQuad q;
		uint w, h;
		t.GetDimensions(w, h);
		float2 p = uv * float2(w, h) - 0.5;
		q.i0 = int2(floor(p));
		q.f = p - q.i0;
		q.hi = int2(w, h) - 1;
		q.a = clamp(q.i0, 0, q.hi);
		q.b = clamp(q.i0 + 1, 0, q.hi);
		q.z00 = t.Load(int3(q.a.x, q.a.y, 0));
		q.z10 = t.Load(int3(q.b.x, q.a.y, 0));
		q.z01 = t.Load(int3(q.a.x, q.b.y, 0));
		q.z11 = t.Load(int3(q.b.x, q.b.y, 0));
		return q;
	}

	float TexelQuadBilinear(TexelQuad q)
	{
		return lerp(lerp(q.z00, q.z10, q.f.x), lerp(q.z01, q.z11, q.f.x), q.f.y);
	}

	/**
	 * Bilinear lookup from texel loads, clamped to the edge, and, when `withGradient`, its central differences one
	 * texel either way, `diff` = (B(+x) - B(-x), B(+y) - B(-y)). Loads, not samples: the game re-binds sampler slots,
	 * and a point sampler would turn the depth into blocks. The five lookups share one 4 x 4 block of texels.
	 */
	float BilinearLoadGrad(Texture2D<float> t, float2 uv, bool withGradient, out float2 diff)
	{
		TexelQuad q = TexelQuadAt(t, uv);
		diff = 0.0;
		[branch] if (withGradient)
		{
			int2 a = q.a, b = q.b;
			int2 m = clamp(q.i0 - 1, 0, q.hi);
			int2 n = clamp(q.i0 + 2, 0, q.hi);
			float zm0 = t.Load(int3(m.x, a.y, 0)), zm1 = t.Load(int3(m.x, b.y, 0));
			float zn0 = t.Load(int3(n.x, a.y, 0)), zn1 = t.Load(int3(n.x, b.y, 0));
			float z0m = t.Load(int3(a.x, m.y, 0)), z1m = t.Load(int3(b.x, m.y, 0));
			float z0n = t.Load(int3(a.x, n.y, 0)), z1n = t.Load(int3(b.x, n.y, 0));
			float2 f = q.f;
			diff.x = lerp(lerp(q.z10, zn0, f.x), lerp(q.z11, zn1, f.x), f.y) - lerp(lerp(zm0, q.z00, f.x), lerp(zm1, q.z01, f.x), f.y);
			diff.y = lerp(lerp(q.z01, q.z11, f.x), lerp(z0n, z1n, f.x), f.y) - lerp(lerp(z0m, z1m, f.x), lerp(q.z00, q.z10, f.x), f.y);
		}
		return TexelQuadBilinear(q);
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
	 * The highest the rendered terrain can stand at `absXY` (absolute z); -1e9 where no terrain covers it. The game
	 * draws each LAND quad as two triangles, which stand off the bilinear height (TerrainHeight) by up to a quarter of
	 * the quad's twist |z00 + z11 - z10 - z01| at its centre and not at all on its edges.
	 */
	float TerrainCeiling(float2 absXY)
	{
		float2 uv = absXY * Terrain0.xy + Terrain0.zw;
		float ceiling = -1e9;
		[branch] if (Terrain1.z > 0.5 && all(uv > 0.0) && all(uv < 1.0))
		{
			TexelQuad q = TexelQuadAt(TerrainTexture, uv);
			// Each triangle stands off the bilinear surface by twist * min(u, v) * (1 - max(u, v)) (u, v from its
			// diagonal's corner), evaluated per point so the bound is continuous across quad edges.
			float twist = q.z00 + q.z11 - q.z10 - q.z01;
			float2 f = q.f;
			float2 g = float2(1.0 - f.x, f.y);
			float alongMain = min(f.x, f.y) * (1.0 - max(f.x, f.y));   // split along 00-11
			float alongCross = min(g.x, g.y) * (1.0 - max(g.x, g.y));  // split along 10-01
			ceiling = lerp(Terrain1.x, Terrain1.y, TexelQuadBilinear(q) + max(twist * alongMain, -twist * alongCross));
		}
		return ceiling;
	}

	/// Fetch (open water distance upwind) in metres, interpolated between the two nearest precomputed directions. Short
	/// fetch means a young, short-wavelength sea.
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

	/// JONSWAP fetch-limited peak angular frequency (Hasselmann et al. 1973), clamped to the fully developed
	/// Pierson-Moskowitz peak.
	float FetchPeakOmega(float fetchMetres)
	{
		float U = max(Params0.y, 0.5);
		float F = max(fetchMetres, 10.0);
		float omega = 22.0 * pow(abs(GravityMetres * GravityMetres / (U * F)), 1.0 / 3.0);
		return max(omega, Params0.z);
	}

	/// Amplitude gain of a young, fetch-limited sea: JONSWAP alpha = 0.076 (gF/U^2)^-0.22 against Pierson-Moskowitz
	/// 0.0081, so the short waves that survive the fetch carry more energy. Amplitude scales with sqrt(alpha).
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
	/// Waves break when their height reaches this share of the depth under the crest (McCowan 1894). Mirrors
	/// PBRWaterModel::ShoreBreakerLimit.
	static const float ShoreBreakerLimit = 0.78;
	/// Height of the regular waves that carry a random sea's energy: Hrms = Hs / sqrt(2). Mirrors PBRWaterModel::ShoreHeightRatio.
	static const float ShoreHeightRatio = 0.7071;

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

	/**
	 * ShoreField0 through a cubic B-spline (four bilinear taps; Sigg and Hadwiger 2005). Bilinear interpolation of the
	 * travel time is only continuous: the crests (its isolines) kink at every texel edge, 1.8 m apart, which a breaker's
	 * sharp lip draws as a row of teeth along its crest. Next to texels the waves never reached (their travel time is
	 * huge) the bilinear value is kept, as before.
	 */
	float4 SampleShoreField0(float2 uv)
	{
		float2 size;
		ShoreField0.GetDimensions(size.x, size.y);
		float2 t = uv * size - 0.5;
		float2 i = floor(t);
		float2 f = t - i;
		float2 f2 = f * f;
		float2 f3 = f2 * f;
		float2 w0 = (-f3 + 3.0 * f2 - 3.0 * f + 1.0) / 6.0;
		float2 w1 = (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0;
		float2 w2 = (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0;
		float2 w3 = f3 / 6.0;
		float2 g0 = w0 + w1;
		float2 g1 = w2 + w3;
		float2 uv0 = (i - 0.5 + w1 / g0) / size;
		float2 uv1 = (i + 1.5 + w3 / g1) / size;
		float4 s = g0.y * (g0.x * ShoreField0.SampleLevel(LinearClampSampler, float2(uv0.x, uv0.y), 0) + g1.x * ShoreField0.SampleLevel(LinearClampSampler, float2(uv1.x, uv0.y), 0)) +
		           g1.y * (g0.x * ShoreField0.SampleLevel(LinearClampSampler, float2(uv0.x, uv1.y), 0) + g1.x * ShoreField0.SampleLevel(LinearClampSampler, float2(uv1.x, uv1.y), 0));
		[branch] if (!(abs(s.x) < 1e6))
			s = ShoreField0.SampleLevel(LinearClampSampler, uv, 0);
		return s;
	}

	ShoreRay SampleShore(float2 absXY)
	{
		ShoreRay r = (ShoreRay)0;
		r.slope = 1.0;
		r.direction = Params1.xy;
		float2 uv;
		[branch] if (ShoreFieldUV(absXY, uv))
		{
			float4 f0 = SampleShoreField0(uv);
			float4 f1 = ShoreField1.SampleLevel(LinearClampSampler, uv, 0);
			r.weight = BorderFade(uv);
			r.travelTime = f0.x;
			r.distance = f0.y;
			r.along = f0.z;
			// Each field's lag uses the frequency it was traced for (Shore4), never this frame's: those ease with the sea, and
			// omega(t) tau would sweep the crests by tau d(omega)/dt until the frequency settled.
			r.phaseLag = Shore4.x * f0.x;
			// A new field (the player moved on, the waves changed period) takes over from its predecessor over a few
			// seconds. Both are traced from the waterline, so for the same waves their travel times agree and the blend
			// moves no crest.
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

	/// tanh for x >= 0 that cannot overflow: FXC lowers the intrinsic to (e^2x - 1) / (e^2x + 1), NaN once kh exceeds
	/// ~44.
	float SafeTanh(float x)
	{
		return 1.0 - 2.0 / (exp(2.0 * min(max(x, 0.0), 20.0)) + 1.0);
	}

	/// Wavenumber (rad/m) of waves of angular frequency `omega` in `depthMetres` of water: Eckart (1952), k = k0 /
	/// sqrt(tanh(k0 h)), within a few percent of omega^2 = g k tanh(k h). Mirrors WaveModel.cpp.
	float Wavenumber(float omega, float depthMetres)
	{
		float k0 = omega * omega / GravityMetres;
		return k0 * rsqrt(max(SafeTanh(k0 * max(depthMetres, 1e-3)), 1e-6));
	}

	/// Shoaling coefficient sqrt(cg0 / cg): energy flux is conserved as waves slow over the shoaling bed (Green's law,
	/// H ~ h^-1/4, in shallow water). Mirrors PBRWaterModel::ShoalingCoefficient.
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

	/// Fetch-limited deep-water amplitude multiplier of cascade `c`: the local Pierson-Moskowitz low-frequency cut-off
	/// over the open sea's at the cascade's mean frequency (no long swell on a pond), times the young sea's energy
	/// gain.
	float FetchWeight(uint c, WaveContext ctx)
	{
		float r = ctx.peakOmega / Cascade1[c].x;
		float r2 = r * r;
		return saturate(exp(-1.25 * r2 * r2) * Cascade1[c].y) * ctx.energyGain;
	}

	/**
	 * The shore train's shoaled height here against the breaker limit of the depth: 1 where its waves break, more
	 * inshore. PBRWaterModel::ShoreSurfShare works out the same ratio.
	 * @param shoreHeightMetres significant height of the sea arriving from deep water (m)
	 * @param depthMetres the depth here (m)
	 */
	float ShoreBreakRatio(float shoreHeightMetres, float depthMetres)
	{
		float depth = max(depthMetres, 0.05);
		float shoaled = ShoreHeightRatio * shoreHeightMetres * ShoalingCoefficient(max(Shore4.x, 1e-3), depth);
		return shoaled / (ShoreBreakerLimit * depth);
	}

	/**
	 * 0..1 how close the shore train is to breaking here. Offshore the FFT sea and the train ride together; only in the
	 * surf does the train take the FFT's long waves over (ShoreHandOver). Mirrors PBRWaterModel::ShoreSurfShare.
	 * @param shoreHeightMetres significant height of the sea arriving from deep water (m)
	 * @param depthMetres the depth here (m)
	 */
	float ShoreSurfShare(float shoreHeightMetres, float depthMetres)
	{
		return smoothstep(0.3, 0.8, ShoreBreakRatio(shoreHeightMetres, depthMetres));
	}

	/// No wave is higher than this multiple of the sea's height (ShoreSet). Mirrors PBRWaterModel::ShoreSetCeiling.
	static const float ShoreSetCeiling = 2.5;
	/// How far a full onshore (+1) or offshore (-1) wind moves the breaker index: onshore wind makes waves break
	/// earlier and spill, offshore wind holds them up to plunge in shallower water (Douglass 1990: some 10 % at 10 m/s;
	/// King and Baker 1996; Feddersen and Veron 2005). Mirrors PBRWaterModel::ShoreWindBreaking.
	static const float ShoreWindBreaking = 0.12;

	/**
	 * The signed share of the wind that reaches the waves here, -1..1: its strength (nothing below 2 m/s, all of it
	 * from 12 m/s) times its component along the waves' travel. Positive: onshore. Mirrors
	 * PBRWaterModel::ShoreOnshoreWind.
	 * @param direction the waves' direction of travel (ShoreRay::direction)
	 */
	float ShoreOnshoreWind(float2 direction)
	{
		return smoothstep(2.0, 12.0, Params0.y) * dot(Params1.xy, direction);
	}

	/**
	 * Everything that makes the sea vary over the map, at undisplaced camera-relative `positionWS`.
	 * @param withGradient also work out the terrain's gradient (WaveContext::terrainGrad: eight more loads), which only
	 *   the
	 *                     geometry's hold over the bed reads (DisplaceSurface)
	 * @param river 1 on a river's mesh: no FFT sea and no breakers, only the shore train's fetch-limited waves and the
	 *   ripples
	 */
	WaveContext BuildWaveContext(float3 positionWS, float damping, bool withGradient, float river = 0.0)
	{
		WaveContext ctx;
		ctx.lipSpacing = -1.0;
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

		// The undisplaced surface point is the water plane: its own z is the water height.
		float z;
		float2 grad;
		if (SampleTerrain(absPos.xy, withGradient, z, grad)) {
			ctx.elevation = z - absPos.z;
			ctx.terrainGrad = grad;
			ctx.hasTerrain = 1.0;
		}
		ctx.depth = ctx.hasTerrain > 0.5 ? max(-ctx.elevation, 0.0) : 1e6;
		ctx.shore = SampleShore(absPos.xy);
		ctx.onshoreWind = ShoreOnshoreWind(ctx.shore.direction);

		// Depth-limited breaking: in the surf zone the significant height of random waves stays below ~0.6 of the depth
		// (Thornton & Guza 1983). Every cascade keeps that share of its shoaled height.
		float depthMetres = ctx.depth * MetresPerUnit;
		float deepVariance = 0.0;
		float shoaledVariance = 0.0;
		float4 shoaledWeight = 0.0;  // fetch weight times shoaling of each cascade
		[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
		{
			float fetch = FetchWeight(c, ctx);
			// Waves only feel a bottom shallower than about half their wavelength (k h < pi). A branch, so deep water
			// skips the shoaling.
			float shoaling = 1.0;
			[branch] if (Cascade1[c].z * ctx.depth < Math::PI)
				shoaling = ShoalingCoefficient(Cascade1[c].x, depthMetres);
			shoaledWeight[c] = fetch * shoaling;
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
		// (Battjes 1974): waves spill (xi < 0.5) and plunge on gentle beaches and surge up rock faces and cliffs (xi >
		// 3.3) without breaking. H0 and L0 are the shore train's (its deep-water height, ShoreDeepHeight, and its own
		// period), not the local wind sea's.
		float breakerHeight = ShoreHeightRatio * ctx.shoreHeight * MetresPerUnit;
		float breakerOmega = Shore4.x;
		if (breakerHeight <= 0.0 || breakerOmega <= 1e-3) {
			breakerHeight = ctx.deepHeight * MetresPerUnit;
			breakerOmega = ctx.peakOmega;
		}
		float steepness = breakerHeight / (Math::TAU * GravityMetres / max(breakerOmega * breakerOmega, 1e-4));
		float iribarren = ctx.shore.slope * rsqrt(max(steepness, 1e-4));
		ctx.iribarren = iribarren;
		ctx.beach = smoothstep(3.3, 2.0, iribarren) * ctx.shore.weight * ctx.shore.room * ctx.hasTerrain * saturate(ctx.deepHeight - 1.0) * (1.0 - river);

		// Hand-over from the FFT to the shore wave train, counted in waves from the shore (Skyrim's beds fall away
		// within a wave or two of the waterline, too close for a depth limit). Narrow water (streams, ponds) has no
		// room for the train; there the FFT sea, fetch- and depth-limited, carries all.
		float shoreCycles = ctx.shore.travelTime * Shore4.x / Math::TAU;
		ctx.train = smoothstep(Tune3.z, 0.5 * Tune3.z, shoreCycles) * ctx.shore.weight * ctx.shore.room * ctx.hasTerrain;

		// Each cascade: the fetch-limited deep-water sea, shoaled over the bed (Green's law) and held to the
		// depth-limited breaking height. Offshore the train rides on the FFT sea; in the surf its breakers take the
		// random sea's energy up to twice their frequency and only the short chop rides on top (ShoreHandOver). Mirrors
		// WaveModel's BuildContext.
		ctx.sea = shoaledWeight * ctx.saturation * ctx.damping * (1.0 - river);
		ctx.cascade = ctx.sea;
		// Away from the train (most water) there is nothing to hand over.
		[branch] if (ctx.train > 0.0)
		{
			float handOver = ctx.train * ShoreSurfShare(ctx.shoreHeight * MetresPerUnit, depthMetres);
			[unroll] for (uint i = 0; i < PBRW_CASCADES; i++)
				ctx.cascade[i] *= 1.0 - handOver * ShoreHandOver(Cascade1[i].x);
		}
		return ctx;
	}

	/// Distance fade of the displacement, so far water converges to the flat vanilla plane (and the LOD meshes).
	float DisplacementDistanceFade(float distance)
	{
		return 1.0 - smoothstep(Params1.z, max(Params1.w, Params1.z + 1.0), distance);
	}

	/**
	 * 0 at the border of the loaded cells, 1 from 2048 units inside them (absolute `absXY`): the flat LOD water meets
	 * the cells' meshes there with T-junctions, so no wave may reach the border.
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

	/// A draw of a projected grid (ProjectedGrid, Water.hlsl): Tess0.x 6 its cells, 7 its breakers' patches.
	bool GridDraw() { return Tess0.x > 5.5; }

	/// Whether the grid's level is at absolute `absXY`: a cell with water at this draw's level (Unified Water's coverage). A
	/// projected grid spans its whole plane; the water is only where the engine's would be.
	bool SeaCovers(float2 absXY)
	{
		float2 cell = floor(absXY * (1.0 / 4096.0)) - Proj1.xy;
		if (Proj0.z < 0.5 || any(cell < 0.0) || any(cell >= Proj1.zw))
			return false;
		return abs(SeaLevels.Load(int3(cell, 0)) - Proj0.y) < 2.0;
	}

	/// Whether camera-relative `positionWS` is inside one of the hulls (FloatingObjects::HullMask): the water is not drawn
	/// there, so the waves passing through a boat stay out of it. Each hull is its outline at the waterline, measured
	/// against its collision at stations along its length, between its keel and its top, turned with the hull.
	bool InsideHull(float3 positionWS)
	{
		uint count = (uint)Hull0.x;
		[branch] if (count == 0) return false;
		float3 p = positionWS + (FrameBuffer::CameraPosAdjust.xyz - RefCamPos.xyz);
		[loop] for (uint h = 0; h < count; ++h)
		{
			uint base = h * PBRW_HULL_STRIDE;
			float3 local = float3(dot(HullData[base].xyz, p) + HullData[base].w, dot(HullData[base + 1].xyz, p) + HullData[base + 1].w,
				dot(HullData[base + 2].xyz, p) + HullData[base + 2].w);
			float4 extent = HullData[base + 3];  // bottom, top, front, back
			if (local.z <= extent.x || local.z >= extent.y || local.x >= extent.z || local.x <= -extent.w)
				continue;
			// The outline between the stations, narrowing to the ends half a station out from the last ones.
			float t = (local.x + extent.w) / max(extent.z + extent.w, 1.0) * PBRW_HULL_STATIONS - 0.5;
			float i0 = clamp(floor(t), 0.0, PBRW_HULL_STATIONS - 2.0);
			uint s0 = (uint)i0;
			uint s1 = s0 + 1;
			float w0 = dot(HullData[base + 4 + s0 / 4], float4((s0 % 4) == uint4(0, 1, 2, 3)));
			float w1 = dot(HullData[base + 4 + s1 / 4], float4((s1 % 4) == uint4(0, 1, 2, 3)));
			float width = lerp(w0, w1, saturate(t - i0));
			width *= saturate(2.0 * (t + 0.5)) * saturate(2.0 * (PBRW_HULL_STATIONS - 0.5 - t));
			if (abs(local.y) < width)
				return true;
		}
		return false;
	}

	/// Tile coordinate of camera-relative position `p` in cascade `c` (world-anchored, re-based on the reference camera
	/// so other cameras sample the same water).
	float2 CascadeUV(float2 p, uint c)
	{
		float2 rel = p + (FrameBuffer::CameraPosAdjust.xy - RefCamPos.xy);
		return rel * Cascade0[c].x + Cascade0[c].yz;
	}

}

#include "PBRWater/ShoreWaves.hlsli"

#include "PBRWater/ShoreSurf.hlsli"  // after ShoreWaves: it composes the train

namespace PBRWater
{
	struct WaveResult
	{
		float3 displacement;          // world-space offset (units)
		float3 previousDisplacement;  // the same last frame (motion vectors)
		float3 normal;                // surface normal
		float fftFoam;                // 0..1 the open sea's foam here: the cascades' accumulated foam (FftFoam), already faded with distance
		float slopeVariance;          // slope variance filtered out of the pixel (for roughness)
		float breakStage;             // 0..1 how far the shore waves here have got with breaking (s)
		float curl;                   // 0..1 a breaker's curl shown here (ShoreTrain::curl): the normal may face down
		float crest;                  // units above the still water the shore wave's crest stands here (ShoreTrain::crest)
		float breakerFoam;            // 0..1 the breaking wave's white water (BreakerFoam)
		float aeration;               // 0..1 churned white water (ShoreTrain::aeration)
		float clearFace;              // 0..1 a face about to plunge, clear of foam (ShoreTrain::clearFace)
		float curlNeed;               // 0..1 a breaker's curl wants a finer mesh here (ShoreTrain::curlNeed)
		float heightRaw;              // units above the still water the waves stand here without the surf simulation (ShoreTrain::rawHeight and the cascades)
		float surf;                   // 0..1 share of the surf simulation in the surface here (ShoreTrain::surf)
		float sheetSurface;           // the surf simulation's surface here (units above the still water; ShoreTrain::sheetSurface)
		float sheetDepth;             // its depth (units)
		float sheetFoam;              // 0..1 the white water it carries
		float3 lip;                   // a breaker's overturning lip, included in `displacement` (ShoreTrain::lip)
		float3 previousLip;           // the same last frame
	};

	/// Added to each cascade's foam before they are summed, longest waves first (GarrettGunnell/Water's _FoamSubtract0..3):
	/// the short cascades foam only where the long ones already do.
	static const float4 FoamCascadeOffset = float4(0.04, -0.04, -0.46, -0.38);

	/**
	 * The open sea's foam, 0..1: the weighted sum of the cascades' foam plus their offsets, clamped. Its fade with
	 * distance is FoamFade, applied to all the foam together.
	 * @param sum      the cascades' weighted foam and offsets
	 */
	float FftFoam(float sum) { return saturate(sum); }

	/**
	 * The fade of every foam with the camera distance, 0..1, linear out to Foam Fade Distance (Foam1.y).
	 * @param distance camera distance of the pixel (units)
	 */
	float FoamFade(float distance)
	{
		return 1.0 - saturate(distance * MetresPerUnit / max(Foam1.y, 1.0));
	}

	/// Running sums of the cascades' fields.
	struct WaveSums
	{
		float3 displacement;
		float3 previous;
		float dzdx, dzdy, dxdx, dydy, dxdy;
		float foam;  // the cascades' foam so far, weighted and offset (FftFoam)
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
	 * The breaking wave's white water, 0..1: the profile's foam (the lip's crown, the spill down the face, the bore's front:
	 * ShoreProfileAt slice 1) and the air the jet churns in (ShoreAeration: the crushed tube, the splash-up, the roller),
	 * scaled by Foam Amount and Foam Coverage (Foam1.z). A plain coverage, drawn through the foam pattern with the rest of
	 * the foam (Water.hlsl).
	 */
	float BreakerFoam(ShoreTrain train)
	{
		return saturate(max(train.foam, train.aeration) * Foam1.z);
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
		o.curl = train.curl;
		o.crest = train.crest;
		o.breakerFoam = BreakerFoam(train);
		o.aeration = train.aeration;
		o.clearFace = train.clearFace;
		o.curlNeed = train.curlNeed;
		o.surf = train.surf;
		o.sheetSurface = train.sheetSurface;
		o.sheetDepth = train.sheetDepth;
		o.sheetFoam = train.sheetFoam;
		o.lip = train.lip;
		o.previousLip = train.previousLip;
	}

	/// Water this deep (units) is kept over the bed where the bed lies below the still water (SeaFloorHold): more than
	/// the colour pass's waterline fade (Water.hlsl, 6 cm), so the rendered terrain a few cm above the bilinear bed
	/// stays under it.
	static const float SeaFloorFilm = 0.1 * UnitsPerMetre;

	/**
	 * Where the bed lies below the still water, out to sea of the inner surf (ShoreTrain::seaHold), the sea never parts
	 * from it: a shore wave's trough stays a film above the bed (the train's troughs are shoaled profiles over the
	 * depth at the crest, not a depth-limited solution). Inshore the simulation's sheet decides (SurfInner). The lip is
	 * left as it is.
	 */
	void SeaFloorHold(inout WaveSums sums, WaveContext ctx, ShoreTrain train)
	{
		[branch] if (ctx.hasTerrain > 0.5 && ctx.elevation < 0.0 && train.seaHold > 0.0)
		{
			float floorZ = ctx.elevation + SeaFloorFilm;
			float z = sums.displacement.z - train.lip.z;
			float previous = sums.previous.z - train.previousLip.z;
			sums.displacement.z = lerp(z, max(z, floorZ), train.seaHold) + train.lip.z;
			sums.previous.z = lerp(previous, max(previous, floorZ), train.seaHold) + train.previousLip.z;
		}
	}

	WaveResult FinishWaves(WaveSums sums, WaveResult o)
	{
		// Normal of the parametric surface P(x, y) = (x + Dx, y + Dy, Dz)
		float3 tx = float3(1.0 + sums.dxdx, sums.dxdy, sums.dzdx);
		float3 ty = float3(sums.dxdy, 1.0 + sums.dydy, sums.dzdy);
		// tx and ty become parallel where the surface folds over itself: the normal is kept defined there and facing up
		// where it has folded past vertical. A breaker's curl is one sheet turning over, so the underside of its lip
		// faces down.
		float3 n = cross(tx, ty);
		o.normal = SafeNormalize(n.z < 0.0 && sums.curl <= 0.0 ? -n : n, float3(0, 0, 1));
		o.displacement = sums.displacement;
		o.previousDisplacement = sums.previous;
		o.slopeVariance = sums.slopeVariance;
		return o;
	}

	/**
	 * Waves for geometry at undisplaced camera-relative `positionWS`. Each cascade is read from the mip whose texels
	 * match the vertex spacing, so waves the mesh cannot carry are averaged out rather than aliased; the shore train is
	 * filtered the same way. Where the surf simulation is the water its surface replaces the train's (ComposeSurf).
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
					// Texels of 1.4x the vertex spacing: the shortest wave left spans ~3 vertices (Nyquist needs 2), so
					// the distant sea does not shimmer as the projected grid's vertices slide over it.
					float lod = spacing > 0.0 ? clamp(log2(spacing / Cascade0[c].w) + 0.5, 0.0, Params2.w) : 0.0;
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
		// The shore train in the geometry only where the vertices are a few metres apart; the pixels carry the distant
		// breakers (EvaluateWavesFiltered).
		float shoreGeometry = saturate(2.0 - spacing / (4.0 * UnitsPerMetre));
		o.heightRaw = sums.displacement.z;
		[branch] if (shoreGeometry > 0.0)
		{
			WaveSums sea = sums;
			ShoreTrain train = ShoreWaves(positionWS, ctx, spacing, previous, false);
			ComposeSurf(train, positionWS, ctx, false, spacing, spacing);
			AddShoreTrain(sums, o, train);
			SeaFloorHold(sums, ctx, train);
			sums.displacement = lerp(sea.displacement, sums.displacement, shoreGeometry);
			sums.previous = lerp(sea.previous, sums.previous, shoreGeometry);
			sums.dzdx = lerp(sea.dzdx, sums.dzdx, shoreGeometry);
			sums.dzdy = lerp(sea.dzdy, sums.dzdy, shoreGeometry);
		}
		return FinishWaves(sums, o);
	}

	/**
	 * Share of a texture's LEAN / Toksvig variance that is real at this pixel: none while the texture is magnified,
	 * where bilinear interpolation of the second moment invents variance between texels. Real from mip 0 on along the
	 * footprint's long axis (anisotropic taps average the short one).
	 * @param gx, gy screen derivatives of the texture coordinate, in texels
	 */
	float MinifiedShare(float2 gx, float2 gy)
	{
		return saturate(0.5 * log2(max(max(dot(gx, gx), dot(gy, gy)), 1e-8)));
	}

#if defined(PBRW_MATERIAL_CS)
	// A compute shader has no screen derivatives: SurfaceMaterialCS sets how the undisplaced position (xy) changes from one
	// pixel to the next one right and down (units); the cascades' texture coordinates follow it linearly (CascadeUV).
	static float2 PositionDdx = 0.0;
	static float2 PositionDdy = 0.0;
#endif

#if defined(PSHADER) || defined(PBRW_MATERIAL_CS)
	// The shore train the deferred surface's pixels read back (DeferredShoreCS): set per pixel by
	// PBRWaterDeferred::Resolve (or by SurfaceMaterialCS), t46-t48 bound with the record.
	Texture2D<uint4> BakedShore0 : register(t46);
	Texture2D<uint4> BakedShore1 : register(t47);
	Texture2D<uint4> BakedShore2 : register(t48);
	static bool BakedShoreActive = false;
	static int2 BakedShorePixel = 0;
	static float BakedShoreDistance = 0.0;

	float2 UnpackHalves(uint v)
	{
		return float2(f16tof32(v & 0xFFFF), f16tof32(v >> 16));
	}

	float4 UnpackUnorm4(uint v)
	{
		return float4(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24) / 255.0;
	}

#	if defined(PBRW_BAKED_SHORE_SHARED)
	// The material pass gathered its tile's blocks into group-shared memory first (SurfaceMaterialCS): read them there, once
	// per tile instead of up to twelve reads per pixel.
	uint4 BakedShoreLoad0(int2 c, int2 block) { return SharedShoreLoad0(block); }
	uint4 BakedShoreLoad1(int2 c, int2 block) { return SharedShoreLoad1(block); }
	uint4 BakedShoreLoad2(int2 c, int2 block) { return SharedShoreLoad2(block); }
#	else
	uint4 BakedShoreLoad0(int2 c, int2 block) { return BakedShore0.Load(int3(c, 0)); }
	uint4 BakedShoreLoad1(int2 c, int2 block) { return BakedShore1.Load(int3(c, 0)); }
	uint4 BakedShoreLoad2(int2 c, int2 block) { return BakedShore2.Load(int3(c, 0)); }
#	endif

	/**
	 * The shore train at this pixel from DeferredShoreCS: the four nearest 2x2 blocks blended bilinearly, each only where
	 * it was evaluated at about this pixel's distance (the same water, not a wave in front of it or the sea behind).
	 * @return false where no block is: the pixel evaluates the train itself
	 */
	bool BakedShoreTrain(out ShoreTrain t)
	{
		t = (ShoreTrain)0;
		uint width, height;
		BakedShore0.GetDimensions(width, height);
		float2 h = (BakedShorePixel + 0.5) * 0.5 - 0.5;
		int2 base = (int2)floor(h);
		float2 f = h - base;
		float4 a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0, a4 = 0.0, u0 = 0.0, u1 = 0.0;
		float total = 0.0;
		[unroll] for (uint i = 0; i < 4; i++)
		{
			int2 offset = int2(i & 1, i >> 1);
			int2 block = base + offset;
			int2 c = clamp(block, 0, int2(width, height) - 1);
			float bilinear = (offset.x ? f.x : 1.0 - f.x) * (offset.y ? f.y : 1.0 - f.y);
			uint4 o0 = BakedShoreLoad0(c, block);
			float2 zd = UnpackHalves(o0.y);
			float w = (zd.y > 0.0 && abs(zd.y - BakedShoreDistance) < 0.1 * BakedShoreDistance) ? max(bilinear, 1e-3) : 0.0;
			[branch] if (w > 0.0)
			{
				uint4 o1 = BakedShoreLoad1(c, block);
				uint4 o2 = BakedShoreLoad2(c, block);
				a0 += w * float4(UnpackHalves(o0.x), zd.x, 0.0);
				a1 += w * float4(UnpackHalves(o0.z), UnpackHalves(o0.w));
				a2 += w * float4(UnpackHalves(o1.x), UnpackHalves(o1.y));
				a3 += w * float4(UnpackHalves(o1.z), UnpackHalves(o1.w));
				a4 += w * float4(UnpackHalves(o2.x), UnpackHalves(o2.y));
				u0 += w * UnpackUnorm4(o2.z);
				u1 += w * UnpackUnorm4(o2.w);
				total += w;
			}
		}
		if (total <= 0.0)
			return false;
		float r = rcp(total);
		a0 *= r, a1 *= r, a2 *= r, a3 *= r, a4 *= r, u0 *= r, u1 *= r;
		t.displacement = a0.xyz;
		t.lip = a1.xyz;
		t.curl = a1.w;
		t.slope = a2.xy;
		t.lipSlope = a2.zw;
		t.compression = a3.xyz;
		t.slopeVariance = a3.w;
		t.lipCompression = a4.xyz;
		t.crest = a4.w;
		t.foam = u0.x;
		t.breakStage = u0.y;
		t.bore = u0.z;
		t.aeration = u0.w;
		t.impact = u1.x;
		t.clearFace = u1.y;
		return true;
	}
#endif

	static const float KelvinWakeLength = 30.0;  // PBRWaterWake::Length
	static const float KelvinWakeWidth = 15.0;   // PBRWaterWake::Width

	/**
	 * The Kelvin wakes of the actors moving through the water at camera-relative `positionWS`: height (units) and its
	 * slope. Each lays the pattern behind it along its heading, scaled by its speed (k0 = g / U^2: the transverse waves are
	 * 2 pi / k0 long). Faded where the sampling (`spacing`, units) cannot carry half a transverse wave.
	 */
	float KelvinWakes(float3 positionWS, float spacing, out float2 slope)
	{
		slope = 0.0;
		float height = 0.0;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		uint count = (uint)WakeInfo.x;
		[loop] for (uint i = 0; i < count; i++)
		{
			float4 a = Wake0[i];
			float2 heading = Wake1[i].xy;
			float2 d = absXY - a.xy;
			float x = -dot(d, heading) * a.z;
			float y = (heading.x * d.y - heading.y * d.x) * a.z;
			if (x <= 0.0 || x >= KelvinWakeLength || abs(y) >= KelvinWakeWidth)
				continue;
			float resolved = saturate(2.0 - spacing * a.z / Math::PI);
			if (resolved <= 0.0)
				continue;
			float4 t = KelvinWakeTexture.SampleLevel(LinearClampSampler, float2(x / KelvinWakeLength, abs(y) / KelvinWakeWidth), 0);
			float amplitude = a.w * resolved;
			height += amplitude * t.x;
			float side = y < 0.0 ? -1.0 : 1.0;
			slope += amplitude * a.z * (-t.y * heading + t.z * side * float2(-heading.y, heading.x));
		}
		return height;
	}

	/**
	 * Waves for shading at undisplaced camera-relative `positionWS`, filtered over the pixel footprint
	 * (trilinear / anisotropic). The slope variance the filter removes is recovered from the mip-filtered
	 * second moments (LEAN mapping) and returned for the roughness, as is the variance of the shore wave
	 * train where the pixel cannot carry it.
	 * @param footprint the pixel's footprint on the water (units): the longer of its screen derivatives
	 */
	WaveResult EvaluateWavesFiltered(float3 positionWS, WaveContext ctx, float footprint)
	{
		WaveResult o = (WaveResult)0;
		WaveSums sums = EmptySums();
		float steepnessScale = rcp(ctx.energyGain);
		if (OceanActive()) {
			[unroll] for (uint c = 0; c < PBRW_CASCADES; c++)
			{
				float vertical = ctx.cascade[c];
				float3 uv = float3(CascadeUV(positionWS.xy, c), c);
				// The screen derivatives need uniform control flow; sampling with them as explicit gradients does not, so a
				// cascade the context has turned off (the shore train's, the open sea's in a pond) costs no texture reads.
#if defined(PBRW_MATERIAL_CS)
				float2 gx = PositionDdx * Cascade0[c].x;
				float2 gy = PositionDdy * Cascade0[c].x;
#else
				float2 gx = ddx_fine(uv.xy);
				float2 gy = ddy_fine(uv.xy);
#endif
				// A pixel whose footprint covers half the tile or more reads the cascade's top mips, its mean: only its
				// slope variance counts (Cascade1.w), so its reads are skipped there, faded in over the footprint
				// before.
				float filtered = smoothstep(0.25, 0.5, sqrt(min(dot(gx, gx), dot(gy, gy))));
				[branch] if (vertical > 0.0 && filtered >= 1.0)
				{
					sums.slopeVariance += Cascade1[c].w * vertical * vertical;
					sums.foam += saturate(vertical) * FoamCascadeOffset[c];
				}
				else if (vertical > 0.0)
				{
					// Two reads: the derivatives, and the rest the pixel needs (OceanSurface: height, shear, squared
					// slope, foam). The horizontal displacement is the vertices' alone.
					float4 g = OceanDerivatives.SampleGrad(OceanSampler, uv, gx, gy);
					float4 s = OceanSurface.SampleGrad(OceanSampler, uv, gx, gy);
					float4 d = float4(0.0, 0.0, s.x, s.y);
					// Enable Foam off: none.
					float foam = FoamEnabled() ? s.w : 0.0;
					// Fading towards the mean: its share of the read, and the band's variance for the rest.
					float kept = 1.0 - filtered;
					float horizontal = vertical * steepnessScale;
					AddCascade(sums, c, vertical * kept, horizontal * kept, d, g);
					sums.foam += saturate(vertical) * (foam * kept + FoamCascadeOffset[c]);
					float variance = max(s.z - dot(g.xy, g.xy), 0.0);
					float texelsPerUV = rcp(Cascade0[c].x * Cascade0[c].w);
					float minified = MinifiedShare(gx * texelsPerUV, gy * texelsPerUV);
					sums.slopeVariance += (variance * minified * kept + Cascade1[c].w * filtered) * vertical * vertical;
				}
			}
		}
		// The actors' Kelvin wakes, filtered to the pixel's footprint.
		{
			float2 wakeSlope;
			sums.displacement.z += KelvinWakes(positionWS, footprint, wakeSlope);
			sums.dzdx += wakeSlope.x;
			sums.dzdy += wakeSlope.y;
		}
		ShoreTrain train = (ShoreTrain)0;
#if defined(PSHADER) || defined(PBRW_MATERIAL_CS)
		// The deferred surface has the train once per 2x2 pixels (DeferredShoreCS).
		bool baked = BakedShoreActive && BakedShoreTrain(train);
		[branch] if (!baked)
#endif
			train = ShoreWaves(positionWS, ctx, footprint, false, true);
		ComposeSurf(train, positionWS, ctx, true, 0.0, footprint);
		AddShoreTrain(sums, o, train);
		SeaFloorHold(sums, ctx, train);
		o = FinishWaves(sums, o);
		// A face about to plunge is clear water: the sea's foam is drawn off it (ShoreClearFace).
		o.fftFoam = FftFoam(sums.foam) * (1.0 - o.clearFace);
		return o;
	}

	// ------------------------------------------------------------------------
	// Low-frequency surface noise: drifting sediment plumes tens of metres across.
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

	/**
	 * The low-frequency surface noise at absolute `absXY`: x TurbidityPatches, y unused (1). Read from this frame's
	 * bake around the camera (SurfaceNoiseCS), fading to its mean towards the bake's edge. Without Water Clarity
	 * nothing reads the sediment and no noise is baked.
	 */
	float2 SurfaceNoise(float2 absXY)
	{
		static const float MeanPatches = 0.5;
		float2 noise = float2(0.0, 1.0);
		[branch] if (ClarityEnabled())
		{
			float2 uv = (absXY - Noise0.xy) * Noise0.z;
			float2 edge = abs(uv - 0.5) * 2.0;
			float kept = Noise0.w > 0.5 ? saturate((1.0 - max(edge.x, edge.y)) / 0.2) : 0.0;
			noise.x = MeanPatches;
			[branch] if (kept > 0.0)
				noise.x = lerp(MeanPatches, SurfaceNoiseTexture.SampleLevel(LinearClampSampler, uv, 0).x, kept);
		}
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

	/// The displayed state (height in simulation units) at `uv`, of this frame (or last frame's copy for motion vectors).
	float RippleDisplayedAt(float2 uv, bool previous)
	{
		float2 steps = previous ? RipplePreviousTexture.SampleLevel(LinearClampSampler, uv, 0).xy :
		                          RippleTexture.SampleLevel(LinearClampSampler, uv, 0).xy;
		return lerp(steps.y, steps.x, previous ? Ripple2.w : Ripple2.z);
	}

	/// Displayed ripple height. The simulation runs at a fixed rate, so the two stored steps are
	/// interpolated to the render time; `previous` samples last frame's copy for motion vectors.
	float RippleHeight(float3 positionWS, bool previous)
	{
		float2 uv;
		if (!RippleUV(positionWS.xy + FrameBuffer::CameraPosAdjust.xy, previous ? Ripple2.xy : Ripple0.xy, uv))
			return 0.0;
		return RippleDisplayedAt(uv, previous) * Ripple1.x * BorderFade(uv);
	}

	// Horizontal ripple displacement (Crest 4 ShapeCombine, _DYNAMIC_WAVE_SIM_ON): the surface shifts along the
	// ripples' gradient like a Gerstner wave, which sharpens the crests. Strength Tune3.x (Crest default 3), clamp
	// Tune3.y in simulation texels (Crest default 0.3).

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
		float hL = RippleDisplayedAt(uv - float2(t, 0), false);
		float hR = RippleDisplayedAt(uv + float2(t, 0), false);
		float hD = RippleDisplayedAt(uv - float2(0, t), false);
		float hU = RippleDisplayedAt(uv + float2(0, t), false);
		float texelUnits = t / Ripple0.z;
		float2 grad = float2(hR - hL, hU - hD) / (2.0 * texelUnits) * Ripple1.x * Ripple1.y * RippleDepthFade(depth);
		return grad * BorderFade(uv);
	}

	// ------------------------------------------------------------------------
	// Waterline
	// ------------------------------------------------------------------------

	/// Largest vertical reach of the surface (units): waves within their statistical bound (doubled for the fetch
	/// energy gain), the shore train (shoaled to about the deep-water height, crest up to 2A above the mean), the
	/// surf's run-up and the ripples. Used to clamp, which also flushes NaN.
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
	 * True where the ground at this pixel lies above the water. The surf simulation is read per pixel (its sheet ends
	 * abruptly), the waves and the simulation's share come from the vertices. Both passes test the same thing; the
	 * colour pass then finds the exact edge against the depth buffer (Water.hlsl), which the slack keeps inside.
	 * @param positionWS camera-relative position of the displaced surface
	 * @param shoreState the vertex shader's SurfaceDisplacement::shore
	 */
	bool AboveWaterline(float3 positionWS, float4 shoreState)
	{
		// The mesh is only lifted onto the bed with the waterline on (DisplaceSurface): otherwise the depth test keeps the ground.
		[branch] if (!WaterlineActive()) return false;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		float terrainZ = TerrainHeight(absXY);
		if (terrainZ < -1e8)
			return false;
		float bed = terrainZ - (shoreState.w + FrameBuffer::CameraPosAdjust.z);
		float water = shoreState.z;
		float hold = 1.0;
		[branch] if (shoreState.x > 0.0)
		{
			SurfSheet sheet = SampleSurf(absXY, false, 0.0);
			// A drained cell below the still water in the hand-over band is still sea (ComposeSurf): the waves' surface
			// stands there. Inshore of it the simulation's sheet is the water.
			float drawn = shoreState.x * sheet.share;
			float inner = SurfInner(drawn);
			hold = 1.0 - inner;
			float belowSea = (1.0 - smoothstep(-0.05 * UnitsPerMetre, 0.0, bed)) * (inner < 0.5 ? 1.0 : 0.0);
			float share = drawn * lerp(1.0, smoothstep(0.0, 2.0 * SwashMinDepth, sheet.depth), belowSea);
			water = lerp(water, sheet.depth > SwashMinDepth ? sheet.surface : -1e6, share);
		}
		// The sea bed below the still water out to sea of the inner surf is always under water (SeaFloorHold).
		if (bed < 0.0)
			water = lerp(water, max(water, bed + SeaFloorFilm), hold);
		return bed > water + 0.05 * UnitsPerMetre;
	}

	// ------------------------------------------------------------------------
	// Full surface displacement used by geometry (VS / DS)
	// ------------------------------------------------------------------------
	struct SurfaceDisplacement
	{
		float3 current;
		float3 previous;
		float4 shore;  // x share of the surf simulation in the surface (SurfSharesAt, before the grid's border fade), y a breaker curls here (WaveResult::curlNeed), z the waves without it (units above the still water), w still water level (camera-relative z)
	};

	/// @param river  1 on a river's mesh (BuildWaveContext)
	/// @param motion also work out last frame's surface: only the stencil pass writes motion vectors
	/// @param lipSpacing the spacing a breaker's lip is filtered to (< 0: `spacing`): a projected grid's patch carries the
	///                   lip finer than the rest of the waves, which stay filtered as on its coarse neighbours
	SurfaceDisplacement DisplaceSurface(float3 positionWS, float river, float spacing, bool motion, float lipSpacing = -1.0)
	{
		SurfaceDisplacement o;
		[branch] if (!WavesActive())
		{
			o.current = 0.0;
			o.previous = 0.0;
			o.shore = float4(0.0, 0.0, 0.0, positionWS.z);
			return o;
		}
		float damping = DisplacementFade(positionWS);

		WaveContext ctx = BuildWaveContext(positionWS, damping, true, river);
		ctx.lipSpacing = lipSpacing;
		WaveResult now = EvaluateWaves(positionWS, ctx, spacing, motion);

		float rippleFade = damping * RippleDepthFade(ctx.depth);
		float3 ripple = RippleDisplacement(positionWS, false) * rippleFade;
		o.current = now.displacement + ripple;
		o.previous = motion ? now.previousDisplacement + RippleDisplacement(positionWS, true) * rippleFade : 0.0;
		// The actors' Kelvin wakes (last frame's taken as this one's).
		float2 wakeSlope;
		float wake = KelvinWakes(positionWS, spacing, wakeSlope) * damping;
		o.current.z += wake;
		o.previous.z += motion ? wake : 0.0;
		float2 absXY = positionWS.xy + FrameBuffer::CameraPosAdjust.xy;
		// The waves alone: AboveWaterline blends them with the surf simulation per pixel.
		float waves = now.heightRaw + ripple.z;

		// Near the waterline the mesh never dips under the bed: where the water ends is decided per pixel against the
		// ground (AboveWaterline, Water.hlsl), so the edge follows the terrain, not the triangles. The mesh lies on the
		// bed within reach of the water at the next vertex, and on the surf simulation's grid up to the highest run-up
		// (Shore0.w), so it is in place before each sheet arrives and after it drains; never above where any water
		// stands. The bed is taken where the waves have carried the vertex, as high as the game's terrain triangles
		// stand there (TerrainCeiling). A breaker's lip is laid over the held surface, not held itself.
		[branch] if (WaterlineActive() && ctx.hasTerrain > 0.5)
		{
			float3 base = o.current - now.lip;
			float ceiling = TerrainCeiling(absXY + base.xy);
			float bed = ceiling > -1e8 ? ceiling - (positionWS.z + FrameBuffer::CameraPosAdjust.z) : ctx.elevation;
			float water = base.z;
			float highest = max(water, max(Shore0.w, 0.0) + 0.5 * UnitsPerMetre);
			float2 uv;
			// Only where the vertices are a few metres apart: the projected grid's distant cells leave the edge to the
			// pixels' waterline (AboveWaterline).
			float holdSpacing = min(spacing, 4.0 * UnitsPerMetre);
			float hold = saturate(2.0 - spacing / (4.0 * UnitsPerMetre));
			float held = ShoreGridUV(absXY, uv) ? highest : min(water + max(0.5 * UnitsPerMetre, 2.0 * holdSpacing * length(ctx.terrainGrad)), highest);
			float lifted = min(bed, held) + WaterlineLift(holdSpacing, ctx.terrainGrad);
			o.current.z = lerp(base.z, max(base.z, lifted), hold) + now.lip.z;
			o.previous.z = lerp(o.previous.z - now.previousLip.z, max(o.previous.z - now.previousLip.z, lifted), hold) + now.previousLip.z;
		}

		// Hard bound. Clamping also flushes NaN (D3D min/max return the non-NaN operand); a bad input lands on -limit,
		// a hole in the water rather than a mesh thrown off-screen.
		float limit = DisplacementLimit();
		o.current = clamp(o.current, -limit, limit);
		o.previous = clamp(o.previous, -limit, limit);
		float2 gridUV;
		float surf = 0.0;
		[branch] if (ctx.hasTerrain > 0.5 && ShoreGridUV(absXY, gridUV))
			surf = SurfSharesAt(ctx).drawn;
		// y: a breaker curls here, where the projected grid refines (ProjectedGridCS FlagCS).
		o.shore = float4(surf, now.curlNeed, clamp(waves, -limit, limit), positionWS.z);
		return o;
	}
}

#endif  // __PBR_WATER_HLSLI__
